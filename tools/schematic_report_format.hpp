#ifndef FARLANDS_TOOLS_SCHEMATIC_REPORT_FORMAT_HPP
#define FARLANDS_TOOLS_SCHEMATIC_REPORT_FORMAT_HPP

// The two phases of the report that print from state the CLI owns: the table of
// states a paste cannot place exactly, and the timed paste plan. They live in
// their own translation unit so schematic_report.cpp stays the CLI and the file
// report, and this one stays the printing.

#include "schematic/mc_palette.hpp"
#include "schematic/schematic_reader.hpp"

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

// Wall time since `start`, in milliseconds. Monotonic, so a clock adjustment
// mid-run cannot produce a negative measurement.
inline double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

// One state a paste cannot place exactly, with the table's own reason for it.
struct FailedPastRow {
    std::string state;
    size_t count = 0;
    std::string detail;
};

// Prints the non-exact states, most common first, capped at 40 rows. Sorts the
// vector it is given (the caller does not read it again).
void print_gap_rows(std::vector<FailedPastRow>& not_placed);

// Builds and times the real paste plan, reporting the best of `plan_reps` runs.
// Returns false with `error` set when the plan refuses the build.
bool print_plan_report(const VoxelEngine::schematic::SchematicData& data,
                       const VoxelEngine::schematic::McPalette& palette,
                       const std::vector<std::string>& block_names, int plan_reps,
                       std::string& error);

#endif // FARLANDS_TOOLS_SCHEMATIC_REPORT_FORMAT_HPP
