#ifndef FARLANDS_TESTS_MESH_FACE_EMISSION_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_MESH_FACE_EMISSION_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the shape face-emission tests.
//
// A `BlockAABB` built from two corners (`box`), a check for whether a mesh drew a
// vertex at a point, and `crucible_model`, the hollow multi-box shape the cavity and
// underside cases are all built from. `box` is used by both halves of the split.
//
// These were file-local to test_mesh_face_emission.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "mesh/mesh_builder.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace VoxelEngine;

namespace mesh_face_emission_test {


inline BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// Vertices are Q8.8 fixed point, so a position is exact when it is a multiple
// of 1/256. `x`/`y`/`z` are CHUNK-local, like the crucible test's checks.
inline bool has_vertex_at(const MeshBuilder& mb, float x, float y, float z) {
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
inline std::vector<BlockAABB> crucible_model() {
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


} // namespace mesh_face_emission_test

#endif // FARLANDS_TESTS_MESH_FACE_EMISSION_TEST_SUPPORT_HPP
