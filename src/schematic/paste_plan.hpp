#ifndef FARLANDS_PASTE_PLAN_HPP
#define FARLANDS_PASTE_PLAN_HPP

// -----------------------------------------------------------------------------
// What a block file would actually change, before anything touches the world.
//
// The reader decodes a file and the palette says what each state should become.
// This is the third step, and it is deliberately separate from the write: it
// decides, per cell, whether that cell is part of the paste at all — a decision
// that depends on POLICY (do liquids land? do stand-ins? does the file's air
// count?) and never on what happens to be in the world. So the plan can be built,
// counted and reported with no chunk map in sight, and the numbers a player is
// shown ("2,500 placed, 289 skipped") are the numbers the file itself implies.
//
// The one policy that cannot be answered here is `replace_solid`, because
// whether a cell is already occupied is a property of the world. It lives in
// PasteOptions so one struct flows through both halves, and the writer enforces
// it and reports what it declined.
//
// Counters are mutual and complete: every non-air cell in the file's box ends up
// in exactly one bucket (placed / substituted / stilled / declined_fluid /
// declined_substitute / skipped / unknown / unresolved), so a report that looks
// wrong is a bug in the policy rather than a mystery. A stilled cell counts as
// stilled and not as placed or substituted, even if its row is a stand-in: the
// counter is there to answer "what happened to the water".
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "schematic/mc_palette.hpp"
#include "schematic/schematic_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace VoxelEngine {
namespace schematic {

struct PasteOptions {
    // Liquids in the file land as LIVE sources (and their falling states), which
    // immediately run. Off by default, and then they still land — as the still
    // form the table names — because a pasted building with holes where its water
    // was is worse than one whose water does not move. Only a liquid with no still
    // form is left out entirely.
    bool fluids = false;
    // Stand-ins from the table land. Off means only exact counterparts do, which
    // is the setting for "I want to see exactly what is missing".
    bool substitutes = true;
    // The file's air is written too, carving away whatever is there. Off by
    // default: attaching a build to terrain should not delete the terrain.
    bool write_air = false;
    // Cells whose current block is not air are overwritten. Enforced by the
    // writer, since only it can see the world; false means "fill gaps only".
    bool replace_solid = true;
    // Refuse the whole paste if it would change more cells than this. 0 = no cap.
    // The plan is refused rather than truncated, because half a building is worse
    // than none and a caller that ignores the count gets a surprise otherwise.
    size_t max_cells = 0;
};

struct PasteStats {
    size_t file_cells = 0;        // every cell in the file's box
    size_t air_ignored = 0;       // air, with write_air off
    size_t placed = 0;            // exact counterparts
    size_t substituted = 0;       // stand-ins that landed
    size_t stilled = 0;           // liquids placed as their still form
    size_t declined_fluid = 0;    // liquids with no still form, with fluids off
    size_t declined_substitute = 0;  // stand-ins, with substitutes off
    size_t skipped = 0;           // the table says this state has no counterpart
    size_t unknown = 0;           // no row for the id at all
    size_t unresolved = 0;        // the table names a block this build does not have
};

struct PastePlan {
    struct Cell {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        BlockID block = 0;
    };

    // World coordinates, in the file's own iteration order (y, then z, then x),
    // so the plan is reproducible and a test can assert an exact sequence.
    std::vector<Cell> cells;
    PasteStats stats;
    // Bounds of the cells themselves (empty when nothing is planned), so a caller
    // can report the volume it would touch without walking the file again.
    int32_t min_x = 0, max_x = -1, min_y = 0, max_y = -1, min_z = 0, max_z = -1;

    [[nodiscard]] bool empty() const noexcept { return cells.empty(); }
    [[nodiscard]] size_t size() const noexcept { return cells.size(); }
};

// A name is resolved to this build's block id; false means the table named
// something that does not exist here, which is a fault worth reporting rather
// than silently dropping.
using BlockResolver = std::function<bool(const std::string& name, BlockID& out)>;

// Where a plan's CONTENT sat inside the file's own box, in blocks, as of the moment
// it was anchored (see anchor_plan_on_content). Zero for a build that starts at its
// own corner, which is the common case.
struct PlanMargin {
    int32_t x = 0, y = 0, z = 0;
};

// Moves every cell, and the bounds, by the same amount. An empty plan is left alone
// (its bounds are an empty sentinel, not a position).
void translate_plan(PastePlan& plan, int32_t dx, int32_t dy, int32_t dz);

// Anchors the plan on its CONTENT instead of on the file's own corner, and reports
// where that content was inside the box.
//
// This is the difference between "paste where I aimed" and "paste somewhere in the
// next field". A build saved from a region selection carries the selection's empty
// margin inside its declared box — one of the sample files holds nothing until 90
// blocks in on x and 114 on z — so planting the BOX corner at the crosshair puts the
// building a hundred blocks away from it. Aiming at the content instead is what a
// player means by "put it there", and for a build that starts at its own corner the
// two are identical.
PlanMargin anchor_plan_on_content(PastePlan& plan, int32_t x, int32_t y, int32_t z);

// Builds the plan for `file` placed with its (0, 0, 0) corner at the origin.
// Fails only on a fault the caller cannot sensibly continue past (the cell cap);
// everything ordinary — an id with no row, a state the table skips — is counted.
bool plan_paste(const SchematicData& file, const McPalette& palette,
                int32_t origin_x, int32_t origin_y, int32_t origin_z,
                const PasteOptions& options, const BlockResolver& resolve,
                PastePlan& out, std::string* error = nullptr);

// The plan that puts a paste back: one cell per written position, holding the
// block that was there. Air is included, so reverting a paste that filled a cave
// re-opens it. A caller records the displaced blocks while writing; this turns
// that record into an ordinary plan, which is what lets undo reuse the writer.
struct PasteUndo {
    std::vector<PastePlan::Cell> cells;  // block = the block that was displaced
    [[nodiscard]] bool valid() const noexcept { return !cells.empty(); }
};

[[nodiscard]] PastePlan to_revert_plan(const PasteUndo& undo);

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_PASTE_PLAN_HPP
