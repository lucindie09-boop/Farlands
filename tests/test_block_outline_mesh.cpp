#include "doctest.h"
#include "render/block_outline_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

using namespace VoxelEngine;

namespace {
OutlineBox cube(float x0, float y0, float z0, float x1, float y1, float z1) {
    OutlineBox b{};
    b.min[0] = x0;
    b.min[1] = y0;
    b.min[2] = z0;
    b.max[0] = x1;
    b.max[1] = y1;
    b.max[2] = z1;
    return b;
}

bool index_in_range(const OutlineMeshData& d) {
    const uint32_t count = static_cast<uint32_t>(d.verts.size() / 3);
    for (uint32_t i : d.indices) {
        if (i >= count) {
            return false;
        }
    }
    return true;
}

bool all_triangles_nondegenerate(const OutlineMeshData& d) {
    // Every emitted triangle must have measurably non-zero area. A collapsed
    // edge quad degenerates to a line (all corner verts coincident along the
    // edge), which this catches as a zero-area triangle.
    constexpr float kMinAreaSq = 1e-10f;
    for (size_t i = 0; i + 2 < d.indices.size(); i += 3) {
        const uint32_t ia = d.indices[i];
        const uint32_t ib = d.indices[i + 1];
        const uint32_t ic = d.indices[i + 2];
        const float ax = d.verts[ia * 3 + 0];
        const float ay = d.verts[ia * 3 + 1];
        const float az = d.verts[ia * 3 + 2];
        const float bx = d.verts[ib * 3 + 0] - ax;
        const float by = d.verts[ib * 3 + 1] - ay;
        const float bz = d.verts[ib * 3 + 2] - az;
        const float cx = d.verts[ic * 3 + 0] - ax;
        const float cy = d.verts[ic * 3 + 1] - ay;
        const float cz = d.verts[ic * 3 + 2] - az;
        const float crx = by * cz - bz * cy;
        const float cry = bz * cx - bx * cz;
        const float crz = bx * cy - by * cx;
        const float area_sq = 0.25f * (crx * crx + cry * cry + crz * crz);
        if (area_sq < kMinAreaSq) {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("block outline: empty box list produces no geometry") {
    const OutlineMeshData d = build_outline_mesh({}, 0.1f);
    CHECK(d.verts.empty());
    CHECK(d.indices.empty());

    float center[3];
    float size[3];
    outline_fill_bounds({}, center, size);
    CHECK(center[0] == 0.0f);
    CHECK(size[0] == 0.0f);
}

TEST_CASE("block outline: single full cube yields the 12 notional edges") {
    const OutlineMeshData d = build_outline_mesh({cube(0, 0, 0, 1, 1, 1)}, 0.1f);
    // Each unique edge is an extruded quad: 8 verts (3 floats each) and
    // 8 triangles (24 indices).
    CHECK(d.verts.size() == 12 * 8 * 3);
    CHECK(d.indices.size() == 12 * 24);
    CHECK(index_in_range(d));
}

TEST_CASE("block outline: overlapping duplicate cube dedups to a single cube") {
    const OutlineMeshData d = build_outline_mesh(
        {cube(0, 0, 0, 1, 1, 1), cube(0, 0, 0, 1, 1, 1)}, 0.1f);
    CHECK(d.verts.size() == 12 * 8 * 3);
    CHECK(d.indices.size() == 12 * 24);
}

TEST_CASE("block outline: side-by-side cubes XOR out the shared internal wall") {
    // Two cubes sharing the x=1 wall. The internal wall's faces cancel, but
    // perimeter edges crossing the seam split into collinear segments, so the
    // union keeps 16 unique edges (verified against a faithful Python port of
    // the original GDScript algorithm: 128 verts / 384 indices).
    const OutlineMeshData d = build_outline_mesh(
        {cube(0, 0, 0, 1, 1, 1), cube(1, 0, 0, 2, 1, 1)}, 0.1f);
    CHECK(d.verts.size() == 16 * 8 * 3);
    CHECK(d.indices.size() == 16 * 24);
    CHECK(index_in_range(d));
}

TEST_CASE("block outline: stacked slab union keeps both perimeters") {
    const OutlineMeshData d = build_outline_mesh(
        {cube(0, 0, 0, 1, 0.5f, 1), cube(0, 0.5f, 0, 1, 1, 1)}, 0.1f);
    // More geometry than a plain cube, and every edge is 8 verts + 24 indices.
    CHECK(d.verts.size() > 12 * 8 * 3);
    CHECK(d.indices.size() == (d.verts.size() / 3 / 8) * 24);
    CHECK(index_in_range(d));
}

TEST_CASE("block outline: short (non-unit) edges stay non-degenerate") {
    // Regression: the vertical edges of a half-height slab run (0,±0.5,0),
    // not unit length. Their perpendicular basis must not depend on the raw
    // delta length — a unit cube is unaffected (edge length 1.0), but a slab's
    // four vertical edges collapsed to zero-thickness lines before the
    // direction normalization fix.
    const OutlineMeshData full = build_outline_mesh({cube(0, 0, 0, 1, 1, 1)}, 0.1f);
    const OutlineMeshData half = build_outline_mesh({cube(0, 0, 0, 1, 0.5f, 1)}, 0.1f);
    CHECK(all_triangles_nondegenerate(full));
    CHECK(all_triangles_nondegenerate(half));
    // A slab still has the same 12 unique edges as the cube it is half of.
    CHECK(half.verts.size() == full.verts.size());
    CHECK(half.indices.size() == full.indices.size());
}

TEST_CASE("block outline: thickness scales the extrusion") {
    const OutlineMeshData thin = build_outline_mesh({cube(0, 0, 0, 1, 1, 1)}, 0.1f);
    const OutlineMeshData thick = build_outline_mesh({cube(0, 0, 0, 1, 1, 1)}, 0.5f);
    CHECK(thin.verts.size() == thick.verts.size());

    // The cross-section half-extent is thickness*0.01, so the thicker outline
    // must poke further past the [0,1] box.
    float min_c = std::numeric_limits<float>::max();
    float max_c = std::numeric_limits<float>::lowest();
    for (size_t i = 0; i < thin.verts.size(); ++i) {
        min_c = std::min(min_c, thin.verts[i]);
        max_c = std::max(max_c, thin.verts[i]);
    }
    float min_t = std::numeric_limits<float>::max();
    float max_t = std::numeric_limits<float>::lowest();
    for (size_t i = 0; i < thick.verts.size(); ++i) {
        min_t = std::min(min_t, thick.verts[i]);
        max_t = std::max(max_t, thick.verts[i]);
    }
    CHECK(std::abs((max_t - min_t) - (max_c - min_c)) > 1e-4f);
}

TEST_CASE("block outline: fill bounds are the union box") {
    float center[3];
    float size[3];
    outline_fill_bounds({cube(0, 0, 0, 1, 1, 1), cube(1, 0, 0, 2, 0.5f, 1)}, center, size);
    CHECK(center[0] == doctest::Approx(1.0f));
    CHECK(center[1] == doctest::Approx(0.5f));
    CHECK(center[2] == doctest::Approx(0.5f));
    CHECK(size[0] == doctest::Approx(2.0f));
    CHECK(size[1] == doctest::Approx(1.0f));
    CHECK(size[2] == doctest::Approx(1.0f));
}