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

    // 9 boxes x up to 5 faces (the emitter skips bottoms on non-full blocks) is
    // far more geometry than the 24 vertices of a full cube.
    CHECK(mb.get_vertex_count() > 100);
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
