// The printing the report does after the CLI has read the file, the table and the
// palette: the states a paste cannot place exactly, and the timed paste plan.
//
// Both phases were the tail of `main`; nothing about them needed the CLI's
// argument parsing or its file reading, only the decoded build, the palette and
// the block-name list it hands over here.

#include "schematic_report_format.hpp"

#include "schematic/paste_plan.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

using VoxelEngine::BlockID;
using namespace VoxelEngine::schematic;

void print_gap_rows(std::vector<FailedPastRow>& not_placed) {
    if (not_placed.empty()) return;

    std::sort(not_placed.begin(), not_placed.end(),
              [](const FailedPastRow& a, const FailedPastRow& b) {
                  if (a.count != b.count) return a.count > b.count;
                  return a.state < b.state;
              });
    // Everything that is not an exact match: the pasted building will either lose
    // these cells or get a different block in them.
    std::printf("\ngaps        %-46s %-8s why\n", "state", "count");
    const size_t notes_shown = not_placed.size() < 40 ? not_placed.size() : 40;
    for (size_t i = 0; i < notes_shown; ++i) {
        const FailedPastRow& row = not_placed[i];
        std::printf("            %-46s %-8zu %s\n", row.state.c_str(), row.count,
                    row.detail.empty() ? "no note in the table" : row.detail.c_str());
    }
    if (notes_shown < not_placed.size()) {
        std::printf("            ... %zu more\n", not_placed.size() - notes_shown);
    }
}

bool print_plan_report(const SchematicData& data, const McPalette& palette,
                       const std::vector<std::string>& block_names, int plan_reps,
                       std::string& error) {
    // The plan for real, timed: every cell into one bucket, with the table's
    // resolution done per distinct state. This is the pass that runs before a
    // paste touches the world, so its cost is the paste's floor.
    std::unordered_map<std::string, BlockID> ids;
    ids.reserve(block_names.size());
    for (size_t i = 0; i < block_names.size(); ++i) {
        ids.emplace(block_names[i], static_cast<BlockID>(i));
    }
    const auto resolve = [&ids](const std::string& name, BlockID& out) {
        const auto found = ids.find(name);
        if (found == ids.end()) return false;
        out = found->second;
        return true;
    };

    PasteOptions options;
    PastePlan plan;
    double best = 0.0;
    double total = 0.0;
    for (int rep = 0; rep < plan_reps; ++rep) {
        const auto start = std::chrono::steady_clock::now();
        if (!plan_paste(data, palette, 0, 0, 0, options, resolve, plan, &error)) {
            std::printf("\nplan        refused: %s\n", error.c_str());
            return false;
        }
        const double ms = elapsed_ms(start);
        total += ms;
        if (rep == 0 || ms < best) best = ms;
    }
    const double per_cell_ns =
        plan.stats.file_cells > 0
            ? 1000000.0 * best / static_cast<double>(plan.stats.file_cells)
            : 0.0;
    std::printf("\nplan        %zu cells to change out of %zu file cells in %.1f ms"
                " (%.0f ns/cell, %s)\n",
                plan.size(), plan.stats.file_cells, best, per_cell_ns,
                plan_reps == 1 ? "1 run"
                               : (std::to_string(plan_reps) + " runs, best of, avg " +
                                  std::to_string(total / plan_reps).substr(0, 5) + " ms")
                                     .c_str());
    std::printf("            placed %zu, substituted %zu, stilled %zu, skipped %zu,"
                " unknown %zu, declined %zu, air ignored %zu\n",
                plan.stats.placed, plan.stats.substituted, plan.stats.stilled,
                plan.stats.skipped, plan.stats.unknown,
                plan.stats.declined_fluid + plan.stats.declined_substitute,
                plan.stats.air_ignored);
    return true;
}
