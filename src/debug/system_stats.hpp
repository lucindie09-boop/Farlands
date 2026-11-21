#ifndef FARLANDS_DEBUG_SYSTEM_STATS_HPP
#define FARLANDS_DEBUG_SYSTEM_STATS_HPP

#include <algorithm>
#include <cstdint>

// Machine-wide CPU and GPU utilization, which Godot does not expose: Performance
// has no such monitor, and its TIME_PROCESS is a rolling average of the frame
// rather than a busy share. The numbers come from the OS instead -- the CPU tick
// counters and, on Windows, the "GPU Engine" performance counters, which are
// what Task Manager reads.
//
// The core here is godot-free so `tests/test_system_stats.cpp` can link it
// without the library: the two functions below are the whole of the arithmetic,
// and the platform half lives in system_stats_windows.cpp / _posix.cpp.
namespace farlands {

// One reading of the machine's CPU counters: the ticks the OS has accumulated
// since boot (or since a counter reset). A percentage is the change between two
// readings, so this is a sample rather than a number.
struct CpuSample {
    std::uint64_t idle_ticks = 0;
    std::uint64_t total_ticks = 0;

    // The busy share of the interval between `previous` and this sample, 0..100.
    // 0 when no time passed, when the counters went backwards, or when the idle
    // counter moved further than the total one -- a reset counter reads "nothing
    // happened" rather than dividing by zero or wrapping the ratio.
    [[nodiscard]] float percent_since(const CpuSample& previous) const {
        if (total_ticks <= previous.total_ticks || idle_ticks < previous.idle_ticks) {
            return 0.0f;
        }
        const std::uint64_t total = total_ticks - previous.total_ticks;
        const std::uint64_t idle = std::min(idle_ticks - previous.idle_ticks, total);
        const double busy = static_cast<double>(total - idle);
        return static_cast<float>(100.0 * busy / static_cast<double>(total));
    }
};

// GPU engine counters are per engine (3d, copy, video) and per process, and
// their sum is what "the GPU is 40% busy" means -- Task Manager sums the same
// way. Capped at a full machine; NaN and negatives are no reading, which is 0
// here because the caller only reaches this with a value it read.
[[nodiscard]] inline float clamp_gpu_percent(double summed_percent) {
    if (!(summed_percent > 0.0)) {
        return 0.0f;
    }
    return static_cast<float>(std::min(summed_percent, 100.0));
}

// --- Platform half: exactly one of system_stats_windows.cpp / _posix.cpp ---
// Fill `out` with the machine's current CPU counters. False when this platform
// has no counter to read.
[[nodiscard]] bool platform_cpu_sample(CpuSample& out);
// Read GPU utilization, 0..100. False when the machine exposes no such counter
// (or this read failed); the counters need two collections before they answer.
[[nodiscard]] bool platform_gpu_percent(double& out);

}  // namespace farlands

#endif  // FARLANDS_DEBUG_SYSTEM_STATS_HPP
