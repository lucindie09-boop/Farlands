#include "render/block_outline_mesh.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <set>
#include <string>

namespace VoxelEngine {

namespace {

constexpr float kEps = 0.0001f;
constexpr float kEdgeMinDistSq = 0.001f * 0.001f;

struct Rect {
    float u0, u1, v0, v1;
};

// Godot-style is_equal_approx (scaled relative tolerance). Only used to pick a
// canonical direction for the dedup key, so exact tolerance doesn't matter.
bool nearly_equal(float a, float b) {
    if (a == b) {
        return true;
    }
    const float scale = std::max(std::abs(a), std::abs(b));
    const float tol = scale > 1.0f ? 1e-5f * scale : 1e-5f;
    return std::abs(a - b) < tol;
}

// Edge accumulator mirroring the GDScript _add_outline_edge/_get_edge_perpendiculars.
class EdgeBuilder {
public:
    EdgeBuilder(float thickness)
        : half_(thickness * 0.01f) {}

    float get_half() const { return half_; }
    const std::vector<float>& verts() const { return verts_; }
    const std::vector<uint32_t>& indices() const { return indices_; }

    void add_edge(const float p0[3], const float p1[3]) {
        const float dx = p1[0] - p0[0];
        const float dy = p1[1] - p0[1];
        const float dz = p1[2] - p0[2];
        if (dx * dx + dy * dy + dz * dz < kEdgeMinDistSq) {
            return;
        }

        // Canonical ordering so the same geometric edge dedups regardless of
        // which face emitted it.
        const float* a = p0;
        const float* b = p1;
        if (p0[0] > p1[0] ||
            (nearly_equal(p0[0], p1[0]) && (p0[1] > p1[1] || (nearly_equal(p0[1], p1[1]) && p0[2] > p1[2])))) {
            a = p1;
            b = p0;
        }

        char key[160];
        std::snprintf(key, sizeof(key), "%.6f_%.6f_%.6f_%.6f_%.6f_%.6f",
                      a[0], a[1], a[2], b[0], b[1], b[2]);
        if (!edge_set_.insert(std::string(key)).second) {
            return;
        }

        // Cross-section perpendiculars around the edge direction.
        float r[3], u[3];
        get_perpendiculars(dx, dy, dz, r, u);

        const float p0r0 = p0[0] - r[0] * half_ - u[0] * half_;
        const float p0r1 = p0[1] - r[1] * half_ - u[1] * half_;
        const float p0r2 = p0[2] - r[2] * half_ - u[2] * half_;
        const float q0a0 = p0[0] + r[0] * half_ - u[0] * half_;
        const float q0a1 = p0[1] + r[1] * half_ - u[1] * half_;
        const float q0a2 = p0[2] + r[2] * half_ - u[2] * half_;
        const float q0b0 = p0[0] + r[0] * half_ + u[0] * half_;
        const float q0b1 = p0[1] + r[1] * half_ + u[1] * half_;
        const float q0b2 = p0[2] + r[2] * half_ + u[2] * half_;
        const float p0r2b0 = p0[0] - r[0] * half_ + u[0] * half_;
        const float p0r2b1 = p0[1] - r[1] * half_ + u[1] * half_;
        const float p0r2b2 = p0[2] - r[2] * half_ + u[2] * half_;

        const float p1r0 = p1[0] - r[0] * half_ - u[0] * half_;
        const float p1r1 = p1[1] - r[1] * half_ - u[1] * half_;
        const float p1r2 = p1[2] - r[2] * half_ - u[2] * half_;
        const float q1a0 = p1[0] + r[0] * half_ - u[0] * half_;
        const float q1a1 = p1[1] + r[1] * half_ - u[1] * half_;
        const float q1a2 = p1[2] + r[2] * half_ - u[2] * half_;
        const float q1b0 = p1[0] + r[0] * half_ + u[0] * half_;
        const float q1b1 = p1[1] + r[1] * half_ + u[1] * half_;
        const float q1b2 = p1[2] + r[2] * half_ + u[2] * half_;
        const float p1r2b0 = p1[0] - r[0] * half_ + u[0] * half_;
        const float p1r2b1 = p1[1] - r[1] * half_ + u[1] * half_;
        const float p1r2b2 = p1[2] - r[2] * half_ + u[2] * half_;

        const float corners[8][3] = {
            {p0r0, p0r1, p0r2},
            {q0a0, q0a1, q0a2},
            {q0b0, q0b1, q0b2},
            {p0r2b0, p0r2b1, p0r2b2},
            {p1r0, p1r1, p1r2},
            {q1a0, q1a1, q1a2},
            {q1b0, q1b1, q1b2},
            {p1r2b0, p1r2b1, p1r2b2},
        };
        const uint32_t base = static_cast<uint32_t>(verts_.size()) / 3;
        for (const auto& corner : corners) {
            verts_.push_back(corner[0]);
            verts_.push_back(corner[1]);
            verts_.push_back(corner[2]);
        }

        static const uint32_t kQuadTris[8][3] = {
            {0, 4, 7}, {0, 7, 3},
            {1, 2, 6}, {1, 6, 5},
            {0, 5, 4}, {0, 1, 5},
            {3, 7, 6}, {3, 6, 2},
        };
        for (const auto& tri : kQuadTris) {
            indices_.push_back(base + tri[0]);
            indices_.push_back(base + tri[1]);
            indices_.push_back(base + tri[2]);
        }
    }

    void emit_face_edges(int axis, int u, int v, float fp, const Rect& r) {
        float corners[4][3] = {};
        for (int ci = 0; ci < 4; ++ci) {
            corners[ci][axis] = fp;
            corners[ci][u] = (ci < 2) ? r.u0 : r.u1;
            corners[ci][v] = (ci == 0 || ci == 3) ? r.v0 : r.v1;
        }
        for (int ci = 0; ci < 4; ++ci) {
            add_edge(corners[ci], corners[(ci + 1) & 3]);
        }
    }

private:
    static void get_perpendiculars(float x, float y, float z, float right[3], float up[3]) {
        // Cross-section basis around the direction (x,y,z).
        float upx = 0.0f, upy = 1.0f, upz = 0.0f;
        if (std::abs(y) > 0.99f) {
            upx = 1.0f;
            upy = 0.0f;
            upz = 0.0f;
        }
        // right = dir x up
        right[0] = y * upz - z * upy;
        right[1] = z * upx - x * upz;
        right[2] = x * upy - y * upx;
        const float rlen = std::sqrt(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
        if (rlen > 1e-9f) {
            right[0] /= rlen;
            right[1] /= rlen;
            right[2] /= rlen;
        }
        // up = dir x right
        up[0] = y * right[2] - z * right[1];
        up[1] = z * right[0] - x * right[2];
        up[2] = x * right[1] - y * right[0];
        const float ulen = std::sqrt(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
        if (ulen > 1e-9f) {
            up[0] /= ulen;
            up[1] /= ulen;
            up[2] /= ulen;
        }
    }

    float half_;
    std::set<std::string> edge_set_;
    std::vector<float> verts_;
    std::vector<uint32_t> indices_;
};

// XOR-culled perimeter emission for a single axis plane (see build_outline_mesh).
void emit_xor_perimeter(int axis, int u, int v, float fp,
                        const std::vector<Rect>& pos_rects, const std::vector<Rect>& neg_rects,
                        EdgeBuilder& builder) {
    const size_t total = pos_rects.size() + neg_rects.size();
    if (total == 0) {
        return;
    }
    if (total == 1) {
        const Rect& r = pos_rects.empty() ? neg_rects[0] : pos_rects[0];
        builder.emit_face_edges(axis, u, v, fp, r);
        return;
    }

    // Collect unique u/v coordinates from all rectangles.
    std::vector<float> u_vals, v_vals;
    u_vals.reserve(pos_rects.size() + neg_rects.size());
    v_vals.reserve(pos_rects.size() + neg_rects.size());
    const auto collect = [&](const std::vector<Rect>& rects) {
        for (const Rect& r : rects) {
            u_vals.push_back(r.u0);
            u_vals.push_back(r.u1);
            v_vals.push_back(r.v0);
            v_vals.push_back(r.v1);
        }
    };
    collect(pos_rects);
    collect(neg_rects);
    std::sort(u_vals.begin(), u_vals.end());
    std::sort(v_vals.begin(), v_vals.end());

    std::vector<float> u_unique, v_unique;
    u_unique.push_back(u_vals[0]);
    v_unique.push_back(v_vals[0]);
    for (size_t i = 1; i < u_vals.size(); ++i) {
        if (std::abs(u_vals[i] - u_unique.back()) > kEps) {
            u_unique.push_back(u_vals[i]);
        }
    }
    for (size_t i = 1; i < v_vals.size(); ++i) {
        if (std::abs(v_vals[i] - v_unique.back()) > kEps) {
            v_unique.push_back(v_vals[i]);
        }
    }

    const int cols = static_cast<int>(u_unique.size()) - 1;
    const int rows = static_cast<int>(v_unique.size()) - 1;
    if (cols < 1 || rows < 1) {
        return;
    }
    const int cells = cols * rows;
    std::vector<bool> pos_mask(cells, false);
    std::vector<bool> neg_mask(cells, false);

    const auto mark = [&](const std::vector<Rect>& rects, std::vector<bool>& mask) {
        for (const Rect& r : rects) {
            for (int ci = 0; ci < cols; ++ci) {
                const float cu = (u_unique[ci] + u_unique[ci + 1]) * 0.5f;
                if (cu < r.u0 || cu > r.u1) {
                    continue;
                }
                for (int ri = 0; ri < rows; ++ri) {
                    const float cv = (v_unique[ri] + v_unique[ri + 1]) * 0.5f;
                    if (cv >= r.v0 && cv <= r.v1) {
                        mask[ci + ri * cols] = true;
                    }
                }
            }
        }
    };
    mark(pos_rects, pos_mask);
    mark(neg_rects, neg_mask);

    std::vector<bool> filled(cells);
    for (int i = 0; i < cells; ++i) {
        filled[i] = pos_mask[i] != neg_mask[i];
    }

    // Horizontal edges (along u, between rows).
    for (int ci = 0; ci < cols; ++ci) {
        for (int ri = 0; ri <= rows; ++ri) {
            const bool above = ri > 0 && filled[ci + (ri - 1) * cols];
            const bool below = ri < rows && filled[ci + ri * cols];
            if (above == below) {
                continue;
            }
            float p0[3] = {};
            float p1[3] = {};
            p0[axis] = fp;
            p0[u] = u_unique[ci];
            p0[v] = v_unique[ri];
            p1[axis] = fp;
            p1[u] = u_unique[ci + 1];
            p1[v] = v_unique[ri];
            builder.add_edge(p0, p1);
        }
    }

    // Vertical edges (along v, between columns).
    for (int ri = 0; ri < rows; ++ri) {
        for (int ci = 0; ci <= cols; ++ci) {
            const bool left = ci > 0 && filled[(ci - 1) + ri * cols];
            const bool right = ci < cols && filled[ci + ri * cols];
            if (left == right) {
                continue;
            }
            float p0[3] = {};
            float p1[3] = {};
            p0[axis] = fp;
            p0[u] = u_unique[ci];
            p0[v] = v_unique[ri];
            p1[axis] = fp;
            p1[u] = u_unique[ci];
            p1[v] = v_unique[ri + 1];
            builder.add_edge(p0, p1);
        }
    }
}

} // namespace

OutlineMeshData build_outline_mesh(const std::vector<OutlineBox>& boxes, float thickness) {
    OutlineMeshData data;
    if (boxes.empty()) {
        return data;
    }

    EdgeBuilder builder(thickness);
    static const int kAxisUV[3][2] = {
        {1, 2},
        {0, 2},
        {0, 1},
    };

    for (int axis = 0; axis < 3; ++axis) {
        const int u = kAxisUV[axis][0];
        const int v = kAxisUV[axis][1];

        // Collect all distinct plane positions on this axis (min and max of
        // every box), sorted.
        std::vector<float> planes;
        for (const OutlineBox& box : boxes) {
            const float neg = box.min[axis];
            const float pos = box.max[axis];
            if (std::find_if(planes.begin(), planes.end(), [neg](float p) { return std::abs(p - neg) < kEps; }) == planes.end()) {
                planes.push_back(neg);
            }
            if (std::find_if(planes.begin(), planes.end(), [pos](float p) { return std::abs(p - pos) < kEps; }) == planes.end()) {
                planes.push_back(pos);
            }
        }
        std::sort(planes.begin(), planes.end());

        for (const float fp : planes) {
            std::vector<Rect> pos_rects;  // faces with normal in +axis
            std::vector<Rect> neg_rects;  // faces with normal in -axis
            for (const OutlineBox& box : boxes) {
                if (std::abs(box.max[axis] - fp) < kEps) {
                    pos_rects.push_back({box.min[u], box.max[u], box.min[v], box.max[v]});
                }
                if (std::abs(box.min[axis] - fp) < kEps) {
                    neg_rects.push_back({box.min[u], box.max[u], box.min[v], box.max[v]});
                }
            }
            emit_xor_perimeter(axis, u, v, fp, pos_rects, neg_rects, builder);
        }
    }

    data.verts.assign(builder.verts().begin(), builder.verts().end());
    data.indices.assign(builder.indices().begin(), builder.indices().end());
    return data;
}

void outline_fill_bounds(const std::vector<OutlineBox>& boxes, float center[3], float size[3]) {
    if (boxes.empty()) {
        center[0] = center[1] = center[2] = 0.0f;
        size[0] = size[1] = size[2] = 0.0f;
        return;
    }
    float bmin[3] = {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity(), std::numeric_limits<float>::infinity()};
    float bmax[3] = {-std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity()};
    for (const OutlineBox& box : boxes) {
        for (int i = 0; i < 3; ++i) {
            bmin[i] = std::min(bmin[i], box.min[i]);
            bmax[i] = std::max(bmax[i], box.max[i]);
        }
    }
    for (int i = 0; i < 3; ++i) {
        center[i] = (bmin[i] + bmax[i]) * 0.5f;
        size[i] = bmax[i] - bmin[i];
    }
}

} // namespace VoxelEngine