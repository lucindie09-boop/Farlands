// The selection ray: the DDA walk, the neighbour-aware shape test, and the
// ray-AABB slab test it is built on. Kept apart from world/block_editor.cpp so a
// change to how a block is aimed at does not mean editing the file that owns the
// write path.

#include "world/block_editor.hpp"
#include "world/chunk_world.hpp"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include <algorithm>
#include <vector>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

namespace {

// A neighbour for a shape claim, read through the chunk map the DDA has already
// locked (that lock covers one block beyond the ray's box, see the key set).
struct RaycastShapeContext {
    ChunkWorld* world;
    int32_t x;
    int32_t y;
    int32_t z;
};

BlockID raycast_shape_neighbor(void* ctx, ShapeFace face) {
    const RaycastShapeContext& c = *static_cast<RaycastShapeContext*>(ctx);
    ChunkMap& map = c.world->get_chunk_map();
    switch (face) {
        case ShapeFace::Top:    return static_cast<BlockID>(map.get_block_world_fast(c.x, c.y + 1, c.z));
        case ShapeFace::Bottom: return static_cast<BlockID>(map.get_block_world_fast(c.x, c.y - 1, c.z));
        case ShapeFace::Right:  return static_cast<BlockID>(map.get_block_world_fast(c.x + 1, c.y, c.z));
        case ShapeFace::Left:   return static_cast<BlockID>(map.get_block_world_fast(c.x - 1, c.y, c.z));
        case ShapeFace::Front:  return static_cast<BlockID>(map.get_block_world_fast(c.x, c.y, c.z + 1));
        case ShapeFace::Back:   break;
    }
    return static_cast<BlockID>(map.get_block_world_fast(c.x, c.y, c.z - 1));
}

// Ray-AABB intersection using the slab method. Returns true if the ray hits
// the box, with the parametric distance t and the outward face normal.
bool ray_aabb_intersect(const Vector3& origin, const Vector3& dir,
                        const Vector3& box_min, const Vector3& box_max,
                        double& t_out, Vector3& normal_out) {
    double tmin = -1e30;
    double tmax = 1e30;
    Vector3 normal(0, 0, 0);

    for (int i = 0; i < 3; ++i) {
        double o = (i == 0) ? origin.x : (i == 1) ? origin.y : origin.z;
        double d = (i == 0) ? dir.x : (i == 1) ? dir.y : dir.z;
        double bmin = (i == 0) ? box_min.x : (i == 1) ? box_min.y : box_min.z;
        double bmax = (i == 0) ? box_max.x : (i == 1) ? box_max.y : box_max.z;

        if (std::abs(d) < 1e-12) {
            if (o < bmin || o > bmax) return false;
        } else {
            double inv_d = 1.0 / d;
            double t1 = (bmin - o) * inv_d;
            double t2 = (bmax - o) * inv_d;
            if (t1 > t2) std::swap(t1, t2);

            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;

            if (tmin > tmax) return false;

            // Track which axis/face was hit at tmin
            if (tmin == t1) {
                normal = Vector3(0, 0, 0);
                if (i == 0) normal.x = (dir.x > 0) ? -1.0 : 1.0;
                else if (i == 1) normal.y = (dir.y > 0) ? -1.0 : 1.0;
                else normal.z = (dir.z > 0) ? -1.0 : 1.0;
            }
        }
    }

    if (tmin < 0 || tmin > 1e30) return false;
    t_out = tmin;
    normal_out = normal;
    return true;
}
} // namespace

RaycastResult BlockEditor::raycast_from_ray(const Vector3& ray_origin,
                                              const Vector3& ray_dir,
                                              double max_distance) const {
    RaycastResult result;

    int32_t current_x = static_cast<int32_t>(std::floor(ray_origin.x));
    int32_t current_y = static_cast<int32_t>(std::floor(ray_origin.y));
    int32_t current_z = static_cast<int32_t>(std::floor(ray_origin.z));

    int32_t step_x = ray_dir.x > 0 ? 1 : (ray_dir.x < 0 ? -1 : 0);
    int32_t step_y = ray_dir.y > 0 ? 1 : (ray_dir.y < 0 ? -1 : 0);
    int32_t step_z = ray_dir.z > 0 ? 1 : (ray_dir.z < 0 ? -1 : 0);

    double t_max_x = step_x != 0 ?
        ((step_x > 0 ? (current_x + 1) : current_x) - ray_origin.x) / ray_dir.x : max_distance;
    double t_max_y = step_y != 0 ?
        ((step_y > 0 ? (current_y + 1) : current_y) - ray_origin.y) / ray_dir.y : max_distance;
    double t_max_z = step_z != 0 ?
        ((step_z > 0 ? (current_z + 1) : current_z) - ray_origin.z) / ray_dir.z : max_distance;

    double t_delta_x = step_x != 0 ? std::abs(1.0 / ray_dir.x) : max_distance;
    double t_delta_y = step_y != 0 ? std::abs(1.0 / ray_dir.y) : max_distance;
    double t_delta_z = step_z != 0 ? std::abs(1.0 / ray_dir.z) : max_distance;

    int32_t prev_x = current_x;
    int32_t prev_y = current_y;
    int32_t prev_z = current_z;

    const int32_t max_steps = static_cast<int32_t>(max_distance) * 3;
    int32_t steps = 0;

    // The DDA below advances up to one block per axis per step, so it stays
    // inside [origin, origin + dir * (3 * max_distance)] blocks. Shared-lock
    // only the shards of the chunks that box can touch (a reach-distance ray
    // spans 1-3 chunks) once, up front — the DDA never leaves the box, so no
    // mid-walk re-acquisition is needed.
    //
    // The box is grown by one block on every side because a neighbour-dependent
    // shape (a fence's arms) is resolved against the cells around each candidate
    // block, and those must be inside the lock too.
    const int32_t end_x = static_cast<int32_t>(std::floor(ray_origin.x + ray_dir.x * (max_distance * 3.0 + 2.0)));
    const int32_t end_y = static_cast<int32_t>(std::floor(ray_origin.y + ray_dir.y * (max_distance * 3.0 + 2.0)));
    const int32_t end_z = static_cast<int32_t>(std::floor(ray_origin.z + ray_dir.z * (max_distance * 3.0 + 2.0)));

    std::vector<uint64_t> keys;
    {
        int32_t min_cx, min_cy, min_cz, max_cx, max_cy, max_cz, dummy;
        world_to_chunk_local(std::min(current_x, end_x) - 1, std::min(current_y, end_y) - 1,
                             std::min(current_z, end_z) - 1,
                             min_cx, min_cy, min_cz, dummy, dummy, dummy);
        world_to_chunk_local(std::max(current_x, end_x) + 1, std::max(current_y, end_y) + 1,
                             std::max(current_z, end_z) + 1,
                             max_cx, max_cy, max_cz, dummy, dummy, dummy);
        for (int32_t cx = min_cx; cx <= max_cx; ++cx)
            for (int32_t cy = min_cy; cy <= max_cy; ++cy)
                for (int32_t cz = min_cz; cz <= max_cz; ++cz)
                    keys.push_back(chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz));
    }
    auto map_lock = chunk_world->get_chunk_map().lock_keys(keys);
    const BlockRegistry& registry = BlockRegistry::get_instance();
    while (steps < max_steps) {
        int block = chunk_world->get_chunk_map().get_block_world_fast(current_x, current_y, current_z);
        if (block != 0) {
            BlockID bid = static_cast<BlockID>(block);
            const BlockType& bt = registry.get_block_fast(bid);
            if (bt.is_full_cube()) {
                Vector3 face_normal(
                    static_cast<double>(prev_x - current_x),
                    static_cast<double>(prev_y - current_y),
                    static_cast<double>(prev_z - current_z));
                // The camera can sit inside a solid block (a head-height block,
                // a flipped view), and then prev == current and there is no entry
                // face at all. Entering the chain below with a zero normal would
                // fall through to the z arm and divide by a ray component that is
                // exactly zero on an axis-aligned ray — an inf/NaN hit_point and a
                // zero hit_normal. The no-face case is answered directly instead:
                // the hit is the origin. Each arm below divides only by the axis
                // that moved, and an axis can only have moved if its ray
                // component is non-zero, so those three are safe.
                double t_face = 0.0;
                if (face_normal.x != 0.0) {
                    double fx = (face_normal.x > 0) ? current_x + 1 : current_x;
                    t_face = (fx - ray_origin.x) / ray_dir.x;
                } else if (face_normal.y != 0.0) {
                    double fy = (face_normal.y > 0) ? current_y + 1 : current_y;
                    t_face = (fy - ray_origin.y) / ray_dir.y;
                } else if (face_normal.z != 0.0) {
                    double fz = (face_normal.z > 0) ? current_z + 1 : current_z;
                    t_face = (fz - ray_origin.z) / ray_dir.z;
                }
                result.success = true;
                result.position = Vector3(current_x, current_y, current_z);
                result.place_position = Vector3(prev_x, prev_y, prev_z);
                result.block_id = static_cast<int>(bid);
                result.hit_normal = face_normal;
                result.hit_point = ray_origin + ray_dir * t_face;
                return result;
            }
            // Non-full block: test ray against each selection_box
            bool hit_any = false;
            double closest_t = max_distance;
            Vector3 hit_normal;
            // Neighbour-dependent parts resolve here too, so a fence is only
            // aimed at where it is actually drawn — the arms are missing from an
            // isolated post, and the ray must not hit a phantom one.
            RaycastShapeContext shape_ctx{chunk_world, current_x, current_y, current_z};
            ShapeBoxes shape;
            resolve_shape_boxes(bt, registry, ShapeNeighborFn{&raycast_shape_neighbor, &shape_ctx},
                                ShapeBoxKind::Selection, shape);
            for (uint8_t bi = 0; bi < shape.count(); ++bi) {
                const BlockAABB& box = shape[bi];
                Vector3 box_min(current_x + box.min[0], current_y + box.min[1], current_z + box.min[2]);
                Vector3 box_max(current_x + box.max[0], current_y + box.max[1], current_z + box.max[2]);
                double t;
                Vector3 n;
                if (ray_aabb_intersect(ray_origin, ray_dir, box_min, box_max, t, n) && t < closest_t) {
                    closest_t = t;
                    hit_normal = n;
                    hit_any = true;
                }
            }
            if (hit_any) {
                result.success = true;
                result.position = Vector3(current_x, current_y, current_z);
                result.place_position = Vector3(current_x, current_y, current_z) + hit_normal;
                result.block_id = static_cast<int>(bid);
                result.hit_normal = hit_normal;
                result.hit_point = ray_origin + ray_dir * closest_t;
                return result;
            }
            // No AABB hit — continue DDA to find next block
            prev_x = current_x;
            prev_y = current_y;
            prev_z = current_z;
        } else {
            prev_x = current_x;
            prev_y = current_y;
            prev_z = current_z;
        }

        if (t_max_x < t_max_y) {
            if (t_max_x < t_max_z) {
                current_x += step_x;
                t_max_x += t_delta_x;
            } else {
                current_z += step_z;
                t_max_z += t_delta_z;
            }
        } else {
            if (t_max_y < t_max_z) {
                current_y += step_y;
                t_max_y += t_delta_y;
            } else {
                current_z += step_z;
                t_max_z += t_delta_z;
            }
        }
        steps++;
    }
    return result;
}

} // namespace VoxelEngine
