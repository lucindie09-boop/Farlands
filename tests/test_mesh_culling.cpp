#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

using namespace VoxelEngine;

TEST_CASE("culling: interior block with all stone neighbors skips faces") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::STONE);
    chunk.set_block(15, 15, 15, BlockIDs::AIR);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    size_t with_hole = mb.get_vertex_count();

    ChunkData full;
    full.fill_blocks(BlockIDs::STONE);
    full.compute_section_flags();
    MeshBuilder mb2;
    mb2.set_greedy_enabled(false);
    mb2.build_mesh(full);
    size_t solid = mb2.get_vertex_count();

    CHECK(with_hole > solid);
}

TEST_CASE("culling: stone next to leaf keeps both faces") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(16, 15, 15, BlockIDs::LEAVES);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
}

TEST_CASE("culling: two adjacent leaves both render (transparent same-type)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(15, 15, 15, BlockIDs::LEAVES);
    chunk.set_block(16, 15, 15, BlockIDs::LEAVES);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    size_t one_leaf_verts;
    {
        ChunkData one;
        one.fill_blocks(BlockIDs::AIR);
        one.set_block(15, 15, 15, BlockIDs::LEAVES);
        one.compute_section_flags();
        MeshBuilder mb1;
        mb1.set_greedy_enabled(false);
        mb1.build_mesh(one);
        one_leaf_verts = mb1.get_vertex_count();
    }
    CHECK(mb.get_vertex_count() >= one_leaf_verts);
}

// A full cube that is drawn with TRANSPARENCY hides nothing, however completely it
// fills its cell. Two paths used to miss that: the vertical greedy "surrounded"
// shortcut (which only asked for full cubes) and the per-AABB culling test (which
// only asked the same). `water_fallen` is where it showed up in play, because it is
// the one liquid at full height — the runoff states are lowered, so they fail the
// offset test either way — and a shaft full of falling water lost the walls' faces.
TEST_CASE("culling: a falling-water column does not hide the shaft walls") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockID fallen = BlockRegistry::get_instance().get_block_id_by_name("water_fallen");
    CHECK(fallen != BlockIDs::AIR);

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    // A block of rock with a 1-wide shaft through it, filled with a column of water.
    // Every wall cell is then surrounded laterally by three stone and the water: all
    // four are non-air full cubes, which is exactly what the shortcut looked for.
    for (int32_t y = 10; y <= 20; ++y) {
        for (int32_t z = 10; z <= 20; ++z) {
            for (int32_t x = 10; x <= 20; ++x) {
                const bool shaft = (x == 15 && z == 15);
                chunk.set_block(x, y, z, shaft ? fallen : BlockIDs::STONE);
            }
        }
    }
    chunk.compute_section_flags();

    MeshBuilder mb;
    mb.set_greedy_enabled(true);
    mb.build_mesh(chunk);

    // The wall's face toward the shaft is drawn across all 11 cells of it; its
    // three faces into the rock are not. Measured as COVERAGE (the quads' own y
    // extent, summed) rather than as a quad count, because side faces merge along Y
    // into one quad anchored at the bottom of the run — and whether that merge
    // happens here is not what this test is about.
    int toward_water = 0;
    int into_rock = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (q.x != 14 || q.z != 15) continue;         // the wall column
        if (q.y < 10 || q.y > 20) continue;           // the shaft's span
        if (q.direction == FaceDirection::Right) toward_water += q.ey;
        if (q.direction == FaceDirection::Left || q.direction == FaceDirection::Front ||
            q.direction == FaceDirection::Back) {
            into_rock += q.ey;
        }
    }
    CHECK(toward_water == 11);
    CHECK(into_rock == 0);
}

TEST_CASE("culling: a partial block keeps its face against a transparent full cube") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fallen = reg.get_block_id_by_name("water_fallen");

    // A half-height slab. The built-in registry has no shapes (block_shapes.json is
    // only loaded by the game), so the box is described here.
    BlockType slab{};
    slab.name = "test_half_slab";
    slab.properties = BlockProperty::Solid | BlockProperty::Opaque;
    slab.visible_faces = {true, true, true, true, true, true};
    BlockAABB box{};
    box.min[0] = 0.0f; box.min[1] = 0.0f; box.min[2] = 0.0f;
    box.max[0] = 1.0f; box.max[1] = 0.5f; box.max[2] = 1.0f;
    slab.selection_boxes = {box};
    slab.full_cube_ = false;
    slab.greedy_mergeable = false;
    const BlockID slab_id = reg.register_block(slab);
    if (slab_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const auto slab_faces_toward = [&](BlockID neighbor) {
        ChunkData chunk;
        chunk.fill_blocks(BlockIDs::AIR);
        chunk.set_block(15, 15, 15, slab_id);
        chunk.set_block(16, 15, 15, neighbor);
        chunk.compute_section_flags();
        MeshBuilder mb;
        mb.set_greedy_enabled(true);
        mb.build_mesh(chunk);
        int faces = 0;
        for (const CachedQuad& q : mb.get_quads()) {
            if (q.x == 15 && q.y == 15 && q.z == 15 &&
                q.direction == FaceDirection::Right) {
                ++faces;
            }
        }
        return faces;
    };

    // The slab's side reaches the cell boundary, so a full cube of water beside it
    // would cover it — but water is transparent, so the face is still drawn.
    CHECK(slab_faces_toward(fallen) == 1);
    // The negative control: an opaque full cube really does hide it.
    CHECK(slab_faces_toward(BlockIDs::STONE) == 0);
}

TEST_CASE("boundary: null neighbor produces faces at chunk edge") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(0, 15, 15, BlockIDs::STONE);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk);
    CHECK(mb.get_vertex_count() > 0);
}

TEST_CASE("boundary: solid neighbor suppresses chunk-edge face") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData neighbor;
    neighbor.fill_blocks(BlockIDs::STONE);
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(0, 15, 15, BlockIDs::STONE);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk, &neighbor);
    MeshBuilder mb_none;
    mb_none.set_greedy_enabled(false);
    mb_none.build_mesh(chunk);
    CHECK(mb.get_vertex_count() <= mb_none.get_vertex_count());
}

TEST_CASE("boundary: leaf neighbor does not suppress stone face") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData neighbor;
    neighbor.fill_blocks(BlockIDs::LEAVES);
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(0, 15, 15, BlockIDs::STONE);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk, &neighbor);
    MeshBuilder mb_none;
    mb_none.set_greedy_enabled(false);
    mb_none.build_mesh(chunk);
    CHECK(mb.get_vertex_count() == mb_none.get_vertex_count());
}

TEST_CASE("boundary: height-offset blocks interact correctly") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData neighbor;
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(0, 15, 15, BlockIDs::STONE);
    chunk.set_block(0, 14, 15, BlockIDs::WATER);
    neighbor.fill_blocks(BlockIDs::AIR);
    neighbor.set_block(CHUNK_WIDTH - 1, 14, 15, BlockIDs::WATER);
    chunk.compute_section_flags();
    MeshBuilder mb;
    mb.set_greedy_enabled(false);
    mb.build_mesh(chunk, &neighbor);
    CHECK(mb.get_vertex_count() > 0);
}

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
