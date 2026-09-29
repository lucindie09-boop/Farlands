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

TEST_CASE("poisoned solid_cache: any read outside the tight populate box must fail (greedy)") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    make_test_terrain(chunk);
    chunk.compute_section_flags();

    MeshBuilder mb0;
    mb0.set_greedy_enabled(true);
    mb0.build_mesh(chunk);

    // Chop the base off the stone pillar at (6, y, 6), y in [17,24). The pillar's
    // vertical side-face runs straddle the region's y-extent, so the passes must
    // read correct block IDs well above the re-emit region to re-emit the full
    // floating-pillar run — exactly the spot a stale solid_cache value would
    // silently split the run and leave a hole.
    chunk.set_block(6, 17, 6, BlockIDs::AIR);
    chunk.compute_section_flags();

    // Before the partial build, fill solid_cache with an invalid sentinel. In
    // partial mode solid_cache is only populated for the tight box around the
    // dirty region; solid_at() routes everything outside it to live chunk data.
    // If any solid_cache read lands outside that box (a too-small box, an
    // underfilled populate loop, or a read not routed through solid_at), the
    // sentinel 0xFFFF surfaces as an obviously-wrong block instead of a stale
    // value that happens to match — and the mesh comparison below must fail.
    MeshBuilder mb_inc;
    mb_inc.set_greedy_enabled(true);
    mb_inc.debug_poison_solid_cache();
    mb_inc.build_mesh_incremental(chunk, mb0.get_quads(), mb0.get_light_checksums(),
                                  block_bbox_for(6, 17, 6));

    MeshBuilder mb_full;
    mb_full.set_greedy_enabled(true);
    mb_full.build_mesh(chunk);

    CHECK(mb_inc.get_vertex_count() == mb_full.get_vertex_count());
    CHECK(mb_inc.get_index_count() == mb_full.get_index_count());
    CHECK(meshes_identical(mb_inc, mb_full));
}

TEST_CASE("AO packing regression test") {
    BlockRegistry::get_instance().initialize_default_blocks();
    AmbientOcclusion ao;
    
    // Test that AO values are packed correctly without face shade multiplication
    // This ensures the shader's smooth curve is applied to pure AO values
    
    // Test pure AO values (no occlusion)
    uint8_t packed_full = ao.pack_vertex_ao(1.0f, FaceDirection::Top);
    uint8_t packed_half = ao.pack_vertex_ao(0.5f, FaceDirection::Top);
    uint8_t packed_quarter = ao.pack_vertex_ao(0.25f, FaceDirection::Top);
    
    // Should be approximately 255, 128, 64 (without face shade multiplier)
    CHECK(packed_full >= 250);  // ~255
    bool half_in_range = (packed_half >= 120) && (packed_half <= 140);
    CHECK(half_in_range);  // ~128
    bool quarter_in_range = (packed_quarter >= 60) && (packed_quarter <= 80);
    CHECK(quarter_in_range);  // ~64
    
    // Direction should not affect packing anymore (face shade moved to shader)
    uint8_t packed_top = ao.pack_vertex_ao(1.0f, FaceDirection::Top);
    uint8_t packed_bottom = ao.pack_vertex_ao(1.0f, FaceDirection::Bottom);
    uint8_t packed_side = ao.pack_vertex_ao(1.0f, FaceDirection::Right);
    
    // All should be identical now (face shade is shader-side)
    CHECK(packed_top == packed_bottom);
    CHECK(packed_top == packed_side);
}
