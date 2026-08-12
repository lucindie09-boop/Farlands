#ifndef FUK_MINECRAFT_VOXEL_ENGINE_THREAD_POOL_HPP
#define FUK_MINECRAFT_VOXEL_ENGINE_THREAD_POOL_HPP
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <type_traits>
#include <queue>
#include <thread>
#include <vector>

namespace VoxelEngine {

struct Task {
    virtual ~Task() = default;
    virtual void execute() = 0;
};

template<typename F>
struct FnTask : Task {
    explicit FnTask(F f) : fn_(std::move(f)) {}
    F fn_;
    void execute() override { fn_(); }
};

class ThreadPool {
public:
    explicit ThreadPool(std::size_t num_threads = default_thread_count())
        : stop_flag_(false), next_worker_(0)
    {
        num_threads = std::max(std::size_t(1), num_threads);

        // Create every per-worker queue before starting any thread. Each
        // worker navigates queues_[idx] on entry, and std::deque keeps its
        // map/finish pointers inside this object; growing it while a freshly
        // spawned worker reads those pointers is a data race (the pool object
        // often lives on the caller's stack, so TSan reports it there).
        for (std::size_t i = 0; i < num_threads; ++i)
            queues_.emplace_back();

        workers_.reserve(num_threads);
        for (std::size_t i = 0; i < num_threads; ++i)
            workers_.emplace_back([this, i] { worker_loop(i); });
    }

    ~ThreadPool() {
        shutdown();
    }

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;
    ThreadPool(ThreadPool&&) = delete;
    ThreadPool& operator=(ThreadPool&&) = delete;

    template<typename F>
    void fire_and_forget(F&& f) {
        enqueue_task(std::make_unique<FnTask<std::decay_t<F>>>(std::forward<F>(f)));
    }

    void enqueue_task(std::unique_ptr<Task> task, bool high_priority = false) {
        if (stop_flag_.load(std::memory_order_acquire))
            std::abort();

        std::size_t idx = next_worker_.fetch_add(1, std::memory_order_relaxed) % queues_.size();
        auto& q = queues_[idx];
        {
            std::unique_lock<std::mutex> lock(q.mtx);
            if (high_priority)
                q.high_pri.emplace(std::move(task));
            else
                q.normal.emplace(std::move(task));
        }
        total_queue_size_.fetch_add(1, std::memory_order_relaxed);
        if (high_priority) {
            high_priority_queue_size_.fetch_add(1, std::memory_order_relaxed);
        }
        q.cv.notify_one();
    }

    [[nodiscard]] std::size_t get_queue_size() const {
        return total_queue_size_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t get_high_priority_queue_size() const {
        return high_priority_queue_size_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t get_steal_count() const noexcept {
        return steal_count_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t get_worker_count() const noexcept {
        return workers_.size();
    }

    void shutdown() {
        if (stop_flag_.exchange(true))
            return;
        for (auto& q : queues_)
            q.cv.notify_all();
        for (auto& w : workers_) {
            if (w.joinable())
                w.join();
        }
    }

private:
    struct PerWorker {
        std::queue<std::unique_ptr<Task>> high_pri;
        std::queue<std::unique_ptr<Task>> normal;
        mutable std::mutex mtx;
        std::condition_variable cv;
    };

    // Pops one task from a queue (high-priority first). Caller MUST hold the
    // queue's mutex. Task costs here are highly non-uniform (an empty-sky mesh
    // build is ~7x cheaper than a dense-terrain one), so a round-robin enqueue
    // can leave one worker with a long tail of expensive builds while others
    // sit idle. Work stealing rebalances: an idle worker grabs a task from a
    // busy worker's queue instead of blocking.
    std::unique_ptr<Task> pop_one(PerWorker& q) {
        std::unique_ptr<Task> task;
        if (!q.high_pri.empty()) {
            task = std::move(q.high_pri.front());
            q.high_pri.pop();
            high_priority_queue_size_.fetch_sub(1, std::memory_order_relaxed);
        } else if (!q.normal.empty()) {
            task = std::move(q.normal.front());
            q.normal.pop();
        }
        if (task) {
            total_queue_size_.fetch_sub(1, std::memory_order_relaxed);
        }
        return task;
    }

    // Scan the other workers' queues (round-robin, try-lock so a busy queue
    // never blocks a thief) and steal one task. High-priority tasks are stolen
    // first so a latency-critical build is not stranded behind slow work on
    // its dispatch worker.
    std::unique_ptr<Task> steal_task(std::size_t self) {
        const std::size_t n = queues_.size();
        for (std::size_t i = 1; i < n; ++i) {
            std::size_t target = (self + i) % n;
            PerWorker& q = queues_[target];
            std::unique_lock<std::mutex> lock(q.mtx, std::try_to_lock);
            if (!lock.owns_lock()) continue;
            std::unique_ptr<Task> task = pop_one(q);
            if (task) {
                steal_count_.fetch_add(1, std::memory_order_relaxed);
                return task;
            }
        }
        return nullptr;
    }

    void worker_loop(std::size_t idx) {
        auto& my = queues_[idx];
        while (true) {
            std::unique_ptr<Task> task;
            {
                std::unique_lock<std::mutex> lock(my.mtx);
                // Poll for steal opportunities aggressively while work exists
                // anywhere in the pool; when everything is idle, sleep long so
                // 15 idle workers do not burn ~15k wakeups/sec churning.
                const auto poll = (total_queue_size_.load(std::memory_order_relaxed) > 0)
                                      ? kStealPollInterval
                                      : kIdlePollInterval;
                my.cv.wait_for(lock, poll, [this, &my] {
                    return stop_flag_.load(std::memory_order_acquire) ||
                           !my.high_pri.empty() || !my.normal.empty();
                });
                if (stop_flag_.load(std::memory_order_acquire) &&
                    my.high_pri.empty() && my.normal.empty())
                    return;
                task = pop_one(my);
            }
            if (!task) {
                // Own queue was empty (spurious wakeup / steal poll): try to
                // steal from a busy worker before going back to sleep.
                task = steal_task(idx);
                if (!task) continue;
            }
            task->execute();
        }
    }

    static std::size_t default_thread_count() noexcept {
        std::size_t hc = std::thread::hardware_concurrency();
        return (hc > 1) ? (hc - 1) : 1;
    }

    // While any task is queued anywhere in the pool, idle workers poll this
    // often to try stealing. Short enough to reclaim a stranded task within a
    // few milliseconds of it landing on a busy worker.
    static constexpr std::chrono::microseconds kStealPollInterval{1000};
    // When the whole pool is idle (nothing queued), idle workers sleep this
    // long between steal attempts. Long enough that 15 idle workers do not
    // burn ~15k wakeups/sec, short enough that a fresh burst of work is
    // picked up within a frame (~16ms). Enqueues still wake the target worker
    // immediately via notify_one, so this only caps steal latency.
    static constexpr std::chrono::microseconds kIdlePollInterval{16000};

    std::vector<std::thread> workers_;
    std::deque<PerWorker> queues_;
    std::atomic<bool> stop_flag_;
    std::atomic<std::size_t> next_worker_;
    std::atomic<std::size_t> total_queue_size_{0};
    std::atomic<std::size_t> high_priority_queue_size_{0};
    std::atomic<std::size_t> steal_count_{0};
};

} // namespace VoxelEngine

#endif
