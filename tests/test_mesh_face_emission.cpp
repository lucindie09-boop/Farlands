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
