#include "doctest.h"

#include "mesh_builder_test_support.hpp"

#include "mesh/mesh_builder.hpp"
#include "mesh/ambient_occlusion.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <vector>

using namespace VoxelEngine;
using namespace mesh_builder_test;

// ---------------------------------------------------------------------------
// Partial remeshing: an incremental rebuild (dirty region re-emit + quad
// carry-forward) must produce exactly the same mesh as a full rebuild.
// ---------------------------------------------------------------------------
TEST_CASE("partial rebuild matches full rebuild after mid-chunk edit (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    // Full build of the pre-edit state -> quad cache + light checksums.
    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Edit: replace the (10,17,10) grass block with a stone block, and move the
    // light source one cell over (a light-only change in a different column).
    chunk.set_block(10, 17, 10, BlockIDs::STONE);
    chunk.set_light_rgb(16, 17, 16, 0, 0, 0);
    chunk.set_light_rgb(18, 18, 18, 9, 9, 9);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  subchunk_bounds_for(10, 17, 10));
    CHECK(mb_inc.get_quads().size() > 0);

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(mb_inc.get_water_vertices().size() == mb_full.get_water_vertices().size());
    CHECK(mb_inc.get_water_indices().size() == mb_full.get_water_indices().size());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild matches full rebuild after mid-chunk edit (fallback)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    MeshBuilder mb0;  // greedy disabled by default -> per-face fallback
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    chunk.set_block(10, 17, 10, BlockIDs::STONE);
    chunk.set_light_rgb(16, 17, 16, 0, 0, 0);
    chunk.set_light_rgb(18, 18, 18, 9, 9, 9);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  subchunk_bounds_for(10, 17, 10));

    MeshBuilder mb_full;
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(mb_inc.get_water_vertices().size() == mb_full.get_water_vertices().size());
    CHECK(mb_inc.get_water_indices().size() == mb_full.get_water_indices().size());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild matches full rebuild after border edit (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Border edit at the chunk's -x face: culls against a missing neighbor (air).
    chunk.set_block(0, 17, 16, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  subchunk_bounds_for(0, 17, 16));

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild matches full rebuild for light-only edit (fallback)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Light-only change: geometry identical, only vertex light data differs.
    // The light-checksum diff must pull the affected columns into the region.
    chunk.set_light_rgb(16, 17, 16, 0, 0, 0);
    chunk.set_light_rgb(16, 18, 16, 8, 8, 8);

    MeshBuilder mb_inc;
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  subchunk_bounds_for(16, 17, 16));

    MeshBuilder mb_full;
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild handles AO-varying run crossing the region boundary (greedy)") {    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 0; y <= 16; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            for (int x = 0; x < CHUNK_WIDTH; x++)
                chunk.set_block(x, y, z, BlockIDs::STONE);
    // Pillar just inside the dirty sub-chunk (x,y,z >= 16): its AO shadow at the
    // plane spans the region boundary (z=15 is outside, z=16 is inside), forcing
    // the plane row to emit per-face quads on a run that crosses the boundary.
    for (int y = 17; y < 24; y++)
        chunk.set_block(17, y, 17, BlockIDs::STONE);
    chunk.compute_section_flags();
    chunk.compute_fully_solid();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Edit inside the same sub-chunk.
    chunk.set_block(20, 17, 20, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  subchunk_bounds_for(17, 17, 17));

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild matches full rebuild with tight block bbox (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Single-block edit with the block-level bbox the manager now snapshots.
    chunk.set_block(10, 17, 10, BlockIDs::STONE);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  block_bbox_for(10, 17, 10));

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(mb_inc.get_water_vertices().size() == mb_full.get_water_vertices().size());
    CHECK(mb_inc.get_water_indices().size() == mb_full.get_water_indices().size());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild tight bbox pulls distant light change into region (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.set_light_rgb(16, 17, 16, 12, 8, 8);
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Edit block + light change in a far-away column (only the light-checksum
    // diff pulls that column's faces into the tight re-emit region).
    chunk.set_block(10, 17, 10, BlockIDs::STONE);
    chunk.set_light_rgb(5, 20, 5, 9, 9, 9);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  block_bbox_for(10, 17, 10));

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("partial rebuild re-emit region stays tight when no light changes (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);  // no light source anywhere in this chunk
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);
    CHECK(mb0.get_quads().size() > 0);

    // Mid-chunk edit with NO light side-effect. The zero-box light_bounds must
    // NOT be unioned in, or the region would be pulled down to (0,0,0) on every
    // axis (the pre-fix behavior re-emitted x:[0,22) y:[0,17) z:[0,10)).
    chunk.set_block(20, 15, 8, BlockIDs::AIR);
    chunk.compute_section_flags();

    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  block_bbox_for(20, 15, 8));

    // expand_bounds(bbox) = bbox expanded by 1, clamped to the chunk.
    const MeshBuilder::SubChunkBounds& actual = mb_inc.get_last_partial_bounds();
    CHECK(actual.x_min == 19);
    CHECK(actual.x_max == 22);
    CHECK(actual.y_min == 14);
    CHECK(actual.y_max == 17);
    CHECK(actual.z_min == 7);
    CHECK(actual.z_max == 10);

    // Correctness unchanged: the partial mesh must still match a full rebuild.
    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);
    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}
