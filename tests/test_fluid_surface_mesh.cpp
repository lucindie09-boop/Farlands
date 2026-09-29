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
// The mesh the pass produces
// ---------------------------------------------------------------------------

TEST_CASE("a pool's top face slopes from its rim to its middle") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int32_t x = 10; x <= 12; ++x) {
        for (int32_t z = 10; z <= 12; ++z) {
            chunk.set_block(x, 9, z, BlockIDs::STONE);
            chunk.set_block(x, 10, z, BlockIDs::WATER);
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.build_mesh(chunk);

    const std::vector<float> heights = water_surface_heights(mb);
    CHECK(heights.size() > 0);
    CHECK(has_height(heights, kRimHeight));      // the rim
    CHECK(has_height(heights, kTwoToOneHeight)); // one step in
    CHECK(has_height(heights, kLevelHeight));    // the middle of the pool

    // And the point of the whole exercise: ONE top quad carries corners at very
    // different heights, which is a slope. The old uniform-height surface could
    // only ever produce a quad whose four corners were identical.
    int sloped_quads = 0;
    int flat_quads = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (!q.water || q.direction != FaceDirection::Top) continue;
        float lo = 1.0f;
        float hi = 0.0f;
        for (const Vertex& v : q.verts) {
            const float y = static_cast<float>(v.y) / 256.0f;
            const float frac = y - std::floor(y);
            lo = std::min(lo, frac);
            hi = std::max(hi, frac);
        }
        if (hi - lo > 0.1f) ++sloped_quads;
        else ++flat_quads;
    }
    CHECK(sloped_quads > 0);
    CHECK(flat_quads > 0);  // the middle of the pool is still level
}

TEST_CASE("a flat run of a pool merges back into one quad") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    // 3 wide by 5 deep: the strictly interior column (x=11, z=11..13) is flat at
    // one height the whole way, and must come back as ONE quad rather than three.
    for (int32_t x = 10; x <= 12; ++x) {
        for (int32_t z = 10; z <= 14; ++z) {
            chunk.set_block(x, 9, z, BlockIDs::STONE);
            chunk.set_block(x, 10, z, BlockIDs::WATER);
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.build_mesh(chunk);

    CHECK(count_water_tops(mb, 3) == 1);
    // None of the run's cells are in the sloped columns, and none are doubled.
    CHECK(count_water_tops(mb, 1) == 12);
}

TEST_CASE("an open sea costs one quad per column, not one per cell") {
    BlockRegistry::get_instance().initialize_default_blocks();

    constexpr int32_t kSeaTop = 16;
    const ChunkData sea = make_sea_chunk(kSeaTop);
    const ChunkData west = make_sea_chunk(kSeaTop);
    const ChunkData east = make_sea_chunk(kSeaTop);
    const ChunkData north = make_sea_chunk(kSeaTop);
    const ChunkData south = make_sea_chunk(kSeaTop);
    const ChunkData nw = make_sea_chunk(kSeaTop);
    const ChunkData ne = make_sea_chunk(kSeaTop);
    const ChunkData sw = make_sea_chunk(kSeaTop);
    const ChunkData se = make_sea_chunk(kSeaTop);

    MeshBuilder mb;
    mb.build_mesh(sea, &west, &east, nullptr, nullptr, &north, &south,
                  &nw, &ne, &sw, &se);

    // 32 x 17 x 32 = 17,408 sea cells. One layer of them shows a surface, and the
    // interior of a sea is FLAT, so each column merges into a single quad: 32
    // quads for the whole chunk, and no side faces at all (a sea cell's
    // neighbours are the same substance, which is never drawn against).
    CHECK(count_water_tops(mb, CHUNK_DEPTH) == CHUNK_WIDTH);
    CHECK(mb.get_water_vertices().size() == CHUNK_WIDTH * 4);
    CHECK(mb.get_water_indices().size() == CHUNK_WIDTH * 6);

    // All at the level height: a flat sea stays flat, and it is the RIM of a sea
    // that falls away, not its middle.
    for (const Vertex& v : mb.get_water_vertices()) {
        const float surface = static_cast<float>(v.y) / 256.0f - static_cast<float>(kSeaTop);
        CHECK(near(surface, kLevelHeight));
    }
}

TEST_CASE("a liquid is drawn exactly once, by the fluid pass") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::WATER);
    chunk.compute_section_flags();

    // A lone cell in air: a top and four sides, no bottom (as everywhere else in
    // the mesher), from both emitter modes — the generic passes must not also
    // emit it.
    MeshBuilder greedy_off;
    greedy_off.set_greedy_enabled(false);
    greedy_off.build_mesh(chunk);
    MeshBuilder greedy_on;
    greedy_on.set_greedy_enabled(true);
    greedy_on.build_mesh(chunk);

    CHECK(greedy_off.get_water_vertices().size() == 20);
    CHECK(greedy_off.get_water_indices().size() == 30);
    CHECK(greedy_on.get_water_vertices().size() == 20);
    CHECK(greedy_on.get_water_indices().size() == 30);
    // All four of a lone cell's corners are the same, so it is one flat slab.
    CHECK(has_height(water_surface_heights(greedy_on), kRimHeight));
}

TEST_CASE("a liquid surface below the cell top keeps its face under a block") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::WATER);
    chunk.set_block(15, 16, 15, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.build_mesh(chunk);

    // A source's surface sits at 8/9: the band between it and the cell top is
    // open air, so the block's floor above does not cover the surface and the
    // top face must draw. Only a full-height (falling) column reaches 1.0 and
    // culls against a block above.
    CHECK(count_water_tops(mb, 1) == 1);
    CHECK(mb.get_water_vertices().size() == 16 + 4);  // four sides + the top
    CHECK(has_height(water_surface_heights(mb), kRimHeight));
}

TEST_CASE("a column of water draws full-height sides") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::WATER);
    chunk.set_block(15, 16, 15, BlockIDs::WATER);
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.build_mesh(chunk);

    // The lower cell has the same substance above it, so all four of its corners
    // are pinned to full height: its sides reach its own cell boundary (y=16)
    // rather than sloping, which is what makes a falling column a column. (The
    // check has to look at the cells at y=15 specifically: the upper cell's sides
    // start at 16 anyway, so "some vertex reaches 16" proves nothing.)
    int lower_sides = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (!q.water || q.direction == FaceDirection::Top || q.y != 15) continue;
        ++lower_sides;
        for (const Vertex& v : q.verts) {
            const float y = static_cast<float>(v.y) / 256.0f;
            const bool at_cell_edge = (y == 15.0f) || (y == 16.0f);
            CHECK(at_cell_edge);
        }
    }
    CHECK(lower_sides == 4);
    // The upper cell is capped normally: four sides and one flat top, while the
    // lower one draws its four sides and no top — nine faces, nothing doubled.
    CHECK(count_water_tops(mb, 1) == 1);
    CHECK(mb.get_water_vertices().size() == (4 + 5) * 4);
}

// ---------------------------------------------------------------------------
// Liquid-liquid side faces, decided per corner
// ---------------------------------------------------------------------------

TEST_CASE("a different-liquid side face hides only when covered at both shared corners") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    // The user's case, reduced to its geometry: two flows of different depths
    // meeting along an edge, with each one's surface height varying ALONG the
    // shared edge (its flow crosses the seam) — so the two edge profiles
    // interleave: each face is the taller one at one end and the shorter one at
    // the other, meaning neither covers the other and both must draw.
    //
    // Layout (stone floor everywhere, not listed): a uniform lava flow in the
    // -X half meets an acid column at x = 10 whose depth steps from shallow to
    // deep partway along the seam. The lava edge sits flat between the acid
    // edge's two corner heights.
    MapWorld w;
    w.registry = &reg;
    for (int32_t z = 5; z <= 15; ++z) {
        for (int32_t x = 6; x <= 9; ++x) w.put(x, 0, z, reg.get_block_id_by_name("lava_runoff_2"));
    }
    w.put(10, 0, 12, reg.get_block_id_by_name("acid_runoff_1"));
    w.put(10, 0, 13, reg.get_block_id_by_name("acid_runoff_1"));
    w.put(10, 0, 14, reg.get_block_id_by_name("acid_runoff_5"));

    // The two corners of the shared edge (the one the faces of cells z = 13
    // span), under each family's own corner rule.
    const float lava_0 = mesh_fluid::corner_height(w, FluidKind::Lava, 10, 0, 13);
    const float lava_1 = mesh_fluid::corner_height(w, FluidKind::Lava, 10, 0, 14);
    const float acid_0 = mesh_fluid::corner_height(w, FluidKind::Acid, 10, 0, 13);
    const float acid_1 = mesh_fluid::corner_height(w, FluidKind::Acid, 10, 0, 14);

    INFO("lava edge: ", lava_0, " / ", lava_1,
         "  acid edge: ", acid_0, " / ", acid_1);
    // Flat lava between the acid's steeply falling profile.
    CHECK(near(lava_0, 1.0f / 3.0f));
    CHECK(near(lava_1, 1.0f / 3.0f));
    CHECK(near(acid_0, 0.388889f));
    CHECK(near(acid_1, 0.277778f));
    CHECK((lava_0 > acid_0) != (lava_1 > acid_1));
    // Each face is covered at one corner only, so both are visible.
    CHECK(mesh_fluid::different_liquid_side_visible(lava_0, lava_1, acid_0, acid_1));
    CHECK(mesh_fluid::different_liquid_side_visible(acid_0, acid_1, lava_0, lava_1));
}

TEST_CASE("a uniformly shorter different-liquid face is still culled") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    // The same seam, but the acid is one step SHALLOWER everywhere along it, so
    // the lava's face is the taller one at both corners and the acid's is fully
    // hidden behind it — drawing both would double-blend the same wall.
    MapWorld w;
    w.registry = &reg;
    for (int32_t z = 5; z <= 15; ++z) {
        for (int32_t x = 6; x <= 9; ++x) w.put(x, 0, z, reg.get_block_id_by_name("lava_runoff_2"));
        for (int32_t x2 = 10; x2 <= 13; ++x2) w.put(x2, 0, z, reg.get_block_id_by_name("acid_runoff_3"));
    }

    const float lava_a = mesh_fluid::corner_height(w, FluidKind::Lava, 10, 0, 10);
    const float lava_b = mesh_fluid::corner_height(w, FluidKind::Lava, 10, 0, 11);
    const float acid_a = mesh_fluid::corner_height(w, FluidKind::Acid, 10, 0, 10);
    const float acid_b = mesh_fluid::corner_height(w, FluidKind::Acid, 10, 0, 11);
    CHECK(lava_a > acid_a);
    CHECK(lava_b > acid_b);
    CHECK_FALSE(mesh_fluid::different_liquid_side_visible(acid_a, acid_b, lava_a, lava_b));
    CHECK(mesh_fluid::different_liquid_side_visible(lava_a, lava_b, acid_a, acid_b));
}

// ---------------------------------------------------------------------------
// Texture flow direction, packed into the liquid vertices' AO byte
// ---------------------------------------------------------------------------

TEST_CASE("a level surface packs no flow, a sloped one flows downhill") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    mesh_fluid::Corners level;
    level.h[0][0] = level.h[0][1] = level.h[1][0] = level.h[1][1] = kLevelHeight;
    CHECK(mesh_fluid::top_face_flow(level) == 0);

    // Dropping the +X edge: the flow points east (bucket 3) and grows with
    // the steepness. (A 4-bit strength saturates quickly — the gentle drop is
    // chosen small enough to stay off the ceiling.)
    mesh_fluid::Corners east_slope = level;
    east_slope.h[0][1] = kLevelHeight - 0.03f;
    east_slope.h[1][1] = kLevelHeight - 0.03f;
    const uint8_t gentle = mesh_fluid::top_face_flow(east_slope);
    CHECK((gentle >> 4) == 3);
    CHECK((gentle & 0xF) > 0);
    CHECK((gentle & 0xF) < 15);

    east_slope.h[0][1] = 0.2f;
    east_slope.h[1][1] = 0.2f;
    const uint8_t steep = mesh_fluid::top_face_flow(east_slope);
    CHECK((steep >> 4) == 3);
    CHECK((steep & 0xF) > (gentle & 0xF));
}

TEST_CASE("flow compass: every direction buckets where it should") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    mesh_fluid::Corners c;
    c.h[0][0] = c.h[0][1] = c.h[1][0] = c.h[1][1] = kLevelHeight;
    // Corners combine both gradients: height(z, x) = z-drop + x-drop - level.
    const auto bucket = [&](float n, float s, float w, float e) {
        c.h[0][0] = w + n - kLevelHeight;
        c.h[0][1] = e + n - kLevelHeight;
        c.h[1][0] = w + s - kLevelHeight;
        c.h[1][1] = e + s - kLevelHeight;
        return mesh_fluid::top_face_flow(c) >> 4;
    };
    // h[cz][cx]: cz=0 is -Z (north), cx=0 is -X (west). Downhill = flow;
    // the LOWER edge gets the smaller height (kRimHeight < kLevelHeight).
    CHECK(bucket(kLevelHeight, kRimHeight, kLevelHeight, kLevelHeight) == 5);  // south low -> S
    CHECK(bucket(kRimHeight, kLevelHeight, kLevelHeight, kLevelHeight) == 1);  // north low -> N
    CHECK(bucket(kLevelHeight, kLevelHeight, kLevelHeight, kRimHeight) == 3);  // east low -> E
    CHECK(bucket(kLevelHeight, kLevelHeight, kRimHeight, kLevelHeight) == 7);  // west low -> W
    // Diagonals: SE low -> SE (4), NW low -> NW (8).
    CHECK(bucket(kLevelHeight, kRimHeight, kLevelHeight, kRimHeight) == 4);
    CHECK(bucket(kRimHeight, kLevelHeight, kRimHeight, kLevelHeight) == 8);
}

TEST_CASE("side faces of moving liquid flow down the wall, sources hold still") {
    CHECK(mesh_fluid::side_face_flow(false, false, 0) == 0);
    CHECK(mesh_fluid::side_face_flow(true, false, 0) == 0);       // a source
    const uint8_t runoff = mesh_fluid::side_face_flow(true, false, 2);
    CHECK((runoff >> 4) == 9);                                     // straight down
    CHECK((runoff & 0xF) > 0);
    const uint8_t falling = mesh_fluid::side_face_flow(true, true, 0);
    CHECK((falling >> 4) == 9);
    CHECK((falling & 0xF) > (runoff & 0xF));                       // a fall is faster
}

TEST_CASE("the fluid pass writes flow into the water vertices") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    // A sloped pool: interior flat (no flow), rim flowing toward the middle.
    for (int32_t x = 10; x <= 12; ++x) {
        for (int32_t z = 10; z <= 12; ++z) {
            chunk.set_block(x, 9, z, BlockIDs::STONE);
            chunk.set_block(x, 10, z, BlockIDs::WATER);
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.build_mesh(chunk);

    int flow_zero = 0;
    int flow_any = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (!q.water || q.direction != FaceDirection::Top) continue;
        for (const Vertex& v : q.verts) {
            if (v.ao == 0) ++flow_zero; else ++flow_any;
        }
    }
    CHECK(flow_zero > 0);  // the pool's level middle
    CHECK(flow_any > 0);   // the sloping rim
}
