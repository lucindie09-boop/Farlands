#include "doctest.h"
#include "mesh/mesh_fluid.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

using namespace VoxelEngine;

namespace {

// ---------------------------------------------------------------------------
// The rule, on a world in a map: everything not listed is air. The surface maths
// reads nothing else, which is the whole reason mesh_fluid.hpp exists.
// ---------------------------------------------------------------------------
struct MapWorld {
    std::map<std::array<int32_t, 3>, BlockID> cells;
    const BlockRegistry* registry = nullptr;

    void put(int32_t x, int32_t y, int32_t z, BlockID id) { cells[{x, y, z}] = id; }

    mesh_fluid::CellInfo operator()(int32_t x, int32_t y, int32_t z) const {
        const auto it = cells.find({x, y, z});
        const BlockID id = (it == cells.end()) ? BlockIDs::AIR : it->second;
        return mesh_fluid::classify(id, registry->get_block_fast(id));
    }
};

// Heights the reference rule lands on, worked out by hand from its weights:
// a corner with one source and three empty cells, and one with four sources.
constexpr float kRimHeight = 0.698413f;
constexpr float kTwoToOneHeight = 0.814815f;
constexpr float kLevelHeight = 0.888889f;  // 8/9

bool near(float a, float b) { return std::fabs(a - b) < 0.002f; }

// Fractional Y of every water vertex: how high the surface sits inside its cell.
std::vector<float> water_surface_heights(const MeshBuilder& mb) {
    std::vector<float> out;
    out.reserve(mb.get_water_vertices().size());
    for (const Vertex& v : mb.get_water_vertices()) {
        const float y = static_cast<float>(v.y) / 256.0f;
        out.push_back(y - std::floor(y));
    }
    return out;
}

bool has_height(const std::vector<float>& heights, float want) {
    for (const float h : heights) {
        if (std::fabs(h - want) < 0.005f) return true;
    }
    return false;
}

// A chunk full of sea: a stone floor and water the rest of the way up. Meshed
// with the same chunk on all eight sides, which is what an ocean chunk in the
// middle of a sea actually looks like.
ChunkData make_sea_chunk(int32_t sea_top) {
    ChunkData c;
    c.fill_blocks(BlockIDs::AIR);
    for (int32_t y = 0; y <= sea_top; ++y) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                c.set_block(x, y, z, (y == 0) ? BlockIDs::STONE : BlockIDs::SURFACE_WATER);
            }
        }
    }
    c.compute_section_flags();
    return c;
}

int count_water_tops(const MeshBuilder& mb, int32_t run_length) {
    int n = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (q.water && q.direction == FaceDirection::Top && q.ez == run_length) ++n;
    }
    return n;
}

}  // namespace

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
