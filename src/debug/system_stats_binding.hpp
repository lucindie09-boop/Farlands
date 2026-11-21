#ifndef FARLANDS_DEBUG_SYSTEM_STATS_BINDING_HPP
#define FARLANDS_DEBUG_SYSTEM_STATS_BINDING_HPP

#include <cstdint>
#include <string>

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include "debug/system_stats.hpp"

// The GDScript side of the two per-process numbers: an instance keeps the
// previous CPU sample and the platform's GPU query, and samples at most once
// per sample interval. The OS counters are averages since the previous read, so
// reading them faster than that returns noise rather than a number.
//
// One instance per reader, main thread only; the HUD creates one in _ready and
// asks it for both values when it repaints.
class SystemStats : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) - GDCLASS generates operator=
    GDCLASS(SystemStats, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Percent 0..100, or -1 while the platform cannot answer: no such counter,
    // or only the first sample has been taken (a CPU percentage needs two).
    double get_cpu_usage();
    double get_gpu_usage();
    // The card's total video memory in MB, or -1 where the platform cannot say.
    // Asked once: a card's capacity does not change while the game runs.
    double get_vram_total();
    // The RAM's generation, speed and module sizes -- "DDR4-3200 (2x 16 GB)" --
    // or "" where the firmware cannot be read. Asked once, like the VRAM total.
    godot::String get_memory_spec();

protected:
    static void _bind_methods();

private:
    void sample_if_stale();

    farlands::CpuSample previous_cpu_;
    bool have_cpu_sample_ = false;
    double cpu_percent_ = -1.0;
    double gpu_percent_ = -1.0;
    std::uint64_t last_sample_usec_ = 0;
    bool vram_sampled_ = false;
    double vram_total_mb_ = -1.0;
    bool memory_sampled_ = false;
    std::string memory_spec_;
};

#endif  // FARLANDS_DEBUG_SYSTEM_STATS_BINDING_HPP
