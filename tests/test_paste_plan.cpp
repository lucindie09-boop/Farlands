#include "doctest.h"
#include "schematic/paste_plan.hpp"

#include <algorithm>
#include <map>
#include <string>
#include <vector>

using VoxelEngine::BlockID;
using VoxelEngine::schematic::McPalette;
using VoxelEngine::schematic::PasteOptions;
using VoxelEngine::schematic::PastePlan;
using VoxelEngine::schematic::PasteStats;
using VoxelEngine::schematic::PasteUndo;
using VoxelEngine::schematic::anchor_plan_on_content;
using VoxelEngine::schematic::plan_paste;
using VoxelEngine::schematic::PlanMargin;
using VoxelEngine::schematic::SchematicData;
using VoxelEngine::schematic::to_revert_plan;

namespace {

// The table used by every case here: one exact block, one stand-in, one liquid,
// one deliberate skip, and a row for an id the rest of the world has never heard
// of is left out on purpose (that is the `unknown` bucket).
constexpr const char* kTable = R"({
  "blocks": [
    { "id": 0, "block": "air" },
    { "id": 1, "block": "stone" },
    { "id": 2, "block": "grass" },
    { "id": 5, "block": "oak_planks", "substitute": true },
    { "id": 8, "block": "water", "fluid": true },
    { "id": 9, "block": "water", "fluid": true, "still": "surface_water" },
    { "id": 10, "block": "lava", "fluid": true, "still": "surface_lava", "substitute": true },
    { "id": 35, "block": "water", "fluid": true, "substitute": true },
    { "id": 50, "skip": true },
    { "id": 999, "block": "no_such_block" }
  ]
})";

// The ids this build has; a resolver over this map is what the engine's
// registry scan does in-game.
const std::map<std::string, BlockID> kBlocks = {
    {"air", 0}, {"stone", 1}, {"grass", 2}, {"oak_planks", 3}, {"water", 4},
    {"surface_water", 5}, {"lava", 6}, {"surface_lava", 7},
};

bool resolve(const std::string& name, BlockID& out) {
    const auto found = kBlocks.find(name);
    if (found == kBlocks.end()) return false;
    out = found->second;
    return true;
}

McPalette table() {
    McPalette palette;
    std::string error;
    CHECK_MESSAGE(palette.load(kTable, &error), error);
    return palette;
}

// A file whose cells are given as legacy (id, data) pairs in the reader's own
// order (x fastest, then z, then y), assembled into the palette the reader
// would have produced.
SchematicData make_file(int32_t width, int32_t height, int32_t length,
                        const std::vector<std::pair<uint16_t, uint8_t>>& states) {
    SchematicData file;
    file.width = width;
    file.height = height;
    file.length = length;
    for (const auto& state : states) {
        uint32_t slot = 0;
        bool found = false;
        for (size_t i = 0; i < file.palette.size(); ++i) {
            if (file.palette[i].id == state.first && file.palette[i].data == state.second) {
                slot = static_cast<uint32_t>(i);
                found = true;
                break;
            }
        }
        if (!found) {
            VoxelEngine::schematic::BlockState entry;
            entry.id = state.first;
            entry.data = state.second;
            file.palette.push_back(entry);
            slot = static_cast<uint32_t>(file.palette.size() - 1);
        }
        file.cells.push_back(slot);
        if (state.first != 0) ++file.non_air_cells;
    }
    return file;
}

std::vector<uint32_t> described(const PastePlan& plan) {
    std::vector<uint32_t> out;
    out.reserve(plan.cells.size());
    for (const PastePlan::Cell& cell : plan.cells) {
        out.push_back(static_cast<uint32_t>(cell.x & 0x3FF) |
                      (static_cast<uint32_t>(cell.y & 0x3FF) << 10) |
                      (static_cast<uint32_t>(cell.z & 0x3FF) << 20) |
                      (static_cast<uint32_t>(cell.block) << 30));
    }
    return out;
}

PastePlan plan_of(const SchematicData& file, const PasteOptions& options, int32_t ox = 0, int32_t oy = 0,
                  int32_t oz = 0, std::string* error = nullptr) {
    PastePlan plan;
    std::string local_error;
    const bool ok = plan_paste(file, table(), ox, oy, oz, options, resolve, plan, &local_error);
    CHECK_MESSAGE(ok, local_error);
    if (error != nullptr) *error = local_error;
    return plan;
}

size_t counted_total(const PasteStats& stats) {
    return stats.air_ignored + stats.placed + stats.substituted + stats.stilled +
           stats.declined_fluid + stats.declined_substitute + stats.skipped + stats.unknown +
           stats.unresolved;
}

} // namespace

TEST_CASE("paste plan: every non-air cell lands in exactly one bucket") {
    // 2×2×2: stone, planks (stand-in), water, torch (skip), air ×3, and an id
    // with no row at all.
    const SchematicData file = make_file(2, 2, 2, {
        {1, 0}, {5, 0},
        {8, 0}, {50, 0},
        {0, 0}, {1, 0},
        {0, 0}, {4000, 0},
    });
    const PastePlan plan = plan_of(file, PasteOptions{});

    CHECK(plan.stats.file_cells == 8);
    CHECK(plan.stats.air_ignored == 2);
    CHECK(plan.stats.placed == 2);         // stone twice
    CHECK(plan.stats.substituted == 1);    // planks
    CHECK(plan.stats.declined_fluid == 1); // water, fluids off
    CHECK(plan.stats.skipped == 1);        // torch
    CHECK(plan.stats.unknown == 1);        // no row
    CHECK(plan.stats.unresolved == 0);
    CHECK(counted_total(plan.stats) == plan.stats.file_cells);

    // Air, the liquid, the skip and the unknown are all left out, so the plan is
    // the two stone cells and the planks: three of the eight.
    CHECK(plan.cells.size() == 3);
    CHECK(plan.min_x == 0);
    CHECK(plan.max_x == 1);
}

TEST_CASE("paste plan: cells carry world coordinates in the file's own order") {
    // A 2×1×2 slab with distinguishable corners, placed at an origin so a
    // transposed axis would be visible rather than plausible.
    const SchematicData file = make_file(2, 1, 2, {
        {1, 0}, {2, 0},   // y0 z0: x0 stone, x1 grass
        {5, 0}, {0, 0},   // y0 z1: x0 planks, x1 air
    });
    const PastePlan plan = plan_of(file, PasteOptions{}, 10, 64, 20);

    CHECK(plan.cells.size() == 3);
    // Order is y, then z, then x.
    CHECK(plan.cells[0].x == 10);
    CHECK(plan.cells[0].y == 64);
    CHECK(plan.cells[0].z == 20);
    CHECK(plan.cells[0].block == kBlocks.at("stone"));

    CHECK(plan.cells[1].x == 11);
    CHECK(plan.cells[1].z == 20);
    CHECK(plan.cells[1].block == kBlocks.at("grass"));

    // x = 0 of the far row is one step in z, not one step in x.
    CHECK(plan.cells[2].x == 10);
    CHECK(plan.cells[2].z == 21);
    CHECK(plan.cells[2].block == kBlocks.at("oak_planks"));

    CHECK(plan.min_x == 10);
    CHECK(plan.max_x == 11);
    CHECK(plan.min_y == 64);
    CHECK(plan.max_y == 64);
    CHECK(plan.min_z == 20);
    CHECK(plan.max_z == 21);

    // The same input twice is the same plan, cell for cell.
    const PastePlan again = plan_of(file, PasteOptions{}, 10, 64, 20);
    CHECK(described(plan) == described(again));
}

TEST_CASE("paste plan: the policy gates are independent") {
    const SchematicData file = make_file(1, 1, 5, {
        {1, 0}, {5, 0}, {8, 0}, {35, 0}, {50, 0},
    });

    {   // Defaults: exact blocks AND stand-ins land, liquids do not.
        const PastePlan plan = plan_of(file, PasteOptions{});
        CHECK(plan.cells.size() == 2);
        CHECK(plan.stats.placed == 1);               // stone
        CHECK(plan.stats.substituted == 1);          // planks
        CHECK(plan.stats.declined_fluid == 2);       // water, and water-as-stand-in
        CHECK(plan.stats.declined_substitute == 0);
        CHECK(plan.stats.skipped == 1);
    }
    {   // Stand-ins off: only exact counterparts land.
        PasteOptions options;
        options.substitutes = false;
        const PastePlan plan = plan_of(file, options);
        CHECK(plan.cells.size() == 1);
        CHECK(plan.stats.placed == 1);
        CHECK(plan.stats.declined_substitute == 1);  // planks
        CHECK(plan.stats.declined_fluid == 2);
    }
    {   // Both on: the stand-in liquid needs both gates, and passes both here.
        PasteOptions options;
        options.substitutes = true;
        options.fluids = true;
        const PastePlan plan = plan_of(file, options);
        CHECK(plan.cells.size() == 4);
        CHECK(plan.stats.placed == 2);       // stone, and water (not a stand-in)
        CHECK(plan.stats.substituted == 2);  // planks, and water-as-stand-in
        CHECK(plan.stats.declined_fluid == 0);
    }
    {   // Liquids on, stand-ins off: the stand-in liquid is declined for the
        // other reason too, so planks and wool-become-water both stay out.
        PasteOptions options;
        options.fluids = true;
        options.substitutes = false;
        const PastePlan plan = plan_of(file, options);
        CHECK(plan.cells.size() == 2);
        CHECK(plan.stats.placed == 2);               // stone, and water
        CHECK(plan.stats.declined_substitute == 2);
        CHECK(plan.stats.declined_fluid == 0);
    }
}

TEST_CASE("paste plan: a liquid lands still unless the paste asks for fluids") {
    // id 8 has no still form in the table, id 9 does, and id 10's row is both a
    // stand-in and a liquid — so one file exercises every branch of the rule.
    const SchematicData file = make_file(1, 1, 4, {
        {8, 0}, {9, 0}, {10, 0}, {1, 0},
    });

    {   // Defaults: the water is THERE, it just is not running.
        const PastePlan plan = plan_of(file, PasteOptions{});
        CHECK(plan.stats.stilled == 2);          // surface_water and surface_lava
        CHECK(plan.stats.substituted == 0);      // a stilled stand-in counts as stilled
        CHECK(plan.stats.placed == 1);           // stone, and only stone
        CHECK(plan.stats.declined_fluid == 1);   // id 8: nothing still to put there
        CHECK(plan.cells.size() == 3);
        CHECK(plan.cells[0].block == 5);         // surface_water
        CHECK(plan.cells[1].block == 7);         // surface_lava
        CHECK(plan.cells[2].block == 1);         // stone
        CHECK(counted_total(plan.stats) == plan.stats.file_cells);
    }
    {   // `fluids`: the same cells become the running sources.
        PasteOptions options;
        options.fluids = true;
        const PastePlan plan = plan_of(file, options);
        CHECK(plan.stats.stilled == 0);
        CHECK(plan.stats.declined_fluid == 0);
        CHECK(plan.stats.placed == 3);          // stone, water, water
        CHECK(plan.stats.substituted == 1);     // the lava row is a stand-in
        CHECK(plan.cells.size() == 4);
        CHECK(plan.cells[0].block == 4);        // water
        CHECK(plan.cells[1].block == 4);        // water
        CHECK(plan.cells[2].block == 6);        // lava
    }
    {   // Stand-ins off does not touch a liquid that is an exact counterpart.
        PasteOptions options;
        options.substitutes = false;
        const PastePlan plan = plan_of(file, options);
        CHECK(plan.stats.stilled == 1);              // surface_water
        CHECK(plan.stats.declined_substitute == 1);  // the lava stand-in
        CHECK(plan.cells.size() == 2);
    }
}

TEST_CASE("paste plan: the file's air is optional and clears cells when it lands") {
    const SchematicData file = make_file(1, 1, 3, {{1, 0}, {0, 0}, {2, 0}});

    PasteOptions options;
    CHECK(plan_of(file, options).stats.air_ignored == 1);

    options.write_air = true;
    const PastePlan plan = plan_of(file, options);
    CHECK(plan.stats.air_ignored == 0);
    CHECK(plan.cells.size() == 3);
    // The middle cell is an explicit air write, not a hole in the plan.
    CHECK(plan.cells[1].block == VoxelEngine::BlockIDs::AIR);
    CHECK(plan.stats.placed == 3);
}

TEST_CASE("paste plan: an unresolvable target is counted, not placed") {
    const SchematicData file = make_file(1, 1, 2, {{999, 0}, {1, 0}});
    const PastePlan plan = plan_of(file, PasteOptions{});
    CHECK(plan.stats.unresolved == 1);
    CHECK(plan.stats.placed == 1);
    CHECK(plan.cells.size() == 1);
    CHECK(counted_total(plan.stats) == 2);
}

TEST_CASE("paste plan: a paste above the cell cap is refused whole") {
    const SchematicData file = make_file(1, 1, 4, {{1, 0}, {1, 0}, {1, 0}, {1, 0}});

    PasteOptions options;
    options.max_cells = 4;
    CHECK(plan_of(file, options).cells.size() == 4);  // exactly at the cap is fine

    options.max_cells = 3;
    PastePlan plan;
    std::string error;
    CHECK_FALSE(plan_paste(file, table(), 0, 0, 0, options, resolve, plan, &error));
    CHECK(error.find("4 cells") != std::string::npos);
    CHECK(error.find("limit of 3") != std::string::npos);
    // Refused means empty, not truncated: half a building is worse than none.
    CHECK(plan.cells.empty());
}

TEST_CASE("paste plan: a plan with nothing to do is empty and still counted") {
    const SchematicData file = make_file(1, 1, 2, {{0, 0}, {0, 0}});
    const PastePlan plan = plan_of(file, PasteOptions{});
    CHECK(plan.empty());
    CHECK(plan.max_x < plan.min_x);
    CHECK(plan.stats.air_ignored == 2);

    // ...and an empty source file's box does not produce a phantom cell.
    SchematicData empty;
    empty.width = 1;
    empty.height = 1;
    empty.length = 1;
    empty.cells = {0};
    empty.palette = {{0, 0}};
    PastePlan empty_plan;
    CHECK(plan_paste(empty, table(), 0, 0, 0, PasteOptions{}, resolve, empty_plan, nullptr));
    CHECK(empty_plan.empty());
}

TEST_CASE("paste plan: the revert plan is the write plan with the old blocks in it") {
    PasteUndo undo;
    undo.cells = {
        {4, 5, 6, 1},
        {4, 5, 7, 0},
        {6, 5, 6, 2},
    };
    const PastePlan revert = to_revert_plan(undo);
    CHECK(revert.cells.size() == 3);
    CHECK(revert.cells[0].x == 4);
    CHECK(revert.cells[0].y == 5);
    CHECK(revert.cells[0].z == 6);
    CHECK(revert.cells[0].block == 1);
    // Air is a legitimate revert target: it re-opens what the paste filled.
    CHECK(revert.cells[1].block == VoxelEngine::BlockIDs::AIR);
    CHECK(revert.min_x == 4);
    CHECK(revert.max_x == 6);
    CHECK(revert.min_z == 6);
    CHECK(revert.max_z == 7);
    CHECK(revert.stats.placed == 3);

    PasteUndo nothing;
    CHECK_FALSE(nothing.valid());
    CHECK(to_revert_plan(nothing).empty());
}

TEST_CASE("anchoring plants the CONTENT at the aim, not the file's box corner") {
    // A 4x1x4 box that holds nothing until 3 in on x and 2 in on z: the shape of a
    // build saved from a region selection, and the reason a big schematic landed a
    // hundred blocks from where it was right-clicked. The file's own order is x
    // fastest, then z, then y.
    std::vector<std::pair<uint16_t, uint8_t>> states(16, {0, 0});
    states[3 + 2 * 4] = {1, 0};  // x = 3, z = 2 -> stone
    const SchematicData file = make_file(4, 1, 4, states);

    PastePlan plan = plan_of(file, PasteOptions{}, /*ox=*/100, /*oy=*/64, /*oz=*/200);
    const int32_t width_before = plan.max_x - plan.min_x;
    const int32_t depth_before = plan.max_z - plan.min_z;
    // Unanchored, the aim has the file's BOX corner, so the content sits away from it
    // by the margin: 3 further out on x and 2 on z. That offset is exactly the reported
    // bug — a big file's margin is a hundred blocks, not three.
    CHECK(plan.min_x == 103);
    CHECK(plan.min_z == 202);
    bool off_before = false;
    for (const PastePlan::Cell& cell : plan.cells) {
        if (cell.block != VoxelEngine::BlockIDs::AIR) off_before = true;
    }
    CHECK(off_before);

    const PlanMargin margin = anchor_plan_on_content(plan, 100, 64, 200);
    // What the margin means: where the content WAS inside the box.
    CHECK(margin.x == 103);
    CHECK(margin.y == 64);
    CHECK(margin.z == 202);

    // Every cell that will actually be written now sits on the aim.
    for (const PastePlan::Cell& cell : plan.cells) {
        if (cell.block == VoxelEngine::BlockIDs::AIR) continue;
        CHECK(cell.x == 100);
        CHECK(cell.y == 64);
        CHECK(cell.z == 200);
    }
    // The bounds were translated with them, so a caller still reports the volume it
    // will touch (and the size of that volume is unchanged). plan_paste's bounds are
    // the PLANNED cells', not the file box's, so this one-cell build bounds to the
    // cell itself.
    CHECK(plan.max_x - plan.min_x == width_before);
    CHECK(plan.max_z - plan.min_z == depth_before);
    CHECK(plan.min_x == 100);
    CHECK(plan.max_x == 100);
    CHECK(plan.min_z == 200);
    CHECK(plan.max_z == 200);
}

TEST_CASE("anchoring a build that starts at its own corner changes nothing") {
    // The common case: content at (0, 0, 0) means the margin is the aim itself and
    // every cell stays exactly where it was. This is why the fix cannot regress an
    // ordinary build.
    const SchematicData file = make_file(2, 2, 2, {
        {1, 0}, {0, 0},
        {0, 0}, {1, 0},
        {1, 0}, {0, 0},
        {0, 0}, {0, 0},
    });
    PastePlan plan = plan_of(file, PasteOptions{}, 10, 20, 30);
    const std::vector<uint32_t> before = described(plan);

    const PlanMargin margin = anchor_plan_on_content(plan, 10, 20, 30);
    CHECK(margin.x == 10);
    CHECK(margin.y == 20);
    CHECK(margin.z == 30);
    CHECK(described(plan) == before);

    // A plan whose cells are all AIR has no content to anchor to. It must fall back to
    // its own bounds rather than to an uninitialised minimum, and it must not be moved
    // to a nonsense position: with the box corner already on the aim, the shift is zero.
    PasteOptions carve;
    carve.write_air = true;  // without this an air-only file plans no cells at all
    const SchematicData empty_file = make_file(2, 1, 2, {
        {0, 0}, {0, 0}, {0, 0}, {0, 0},
    });
    PastePlan air_only = plan_of(empty_file, carve, 5, 5, 5);
    CHECK_FALSE(air_only.cells.empty());
    const PlanMargin empty_margin = anchor_plan_on_content(air_only, 5, 5, 5);
    CHECK(empty_margin.x == 5);
    CHECK(empty_margin.y == 5);
    CHECK(empty_margin.z == 5);
}
