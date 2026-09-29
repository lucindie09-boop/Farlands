#include "doctest.h"

#include "mesh/mesh_builder.hpp"
#include "mesh/ambient_occlusion.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <vector>

using namespace VoxelEngine;

TEST_CASE("build mesh for 2x2x2 stone cube") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int dx = 0; dx < 2; dx++) {
        for (int dy = 0; dy < 2; dy++) {
            for (int dz = 0; dz < 2; dz++) {
                chunk.set_block(15 + dx, 15 + dy, 15 + dz, BlockIDs::STONE);
            }
        }
    }
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    CHECK(mb.get_index_count() > 0);
    CHECK(mb.get_index_count() % 3 == 0);
    CHECK(mb.get_triangle_count() > 0);
}

TEST_CASE("build mesh for empty chunk") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.clear();
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() == 0);
    CHECK(mb.get_index_count() == 0);
}

TEST_CASE("build mesh for fully solid chunk") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::STONE);
    chunk.compute_fully_solid();
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
    CHECK(mb.get_triangle_count() > 0);
}

TEST_CASE("LOD stride computation from detail level") {
    MeshBuilder mb;
    SUBCASE("detail_level 1.0 -> stride 1") {
        mb.set_detail_level(1.0f);
        CHECK(mb.get_stride_xz() == 1);
        CHECK(mb.get_detail_level() == doctest::Approx(1.0f));
    }
    SUBCASE("detail_level 0.5 -> stride 2") {
        mb.set_detail_level(0.5f);
        CHECK(mb.get_stride_xz() == 2);
        CHECK(mb.get_detail_level() == doctest::Approx(0.5f));
    }
    SUBCASE("detail_level 0.25 -> stride 4") {
        mb.set_detail_level(0.25f);
        CHECK(mb.get_stride_xz() == 4);
        CHECK(mb.get_detail_level() == doctest::Approx(0.25f));
    }
    SUBCASE("detail_level 0.125 -> stride 8") {
        mb.set_detail_level(0.125f);
        CHECK(mb.get_stride_xz() == 8);
    }
    SUBCASE("detail_level clamped to 0.125 minimum") {
        mb.set_detail_level(0.01f);
        CHECK(mb.get_detail_level() == doctest::Approx(0.125f));
    }
    SUBCASE("detail_level clamped to 1.0 maximum") {
        mb.set_detail_level(5.0f);
        CHECK(mb.get_detail_level() == doctest::Approx(1.0f));
        CHECK(mb.get_stride_xz() == 1);
    }
}

TEST_CASE("LOD detail level 0.5 reduces vertices for large flat plane") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int z = 0; z < CHUNK_DEPTH; z++)
        for (int x = 0; x < CHUNK_WIDTH; x++)
            chunk.set_block(x, 16, z, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb_full;
    mb_full.set_detail_level(1.0f);
    mb_full.build_mesh(chunk);
    size_t full_verts = mb_full.get_vertex_count();

    MeshBuilder mb_lod;
    mb_lod.set_detail_level(0.5f);
    mb_lod.build_mesh(chunk);
    size_t lod_verts = mb_lod.get_vertex_count();

    CHECK(full_verts > 0);
    CHECK(lod_verts > 0);
    CHECK(lod_verts < full_verts);
}

TEST_CASE("LOD detail level 0.25 produces fewer vertices than 0.5") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int z = 0; z < CHUNK_DEPTH; z++)
        for (int x = 0; x < CHUNK_WIDTH; x++)
            chunk.set_block(x, 16, z, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb_half;
    mb_half.set_detail_level(0.5f);
    mb_half.build_mesh(chunk);
    size_t half_verts = mb_half.get_vertex_count();

    MeshBuilder mb_quarter;
    mb_quarter.set_detail_level(0.25f);
    mb_quarter.build_mesh(chunk);
    size_t quarter_verts = mb_quarter.get_vertex_count();

    CHECK(half_verts > 0);
    CHECK(quarter_verts > 0);
    CHECK(quarter_verts < half_verts);
}

TEST_CASE("LOD detail level does not produce empty mesh for visible geometry") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int z = 0; z < CHUNK_DEPTH; z++)
        for (int x = 0; x < CHUNK_WIDTH; x++)
            chunk.set_block(x, 16, z, BlockIDs::STONE);
    chunk.compute_section_flags();

    float levels[] = {1.0f, 0.5f, 0.25f};
    for (float level : levels) {
        MeshBuilder mb;
        mb.set_detail_level(level);
        mb.build_mesh(chunk);
        CHECK(mb.get_vertex_count() > 0);
        CHECK(mb.get_index_count() > 0);
        CHECK(mb.get_index_count() % 3 == 0);
    }
}
