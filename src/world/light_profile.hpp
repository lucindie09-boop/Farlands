#ifndef FARLANDS_LIGHT_PROFILE_HPP
#define FARLANDS_LIGHT_PROFILE_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace VoxelEngine {

// Runtime-gated timing for the worker light band. A band is one exclusive
// acquisition -- a 3x3x3 neighborhood is 9 column shards, and a batch of
// arriving chunks inside a 2x2 footprint widens that to at most 16 -- and it
// can carry several passes now. So the wait and the hold are counted per
// acquisition, while the block-light phases, the sky walk and the sources they
// touched are counted per pass.
//
// Off unless FARLANDS_LIGHT_PROFILE=1: every band otherwise pays the atomics
// and clock reads in record_band/record_pass, which is not a streaming-hot-path
// cost worth taking for a diagnostic that is read once at the end of a probe.
class LightProfile {
public:
    struct PassSample {
        uint64_t block_ns = 0;
        uint64_t block_clear_ns = 0;
        uint64_t block_collect_ns = 0;
        uint64_t block_propagate_ns = 0;
        uint64_t sky_ns = 0;
        uint32_t sources = 0;
        uint32_t modified_bits = 0;
        bool had_block = false;
        bool had_sky = false;
    };

    [[nodiscard]] static bool enabled() {
#ifdef DEBUG_ENABLED
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        static const bool on = std::getenv("FARLANDS_LIGHT_PROFILE") != nullptr;
        return on;
#else
        return false;
#endif
    }

    // One exclusive acquisition: how long it waited and how long it was held.
    void record_band(uint64_t wait_ns, uint64_t hold_ns) {
        if (!enabled()) return;
        bands_.fetch_add(1, std::memory_order_relaxed);
        add(wait_total_, wait_max_, wait_ns);
        add(hold_total_, hold_max_, hold_ns);
    }

    // One pass under a band: what it spent and what it changed.
    void record_pass(const PassSample& s) {
        if (!enabled()) return;
        passes_.fetch_add(1, std::memory_order_relaxed);
        if (s.had_block) block_passes_.fetch_add(1, std::memory_order_relaxed);
        if (s.had_sky) sky_passes_.fetch_add(1, std::memory_order_relaxed);
        if (s.had_block && s.had_sky) both_passes_.fetch_add(1, std::memory_order_relaxed);
        add(block_total_, block_max_, s.block_ns);
        add(sky_total_, sky_max_, s.sky_ns);
        block_clear_total_.fetch_add(s.block_clear_ns, std::memory_order_relaxed);
        block_collect_total_.fetch_add(s.block_collect_ns, std::memory_order_relaxed);
        block_propagate_total_.fetch_add(s.block_propagate_ns, std::memory_order_relaxed);
        sources_total_.fetch_add(s.sources, std::memory_order_relaxed);
        bump_max(sources_max_, s.sources);
        modified_total_.fetch_add(popcount(s.modified_bits), std::memory_order_relaxed);
    }

    [[nodiscard]] std::string report() const {
        if (!enabled()) {
            return "light profile disabled (set FARLANDS_LIGHT_PROFILE=1)";
        }
        const uint64_t bands = bands_.load(std::memory_order_relaxed);
        const uint64_t passes = passes_.load(std::memory_order_relaxed);
        const double wait_avg = bands
                                    ? ms(wait_total_.load(std::memory_order_relaxed) / bands)
                                    : 0.0;
        const double hold_avg = bands
                                    ? ms(hold_total_.load(std::memory_order_relaxed) / bands)
                                    : 0.0;
        char buf[768];
        std::snprintf(
            buf, sizeof(buf),
            "bands=%llu passes=%llu block=%llu sky=%llu both=%llu | "
            "wait total=%.1fms max=%.2fms avg=%.2fms | "
            "hold total=%.1fms max=%.2fms avg=%.2fms | block total=%.1fms max=%.2fms "
            "clear=%.1fms collect=%.1fms propagate=%.1fms | sky total=%.1fms max=%.2fms | "
            "sources=%llu max=%llu | changed slots=%llu",
            static_cast<unsigned long long>(bands),
            static_cast<unsigned long long>(passes),
            static_cast<unsigned long long>(block_passes_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sky_passes_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(both_passes_.load(std::memory_order_relaxed)),
            ms(wait_total_.load(std::memory_order_relaxed)),
            ms(wait_max_.load(std::memory_order_relaxed)), wait_avg,
            ms(hold_total_.load(std::memory_order_relaxed)),
            ms(hold_max_.load(std::memory_order_relaxed)), hold_avg,
            ms(block_total_.load(std::memory_order_relaxed)),
            ms(block_max_.load(std::memory_order_relaxed)),
            ms(block_clear_total_.load(std::memory_order_relaxed)),
            ms(block_collect_total_.load(std::memory_order_relaxed)),
            ms(block_propagate_total_.load(std::memory_order_relaxed)),
            ms(sky_total_.load(std::memory_order_relaxed)),
            ms(sky_max_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sources_total_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(sources_max_.load(std::memory_order_relaxed)),
            static_cast<unsigned long long>(modified_total_.load(std::memory_order_relaxed)));
        return std::string(buf);
    }

private:
    static void bump_max(std::atomic<uint64_t>& max, uint64_t value) {
        uint64_t old = max.load(std::memory_order_relaxed);
        while (value > old &&
               !max.compare_exchange_weak(old, value, std::memory_order_relaxed)) {
        }
    }

    static void add(std::atomic<uint64_t>& total, std::atomic<uint64_t>& max, uint64_t value) {
        total.fetch_add(value, std::memory_order_relaxed);
        bump_max(max, value);
    }

    static double ms(uint64_t ns) { return static_cast<double>(ns) / 1.0e6; }

    // A pass reports a 27-bit slot mask; the useful aggregate is how many chunks
    // it marked, not the sum of the masks.
    static uint32_t popcount(uint32_t v) {
        uint32_t n = 0;
        while (v) {
            v &= v - 1;
            ++n;
        }
        return n;
    }

    std::atomic<uint64_t> bands_{0};
    std::atomic<uint64_t> passes_{0};
    std::atomic<uint64_t> block_passes_{0};
    std::atomic<uint64_t> sky_passes_{0};
    std::atomic<uint64_t> both_passes_{0};
    std::atomic<uint64_t> wait_total_{0};
    std::atomic<uint64_t> wait_max_{0};
    std::atomic<uint64_t> hold_total_{0};
    std::atomic<uint64_t> hold_max_{0};
    std::atomic<uint64_t> block_total_{0};
    std::atomic<uint64_t> block_max_{0};
    std::atomic<uint64_t> block_clear_total_{0};
    std::atomic<uint64_t> block_collect_total_{0};
    std::atomic<uint64_t> block_propagate_total_{0};
    std::atomic<uint64_t> sky_total_{0};
    std::atomic<uint64_t> sky_max_{0};
    std::atomic<uint64_t> sources_total_{0};
    std::atomic<uint64_t> sources_max_{0};
    std::atomic<uint64_t> modified_total_{0};
};

} // namespace VoxelEngine

#endif // FARLANDS_LIGHT_PROFILE_HPP
