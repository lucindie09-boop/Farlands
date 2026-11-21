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
    godot::ClassDB::bind_method(godot::D_METHOD("get_vram_total"), &SystemStats::get_vram_total);
    godot::ClassDB::bind_method(godot::D_METHOD("get_memory_spec"), &SystemStats::get_memory_spec);
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

double SystemStats::get_vram_total() {
    // Once, including a failure: the capacity is a property of the card, and a
    // machine without a way to read it does not grow one during a session.
    if (!vram_sampled_) {
        vram_sampled_ = true;
        std::uint64_t bytes = 0;
        if (farlands::platform_vram_total_bytes(bytes)) {
            vram_total_mb_ = static_cast<double>(bytes) / (1024.0 * 1024.0);
        }
    }
    return vram_total_mb_ > 0.0 ? vram_total_mb_ : -1.0;
}

godot::String SystemStats::get_memory_spec() {
    // Once, including a failure: fitted modules do not change during a session.
    if (!memory_sampled_) {
        memory_sampled_ = true;
        farlands::RamSummary ram;
        if (farlands::platform_memory_spec(ram)) {
            memory_spec_ = farlands::format_memory_spec(ram);
        }
    }
    return godot::String(memory_spec_.c_str());
}
