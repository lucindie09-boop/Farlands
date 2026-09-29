#include "doctest.h"

#include "fluid_surface_test_support.hpp"

#include "mesh/mesh_fluid.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

using namespace fluid_surface_test;

using namespace VoxelEngine;

// ---------------------------------------------------------------------------
// The rule itself
// ---------------------------------------------------------------------------

TEST_CASE("fluid surface: a pool is level and its rim falls away") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    MapWorld w;
    w.registry = &reg;
    for (int32_t x = 10; x <= 12; ++x) {
        for (int32_t z = 10; z <= 12; ++z) {
            w.put(x, 0, z, BlockIDs::WATER);
        }
    }

    // Where four sources meet, the surface sits one ninth of a block down: the
    // flat height of a full pool.
    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 11, 0, 11), kLevelHeight));

    // The pool's outer corner has one source and three empty cells, so it drops
    // to roughly 0.70 — the rim.
    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 10, 0, 10), kRimHeight));

    // A cell in the middle of the pool is flat; a cell on its edge is the slope.
    CHECK(mesh_fluid::corners_of(w, FluidKind::Water, 11, 0, 11).flat());
    const mesh_fluid::Corners edge = mesh_fluid::corners_of(w, FluidKind::Water, 10, 0, 11);
    CHECK_FALSE(edge.flat());
    CHECK(near(edge.h[0][0], kTwoToOneHeight));  // two sources, two empty
    CHECK(near(edge.h[0][1], kLevelHeight));     // four sources
    CHECK(near(edge.h[1][0], kTwoToOneHeight));
    CHECK(near(edge.h[1][1], kLevelHeight));
    CHECK(near(edge.max_height(), kLevelHeight));
}

TEST_CASE("fluid surface: the cell above pins a corner to full height") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    MapWorld w;
    w.registry = &reg;
    w.put(10, 0, 10, BlockIDs::WATER);
    w.put(11, 0, 10, BlockIDs::WATER);
    // The same substance standing on the 11 corner: a column, so it is full.
    w.put(11, 1, 10, BlockIDs::WATER);

    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 11, 0, 10), 1.0f));
    // The pinning is per corner, not per cell: the corner on the far side of the
    // column is full too (the column cell is one of its four), while the corner on
    // the plain cell is untouched by it and keeps its rim slope.
    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 12, 0, 10), 1.0f));
    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 10, 0, 10), kRimHeight));

    // A different substance above is not a column: stone over water contributes
    // nothing at all, so the corner is measured as if the stone were not there.
    MapWorld roofed;
    roofed.registry = &reg;
    roofed.put(10, 0, 10, BlockIDs::WATER);
    roofed.put(10, 1, 10, BlockIDs::STONE);
    CHECK(near(mesh_fluid::corner_height(roofed, FluidKind::Water, 10, 0, 10), kRimHeight));
}

TEST_CASE("fluid surface: a corner buried in rock has no gap to measure") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    MapWorld w;
    w.registry = &reg;
    // All four cells that share this corner hold a body: no gap, no weight, and
    // the corner is therefore at full height rather than somewhere in the rock.
    w.put(10, 0, 10, BlockIDs::STONE);
    w.put(9, 0, 10, BlockIDs::STONE);
    w.put(10, 0, 9, BlockIDs::STONE);
    w.put(9, 0, 9, BlockIDs::STONE);
    CHECK(near(mesh_fluid::corner_height(w, FluidKind::Water, 10, 0, 10), 1.0f));

    // Air is not a body however the registry flags it, so it is a full gap: this
    // is the trap that makes air solid in the built-in default registry.
    MapWorld open;
    open.registry = &reg;
    CHECK(near(mesh_fluid::corner_height(open, FluidKind::Water, 10, 0, 10), 1.0f - 1.0f));
}

TEST_CASE("fluid surface: a deeper cell sits lower, and a falling cell is full strength") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const BlockID runoff1 = reg.get_block_id_by_name("water_runoff_1");
    const BlockID runoff7 = reg.get_block_id_by_name("water_runoff_7");
    const BlockID fallen = reg.get_block_id_by_name("water_fallen");
    CHECK(runoff1 != BlockIDs::AIR);
    CHECK(runoff7 != BlockIDs::AIR);
    CHECK(fallen != BlockIDs::AIR);

    // One cell ringed by air, so the only thing driving the corner is that cell's
    // own strength.
    const auto alone = [&](BlockID id) {
        MapWorld w;
        w.registry = &reg;
        w.put(0, 0, 0, id);
        return mesh_fluid::corner_height(w, FluidKind::Water, 0, 0, 0);
    };
    const float source = alone(BlockIDs::WATER);
    const float depth1 = alone(runoff1);
    const float depth7 = alone(runoff7);

    CHECK(near(source, kRimHeight));
    CHECK(depth7 < depth1);
    CHECK(depth1 < source);
    // A falling cell has no depth to store and is drawn at full strength, so it
    // measures exactly like a source.
    CHECK(near(alone(fallen), source));
}

TEST_CASE("fluid surface: a face is hidden only by its own substance or a full wall") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto shows = [&](FluidKind family, BlockID neighbor, float surface) {
        const BlockType& nt = reg.get_block_fast(neighbor);
        return mesh_fluid::face_visible(family, mesh_fluid::classify(neighbor, nt), nt, surface);
    };

    CHECK_FALSE(shows(FluidKind::Water, BlockIDs::WATER, kLevelHeight));
    // The sea is the same substance as poured water even though it is not a fluid
    // state, so the two must not draw faces against each other.
    CHECK_FALSE(shows(FluidKind::Water, BlockIDs::SURFACE_WATER, kLevelHeight));
    CHECK_FALSE(shows(FluidKind::Water, BlockIDs::STONE, kLevelHeight));

    CHECK(shows(FluidKind::Water, BlockIDs::AIR, kLevelHeight));
    // A slab is not a full cube, so it does not hide the surface. Its boxes come
    // from block_shapes.json, which the built-in registry does not load, so the
    // shape is described here rather than looked up.
    BlockType slab{};
    slab.full_cube_ = false;
    slab.properties = BlockProperty::Solid | BlockProperty::Opaque;
    CHECK(mesh_fluid::face_visible(FluidKind::Water, mesh_fluid::classify(BlockIDs::OAK_SLAB, slab),
                                   slab, kLevelHeight));
    // Mud is a full cube whose top is one pixel down: it shows through a
    // full-strength surface but hides a level one.
    CHECK(shows(FluidKind::Water, BlockIDs::MUD, 1.0f));
    CHECK_FALSE(shows(FluidKind::Water, BlockIDs::MUD, kLevelHeight));
}

TEST_CASE("fluid surface: only liquids are a surface family") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto family = [&](BlockID id) {
        return mesh_fluid::family_of(id, reg.get_block_fast(id));
    };
    CHECK(family(BlockIDs::WATER) == FluidKind::Water);
    CHECK(family(BlockIDs::SURFACE_WATER) == FluidKind::Water);
    CHECK(family(reg.get_block_id_by_name("water_runoff_3")) == FluidKind::Water);
    // Lowered but not liquid: mud is drawn by the generic emitters, with one
    // top height, exactly as before.
    CHECK(family(BlockIDs::MUD) == FluidKind::None);
    CHECK(family(BlockIDs::STONE) == FluidKind::None);
    CHECK(family(BlockIDs::AIR) == FluidKind::None);
}
