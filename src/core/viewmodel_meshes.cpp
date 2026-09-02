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
        const float min_x = boxes[bi + 0] - 0.5f;
        const float min_y = boxes[bi + 1] - 0.5f;
        const float min_z = boxes[bi + 2] - 0.5f;
        const float max_x = boxes[bi + 3] - 0.5f;
        const float max_y = boxes[bi + 4] - 0.5f;
        const float max_z = boxes[bi + 5] - 0.5f;

        const float faces[6][4][3] = {
                // +X
                {{max_x, min_y, min_z}, {max_x, max_y, min_z}, {max_x, max_y, max_z}, {max_x, min_y, max_z}},
                // -X
                {{min_x, min_y, min_z}, {min_x, max_y, min_z}, {min_x, max_y, max_z}, {min_x, min_y, max_z}},
                // +Y
                {{min_x, max_y, min_z}, {max_x, max_y, min_z}, {max_x, max_y, max_z}, {min_x, max_y, max_z}},
                // -Y
                {{min_x, min_y, max_z}, {max_x, min_y, max_z}, {max_x, min_y, min_z}, {min_x, min_y, min_z}},
                // +Z
                {{max_x, min_y, min_z}, {max_x, max_y, min_z}, {min_x, max_y, min_z}, {min_x, min_y, min_z}},
                // -Z
                {{min_x, min_y, max_z}, {min_x, max_y, max_z}, {max_x, max_y, max_z}, {max_x, min_y, max_z}},
        };
        const float normals[6][3] = {
                {1.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f},
                {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f},
        };
        static const float uv[4][2] = {{0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}};
        for (int f = 0; f < 6; ++f) {
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

    const float back[3] = {0.0f, 0.0f, -1.0f};
    const float fwd[3] = {0.0f, 0.0f, 1.0f};
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

            const float front[4][3] = {
                    {px - hw, py - hw, pz_front}, {px - hw, py + hw, pz_front},
                    {px + hw, py + hw, pz_front}, {px + hw, py - hw, pz_front},
            };
            const float front_uv[4][2] = {
                    {u, v + texel_size}, {u, v}, {u + texel_size, v}, {u + texel_size, v + texel_size},
            };
            push_quad(g, front, back, front_uv);

            const float back_verts[4][3] = {
                    {px + hw, py - hw, pz_back}, {px + hw, py + hw, pz_back},
                    {px - hw, py + hw, pz_back}, {px - hw, py - hw, pz_back},
            };
            const float back_uv[4][2] = {
                    {u + texel_size, v + texel_size}, {u + texel_size, v}, {u, v}, {u, v + texel_size},
            };
            push_quad(g, back_verts, fwd, back_uv);

            const float center_uv[2] = {u + texel_size / 2.0f, v + texel_size / 2.0f};
            const float center_uvs[4][2] = {
                    {center_uv[0], center_uv[1]}, {center_uv[0], center_uv[1]},
                    {center_uv[0], center_uv[1]}, {center_uv[0], center_uv[1]},
            };

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