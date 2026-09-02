#include "core/viewmodel_meshes.hpp"

#include <cmath>
#include <cstdint>
#include <vector>

#include "doctest.h"

using namespace VoxelEngine;

namespace {
int vert_count(const MeshGeometry& g) {
    return static_cast<int>(g.verts.size() / 3);
}
int index_count(const MeshGeometry& g) {
    return static_cast<int>(g.indices.size());
}

// Sum of |index delta| chains: the index stream always runs [k, k+1, k+2, k,
// k+2, k+3] per quad, so a well-formed mesh's even/odd positions take the
// expected pattern. Looser sanity: indices must stay in bounds and cover each
// vertex exactly as the push_quad pattern dictates (each quad adds exactly 4
// new verts and 6 indices, sequentially).
bool index_stream_is_sequential(const MeshGeometry& g) {
    for (size_t i = 0; i < g.indices.size(); ++i) {
        const int32_t expected = static_cast<int32_t>((i / 6) * 4 + (i % 6 == 5 ? 3 : (i % 6 == 4 ? 2 : (i % 6 == 3 ? 0 : i % 6))));
        if (g.indices[i] != expected) {
            return false;
        }
    }
    return true;
}
} // namespace

TEST_CASE("viewmodel cube: 24 verts / 36 indices with per-face normals and UVs") {
    const MeshGeometry g = build_unit_cube_mesh();
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(static_cast<int>(g.uvs.size() / 2) == 24);
    CHECK(static_cast<int>(g.normals.size() / 3) == 24);
    CHECK(index_stream_is_sequential(g));

    // Each face contributes 4 sequential verts sharing one axis-aligned unit normal.
    for (int f = 0; f < 6; ++f) {
        const float nx = g.normals[f * 4 * 3 + 0];
        const float ny = g.normals[f * 4 * 3 + 1];
        const float nz = g.normals[f * 4 * 3 + 2];
        int axis_components = 0;
        if (std::abs(nx) > 0.5f) ++axis_components;
        if (std::abs(ny) > 0.5f) ++axis_components;
        if (std::abs(nz) > 0.5f) ++axis_components;
        CHECK(axis_components == 1);
        for (int v = 1; v < 4; ++v) {
            CHECK(g.normals[f * 4 * 3 + v * 3 + 0] == nx);
            CHECK(g.normals[f * 4 * 3 + v * 3 + 1] == ny);
            CHECK(g.normals[f * 4 * 3 + v * 3 + 2] == nz);
        }
    }

    // Top face (+Y, face index 2) texture rows are up: UVs are (0,1)(0,0)(1,0)(1,1).
    const int top = 2;
    CHECK(g.uvs[top * 4 * 2 + 0] == doctest::Approx(0.0f));
    CHECK(g.uvs[top * 4 * 2 + 1] == doctest::Approx(1.0f));
    CHECK(g.uvs[top * 4 * 2 + 2] == doctest::Approx(0.0f));
    CHECK(g.uvs[top * 4 * 2 + 3] == doctest::Approx(0.0f));
    CHECK(g.uvs[top * 4 * 2 + 4] == doctest::Approx(1.0f));
    CHECK(g.uvs[top * 4 * 2 + 5] == doctest::Approx(0.0f));
    CHECK(g.uvs[top * 4 * 2 + 6] == doctest::Approx(1.0f));
    CHECK(g.uvs[top * 4 * 2 + 7] == doctest::Approx(1.0f));

    // Vertices live inside the unit cube.
    for (int i = 0; i < 24; ++i) {
        CHECK(std::abs(g.verts[i * 3 + 0]) <= 0.5f + 1e-6f);
        CHECK(std::abs(g.verts[i * 3 + 1]) <= 0.5f + 1e-6f);
        CHECK(std::abs(g.verts[i * 3 + 2]) <= 0.5f + 1e-6f);
    }
}

TEST_CASE("viewmodel shaped mesh: slab selection box builds a 0.5-high lean cube") {
    // One bottom slab box: min (0,0,0) max (1,0.5,1) -> centred, y max sits at 0.
    std::vector<float> boxes = {0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f};
    const MeshGeometry g = build_box_mesh(boxes);
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(index_stream_is_sequential(g));

    bool saw_y_positive = false;
    for (int i = 0; i < 24; ++i) {
        const float y = g.verts[i * 3 + 1];
        if (y > 1e-6f) saw_y_positive = true;
        CHECK(std::abs(g.verts[i * 3 + 0]) <= 0.5f + 1e-6f);
        CHECK(y <= 1e-6f);
        CHECK(std::abs(g.verts[i * 3 + 2]) <= 0.5f + 1e-6f);
    }
    CHECK(!saw_y_positive);
}

TEST_CASE("viewmodel sprite: 1x1 solid texel has front+back plus all four rims") {
    const uint8_t rgba[4] = {255, 0, 0, 255};
    const MeshGeometry g = build_sprite_mesh(rgba, 1, 1);
    // A lone texel is surrounded by air, so it gets front + back + 4 rims
    // (mirroring the GDScript: `is_solid` returns false out of bounds).
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(static_cast<int>(g.uvs.size() / 2) == 24);
    // Single pixel spans the whole sprite, so front-face UVs are the full rect.
    CHECK(g.uvs[0] == doctest::Approx(0.0f));
    CHECK(g.uvs[1] == doctest::Approx(1.0f));
    CHECK(g.uvs[6] == doctest::Approx(1.0f));
    CHECK(g.uvs[7] == doctest::Approx(1.0f));
    // Front normal points away from the viewer (-Z), back +Z.
    CHECK(g.normals[0] == doctest::Approx(0.0f));
    CHECK(g.normals[2] == doctest::Approx(-1.0f));
    CHECK(g.normals[4 * 3 + 2] == doctest::Approx(1.0f));
    // Extrusion depth 0.05 -> front z +0.025, back z -0.025.
    CHECK(std::abs(g.verts[6]) - 0.025f < 1e-6f);
    CHECK(std::abs(g.verts[4 * 3 + 2] + 0.025f) < 1e-6f);
    // All four rim normals appear (top/bottom/right/left).
    bool saw_y_up = false;
    bool saw_y_down = false;
    for (int i = 0; i < 24; ++i) {
        const float ny = g.normals[i * 3 + 1];
        if (ny > 0.5f) saw_y_up = true;
        if (ny < -0.5f) saw_y_down = true;
    }
    CHECK(saw_y_up);
    CHECK(saw_y_down);
}

TEST_CASE("viewmodel sprite: corner pixel adds all four silhouette rims") {
    // 2x2 image with only the top-left texel solid: rims on top (OOB), bottom,
    // right, and left (OOB).
    const uint8_t rgba[16] = {
            255, 255, 255, 255, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0,
    };
    const MeshGeometry g = build_sprite_mesh(rgba, 2, 2);
    // 1 pixel * (front + back + 4 rims) = 6 quads = 24 verts / 36 indices.
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(index_stream_is_sequential(g));

    // Quad order: front(0-3), back(4-7), top(8-11), bottom(12-15), right(16-19), left(20-23).
    CHECK(g.normals[8 * 3 + 1] == doctest::Approx(1.0f)); // top rim +Y
    CHECK(g.normals[12 * 3 + 1] == doctest::Approx(-1.0f)); // bottom rim -Y
    CHECK(g.normals[16 * 3 + 0] == doctest::Approx(1.0f)); // right rim +X
    CHECK(g.normals[20 * 3 + 0] == doctest::Approx(-1.0f)); // left rim -X
    // Rim UVs degenerate to the texel centre.
    const float cu = 0.25f;
    const float cv = 0.25f;
    CHECK(g.uvs[8 * 2 + 0] == doctest::Approx(cu));
    CHECK(g.uvs[8 * 2 + 1] == doctest::Approx(cv));
    CHECK(g.uvs[12 * 2 + 0] == doctest::Approx(cu));
    CHECK(g.uvs[12 * 2 + 1] == doctest::Approx(cv));
}

TEST_CASE("viewmodel sprite: isolated centre pixel in 3x3 gains all four rims") {
    const uint8_t rgba[36] = {
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
            0, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0, 0,
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    };
    const MeshGeometry g = build_sprite_mesh(rgba, 3, 3);
    // 1 pixel * (front + back + 4 rims) = 6 quads = 24 verts / 36 indices.
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(index_stream_is_sequential(g));
    // Both horizontal rim normals present.
    bool saw_right = false;
    bool saw_left = false;
    for (int i = 0; i < 24; ++i) {
        const float nx = g.normals[i * 3 + 0];
        if (nx > 0.5f) saw_right = true;
        if (nx < -0.5f) saw_left = true;
    }
    CHECK(saw_right);
    CHECK(saw_left);
    // Vertices stay within the pixel's column band: centre y = (1.5 - 1)/3 =
    // 0.1667, rims reach +/- another half texel, so |y| <= texel.
    const float texel = 1.0f / 3.0f;
    for (int i = 0; i < 24; ++i) {
        CHECK(std::abs(g.verts[i * 3 + 1]) <= texel + 1e-6f);
    }
}