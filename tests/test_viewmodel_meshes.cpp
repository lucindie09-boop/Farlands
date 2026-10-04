#include "core/viewmodel_meshes.hpp"

#include <algorithm>
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
struct F3 {
    float x, y, z;
};

F3 vertex_at(const MeshGeometry& g, int i) {
    return {g.verts[i * 3], g.verts[i * 3 + 1], g.verts[i * 3 + 2]};
}

F3 subtract(F3 a, F3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}

F3 cross(F3 a, F3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

float dot(F3 a, F3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

F3 normalized(F3 v) {
    const float length = std::sqrt(dot(v, v));
    return {v.x / length, v.y / length, v.z / length};
}

// Every quad of a box mesh has to agree on both of these, per box:
// - the cross product of its first three corners (the winding push_quad emits
//   as triangle 0-1-2) points INTO the box, which is the side Godot's
//   clockwise-front culling keeps visible. Mirroring one coordinate of an
//   otherwise correct corner order reverses that cross, which is how the +X
//   face of build_box_mesh ended up with a hole in it.
// - the declared vertex normal faces OUT of the box, or the wall is lit as if
//   it faced into the block.
void check_quads_face_outward(const MeshGeometry& g, int first_quad, int quad_count, F3 box_centre) {
    for (int q = 0; q < quad_count; ++q) {
        const int quad = first_quad + q;
        const F3 v0 = vertex_at(g, quad * 4 + 0);
        const F3 v1 = vertex_at(g, quad * 4 + 1);
        const F3 v2 = vertex_at(g, quad * 4 + 2);
        const F3 v3 = vertex_at(g, quad * 4 + 3);
        const F3 quad_centre{ (v0.x + v1.x + v2.x + v3.x) / 4.0f, (v0.y + v1.y + v2.y + v3.y) / 4.0f,
                              (v0.z + v1.z + v2.z + v3.z) / 4.0f };
        const F3 outward = normalized(subtract(quad_centre, box_centre));
        const F3 winding = normalized(cross(subtract(v1, v0), subtract(v2, v0)));
        CHECK(dot(winding, outward) <= doctest::Approx(-1.0f).epsilon(0.001));
        const F3 normal{g.normals[quad * 4 * 3 + 0], g.normals[quad * 4 * 3 + 1], g.normals[quad * 4 * 3 + 2]};
        CHECK(dot(normal, outward) >= doctest::Approx(1.0f).epsilon(0.001));
    }
}

bool index_stream_is_sequential(const MeshGeometry& g) {
    for (size_t i = 0; i < g.indices.size(); ++i) {
        const int32_t expected = static_cast<int32_t>((i / 6) * 4 + (i % 6 == 5 ? 3 : (i % 6 == 4 ? 2 : (i % 6 == 3 ? 0 : i % 6))));
        if (g.indices[i] != expected) {
            return false;
        }
    }
    return true;
}

// The (u,v) rectangle each face has to sample: the box's own extent along the
// face's horizontal and vertical axes. A face covering the whole of both axes is
// the plain full-cube case (u and v are the full 0..1 texture); a stair step or
// a slab covers fractions of the cell, and its UV rectangle must be the same
// fractions -- anything else is the texture stretched over the box.
struct UvSlice {
    float u;
    float v;
};

UvSlice uv_slice_for_face(int face, const float box[6]) {
    const float extent_x = box[3] - box[0];
    const float extent_y = box[4] - box[1];
    const float extent_z = box[5] - box[2];
    if (face == 0 || face == 1) {
        return {extent_z, extent_y}; // +X / -X: u runs along z, v down y
    }
    if (face == 2 || face == 3) {
        return {extent_x, extent_z}; // +Y / -Y: u along x, v along z
    }
    return {extent_x, extent_y}; // +Z / -Z: u along x, v down y
}

void check_face_uv_is_sliced(const MeshGeometry& g, int box_index, const float box[6]) {
    for (int face = 0; face < 6; ++face) {
        const int quad = box_index * 6 + face;
        float u_min = 2.0f;
        float u_max = -1.0f;
        float v_min = 2.0f;
        float v_max = -1.0f;
        for (int i = 0; i < 4; ++i) {
            const float u = g.uvs[(quad * 4 + i) * 2 + 0];
            const float v = g.uvs[(quad * 4 + i) * 2 + 1];
            u_min = std::min(u_min, u);
            u_max = std::max(u_max, u);
            v_min = std::min(v_min, v);
            v_max = std::max(v_max, v);
        }
        const UvSlice slice = uv_slice_for_face(face, box);
        CHECK(u_max - u_min == doctest::Approx(slice.u).epsilon(0.001));
        CHECK(v_max - v_min == doctest::Approx(slice.v).epsilon(0.001));
        // The slice has to stay inside the cell it was cut from.
        CHECK(u_min >= -0.001f);
        CHECK(u_max <= 1.001f);
        CHECK(v_min >= -0.001f);
        CHECK(v_max <= 1.001f);
    }
}

// Every side face (+X, -X, -Z, +Z) of one box must show the v band its own y
// extent covers, so a slab shows half the texture rather than all of it.
void check_side_faces_cover_v(const MeshGeometry& g, int box_index, float v_lo, float v_hi) {
    const int side_faces[4] = {0, 1, 4, 5};
    for (int k = 0; k < 4; ++k) {
        const int quad = box_index * 6 + side_faces[k];
        float v_min = 2.0f;
        float v_max = -1.0f;
        for (int i = 0; i < 4; ++i) {
            const float v = g.uvs[(quad * 4 + i) * 2 + 1];
            v_min = std::min(v_min, v);
            v_max = std::max(v_max, v);
        }
        CHECK(v_min == doctest::Approx(v_lo).epsilon(0.001));
        CHECK(v_max == doctest::Approx(v_hi).epsilon(0.001));
    }
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

TEST_CASE("viewmodel cube: all six quads wind inward and face out") {
    check_quads_face_outward(build_unit_cube_mesh(), 0, 6, {0.0f, 0.0f, 0.0f});
}

TEST_CASE("viewmodel shaped mesh: every box quads wind inward and face out") {
    // A stair-like pair, the smallest shape with two boxes. Box centres are the
    // box's own middle in centred coordinates (each box is shifted by -0.5).
    std::vector<float> boxes = {
            0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f, // bottom slab
            0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 1.0f, // upper step
    };
    const MeshGeometry g = build_box_mesh(boxes);
    check_quads_face_outward(g, 0, 6, {0.0f, -0.25f, 0.0f});
    check_quads_face_outward(g, 6, 6, {-0.25f, 0.25f, 0.0f});
}

TEST_CASE("viewmodel shaped mesh: each face samples its own slice of the texture cell") {
    const float slab[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f};
    check_face_uv_is_sliced(build_box_mesh({0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f}), 0, slab);

    // A stair's pair of boxes: the full-footprint lower step and the half-depth
    // upper step -- the shape whose squashed sides/pieces the old full-cell UVs
    // stretched worst.
    const MeshGeometry stair = build_box_mesh({
            0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f,
            0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f,
    });
    const float lower[6] = {0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f};
    const float upper[6] = {0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f};
    check_face_uv_is_sliced(stair, 0, lower);
    check_face_uv_is_sliced(stair, 1, upper);
}

TEST_CASE("viewmodel shaped mesh: slab and stair sides sample the matching texture half") {
    // A bottom slab covers y 0..0.5, so its sides show the bottom half of the
    // cell (v 0.5..1), the slice a placed bottom slab shows, instead of the
    // whole texture pressed into the half-height side.
    check_side_faces_cover_v(build_box_mesh({0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f}), 0, 0.5f, 1.0f);
    // A top slab covers y 0.5..1: the top half instead.
    check_side_faces_cover_v(build_box_mesh({0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f}), 0, 0.0f, 0.5f);

    // A stair's upper step (the north variant: z 0..0.5, y 0.5..1). Its sides
    // are the texture's top half, and its top face -- which spans only half the
    // cell's z axis -- samples the matching half of the v axis: v = 0.5 at the
    // north edge (z = 0) down to v = 0 at the cut edge (z = 0.5). That is the
    // placed stair's own stretch-free mapping (the world's Top face uses
    // offset_v = min_z, mesh_builder_faces_aabb.cpp).
    const MeshGeometry step = build_box_mesh({0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f});
    check_side_faces_cover_v(step, 0, 0.0f, 0.5f);
    const int top_quad = 2;
    for (int i = 0; i < 4; ++i) {
        const float z = step.verts[(top_quad * 4 + i) * 3 + 2] + 0.5f; // back to 0..1
        const float v = step.uvs[(top_quad * 4 + i) * 2 + 1];
        CHECK(v == doctest::Approx(0.5f - z).epsilon(0.001));
    }
}

TEST_CASE("viewmodel sprite: 1x1 solid texel has front+back plus all four rims") {
    const uint8_t rgba[4] = {255, 0, 0, 255};
    const MeshGeometry g = build_sprite_mesh(rgba, 1, 1);
    // A lone texel is surrounded by air, so it gets front + back + 4 rims
    // (mirroring the GDScript: `is_solid` returns false out of bounds).
    CHECK(vert_count(g) == 24);
    CHECK(index_count(g) == 36);
    CHECK(static_cast<int>(g.uvs.size() / 2) == 24);
    // Front and back faces are pinned to the texel centre (like the rims): a
    // quad samples only its own texel, so nearest filtering can never round
    // into a neighbouring texel at the sprite's silhouette — the fix for the
    // hairline see-through on held items (the alpha scissor can no longer
    // discard an edge fragment).
    CHECK(g.uvs[0] == doctest::Approx(0.5f));
    CHECK(g.uvs[1] == doctest::Approx(0.5f));
    CHECK(g.uvs[6] == doctest::Approx(0.5f));
    CHECK(g.uvs[7] == doctest::Approx(0.5f));
    // Back face (verts 4-7, uvs 8-15) is centred too.
    CHECK(g.uvs[8] == doctest::Approx(0.5f));
    CHECK(g.uvs[9] == doctest::Approx(0.5f));
    CHECK(g.uvs[14] == doctest::Approx(0.5f));
    CHECK(g.uvs[15] == doctest::Approx(0.5f));
    // The original GDScript used Vector3.BACK (0,0,+1, toward the viewer) on
    // the front quad and Vector3.FORWARD (0,0,-1) on the back quad; the mesh is
    // per-pixel lit, so these normals must stay outward-facing.
    CHECK(g.normals[0] == doctest::Approx(0.0f));
    CHECK(g.normals[2] == doctest::Approx(1.0f));
    CHECK(g.normals[4 * 3 + 2] == doctest::Approx(-1.0f));
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