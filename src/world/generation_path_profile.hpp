#ifndef FARLANDS_GENERATION_PATH_PROFILE_HPP
#define FARLANDS_GENERATION_PATH_PROFILE_HPP

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {

// One steady-clock timebase shared by the main thread and the workers.
inline uint64_t generation_path_now_ns() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

// One chunk's trip through the generation pipeline:
//
//   admitted  (main thread, just before enqueue_generation)
//   started   (worker thread, first line of the generation lambda)
//   finished  (worker thread, just before the chunk is handed to the scheduler)
//   installed (main thread, after chunk_map.insert in process_completed_chunks)
//
// wait  = started - admitted   (worker-queue wait, including the enqueue's locks)
// work  = finished - started   (terrain + edit map + light on the worker)
// drain = installed - finished (completed queue + light/dirty stages + install)
// total = installed - admitted (wait + work + drain)
//
// Off unless FARLANDS_GEN_PROFILE=1: the map and its mutex otherwise cost more
// than the diagnostic is worth. Every method checks the flag before touching the
// clock or the lock, and the report is read once at the end of a probe.
class GenerationPathProfile {
public:
    struct Sample {
        uint64_t admit_ns = 0;
        uint64_t start_ns = 0;
        uint64_t end_ns = 0;
        uint64_t install_ns = 0;
    };

    struct Stats {
        double avg_ms = 0.0;
        double p50_ms = 0.0;
        double p90_ms = 0.0;
        double p99_ms = 0.0;
        double max_ms = 0.0;
    };

    [[nodiscard]] static bool enabled() {
#ifdef DEBUG_ENABLED
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        static const bool on = std::getenv("FARLANDS_GEN_PROFILE") != nullptr;
        return on;
#else
        return false;
#endif
    }

    // A unique id per enqueue attempt. The chunk key is not enough: a chunk can
    // be generated again after an unload, and the old attempt may still be
    // waiting to install when the new one is admitted, so keying the samples by
    // chunk would mix the two attempts' timestamps.
    [[nodiscard]] static uint64_t new_attempt() {
        if (!enabled()) return 0;
        static std::atomic<uint64_t> next{1};
        return next.fetch_add(1, std::memory_order_relaxed);
    }

    // `ns` is taken BEFORE the enqueue, so the wait includes the enqueue's own
    // lock acquisition. A worker may already have started (and even finished)
    // by the time this lands; started() creates the sample in that case, and
    // this only fills in the admission time.
    void admitted(uint64_t id, uint64_t ns) {
        if (!enabled() || id == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id].admit_ns = ns;
        ++admitted_;
    }

    void started(uint64_t id) {
        if (!enabled() || id == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        pending_[id].start_ns = generation_path_now_ns();
        ++started_;
    }

    void finished(uint64_t id) {
        if (!enabled() || id == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(id);
        if (it == pending_.end()) return;
        it->second.end_ns = generation_path_now_ns();
        ++finished_;
    }

    void installed(uint64_t id) {
        if (!enabled() || id == 0) return;
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = pending_.find(id);
        if (it == pending_.end()) return;
        Sample sample = it->second;
        if (sample.admit_ns == 0 || sample.start_ns == 0 || sample.end_ns == 0) {
            pending_.erase(it);
            return;
        }
        sample.install_ns = generation_path_now_ns();
        samples_.push_back(sample);
        pending_.erase(it);
        ++installed_;
    }

    [[nodiscard]] std::string report() const {
        if (!enabled()) return "generation path disabled (set FARLANDS_GEN_PROFILE=1)";
        std::vector<Sample> samples;
        uint64_t admitted = 0;
        uint64_t started = 0;
        uint64_t finished = 0;
        uint64_t installed = 0;
        size_t pending = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            samples = samples_;
            admitted = admitted_;
            started = started_;
            finished = finished_;
            installed = installed_;
            pending = pending_.size();
        }
        std::vector<uint64_t> wait;
        std::vector<uint64_t> work;
        std::vector<uint64_t> drain;
        std::vector<uint64_t> total;
        wait.reserve(samples.size());
        work.reserve(samples.size());
        drain.reserve(samples.size());
        total.reserve(samples.size());
        for (const Sample& s : samples) {
            wait.push_back(s.start_ns - s.admit_ns);
            work.push_back(s.end_ns - s.start_ns);
            drain.push_back(s.install_ns - s.end_ns);
            total.push_back(s.install_ns - s.admit_ns);
        }
        const Stats w = summarize(std::move(wait));
        const Stats k = summarize(std::move(work));
        const Stats d = summarize(std::move(drain));
        const Stats t = summarize(std::move(total));
        char buf[1024];
        std::snprintf(
            buf, sizeof(buf),
            "gen path samples=%llu pending=%llu admitted=%llu started=%llu finished=%llu installed=%llu | "
            "wait avg=%.3fms p50=%.3f p90=%.3f p99=%.3f max=%.3f | "
            "work avg=%.3fms p50=%.3f p90=%.3f p99=%.3f max=%.3f | "
            "drain avg=%.3fms p50=%.3f p90=%.3f p99=%.3f max=%.3f | "
            "total avg=%.3fms p50=%.3f p90=%.3f p99=%.3f max=%.3f",
            static_cast<unsigned long long>(samples.size()),
            static_cast<unsigned long long>(pending),
            static_cast<unsigned long long>(admitted),
            static_cast<unsigned long long>(started),
            static_cast<unsigned long long>(finished),
            static_cast<unsigned long long>(installed),
            w.avg_ms, w.p50_ms, w.p90_ms, w.p99_ms, w.max_ms,
            k.avg_ms, k.p50_ms, k.p90_ms, k.p99_ms, k.max_ms,
            d.avg_ms, d.p50_ms, d.p90_ms, d.p99_ms, d.max_ms,
            t.avg_ms, t.p50_ms, t.p90_ms, t.p99_ms, t.max_ms);
        return std::string(buf);
    }

    // The last `count` installs, in install order. The full report's averages
    // are dominated by the fill's middle; this is the only view of the tail that
    // actually settles the map.
    [[nodiscard]] std::string report_tail(size_t count) const {
        if (!enabled()) return "disabled";
        std::vector<Sample> samples;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const size_t first = samples_.size() > count ? samples_.size() - count : 0;
            samples.assign(samples_.begin() + static_cast<std::ptrdiff_t>(first), samples_.end());
        }
        if (samples.empty()) return "tail samples=0";
        std::vector<uint64_t> wait;
        std::vector<uint64_t> work;
        std::vector<uint64_t> drain;
        std::vector<uint64_t> total;
        wait.reserve(samples.size());
        work.reserve(samples.size());
        drain.reserve(samples.size());
        total.reserve(samples.size());
        for (const Sample& s : samples) {
            wait.push_back(s.start_ns - s.admit_ns);
            work.push_back(s.end_ns - s.start_ns);
            drain.push_back(s.install_ns - s.end_ns);
            total.push_back(s.install_ns - s.admit_ns);
        }
        const Stats w = summarize(std::move(wait));
        const Stats k = summarize(std::move(work));
        const Stats d = summarize(std::move(drain));
        const Stats t = summarize(std::move(total));
        char buf[640];
        std::snprintf(
            buf, sizeof(buf),
            "tail n=%llu wait p50=%.3f p90=%.3f max=%.3f | work p50=%.3f p90=%.3f max=%.3f | "
            "drain p50=%.3f p90=%.3f max=%.3f | total p50=%.3f p90=%.3f max=%.3f",
            static_cast<unsigned long long>(samples.size()),
            w.p50_ms, w.p90_ms, w.max_ms,
            k.p50_ms, k.p90_ms, k.max_ms,
            d.p50_ms, d.p90_ms, d.max_ms,
            t.p50_ms, t.p90_ms, t.max_ms);
        return std::string(buf);
    }

private:
    static Stats summarize(std::vector<uint64_t> values_ns) {
        Stats stats;
        if (values_ns.empty()) return stats;
        std::sort(values_ns.begin(), values_ns.end());
        double sum = 0.0;
        for (const uint64_t ns : values_ns) sum += static_cast<double>(ns);
        stats.avg_ms = sum / static_cast<double>(values_ns.size()) / 1.0e6;
        const auto at = [&values_ns](double p) {
            const size_t idx = static_cast<size_t>(p * static_cast<double>(values_ns.size() - 1));
            return static_cast<double>(values_ns[idx]) / 1.0e6;
        };
        stats.p50_ms = at(0.50);
        stats.p90_ms = at(0.90);
        stats.p99_ms = at(0.99);
        stats.max_ms = static_cast<double>(values_ns.back()) / 1.0e6;
        return stats;
    }

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, Sample> pending_;
    std::vector<Sample> samples_;
    uint64_t admitted_ = 0;
    uint64_t started_ = 0;
    uint64_t finished_ = 0;
    uint64_t installed_ = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_GENERATION_PATH_PROFILE_HPP
