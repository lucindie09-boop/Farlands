#include "schematic/paste_plan.hpp"

#include <unordered_map>
#include <unordered_set>

namespace VoxelEngine {
namespace schematic {

namespace {

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

} // namespace

bool plan_paste(const SchematicData& file, const McPalette& palette,
                int32_t origin_x, int32_t origin_y, int32_t origin_z,
                const PasteOptions& options, const BlockResolver& resolve,
                PastePlan& out, std::string* error) {
    out = PastePlan{};
    if (!resolve) return fail(error, "plan_paste: no block resolver");

    // Each distinct target name the table can ask for is resolved ONCE, not once
    // per cell: the engine's resolver is a scan over the block table, and a build
    // repeats a handful of names tens of thousands of times. Names that do not
    // resolve are kept so the cells naming them are still counted per cell below.
    //
    // The list is total by construction (see target_names): a pattern only ever
    // brings a species in, and a shape expands to its family's members, so no
    // cell can name a target the table did not enumerate.
    std::unordered_map<std::string, BlockID> targets;
    std::unordered_set<std::string> unresolved_names;
    for (const std::string& name : target_names(palette)) {
        BlockID id = BlockIDs::AIR;
        if (resolve(name, id)) {
            targets.emplace(name, id);
        } else {
            unresolved_names.insert(name);
        }
    }

    // The table is asked once per distinct STATE, never once per cell. A build
    // repeats the same handful of states across hundreds of thousands of cells,
    // and resolving one allocates several strings, so this is the difference
    // between a pass that is a scan and one that re-translates every block.
    std::vector<PaletteTarget> state_targets(file.palette.size());
    std::vector<uint8_t> state_resolved(file.palette.size(), 0);

    // Cells are walked in the file's own order but straight off the flat array,
    // which is stored in exactly that order: state_at() would recompute and
    // re-check the index for every one of a million cells to no benefit.
    size_t cell_index = 0;
    for (int32_t y = 0; y < file.height; ++y) {
        for (int32_t z = 0; z < file.length; ++z) {
            for (int32_t x = 0; x < file.width; ++x, ++cell_index) {
                if (cell_index >= file.cells.size()) continue;
                const uint32_t slot = file.cells[cell_index];
                if (slot >= file.palette.size()) continue;
                ++out.stats.file_cells;

                const BlockState& state = file.palette[slot];
                // Air in the file is air here: it clears the cell rather than
                // naming a block, so it does not go through the table (and a
                // table with no air row must not turn a carve-out into unknown).
                const bool file_air = state.is_air();
                if (file_air && !options.write_air) {
                    ++out.stats.air_ignored;
                    continue;
                }

                PaletteTarget air_target;
                const PaletteTarget* target = nullptr;
                if (file_air) {
                    air_target.kind = PaletteTarget::Kind::Mapped;
                    target = &air_target;
                } else {
                    // One call for both languages: the state itself says whether
                    // it is an id or a name, and the palette knows both.
                    if (!state_resolved[slot]) {
                        state_targets[slot] = palette.resolve(state);
                        state_resolved[slot] = 1;
                    }
                    target = &state_targets[slot];
                }
                if (target->unknown()) {
                    ++out.stats.unknown;
                    continue;
                }
                if (target->skipped()) {
                    ++out.stats.skipped;
                    continue;
                }
                // A liquid lands either way; `fluids` chooses which form. The
                // still form is the same substance with no fluid state, so it is
                // drawn and collided as the liquid it is without ever ticking —
                // which is what keeps a pasted lake from running downhill. A
                // liquid the table gives no still form is the only one a paste
                // without `fluids` leaves out.
                const bool still = target->fluid && !options.fluids;
                if (still && target->still_name.empty()) {
                    ++out.stats.declined_fluid;
                    continue;
                }
                // A stand-in that is also a liquid has to clear both gates: it is
                // still a stand-in, and it still lands as the liquid it is.
                if (target->substitute && !options.substitutes) {
                    ++out.stats.declined_substitute;
                    continue;
                }
                // A shape row names no block of its own, so a still form (also
                // none, for every row that has one) is simply not asked for.
                const std::string& want = still ? target->still_name : target->block_name;

                BlockID block = BlockIDs::AIR;
                if (!file_air) {
                    if (want.empty()) {
                        ++out.stats.unresolved;
                        continue;
                    }
                    const auto found = targets.find(want);
                    if (found == targets.end()) {
                        ++out.stats.unresolved;
                        continue;
                    }
                    block = found->second;
                }

                PastePlan::Cell cell;
                cell.x = origin_x + x;
                cell.y = origin_y + y;
                cell.z = origin_z + z;
                cell.block = block;
                out.cells.push_back(cell);

                if (still) {
                    ++out.stats.stilled;
                } else if (target->substitute) {
                    ++out.stats.substituted;
                } else {
                    ++out.stats.placed;
                }

                if (out.cells.size() == 1) {
                    out.min_x = out.max_x = cell.x;
                    out.min_y = out.max_y = cell.y;
                    out.min_z = out.max_z = cell.z;
                } else {
                    if (cell.x < out.min_x) out.min_x = cell.x;
                    if (cell.x > out.max_x) out.max_x = cell.x;
                    if (cell.y < out.min_y) out.min_y = cell.y;
                    if (cell.y > out.max_y) out.max_y = cell.y;
                    if (cell.z < out.min_z) out.min_z = cell.z;
                    if (cell.z > out.max_z) out.max_z = cell.z;
                }
            }
        }
    }

    if (options.max_cells > 0 && out.cells.size() > options.max_cells) {
        const size_t needed = out.cells.size();
        out = PastePlan{};
        return fail(error, "the paste would change " + std::to_string(needed) +
                               " cells, above the limit of " +
                               std::to_string(options.max_cells));
    }
    return true;
}

PastePlan to_revert_plan(const PasteUndo& undo) {
    PastePlan plan;
    plan.cells = undo.cells;
    plan.stats.file_cells = undo.cells.size();
    plan.stats.placed = undo.cells.size();
    for (size_t i = 0; i < undo.cells.size(); ++i) {
        const PastePlan::Cell& cell = undo.cells[i];
        if (i == 0) {
            plan.min_x = plan.max_x = cell.x;
            plan.min_y = plan.max_y = cell.y;
            plan.min_z = plan.max_z = cell.z;
        } else {
            if (cell.x < plan.min_x) plan.min_x = cell.x;
            if (cell.x > plan.max_x) plan.max_x = cell.x;
            if (cell.y < plan.min_y) plan.min_y = cell.y;
            if (cell.y > plan.max_y) plan.max_y = cell.y;
            if (cell.z < plan.min_z) plan.min_z = cell.z;
            if (cell.z > plan.max_z) plan.max_z = cell.z;
        }
    }
    return plan;
}

} // namespace schematic
} // namespace VoxelEngine
