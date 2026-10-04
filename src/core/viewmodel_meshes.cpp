#include "core/viewmodel_meshes.hpp"

#include <cmath>

namespace VoxelEngine {

namespace {

constexpr float kExtrusionDepth = 0.05f;
constexpr float kHalfDepth = kExtrusionDepth / 2.0f;

inline void push_quad(MeshGeometry& g, const float v[4][3], const float n[3], const float uv[4][2]) {
    const int32_t base = static_cast<int32_t>(g.verts.size() / 3);
    for (int i = 0; i < 4; ++i) {
        g.verts.push_back(v[i][0]);
        g.verts.push_back(v[i][1]);
        g.verts.push_back(v[i][2]);
        g.normals.push_back(n[0]);
        g.normals.push_back(n[1]);
        g.normals.push_back(n[2]);
        g.uvs.push_back(uv[i][0]);
        g.uvs.push_back(uv[i][1]);
    }
    g.indices.push_back(base + 0);
    g.indices.push_back(base + 1);
    g.indices.push_back(base + 2);
    g.indices.push_back(base + 0);
    g.indices.push_back(base + 2);
    g.indices.push_back(base + 3);
}

struct Box {
    float x0, y0, z0, x1, y1, z1;
};

} // namespace

MeshGeometry build_unit_cube_mesh() {
    static const float kFaceDefs[6][4][3] = {
            // +X
            {{0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}},
            // -X
            {{-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}},
            // +Y
            {{-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
            // -Y
            {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}},
            // +Z
            {{-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}},
            // -Z
            {{0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}},
    };
    static const float kFaceNormals[6][3] = {
            {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
            {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
    };
    static const float kFaceUvs[4][2] = {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};

    MeshGeometry g;
    for (int f = 0; f < 6; ++f) {
        push_quad(g, kFaceDefs[f], kFaceNormals[f], kFaceUvs);
    }
    return g;
}

MeshGeometry build_box_mesh(const std::vector<float>& boxes) {
    MeshGeometry g;
    for (size_t bi = 0; bi + 5 < boxes.size(); bi += 6) {
        // Block-space extents, kept before the -0.5 centring below: a face's
        // texture slice is measured in the 0..1 cell the box occupies, so the
        // formulas need the box's own min/max, not the centred mesh's.
        const float bz0 = boxes[bi + 2];
        const float bz1 = boxes[bi + 5];
        const float bx0 = boxes[bi + 0];
        const float bx1 = boxes[bi + 3];

        const float min_x = bx0 - 0.5f;
        const float min_y = boxes[bi + 1] - 0.5f;
        const float min_z = bz0 - 0.5f;
        const float max_x = bx1 - 0.5f;
        const float max_y = boxes[bi + 4] - 0.5f;
        const float max_z = bz1 - 0.5f;

        // Every quad is wound so the cross product of its first three corners
        // points INTO the box, which is the clockwise-front winding
        // build_unit_cube_mesh uses (Godot culls the other side). A pair of
        // opposite faces cannot share a corner order: mirroring the x or y
        // coordinate reverses the cross, so each side gets its own order. The
        // +X face used to be a straight copy of -X and was back-facing -- a
        // hole in the right-hand side of every dropped/held shaped mesh.
        const float faces[6][4][3] = {
                // +X
                {{max_x, min_y, max_z}, {max_x, max_y, max_z}, {max_x, max_y, min_z}, {max_x, min_y, min_z}},
                // -X
                {{min_x, min_y, min_z}, {min_x, max_y, min_z}, {min_x, max_y, max_z}, {min_x, min_y, max_z}},
                // +Y
                {{min_x, max_y, min_z}, {max_x, max_y, min_z}, {max_x, max_y, max_z}, {min_x, max_y, max_z}},
                // -Y
                {{min_x, min_y, max_z}, {max_x, min_y, max_z}, {max_x, min_y, min_z}, {min_x, min_y, min_z}},
                // -Z (the min_z wall)
                {{max_x, min_y, min_z}, {max_x, max_y, min_z}, {min_x, max_y, min_z}, {min_x, min_y, min_z}},
                // +Z (the max_z wall)
                {{min_x, min_y, max_z}, {min_x, max_y, max_z}, {max_x, max_y, max_z}, {max_x, min_y, max_z}},
        };
        // Each normal names the wall its quad sits on (the -Z wall is min_z,
        // the +Z wall is max_z, as in build_unit_cube_mesh). The two Z entries
        // used to be swapped, which lit both of those walls as if they faced
        // into the block.
        const float normals[6][3] = {
                {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f},
        };
        // UVs are derived from each corner's own 0..1 block position, so every
        // face samples the slice of the texture its box covers -- the way a
        // placed slab or stair does -- instead of the whole cell, which used to
        // stretch a full copy of the texture over each half-height step. These
        // formulas are the partial-block walk in mesh/mesh_builder_faces_aabb.cpp
        // written position-wise: u along the face's horizontal axis, v down
        // from the block top (the world's Top/Bottom/Left/Front offsets are
        // folded into the expressions, and its Right/Back faces are the plain
        // cases). The old constant table got away with the same values on both
        // axes of every face only because a full box covers the whole cell.
        for (int f = 0; f < 6; ++f) {
            float uv[4][2];
            for (int i = 0; i < 4; ++i) {
                // Block-space position of this corner (undo the centring).
                const float x = faces[f][i][0] + 0.5f;
                const float y = faces[f][i][1] + 0.5f;
                const float z = faces[f][i][2] + 0.5f;
                switch (f) {
                    case 0: // +X, the world's Right face
                        uv[i][0] = z;
                        uv[i][1] = 1.0f - y;
                        break;
                    case 1: // -X, the world's Left face: u runs from the far side
                        uv[i][0] = z + (1.0f - bz0 - bz1);
                        uv[i][1] = 1.0f - y;
                        break;
                    case 2: // +Y, the world's Top face
                        uv[i][0] = x;
                        uv[i][1] = bz0 + bz1 - z;
                        break;
                    case 3: // -Y, the world's Bottom face
                        uv[i][0] = x;
                        uv[i][1] = 1.0f - z;
                        break;
                    case 4: // -Z (min_z wall), the world's Back face
                        uv[i][0] = x;
                        uv[i][1] = 1.0f - y;
                        break;
                    default: // +Z (max_z wall), the world's Front face
                        uv[i][0] = bx0 + bx1 - x;
                        uv[i][1] = 1.0f - y;
                        break;
                }
            }
            push_quad(g, faces[f], normals[f], uv);
        }
    }
    return g;
}

MeshGeometry build_sprite_mesh(const uint8_t* rgba, int width, int height) {
    MeshGeometry g;
    if (rgba == nullptr || width <= 0 || height <= 0) {
        return g;
    }
    const float texel_size = 1.0f / static_cast<float>(width > height ? width : height);
    const float pz_front = kHalfDepth;
    const float pz_back = -kHalfDepth;
    const float w_half = static_cast<float>(width) / 2.0f;
    const float h_half = static_cast<float>(height) / 2.0f;

    auto solid = [&](int px, int py) -> bool {
        if (px < 0 || px >= width || py < 0 || py >= height) {
            return false;
        }
        return rgba[(py * width + px) * 4 + 3] > 0;
    };

    // Godot semantics: BACK = (0,0,+1) faces the viewer, FORWARD = (0,0,-1)
    // faces away. The original GDScript used Vector3.BACK on the front quad and
    // Vector3.FORWARD on the back quad; keep those exact normals.
    const float back[3] = {0.0f, 0.0f, 1.0f};
    const float fwd[3] = {0.0f, 0.0f, -1.0f};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    const float down[3] = {0.0f, -1.0f, 0.0f};
    const float right[3] = {1.0f, 0.0f, 0.0f};
    const float left[3] = {-1.0f, 0.0f, 0.0f};

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (!solid(x, y)) {
                continue;
            }
            const float u = static_cast<float>(x) / static_cast<float>(width);
            const float v = static_cast<float>(y) / static_cast<float>(height);
            const float px = (static_cast<float>(x) - w_half) * texel_size;
            const float py = (h_half - static_cast<float>(y)) * texel_size;
            const float hw = texel_size / 2.0f;

            // Every quad (front, back, and rims) samples its OWN texel's
            // centre. A quad's footprint IS the texel's cell, so under nearest
            // filtering the result is identical to a full-rect UV — but a
            // full-rect UV sits exactly on texel boundaries, where nearest
            // sampling can round into a neighbouring (possibly transparent)
            // texel and the alpha scissor discards the fragment: hairline
            // see-through at the sprite's silhouette. Centre-pinning makes
            // boundary rounding impossible.
            const float center_uv[2] = {u + texel_size / 2.0f, v + texel_size / 2.0f};
            const float center_uvs[4][2] = {
                    {center_uv[0], center_uv[1]}, {center_uv[0], center_uv[1]},
                    {center_uv[0], center_uv[1]}, {center_uv[0], center_uv[1]},
            };

            const float front[4][3] = {
                    {px - hw, py - hw, pz_front}, {px - hw, py + hw, pz_front},
                    {px + hw, py + hw, pz_front}, {px + hw, py - hw, pz_front},
            };
            push_quad(g, front, back, center_uvs);

            const float back_verts[4][3] = {
                    {px + hw, py - hw, pz_back}, {px + hw, py + hw, pz_back},
                    {px - hw, py + hw, pz_back}, {px - hw, py - hw, pz_back},
            };
            push_quad(g, back_verts, fwd, center_uvs);

            if (!solid(x, y - 1)) {
                const float q[4][3] = {
                        {px - hw, py + hw, pz_front}, {px - hw, py + hw, pz_back},
                        {px + hw, py + hw, pz_back}, {px + hw, py + hw, pz_front},
                };
                push_quad(g, q, up, center_uvs);
            }
            if (!solid(x, y + 1)) {
                const float q[4][3] = {
                        {px - hw, py - hw, pz_back}, {px - hw, py - hw, pz_front},
                        {px + hw, py - hw, pz_front}, {px + hw, py - hw, pz_back},
                };
                push_quad(g, q, down, center_uvs);
            }
            if (!solid(x + 1, y)) {
                const float q[4][3] = {
                        {px + hw, py - hw, pz_front}, {px + hw, py + hw, pz_front},
                        {px + hw, py + hw, pz_back}, {px + hw, py - hw, pz_back},
                };
                push_quad(g, q, right, center_uvs);
            }
            if (!solid(x - 1, y)) {
                const float q[4][3] = {
                        {px - hw, py - hw, pz_back}, {px - hw, py + hw, pz_back},
                        {px - hw, py + hw, pz_front}, {px - hw, py - hw, pz_front},
                };
                push_quad(g, q, left, center_uvs);
            }
        }
    }
    return g;
}

} // namespace VoxelEngine