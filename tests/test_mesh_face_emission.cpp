#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cmath>

using namespace VoxelEngine;

TEST_CASE("water faces route to water vertex/index buffers") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() > 0);
    CHECK(mb.get_water_indices().size() > 0);
}

TEST_CASE("opaque blocks do not appear in water buffers") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() == 0);
    CHECK(mb.get_water_indices().size() == 0);
}

TEST_CASE("surface water vs deep water both produce water mesh") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(10, 10, 10, BlockIDs::SURFACE_WATER);
    chunk.set_block(10, 9, 10, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_water_vertices().size() > 0);
}

TEST_CASE("solid block adjacent to water produces non-water mesh") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(16, 15, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    CHECK(mb.get_water_vertices().size() > 0);
}

TEST_CASE("side face above water is NOT shortened (side_lowered_offset removed)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(15, 14, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    bool found_normal = false;
    for (const auto& v : mb.get_vertices()) {
        float fy = static_cast<float>(v.y) / 256.0f;
        if (std::abs(fy - 16.0f) < 0.02f) {
            found_normal = true;
        }
    }
    CHECK(found_normal);
}

namespace {

BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// Vertices are Q8.8 fixed point, so a position is exact when it is a multiple
// of 1/256. `x`/`y`/`z` are CHUNK-local, like the crucible test's checks.
bool has_vertex_at(const MeshBuilder& mb, float x, float y, float z) {
    for (const Vertex& v : mb.get_vertices()) {
        if (std::fabs(static_cast<float>(v.x) / 256.0f - x) < 0.002f &&
            std::fabs(static_cast<float>(v.y) / 256.0f - y) < 0.002f &&
            std::fabs(static_cast<float>(v.z) / 256.0f - z) < 0.002f) {
            return true;
        }
    }
    return false;
}

// data/block_shapes.json "crucible": four 1x1px corner legs 2px tall, a 1px
// floor plate across the whole footprint, and four 1px walls open at the top.
std::vector<BlockAABB> crucible_model() {
    return {
        box(0.0f, 0.0f, 0.0f, 0.0625f, 0.125f, 0.0625f),
        box(0.9375f, 0.0f, 0.0f, 1.0f, 0.125f, 0.0625f),
        box(0.0f, 0.0f, 0.9375f, 0.0625f, 0.125f, 1.0f),
        box(0.9375f, 0.0f, 0.9375f, 1.0f, 0.125f, 1.0f),
        box(0.0f, 0.125f, 0.0f, 1.0f, 0.1875f, 1.0f),
        box(0.0f, 0.1875f, 0.0f, 0.0625f, 1.0f, 1.0f),
        box(0.9375f, 0.1875f, 0.0f, 1.0f, 1.0f, 1.0f),
        box(0.0625f, 0.1875f, 0.0f, 0.9375f, 1.0f, 0.0625f),
        box(0.0625f, 0.1875f, 0.9375f, 0.9375f, 1.0f, 1.0f),
    };
}

} // namespace

TEST_CASE("a hollow multi-box block emits its cavity, not a solid cube") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    BlockType crucible{};
    crucible.name = "test_crucible";
    crucible.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    crucible.visible_faces = {true, true, true, true, true, true};
    crucible.selection_boxes = crucible_model();
    crucible.full_cube_ = false;
    crucible.greedy_mergeable = false;
    const BlockID id = reg.register_block(crucible);
    if (id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, id);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);

    // Vertices are Q8.8 fixed point, so 0.0625 lands exactly on 16/256.
    std::vector<float> xs, ys, zs;
    for (const Vertex& v : mb.get_vertices()) {
        xs.push_back(static_cast<float>(v.x) / 256.0f);
        ys.push_back(static_cast<float>(v.y) / 256.0f);
        zs.push_back(static_cast<float>(v.z) / 256.0f);
    }
    if (xs.empty()) {
        CHECK(false);
        return;
    }
    const auto has_point = [&](float x, float y, float z) {
        for (size_t i = 0; i < xs.size(); ++i) {
            if (std::fabs(xs[i] - x) < 0.002f && std::fabs(ys[i] - y) < 0.002f &&
                std::fabs(zs[i] - z) < 0.002f) {
                return true;
            }
        }
        return false;
    };

    // The walls reach the top of the block and the floor plate sits on the legs
    // (2/16), so the vessel is open-mouthed rather than a stubby lip.
    CHECK(has_point(15.0625f, 16.0f, 15.0f));       // -X wall, inner edge, full height
    CHECK(has_point(16.0f, 16.0f, 16.0f));          // +X/+Z corner at full height
    CHECK(has_point(15.0f, 15.1875f, 15.0f));       // floor plate top (3/16)
    CHECK(has_point(15.0f, 15.125f, 15.0f));        // leg top (2/16)
    CHECK(has_point(15.0f, 15.0f, 15.0f));          // leg bottom, outer corner

    // The cavity's inner wall planes exist: a single full cube would have none
    // of these, so this is the check that the shape was expanded, not ignored.
    CHECK(has_point(15.0625f, 15.1875f, 15.0f));    // -X wall inner face
    CHECK(has_point(15.9375f, 15.1875f, 16.0f));    // +X wall inner face
    // The +/-Z walls sit between the +/-X walls, so their inner faces span x
    // from 1/16 to 15/16 rather than the whole footprint.
    CHECK(has_point(15.0625f, 15.1875f, 15.0625f)); // -Z wall inner face
    CHECK(has_point(15.9375f, 15.1875f, 15.9375f)); // +Z wall inner face

    // 9 boxes, each emitting its visible faces (bottoms included where the box
    // floats), is far more geometry than the 24 vertices of a full cube.
    CHECK(mb.get_vertex_count() > 100);
}

// A shape box raised off the cell floor has an underside you can see from below
// (a wall torch, a fence rail, a lantern). A box resting on the floor does not,
// and emitting it would cost a sixth of the per-AABB pass for nothing. The rule
// is what makes a floating model read as a model instead of a hole.
TEST_CASE("a shape box raised off the floor emits its underside; a floor box does not") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto bottom_vertices = [](const MeshBuilder& mb) {
        int count = 0;
        for (const Vertex& v : mb.get_vertices()) {
            if (v.ny == -127) ++count;  // packed face normal for -Y
        }
        return count;
    };

    // Exactly the wall-torch box: a 2/16 stick hugging one face, raised to 3.5/16.
    BlockType raised{};
    raised.name = "test_raised_stick";
    raised.properties = BlockProperty::NoOcclusion;
    raised.visible_faces = {true, true, true, true, true, true};
    raised.selection_boxes = {box(0.4375f, 0.21875f, 0.0f, 0.5625f, 0.84375f, 0.125f)};
    raised.full_cube_ = false;
    raised.greedy_mergeable = false;

    // The same box dropped onto the floor, which is the floor-torch case.
    BlockType grounded = raised;
    grounded.name = "test_floor_stick";
    grounded.selection_boxes = {box(0.4375f, 0.0f, 0.0f, 0.5625f, 0.625f, 0.125f)};

    const BlockID raised_id = reg.register_block(raised);
    const BlockID grounded_id = reg.register_block(grounded);
    if (raised_id == BlockIDs::AIR || grounded_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, raised_id);
    chunk.compute_section_flags();
    MeshBuilder raised_mb;
    raised_mb.set_greedy_enabled(false);
    raised_mb.build_mesh(chunk);

    // One quad, four vertices, all of them on the box's own bottom plane.
    CHECK(bottom_vertices(raised_mb) == 4);
    // Vertices are chunk-local, so the cell at (15,15,15) puts that plane at
    // 15.21875 and the box's corners at its own edges.
    CHECK(has_vertex_at(raised_mb, 15.4375f, 15.21875f, 15.0f));
    CHECK(has_vertex_at(raised_mb, 15.5625f, 15.21875f, 15.125f));

    ChunkData floor_chunk;
    floor_chunk.fill_blocks(BlockIDs::AIR);
    floor_chunk.set_block(15, 15, 15, grounded_id);
    floor_chunk.compute_section_flags();
    MeshBuilder floor_mb;
    floor_mb.set_greedy_enabled(false);
    floor_mb.build_mesh(floor_chunk);

    CHECK(bottom_vertices(floor_mb) == 0);
    // ...and the rest of the box really was drawn, so the check above is about
    // the bottom face rather than about a block that emitted nothing at all.
    CHECK(floor_mb.get_vertex_count() > 4);
}

// The single exception to that rule: when a sibling box of the same shape sits
// directly under the box, its underside is inside the shape and must not be
// emitted. A stair is exactly this case — its upper step over its own lower
// step — and an emitted bottom face there is a hidden quad that ships to the
// GPU on every stair block in the world.
TEST_CASE("a shape box whose underside is buried in a sibling box emits no bottom face") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();

    const auto bottom_vertices = [](const MeshBuilder& mb) {
        int count = 0;
        for (const Vertex& v : mb.get_vertices()) {
            if (v.ny == -127) ++count;  // packed face normal for -Y
        }
        return count;
    };

    const auto shaped = [&](const char* name, std::vector<BlockAABB> boxes) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::NoOcclusion;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.selection_boxes = std::move(boxes);
        bt.full_cube_ = false;
        bt.greedy_mergeable = false;
        return reg.register_block(bt);
    };

    // A stair: full-footprint lower step, half-footprint upper step on top of it.
    const BlockID stair = shaped("test_buried_step", {
        box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f),
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f),
    });
    // The upper step alone, to show the sibling is what suppresses the face.
    const BlockID step_only = shaped("test_step_alone", {
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f),
    });
    // The upper step over a narrow post: the lower box does not span the upper
    // box's footprint, so the underside is real and stays.
    const BlockID step_on_post = shaped("test_step_on_post", {
        box(0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 1.0f),
        box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f),
    });
    if (stair == BlockIDs::AIR || step_only == BlockIDs::AIR || step_on_post == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const auto mesh_of = [&](BlockID id) {
        ChunkData chunk;
        chunk.fill_blocks(BlockIDs::AIR);
        chunk.set_block(15, 15, 15, id);
        chunk.compute_section_flags();
        MeshBuilder mb;
        mb.set_greedy_enabled(false);
        mb.build_mesh(chunk);
        return mb;
    };

    // Lower step's bottom is floor-anchored (always skipped) and the upper
    // step's is buried, so a stair contributes no downward faces at all.
    CHECK(bottom_vertices(mesh_of(stair)) == 0);
    // Same box with nothing under it: four vertices on its own bottom plane.
    CHECK(bottom_vertices(mesh_of(step_only)) == 4);
    // A partial sibling does not cover the footprint, so the face survives.
    CHECK(bottom_vertices(mesh_of(step_on_post)) == 4);
}

TEST_CASE("AO: hole in solid chunk produces more faces than solid") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData solid;
    solid.fill_blocks(BlockIDs::STONE);
    solid.compute_section_flags();
    MeshBuilder mb_solid;
    mb_solid.set_greedy_enabled(false);
    mb_solid.build_mesh(solid);

    ChunkData with_hole;
    with_hole.fill_blocks(BlockIDs::STONE);
    with_hole.set_block(15, 15, 15, BlockIDs::AIR);
    with_hole.compute_section_flags();
    MeshBuilder mb_hole;
    mb_hole.set_greedy_enabled(false);
    mb_hole.build_mesh(with_hole);

    CHECK(mb_solid.get_vertex_count() > 0);
    CHECK(mb_hole.get_vertex_count() > mb_solid.get_vertex_count());
}
