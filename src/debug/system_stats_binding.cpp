#include "debug/system_stats_binding.hpp"

#include <chrono>

namespace {

// The OS answers a percentage over the interval between two reads. At 500 ms the
// CPU counters have enough ticks behind them to be stable and the GPU counters
// have had two collections; the HUD asks five times a second and the rest are
// cached reads.
constexpr std::uint64_t kSampleIntervalUsec = 500'000;

std::uint64_t now_usec() {
    using steady = std::chrono::steady_clock;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(steady::now().time_since_epoch()).count());
}

}  // namespace

void SystemStats::_bind_methods() {
    godot::ClassDB::bind_method(godot::D_METHOD("get_cpu_usage"), &SystemStats::get_cpu_usage);
    godot::ClassDB::bind_method(godot::D_METHOD("get_gpu_usage"), &SystemStats::get_gpu_usage);
}

void SystemStats::sample_if_stale() {
    const std::uint64_t now = now_usec();
    if (have_cpu_sample_ && now - last_sample_usec_ < kSampleIntervalUsec) {
        return;
    }
    last_sample_usec_ = now;

    farlands::CpuSample current;
    if (farlands::platform_cpu_sample(current)) {
        if (have_cpu_sample_) {
            // The first sample has nothing to compare against, so it only seeds
            // the baseline; cpu_percent_ stays -1 until the second.
            cpu_percent_ = static_cast<double>(current.percent_since(previous_cpu_));
        }
        previous_cpu_ = current;
        have_cpu_sample_ = true;
    }

    double gpu = -1.0;
    if (farlands::platform_gpu_percent(gpu)) {
        gpu_percent_ = gpu;
    }
}

double SystemStats::get_cpu_usage() {
    sample_if_stale();
    return cpu_percent_;
}

double SystemStats::get_gpu_usage() {
    sample_if_stale();
    return gpu_percent_;
}
