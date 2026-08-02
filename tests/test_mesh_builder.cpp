#include "doctest.h"
#include "mesh/mesh_builder.hpp"
#include "mesh/ambient_occlusion.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <set>
#include <array>

using namespace VoxelEngine;

// ---------------------------------------------------------------------------
// Partial-remesh helpers: a partial rebuild re-emits the dirty region first and
// appends carried quads afterwards, so its vertex ordering differs from a full
// rebuild. The MESH must still be identical: compare canonical triangle sets.
// ---------------------------------------------------------------------------
namespace {

bool same_vertex(const Vertex& a, const Vertex& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z &&
           a.nx == b.nx && a.ny == b.ny && a.nz == b.nz &&
           a.u == b.u && a.v == b.v &&
           a.texture_index == b.texture_index &&
           a.ao == b.ao &&
           a.emissive_index == b.emissive_index &&
           a.light_r == b.light_r && a.light_g == b.light_g &&
           a.light_b == b.light_b && a.sky_light == b.sky_light;
}

struct VertexLess {
    bool operator()(const Vertex& a, const Vertex& b) const {
        if (a.x != b.x) return a.x < b.x;
        if (a.y != b.y) return a.y < b.y;
        if (a.z != b.z) return a.z < b.z;
        if (a.nx != b.nx) return a.nx < b.nx;
        if (a.ny != b.ny) return a.ny < b.ny;
        if (a.nz != b.nz) return a.nz < b.nz;
        if (a.u != b.u) return a.u < b.u;
        if (a.v != b.v) return a.v < b.v;
        if (a.texture_index != b.texture_index) return a.texture_index < b.texture_index;
        if (a.ao != b.ao) return a.ao < b.ao;
        if (a.emissive_index != b.emissive_index) return a.emissive_index < b.emissive_index;
        if (a.light_r != b.light_r) return a.light_r < b.light_r;
        if (a.light_g != b.light_g) return a.light_g < b.light_g;
        if (a.light_b != b.light_b) return a.light_b < b.light_b;
        return a.sky_light < b.sky_light;
    }
};

using TriKey = std::array<Vertex, 3>;
struct TriLess {
    bool operator()(const TriKey& a, const TriKey& b) const {
        for (int i = 0; i < 3; ++i) {
            if (VertexLess{}(a[i], b[i])) return true;
            if (VertexLess{}(b[i], a[i])) return false;
        }
        return false;
    }
};

void add_triangles(const std::vector<Vertex>& verts, const std::vector<uint32_t>& idx,
                   std::multiset<TriKey, TriLess>& out) {
    for (size_t i = 0; i + 2 < idx.size(); i += 3) {
        TriKey t;
        t[0] = verts[idx[i]];
        t[1] = verts[idx[i + 1]];
        t[2] = verts[idx[i + 2]];
        std::sort(t.begin(), t.end(), VertexLess{});
        out.insert(t);
    }
}

void collect(const MeshBuilder& mb, std::multiset<TriKey, TriLess>& opaque,
             std::multiset<TriKey, TriLess>& water) {
    add_triangles(mb.get_vertices(), mb.get_indices(), opaque);
    add_triangles(mb.get_water_vertices(), mb.get_water_indices(), water);
}

bool tri_key_equal(const TriKey& a, const TriKey& b) {
    for (int i = 0; i < 3; ++i) {
        if (!same_vertex(a[i], b[i])) return false;
    }
    return true;
}

bool multisets_equal(const std::multiset<TriKey, TriLess>& a, const std::multiset<TriKey, TriLess>& b) {
    if (a.size() != b.size()) return false;
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (!tri_key_equal(*ia, *ib)) return false;
    }
    return true;
}

bool meshes_identical(const MeshBuilder& a, const MeshBuilder& b) {
    std::multiset<TriKey, TriLess> oa, wa, ob, wb;
    collect(a, oa, wa);
    collect(b, ob, wb);
    return multisets_equal(oa, ob) && multisets_equal(wa, wb);
}

void make_test_terrain(ChunkData& chunk) {
    chunk.fill_blocks(BlockIDs::AIR);
    for (int y = 0; y <= 16; y++)
        for (int z = 0; z < CHUNK_DEPTH; z++)
            for (int x = 0; x < CHUNK_WIDTH; x++)
                chunk.set_block(x, y, z, BlockIDs::STONE);
    for (int y = 17; y < 24; y++) {
        chunk.set_block(6, y, 6, BlockIDs::STONE);
        chunk.set_block(24, y, 24, BlockIDs::STONE);
    }
    chunk.set_block(10, 17, 10, BlockIDs::GRASS);
    chunk.set_block(20, 17, 20, BlockIDs::GRASS);
    for (int x = 12; x <= 16; x++)
        for (int z = 12; z <= 16; z++)
            chunk.set_block(x, 8, z, BlockIDs::WATER);
    chunk.compute_section_flags();
    chunk.compute_fully_solid();
}

MeshBuilder::SubChunkBounds subchunk_bounds_for(int32_t bx, int32_t by, int32_t bz) {
    const int32_t sx = bx / SUBCHUNK_SIZE;
    const int32_t sy = by / SUBCHUNK_SIZE;
    const int32_t sz = bz / SUBCHUNK_SIZE;
    MeshBuilder::SubChunkBounds b;
    b.x_min = sx * SUBCHUNK_SIZE;       b.x_max = (sx + 1) * SUBCHUNK_SIZE;
    b.y_min = sy * SUBCHUNK_SIZE;       b.y_max = (sy + 1) * SUBCHUNK_SIZE;
    b.z_min = sz * SUBCHUNK_SIZE;       b.z_max = (sz + 1) * SUBCHUNK_SIZE;
    return b;
}

// Tight block-level dirty bbox for a single edited block (what ChunkRenderData's
// mark_block_dirty produces): [min, max] inclusive converted to [min, max+1).
MeshBuilder::SubChunkBounds block_bbox_for(int32_t lx, int32_t ly, int32_t lz) {
    MeshBuilder::SubChunkBounds b;
    b.x_min = lx;       b.x_max = lx + 1;
    b.y_min = ly;       b.y_max = ly + 1;
    b.z_min = lz;       b.z_max = lz + 1;
    return b;
}

} // namespace

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
