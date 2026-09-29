#include "doctest.h"

#include "core/thread_pool.hpp"

#include <atomic>
#include <chrono>
#include <thread>

using namespace VoxelEngine;

// =========================================================================
// Work stealing: a task queued on a worker that is busy executing
// something else must be reclaimed by an idle worker, not left stranded.
// Worker 0 is blocked by a gated task, then a 4-task round-robin batch lands
// its last task on worker 0's queue. Only the idle workers can run it, so
// the pool's steal counter must be nonzero. Fully deterministic (no timers).
// =========================================================================
TEST_CASE("thread pool work stealing reclaims tasks from a busy worker") {
    constexpr std::size_t kWorkers = 4;
    ThreadPool pool(kWorkers);
    CHECK(pool.get_worker_count() == kWorkers);

    auto blocker_running = std::make_shared<std::atomic<bool>>(false);
    auto release_blocker = std::make_shared<std::atomic<bool>>(false);
    auto tasks_done = std::make_shared<std::atomic<int>>(0);

    // First task lands on worker 0 (round-robin starts at 0) and blocks it.
    pool.fire_and_forget([blocker_running, release_blocker]() {
        blocker_running->store(true, std::memory_order_release);
        while (!release_blocker->load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    });

    while (!blocker_running->load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Four tasks, round-robin: workers 1, 2, 3, then 0. The last one lands on
    // the blocked worker 0, so it can only run via stealing.
    for (int i = 0; i < 4; ++i) {
        pool.fire_and_forget([tasks_done]() { tasks_done->fetch_add(1, std::memory_order_relaxed); });
    }

    // Worker 0 stays blocked, so all four tasks must complete without it.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (tasks_done->load(std::memory_order_acquire) < 4 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    release_blocker->store(true, std::memory_order_release);

    CHECK(tasks_done->load(std::memory_order_acquire) == 4);
    CHECK(pool.get_steal_count() > 0);
}
