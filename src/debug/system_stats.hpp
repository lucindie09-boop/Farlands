#ifndef FARLANDS_DEBUG_SYSTEM_STATS_HPP
#define FARLANDS_DEBUG_SYSTEM_STATS_HPP

#include <algorithm>
#include <cstdint>
#include <string>

// This process's own CPU and GPU utilization, which Godot does not expose:
// Performance has no such monitor, and its TIME_PROCESS is a rolling average of
// the frame rather than a busy share. The numbers come from the OS instead --
// the process's own CPU times against the wall clock, and, on Windows, the GPU
// engine counters for its own instances. A whole-machine reading would move
// with every other application on the box, which is not what the row asks.
//
// The core here is godot-free so `tests/test_system_stats.cpp` can link it
// without the library: the functions below are the whole of the arithmetic, and
// the platform half lives in system_stats_windows.cpp / _posix.cpp.
namespace farlands {

// One reading of the process's own time: `total_ticks` is the wall time it took
// scaled by the core count, so a percentage is a share of the WHOLE machine, and
// `idle_ticks` is that total minus the process's busy time. A percentage is the
// change between two readings, so this is a sample rather than a number.
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

// Build a sample from the process's own CPU time (user + kernel) over the wall
// time it took to accrue it, normalised by the core count so percent_since()
// answers "share of the machine": four busy threads on eight cores is 50%, and
// eight on eight is 100% rather than 800%. A busy time past the scaled total
// floors idle, so the share saturates at 100 instead of overshooting, and zero
// cores counts as one rather than dividing by a zero width.
[[nodiscard]] inline CpuSample process_cpu_sample(std::uint64_t busy_ticks,
                                                  std::uint64_t wall_ticks,
                                                  unsigned cores) {
    const std::uint64_t width = cores == 0 ? 1u : static_cast<std::uint64_t>(cores);
    CpuSample sample;
    sample.total_ticks = wall_ticks * width;
    sample.idle_ticks = sample.total_ticks > busy_ticks ? sample.total_ticks - busy_ticks : 0;
    return sample;
}

// GPU engine counters are per engine (3d, copy, video) and per process. Their
// sum over THIS process's engines is its share of the GPU -- the per-app column
// Task Manager shows -- and the cap is a full machine, because engines overlap
// and can sum past 100. NaN and negatives are no reading, which is 0 here
// because the caller only reaches this with a value it read.
[[nodiscard]] inline float clamp_gpu_percent(double summed_percent) {
    if (!(summed_percent > 0.0)) {
        return 0.0f;
    }
    return static_cast<float>(std::min(summed_percent, 100.0));
}

// What the fitted modules are, as the firmware describes them (SMBIOS Memory
// Device records): the generation and rated speed of the RAM, which no memory
// total carries. Filled by platform_memory_spec() through
// parse_smbios_memory() (debug/smbios_memory.hpp).
struct RamSummary {
    std::string type;   // "DDR4", "LPDDR5", ...; empty when the firmware says nothing
    int speed_mts = 0;  // rated transfer rate, MT/s; 0 when unknown
    int modules = 0;    // populated slots
    int module_mb = 0;  // size of one module when they are all equal, else 0
    int total_mb = 0;   // the modules' sum
};

// "DDR4-3200 (2x16 GB)", or as much of it as is known: the generation and the
// speed join with a dash, the module count is added only when the modules are
// identical and there is more than one (the total already says the size of a
// single module), and an empty string means nothing was readable.
[[nodiscard]] inline std::string format_memory_spec(const RamSummary& ram) {
    std::string text;
    if (!ram.type.empty()) {
        text += ram.type;
    }
    if (ram.speed_mts > 0) {
        if (!text.empty()) {
            text += '-';
        }
        text += std::to_string(ram.speed_mts);
    }
    if (ram.modules > 1 && ram.module_mb > 0 &&
        ram.total_mb == ram.module_mb * ram.modules) {
        if (!text.empty()) {
            text += ' ';
        }
        text += "(" + std::to_string(ram.modules) + "x" +
                std::to_string(ram.module_mb / 1024) + " GB)";
    }
    return text;
}

// --- Platform half: exactly one of system_stats_windows.cpp / _posix.cpp ---
// Fill `out` with this process's current own-time reading (CPU time against the
// wall clock, through process_cpu_sample()). False when this platform has no
// counter to read.
[[nodiscard]] bool platform_cpu_sample(CpuSample& out);
// Read this process's GPU utilization, 0..100. False when the platform has no
// per-process counter (or this read failed); the counters need two collections
// before they answer.
[[nodiscard]] bool platform_gpu_percent(double& out);
// The card's own video memory in bytes, which Godot has no monitor for: the
// engine reports what it has ALLOCATED (RENDER_VIDEO_MEM_USED), never the
// capacity to read it against. False where the platform cannot say, so the HUD
// prints the used figure alone rather than a size it guessed.
[[nodiscard]] bool platform_vram_total_bytes(std::uint64_t& out);
// Fill `out` from the firmware's SMBIOS table. False where the table cannot be
// read (on Linux it is usually root-only) or holds no populated module.
[[nodiscard]] bool platform_memory_spec(RamSummary& out);

}  // namespace farlands

#endif  // FARLANDS_DEBUG_SYSTEM_STATS_HPP
