#ifndef FARLANDS_TESTS_MESH_BUILDER_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_MESH_BUILDER_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the mesh builder tests.
//
// A partial rebuild re-emits the dirty region first and appends carried quads
// afterwards, so its vertex ORDER differs from a full rebuild while the mesh
// must be identical. Everything here exists to make that comparison: a vertex
// comparator, the triangle multiset it keys (`collect` / `meshes_identical`)
// and the terrain and bounding boxes the partial cases rebuild against. The
// `block_bbox_for` box is deliberately NOT the same as the sub-chunk bounds --
// the tight-box cases depend on that difference.
//
// These were file-local to test_mesh_builder.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <vector>

using namespace VoxelEngine;

namespace mesh_builder_test {

// ---------------------------------------------------------------------------
// Partial-remesh helpers: a partial rebuild re-emits the dirty region first and
// appends carried quads afterwards, so its vertex ordering differs from a full
// rebuild. The MESH must still be identical: compare canonical triangle sets.
// ---------------------------------------------------------------------------


inline bool same_vertex(const Vertex& a, const Vertex& b) {
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

inline void add_triangles(const std::vector<Vertex>& verts, const std::vector<uint32_t>& idx,
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

inline void collect(const MeshBuilder& mb, std::multiset<TriKey, TriLess>& opaque,
             std::multiset<TriKey, TriLess>& water) {
    add_triangles(mb.get_vertices(), mb.get_indices(), opaque);
    add_triangles(mb.get_water_vertices(), mb.get_water_indices(), water);
}

inline bool tri_key_equal(const TriKey& a, const TriKey& b) {
    for (int i = 0; i < 3; ++i) {
        if (!same_vertex(a[i], b[i])) return false;
    }
    return true;
}

inline bool multisets_equal(const std::multiset<TriKey, TriLess>& a, const std::multiset<TriKey, TriLess>& b) {
    if (a.size() != b.size()) return false;
    auto ia = a.begin();
    auto ib = b.begin();
    for (; ia != a.end(); ++ia, ++ib) {
        if (!tri_key_equal(*ia, *ib)) return false;
    }
    return true;
}

inline bool meshes_identical(const MeshBuilder& a, const MeshBuilder& b) {
    std::multiset<TriKey, TriLess> oa, wa, ob, wb;
    collect(a, oa, wa);
    collect(b, ob, wb);
    return multisets_equal(oa, ob) && multisets_equal(wa, wb);
}

inline void make_test_terrain(ChunkData& chunk) {
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

inline MeshBuilder::SubChunkBounds subchunk_bounds_for(int32_t bx, int32_t by, int32_t bz) {
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
inline MeshBuilder::SubChunkBounds block_bbox_for(int32_t lx, int32_t ly, int32_t lz) {
    MeshBuilder::SubChunkBounds b;
    b.x_min = lx;       b.x_max = lx + 1;
    b.y_min = ly;       b.y_max = ly + 1;
    b.z_min = lz;       b.z_max = lz + 1;
    return b;
}

} // namespace mesh_builder_test

#endif // FARLANDS_TESTS_MESH_BUILDER_TEST_SUPPORT_HPP
