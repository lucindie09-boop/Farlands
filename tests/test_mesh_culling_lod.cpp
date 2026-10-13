#include "doctest.h"

#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

using namespace VoxelEngine;

static bool has_vertex_at_x(const std::vector<Vertex>& verts, float target_x, float tolerance) {
    float target_fp = target_x * 256.0f;
    for (const auto& v : verts) {
        float fx = static_cast<float>(v.x);
        if (std::abs(fx - target_fp) < tolerance * 256.0f) return true;
    }
    return false;
}

TEST_CASE("LOD boundary: sand face emitted against water neighbor at stride=2 greedy") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.5f);
    CHECK(mb.get_stride_xz() == 2);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_greedy_verts = mb.get_vertex_count();
    CHECK(lod_greedy_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: sand face emitted against water neighbor at stride=1 greedy") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(1.0f);
    CHECK(mb.get_stride_xz() == 1);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t full_verts = mb.get_vertex_count();
    CHECK(full_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: sand face emitted against water at stride=2 non-greedy") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.set_detail_level(0.5f);
    CHECK(mb.get_stride_xz() == 2);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: sand face emitted against null neighbor at stride=2") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.5f);
    mb.build_mesh(chunk);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: full-detail and LOD produce same boundary face presence") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.set_detail_level(1.0f);
    mb_full.build_mesh(chunk, nullptr, &water_neighbor);
    bool full_has_face = has_vertex_at_x(mb_full.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    MeshBuilder mb_lod;
    mb_lod.set_greedy_enabled(true);
    mb_lod.set_detail_level(0.5f);
    mb_lod.build_mesh(chunk, nullptr, &water_neighbor);
    bool lod_has_face = has_vertex_at_x(mb_lod.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    INFO("Full detail has boundary face: ", full_has_face, " LOD has boundary face: ", lod_has_face);
    CHECK(full_has_face == lod_has_face);
}

TEST_CASE("LOD boundary: sand face emitted against water at stride=4 greedy") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: sand face emitted against water at stride=4 non-greedy") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: stride=4 and full-detail produce boundary faces") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.set_detail_level(1.0f);
    mb_full.build_mesh(chunk, nullptr, &water_neighbor);
    bool full_has = has_vertex_at_x(mb_full.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    MeshBuilder mb_s4;
    mb_s4.set_greedy_enabled(true);
    mb_s4.set_detail_level(0.25f);
    mb_s4.build_mesh(chunk, nullptr, &water_neighbor);
    bool s4_has = has_vertex_at_x(mb_s4.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    MeshBuilder mb_s2;
    mb_s2.set_greedy_enabled(true);
    mb_s2.set_detail_level(0.5f);
    mb_s2.build_mesh(chunk, nullptr, &water_neighbor);
    bool s2_has = has_vertex_at_x(mb_s2.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    INFO("Full has: ", full_has, " stride=2 has: ", s2_has, " stride=4 has: ", s4_has);
    CHECK(full_has == s4_has);
    CHECK(full_has == s2_has);
}

TEST_CASE("LOD boundary: partial 4x4 sand face against 4x4 water at stride=4") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; y++) {
        for (int z = 0; z < 4; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; y++) {
        for (int z = 0; z < 4; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: partial 4x4 sand with surrounding sand at stride=4") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; y++) {
        for (int z = 0; z < 4; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
            chunk.set_block(CHUNK_WIDTH - 2, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; y++) {
        for (int z = 0; z < 4; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: mixed sand/water column at stride=4 boundary") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 0; y < 10; y++) {
        for (int z = 0; z < 4; z++) {
            chunk.set_block(CHUNK_WIDTH - 1, y, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 20; y++) {
        for (int z = 0; z < 4; z++) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk, nullptr, &water_neighbor);
    size_t lod_verts = mb.get_vertex_count();
    CHECK(lod_verts > 0);
    CHECK(has_vertex_at_x(mb.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f));
}

TEST_CASE("LOD boundary: mixed shoreline footprint still emits opaque face against water") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; ++y) {
        for (int x = CHUNK_WIDTH - 4; x < CHUNK_WIDTH; ++x) {
            for (int z = 0; z < 4; ++z) {
                chunk.set_block(x, y, z, BlockIDs::WATER);
            }
        }
        chunk.set_block(CHUNK_WIDTH - 1, y, 1, BlockIDs::SAND);
    }
    chunk.compute_section_flags();

    ChunkData water_neighbor;
    water_neighbor.fill_blocks(BlockIDs::AIR);
    for (int y = 10; y < 14; ++y) {
        for (int z = 0; z < 4; ++z) {
            water_neighbor.set_block(0, y, z, BlockIDs::WATER);
        }
    }
    water_neighbor.compute_section_flags();

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.set_detail_level(1.0f);
    mb_full.build_mesh(chunk, nullptr, &water_neighbor);
    const bool full_has_opaque_boundary =
        has_vertex_at_x(mb_full.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);
    CHECK(full_has_opaque_boundary);

    MeshBuilder mb_lod;
    mb_lod.set_greedy_enabled(true);
    mb_lod.set_detail_level(0.25f);
    CHECK(mb_lod.get_stride_xz() == 4);
    mb_lod.build_mesh(chunk, nullptr, &water_neighbor);
    const bool lod_has_opaque_boundary =
        has_vertex_at_x(mb_lod.get_vertices(), static_cast<float>(CHUNK_WIDTH), 0.01f);

    INFO("full opaque boundary face: ", full_has_opaque_boundary,
         " lod opaque boundary face: ", lod_has_opaque_boundary);
    CHECK(lod_has_opaque_boundary);
}

namespace {
void uv_range_for_normal(const std::vector<Vertex>& verts, int8_t nx, int8_t ny, int8_t nz,
                         float& u_lo, float& u_hi, float& v_lo, float& v_hi) {
    u_lo = 1e9f;
    u_hi = -1e9f;
    v_lo = 1e9f;
    v_hi = -1e9f;
    for (const auto& vert : verts) {
        if (vert.nx != nx || vert.ny != ny || vert.nz != nz) continue;
        if (vert.u < u_lo) u_lo = vert.u;
        if (vert.u > u_hi) u_hi = vert.u;
        if (vert.v < v_lo) v_lo = vert.v;
        if (vert.v > v_hi) v_hi = vert.v;
    }
}
}

TEST_CASE("LOD stride: unmerged faces tile the texture once per block") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int x = 0; x < 4; x++) {
        for (int z = 0; z < 4; z++) {
            chunk.set_block(x, 10, z, BlockIDs::SAND);
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.set_detail_level(0.25f);
    CHECK(mb.get_stride_xz() == 4);
    mb.build_mesh(chunk);
    const auto& verts = mb.get_vertices();

    float u_lo, u_hi, v_lo, v_hi;

    // The top face covers a 4x4 cell: the texture repeats once per block in
    // both axes. Before the fix a stride face kept the unit-square UVs and one
    // texture stretched across the whole merged cell.
    uv_range_for_normal(verts, 0, 127, 0, u_lo, u_hi, v_lo, v_hi);
    CHECK(u_hi - u_lo == doctest::Approx(4.0f));
    CHECK(v_hi - v_lo == doctest::Approx(4.0f));

    // A side face is stride wide but one block tall: the horizontal axis tiles
    // once per block, the vertical stays a single repeat.
    uv_range_for_normal(verts, 127, 0, 0, u_lo, u_hi, v_lo, v_hi);
    CHECK(u_hi - u_lo == doctest::Approx(4.0f));
    CHECK(v_hi - v_lo == doctest::Approx(1.0f));

    uv_range_for_normal(verts, 0, 0, 127, u_lo, u_hi, v_lo, v_hi);
    CHECK(u_hi - u_lo == doctest::Approx(4.0f));
    CHECK(v_hi - v_lo == doctest::Approx(1.0f));
}
