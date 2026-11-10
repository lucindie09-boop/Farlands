// The far-tier pending set measured against what the GPU currently draws for
// it. The queue alone cannot answer whether a queued far chunk is a hole: a
// far-region mesh may still be drawing the chunk's previous geometry, and the
// chunk's own instance may be hidden because that region owns the draw. The
// no-geometry entries are then asked the question the mesher's buried test
// asks: fully solid, with six solid neighbours (or a solid prediction for an
// ungenerated one), it would build no faces at all. Finally the potential holes
// go through a camera-ray occlusion pass against the blocks that are DRAWN, so
// a chunk behind a hill and a chunk behind an unbuilt neighbour are not counted
// alike.
#include "mesh/mesh_manager_internal.hpp"

#include "core/block_types.hpp"

#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace {

// The buried test's decision, with the six neighbour answers supplied by the
// caller (the queue report and the visibility pass resolve missing neighbours
// the same way). A chunk at y == 0 stands on the world floor.
template <typename NeighborSolid>
bool would_build_empty(int32_t cx, int32_t cy, int32_t cz, const ChunkData* data,
                       NeighborSolid&& neighbor_solid) {
    if (!data || !data->fully_solid()) return false;
    return neighbor_solid(cx - 1, cy, cz) &&
           neighbor_solid(cx + 1, cy, cz) &&
           neighbor_solid(cx, cy + 1, cz) &&
           neighbor_solid(cx, cy, cz - 1) &&
           neighbor_solid(cx, cy, cz + 1) &&
           (neighbor_solid(cx, cy - 1, cz) || cy == 0);
}

// A full opaque cube is the only thing that hides a face, and therefore the
// only thing that should stop an occlusion ray. A transparent full cube (the
// falling-water column) does not.
bool is_opaque_occluder(const BlockType& type) {
    return type.is_full_cube() &&
           HasProperty(type.properties, BlockProperty::Opaque) &&
           !HasProperty(type.properties, BlockProperty::Transparent);
}

// The entry distance of a ray into an AABB. `t` is zero when the origin is
// inside; false when the ray misses or the box is entirely behind it.
bool ray_aabb_entry(const godot::Vector3& origin, const godot::Vector3& dir,
                    const godot::Vector3& lo, const godot::Vector3& hi, float& t) {
    float t_min = 0.0f;
    float t_max = std::numeric_limits<float>::infinity();
    const float o[3] = {static_cast<float>(origin.x), static_cast<float>(origin.y),
                        static_cast<float>(origin.z)};
    const float d[3] = {static_cast<float>(dir.x), static_cast<float>(dir.y),
                        static_cast<float>(dir.z)};
    const float l[3] = {static_cast<float>(lo.x), static_cast<float>(lo.y),
                        static_cast<float>(lo.z)};
    const float h[3] = {static_cast<float>(hi.x), static_cast<float>(hi.y),
                        static_cast<float>(hi.z)};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(d[axis]) < 1e-6f) {
            if (o[axis] < l[axis] || o[axis] > h[axis]) return false;
            continue;
        }
        float t1 = (l[axis] - o[axis]) / d[axis];
        float t2 = (h[axis] - o[axis]) / d[axis];
        if (t1 > t2) std::swap(t1, t2);
        t_min = std::max(t_min, t1);
        t_max = std::min(t_max, t2);
        if (t_min > t_max) return false;
    }
    if (t_max < 0.0f) return false;
    t = t_max >= 0.0f ? std::max(t_min, 0.0f) : 0.0f;
    return true;
}

} // namespace

PendingFarGeometry MeshManager::pending_far_geometry() const {
    PendingFarGeometry out;
    if (!chunk_map) return out;
    const std::vector<uint64_t> keys =
        mesh_queue.visible_pending_far_keys(lod_distance, far_lod_distance);
    out.total = keys.size();

    // The buried test's missing-neighbour half, memoised for this report only:
    // the predicate samples worldgen (~25 us) and the same frontier coordinates
    // recur across the six probes of adjacent chunks.
    std::unordered_map<uint64_t, uint8_t> missing_solid;
    const auto neighbor_solid = [&](int32_t nx, int32_t ny, int32_t nz) -> bool {
        const ChunkRenderData* n = chunk_map->get_chunk_render_data(nx, ny, nz);
        if (n && n->data) return n->data->fully_solid();
        if (!chunk_would_be_solid_fn_) return false;
        const uint64_t nkey = chunk_map->get_chunk_key(nx, ny, nz);
        const auto it = missing_solid.find(nkey);
        if (it != missing_solid.end()) return it->second != 0;
        const bool solid = chunk_would_be_solid_fn_(nx, ny, nz);
        if (missing_solid.size() >= 8192) missing_solid.clear();
        missing_solid.emplace(nkey, solid ? uint8_t{1} : uint8_t{0});
        return solid;
    };

    for (uint64_t key : keys) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        const ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (!render_data) {
            ++out.no_chunk;
            continue;
        }
        const bool all_air = render_data->data && render_data->data->is_all_air();
        // What the region DRAWS is its active mesh, not what its sources would
        // like to build next: the chunk is in the old region mesh until an
        // upload swaps it out.
        bool region_drawn = false;
        const auto region_it = far_regions.find(get_far_region_key(cx, cy, cz));
        if (region_it != far_regions.end() && region_it->second.active &&
            region_it->second.instance_rid.is_valid()) {
            const auto& members = region_it->second.active_chunk_keys;
            region_drawn = std::find(members.begin(), members.end(), key) != members.end();
        }
        if (render_data->instance_rid.is_valid() && render_data->instance_visible) {
            ++out.drawn_chunk;
        } else if (region_drawn) {
            ++out.drawn_region;
        } else if (render_data->instance_rid.is_valid() || render_data->far_mesh_cache) {
            ++out.hidden;
        } else if (all_air) {
            ++out.empty;
        } else {
            ++out.no_geometry;
            if (would_build_empty(cx, cy, cz, render_data->data.get(), neighbor_solid)) {
                ++out.solid;
                ++out.enclosed;
            } else if (render_data->data && render_data->data->fully_solid()) {
                ++out.solid;
            }
        }
    }
    return out;
}

VisibleFarHoles MeshManager::visible_far_holes(const godot::Vector3& eye) const {
    godot::Transform3D camera;
    camera.origin = eye;
    return visible_far_holes(camera, godot::Projection(), godot::Vector2());
}

VisibleFarHoles MeshManager::visible_far_holes(const godot::Transform3D& camera,
                                               const godot::Projection& projection,
                                               const godot::Vector2& viewport) const {
    VisibleFarHoles out;
    if (!chunk_map) return out;
    const godot::Vector3 eye = camera.origin;
    if (!std::isfinite(eye.x) || !std::isfinite(eye.y) || !std::isfinite(eye.z)) return out;
    const bool screen_enabled = viewport.x >= 1.0f && viewport.y >= 1.0f;

    const std::vector<uint64_t> keys =
        mesh_queue.visible_pending_far_keys(lod_distance, far_lod_distance);
    std::unordered_map<uint64_t, uint8_t> missing_solid;
    const auto neighbor_solid = [&](int32_t nx, int32_t ny, int32_t nz) -> bool {
        const ChunkRenderData* n = chunk_map->get_chunk_render_data(nx, ny, nz);
        if (n && n->data) return n->data->fully_solid();
        if (!chunk_would_be_solid_fn_) return false;
        const uint64_t nkey = chunk_map->get_chunk_key(nx, ny, nz);
        const auto it = missing_solid.find(nkey);
        if (it != missing_solid.end()) return it->second != 0;
        const bool solid = chunk_would_be_solid_fn_(nx, ny, nz);
        if (missing_solid.size() >= 8192) missing_solid.clear();
        missing_solid.emplace(nkey, solid ? uint8_t{1} : uint8_t{0});
        return solid;
    };

    struct Hole {
        int32_t cx, cy, cz;
        bool visible = false;
    };
    std::vector<Hole> holes;
    for (uint64_t key : keys) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        const ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        bool drawn = false;
        if (render_data && render_data->instance_rid.is_valid() && render_data->instance_visible) {
            drawn = true;
        } else {
            const auto region_it = far_regions.find(get_far_region_key(cx, cy, cz));
            if (region_it != far_regions.end() && region_it->second.active &&
                region_it->second.instance_rid.is_valid()) {
                const auto& members = region_it->second.active_chunk_keys;
                drawn = std::find(members.begin(), members.end(), key) != members.end();
            }
        }
        if (drawn) continue;
        if (render_data && render_data->data && render_data->data->is_all_air()) continue;
        if (render_data &&
            would_build_empty(cx, cy, cz, render_data->data.get(), neighbor_solid)) {
            continue;
        }
        holes.push_back({cx, cy, cz});
    }
    out.total = holes.size();
    if (holes.empty()) return out;

    const BlockRegistry& registry = BlockRegistry::get_instance();
    // One shared snapshot for the whole pass: every lookup below is a _fast
    // accessor, and the draw test is the same one the queue split uses.
    auto lock = chunk_map->lock_all();
    const auto drawn_fast = [&](int32_t cx, int32_t cy, int32_t cz) -> bool {
        const ChunkRenderData* rd = chunk_map->get_chunk_render_data_fast(cx, cy, cz);
        if (rd && rd->instance_rid.is_valid() && rd->instance_visible) return true;
        const auto it = far_regions.find(get_far_region_key(cx, cy, cz));
        if (it == far_regions.end() || !it->second.active ||
            !it->second.instance_rid.is_valid()) {
            return false;
        }
        const uint64_t key = chunk_map->get_chunk_key(cx, cy, cz);
        const auto& members = it->second.active_chunk_keys;
        return std::find(members.begin(), members.end(), key) != members.end();
    };
    const auto opaque_drawn_block = [&](int32_t wx, int32_t wy, int32_t wz) -> bool {
        const int id = chunk_map->get_block_world_fast(wx, wy, wz);
        if (id == static_cast<int>(BlockIDs::AIR)) return false;
        const BlockType& type = registry.get_block_fast(static_cast<BlockID>(id));
        if (!is_opaque_occluder(type)) return false;
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
        return drawn_fast(cx, cy, cz);
    };

    const auto ray_blocked = [&](const godot::Vector3& origin,
                                 const godot::Vector3& dir, float max_t) -> bool {
        int32_t x = static_cast<int32_t>(std::floor(origin.x));
        int32_t y = static_cast<int32_t>(std::floor(origin.y));
        int32_t z = static_cast<int32_t>(std::floor(origin.z));
        const int32_t step_x = dir.x > 0.0f ? 1 : (dir.x < 0.0f ? -1 : 0);
        const int32_t step_y = dir.y > 0.0f ? 1 : (dir.y < 0.0f ? -1 : 0);
        const int32_t step_z = dir.z > 0.0f ? 1 : (dir.z < 0.0f ? -1 : 0);
        const float inf = std::numeric_limits<float>::infinity();
        float t_max_x = inf, t_max_y = inf, t_max_z = inf;
        float t_delta_x = inf, t_delta_y = inf, t_delta_z = inf;
        if (step_x != 0) {
            t_delta_x = 1.0f / std::fabs(static_cast<float>(dir.x));
            const float edge = static_cast<float>(step_x > 0 ? x + 1 : x);
            t_max_x = (edge - static_cast<float>(origin.x)) / static_cast<float>(dir.x);
        }
        if (step_y != 0) {
            t_delta_y = 1.0f / std::fabs(static_cast<float>(dir.y));
            const float edge = static_cast<float>(step_y > 0 ? y + 1 : y);
            t_max_y = (edge - static_cast<float>(origin.y)) / static_cast<float>(dir.y);
        }
        if (step_z != 0) {
            t_delta_z = 1.0f / std::fabs(static_cast<float>(dir.z));
            const float edge = static_cast<float>(step_z > 0 ? z + 1 : z);
            t_max_z = (edge - static_cast<float>(origin.z)) / static_cast<float>(dir.z);
        }
        float t = 0.0f;
        for (int guard = 0; guard < 8192 && t <= max_t; ++guard) {
            if (opaque_drawn_block(x, y, z)) return true;
            if (t_max_x <= t_max_y && t_max_x <= t_max_z) {
                x += step_x;
                t = t_max_x;
                t_max_x += t_delta_x;
            } else if (t_max_y <= t_max_z) {
                y += step_y;
                t = t_max_y;
                t_max_y += t_delta_y;
            } else {
                z += step_z;
                t = t_max_z;
                t_max_z += t_delta_z;
            }
        }
        return false;
    };

    constexpr int32_t kChunkSize = 32;
    constexpr float kFractions[3] = {0.25f, 0.5f, 0.75f};
    for (Hole& hole : holes) {
        const godot::Vector3 lo(static_cast<float>(hole.cx * kChunkSize),
                                static_cast<float>(hole.cy * kChunkSize),
                                static_cast<float>(hole.cz * kChunkSize));
        const godot::Vector3 hi(lo.x + kChunkSize, lo.y + kChunkSize, lo.z + kChunkSize);
        bool visible = false;
        for (int sx = 0; sx < 3 && !visible; ++sx) {
            for (int sy = 0; sy < 3 && !visible; ++sy) {
                for (int sz = 0; sz < 3 && !visible; ++sz) {
                    const godot::Vector3 sample(lo.x + kFractions[sx] * kChunkSize,
                                                lo.y + kFractions[sy] * kChunkSize,
                                                lo.z + kFractions[sz] * kChunkSize);
                    godot::Vector3 dir = sample - eye;
                    const float distance = static_cast<float>(dir.length());
                    if (distance < 1e-4f) {
                        visible = true;
                        break;
                    }
                    dir = dir / distance;
                    float entry = 0.0f;
                    if (!ray_aabb_entry(eye, dir, lo, hi, entry)) continue;
                    if (entry <= 0.01f) {
                        visible = true;
                        break;
                    }
                    if (!ray_blocked(eye, dir, entry - 0.01f)) visible = true;
                }
            }
        }
        if (visible) {
            ++out.visible;
            hole.visible = true;
        } else {
            ++out.occluded;
        }
    }

    if (screen_enabled) {
        // The mask is the union, not a per-hole sum: overlapping holes must not
        // make the reported screen area exceed the viewport. A hit fills the
        // sample's whole cell, so each cell still carries its projected area.
        constexpr float kScreenCell = 4.0f;
        const int mask_width =
            std::max(1, static_cast<int>(std::ceil(viewport.x / kScreenCell)));
        const int mask_height =
            std::max(1, static_cast<int>(std::ceil(viewport.y / kScreenCell)));
        std::vector<uint8_t> covered(static_cast<size_t>(mask_width) * mask_height, 0);
        const godot::Transform3D camera_inv = camera.affine_inverse();
        const godot::Projection projection_inv = projection.inverse();
        for (const Hole& hole : holes) {
            if (!hole.visible) continue;
            const godot::Vector3 lo(static_cast<float>(hole.cx * kChunkSize),
                                    static_cast<float>(hole.cy * kChunkSize),
                                    static_cast<float>(hole.cz * kChunkSize));
            const godot::Vector3 hi(lo.x + kChunkSize, lo.y + kChunkSize, lo.z + kChunkSize);

            // Project the eight corners. A hole straddling the near plane has
            // no usable bounds from the corners that are behind it, so use the
            // whole viewport as a conservative rectangle instead of dropping it.
            float sx0 = std::numeric_limits<float>::infinity();
            float sy0 = std::numeric_limits<float>::infinity();
            float sx1 = -std::numeric_limits<float>::infinity();
            float sy1 = -std::numeric_limits<float>::infinity();
            bool any_front = false;
            bool all_front = true;
            for (int corner = 0; corner < 8; ++corner) {
                const godot::Vector3 world((corner & 1) ? hi.x : lo.x,
                                           (corner & 2) ? hi.y : lo.y,
                                           (corner & 4) ? hi.z : lo.z);
                const godot::Vector3 view = camera_inv.xform(world);
                const godot::Vector4 clip =
                    projection.xform(godot::Vector4(view.x, view.y, view.z, 1.0f));
                if (!std::isfinite(clip.w) || clip.w <= 1e-5f) {
                    all_front = false;
                    continue;
                }
                const float ndc_x = clip.x / clip.w;
                const float ndc_y = clip.y / clip.w;
                if (!std::isfinite(ndc_x) || !std::isfinite(ndc_y)) {
                    all_front = false;
                    continue;
                }
                const float px = (ndc_x * 0.5f + 0.5f) * viewport.x;
                const float py = (0.5f - ndc_y * 0.5f) * viewport.y;
                sx0 = std::min(sx0, px);
                sy0 = std::min(sy0, py);
                sx1 = std::max(sx1, px);
                sy1 = std::max(sy1, py);
                any_front = true;
            }
            if (!any_front) continue;

            float x0 = all_front ? sx0 : 0.0f;
            float y0 = all_front ? sy0 : 0.0f;
            float x1 = all_front ? sx1 : viewport.x;
            float y1 = all_front ? sy1 : viewport.y;
            x0 = std::max(0.0f, std::min(x0, viewport.x));
            y0 = std::max(0.0f, std::min(y0, viewport.y));
            x1 = std::max(0.0f, std::min(x1, viewport.x));
            y1 = std::max(0.0f, std::min(y1, viewport.y));
            if (x1 <= x0 || y1 <= y0) continue;

            const float width = x1 - x0;
            const float height = y1 - y0;
            const int nx = std::min(8, std::max(1, static_cast<int>(std::ceil(width))));
            const int ny = std::min(8, std::max(1, static_cast<int>(std::ceil(height))));
            bool any_hit = false;
            for (int iy = 0; iy < ny; ++iy) {
                for (int ix = 0; ix < nx; ++ix) {
                    const float cell_x0 = x0 + static_cast<float>(ix) * width /
                                                   static_cast<float>(nx);
                    const float cell_x1 = x0 + static_cast<float>(ix + 1) * width /
                                                   static_cast<float>(nx);
                    const float cell_y0 = y0 + static_cast<float>(iy) * height /
                                                   static_cast<float>(ny);
                    const float cell_y1 = y0 + static_cast<float>(iy + 1) * height /
                                                   static_cast<float>(ny);
                    const float px = (cell_x0 + cell_x1) * 0.5f;
                    const float py = (cell_y0 + cell_y1) * 0.5f;
                    const godot::Vector2 ndc(2.0f * px / viewport.x - 1.0f,
                                             1.0f - 2.0f * py / viewport.y);
                    const godot::Vector4 view4 = projection_inv.xform(
                        godot::Vector4(ndc.x, ndc.y, -1.0f, 1.0f));
                    if (!std::isfinite(view4.w) || std::fabs(view4.w) < 1e-6f) continue;
                    const godot::Vector3 view_point(view4.x / view4.w, view4.y / view4.w,
                                                    view4.z / view4.w);
                    godot::Vector3 origin;
                    godot::Vector3 dir;
                    if (projection.is_orthogonal()) {
                        origin = camera.xform(
                            godot::Vector3(view_point.x, view_point.y, 0.0f));
                        dir = camera.basis.xform(godot::Vector3(0.0f, 0.0f, -1.0f));
                    } else {
                        origin = eye;
                        dir = camera.basis.xform(view_point);
                    }
                    if (!dir.is_finite() || dir.length_squared() < 1e-8f) continue;
                    dir = dir.normalized();
                    float entry = 0.0f;
                    if (!ray_aabb_entry(origin, dir, lo, hi, entry)) continue;
                    if (entry > 0.01f && ray_blocked(origin, dir, entry - 0.01f)) continue;
                    const int mx0 = std::max(0, static_cast<int>(cell_x0 / kScreenCell));
                    const int mx1 = std::min(mask_width,
                                             static_cast<int>(std::ceil(cell_x1 / kScreenCell)));
                    const int my0 = std::max(0, static_cast<int>(cell_y0 / kScreenCell));
                    const int my1 = std::min(mask_height,
                                             static_cast<int>(std::ceil(cell_y1 / kScreenCell)));
                    for (int my = my0; my < my1; ++my) {
                        const size_t row = static_cast<size_t>(my) * mask_width;
                        std::fill(covered.begin() + row + mx0,
                                  covered.begin() + row + mx1, uint8_t{1});
                    }
                    any_hit = true;
                }
            }
            if (any_hit) ++out.screen_visible;
        }
        size_t covered_cells = 0;
        for (uint8_t cell : covered) {
            if (cell != 0) ++covered_cells;
        }
        const double viewport_area = static_cast<double>(viewport.x) * viewport.y;
        out.screen_pixels = std::min(viewport_area,
                                     static_cast<double>(covered_cells) * kScreenCell *
                                         kScreenCell);
    }
    return out;
}

} // namespace VoxelEngine
