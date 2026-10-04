#include "engine/collision_resolver.hpp"
#include "engine/collision_resolver_internal.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_coords.hpp"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace VoxelEngine {

using namespace godot;

// The shape neighbour context, the neighbour reader and the chunk key set live
// in collision_resolver_internal.hpp: the turned-box translation unit needs them
// too, so they are inline there rather than file-local here.

template<typename Pred>
static void resolve_axis(const godot::Vector3& position,
                         const godot::Vector3& motion,
                         const godot::Vector3& size,
                         int axis,
                         godot::Vector3& result,
                         bool& collided,
                         Pred is_solid) {
    if (motion[axis] == 0.0f) {
        collided = false;
        return;
    }
    float direction = motion[axis] > 0.0f ? 1.0f : -1.0f;
    float remaining = std::abs(motion[axis]);

    while (remaining > 0.001f) {

        float leading_edge = result[axis] + (direction > 0.0f ? size[axis] : 0.0f);
        float next_boundary = (direction > 0.0f) ? (std::floor(leading_edge) + 1.0f) : std::ceil(leading_edge);
        float dist = std::abs(next_boundary - leading_edge);
        if (dist < 0.001f) {
            dist = 1.0f;
        }
        float current_step = std::min(dist, remaining);
        Vector3 test_pos = result;
        test_pos[axis] += direction * current_step;
        AABB test_aabb(test_pos, size);
        if (is_solid(test_aabb)) {
            collided = true;
            float low = result[axis];
            float high = test_pos[axis];
            float best = result[axis];
            for (int i = 0; i < 10; ++i) {
                float mid = (low + high) * 0.5f;
                Vector3 mid_pos = result;
                mid_pos[axis] = mid;
                AABB mid_aabb(mid_pos, size);
                if (is_solid(mid_aabb)) {
                    high = mid;
                } else {
                    best = mid;
                    low = mid;
                }
            }
            result[axis] = best;
            break;
        }
        result[axis] += direction * current_step;
        remaining -= current_step;
    }
}

std::vector<CollisionResolver::PointContact> CollisionResolver::contacts_for_points(
    const Vector3* points, size_t count, float radius) const {
    std::vector<PointContact> out;
    if (!chunk_map_ || count == 0) return out;

    const BlockRegistry& registry = BlockRegistry::get_instance();

    // The union of everything any point can touch, read under ONE lock. The
    // visit ring below reaches one cell past a point, and a shape rule reaches
    // one cell past the cell it resolves, so the lock is padded by two.
    Vector3 lo = points[0];
    Vector3 hi = points[0];
    for (size_t i = 1; i < count; ++i) {
        lo = lo.min(points[i]);
        hi = hi.max(points[i]);
    }
    const Vector3 pad(2.0f + radius, 2.0f + radius, 2.0f + radius);
    auto lock = chunk_map_->lock_keys(chunk_keys_for_box(chunk_map_, lo - pad, hi + pad));

    // Every solid cell a point could touch, resolved ONCE to the boxes it really
    // collides as. The contact a point makes is with the block's own shape -- a
    // torch is a post and a slab is half a cell -- and answering from the cell
    // around them is what had a dropped item rest on the torch's cell boundary
    // instead of on the ground beside it. A full cube answers as its own cell box,
    // so the common case still costs no resolution.
    struct CellBoxes {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        ShapeBoxes boxes;
    };
    std::vector<CellBoxes> cells;
    const int32_t min_x = static_cast<int32_t>(std::floor(lo.x - radius)) - 1;
    const int32_t min_y = static_cast<int32_t>(std::floor(lo.y - radius)) - 1;
    const int32_t min_z = static_cast<int32_t>(std::floor(lo.z - radius)) - 1;
    const int32_t max_x = static_cast<int32_t>(std::floor(hi.x + radius)) + 1;
    const int32_t max_y = static_cast<int32_t>(std::floor(hi.y + radius)) + 1;
    const int32_t max_z = static_cast<int32_t>(std::floor(hi.z + radius)) + 1;
    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t z = min_z; z <= max_z; ++z) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                const BlockID bid = static_cast<BlockID>(chunk_map_->get_block_world_fast(x, y, z));
                if (bid == BlockIDs::AIR) continue;
                const BlockType& bt = registry.get_block_fast(bid);
                if (!bt.stops_bodies()) continue;
                CellBoxes cell;
                cell.x = x;
                cell.y = y;
                cell.z = z;
                if (bt.is_full_cube()) {
                    // A full cube is the cell's own box, whatever its static list
                    // says: the built-in default registry (tests, headless tools)
                    // never fills one in, and a body still has to stand on it.
                    BlockAABB& full = cell.boxes.resolved[0];
                    for (int axis = 0; axis < 3; ++axis) {
                        full.min[axis] = 0.0f;
                        full.max[axis] = 1.0f;
                    }
                    cell.boxes.resolved_count = 1;
                } else {
                    CollisionShapeContext shape_ctx{chunk_map_, x, y, z};
                    resolve_shape_boxes(bt, registry,
                                        ShapeNeighborFn{&collision_shape_neighbor, &shape_ctx},
                                        ShapeBoxKind::Collision, cell.boxes);
                    if (cell.boxes.empty()) continue;
                }
                cells.push_back(cell);
            }
        }
    }

    // The point is inside (or within `radius` of) one of these boxes: pushed out
    // of the face it is nearest, which is the shortest way out.
    auto append = [&out](const Vector3& p, const Vector3& local, const BlockAABB& box) {
        const float ox = std::min(local.x - box.min[0], box.max[0] - local.x);
        const float oy = std::min(local.y - box.min[1], box.max[1] - local.y);
        const float oz = std::min(local.z - box.min[2], box.max[2] - local.z);
        PointContact contact;
        contact.point = p;
        contact.normal = Vector3(0.0f, 1.0f, 0.0f);
        contact.depth = oy;
        if (ox <= oy && ox <= oz) {
            const float cx = (box.min[0] + box.max[0]) * 0.5f;
            contact.normal = Vector3(local.x < cx ? -1.0f : 1.0f, 0.0f, 0.0f);
            contact.depth = ox;
        } else if (oz <= oy && oz <= ox) {
            const float cz = (box.min[2] + box.max[2]) * 0.5f;
            contact.normal = Vector3(0.0f, 0.0f, local.z < cz ? -1.0f : 1.0f);
            contact.depth = oz;
        } else {
            const float cy = (box.min[1] + box.max[1]) * 0.5f;
            contact.normal = Vector3(0.0f, local.y < cy ? -1.0f : 1.0f, 0.0f);
        }
        out.push_back(contact);
    };

    for (size_t i = 0; i < count; ++i) {
        const Vector3 p = points[i];
        for (const CellBoxes& cell : cells) {
            const Vector3 local = p - Vector3(static_cast<float>(cell.x),
                                              static_cast<float>(cell.y),
                                              static_cast<float>(cell.z));
            for (uint8_t bi = 0; bi < cell.boxes.count(); ++bi) {
                const BlockAABB& box = cell.boxes[bi];
                if (local.x < box.min[0] - radius || local.x > box.max[0] + radius ||
                    local.y < box.min[1] - radius || local.y > box.max[1] + radius ||
                    local.z < box.min[2] - radius || local.z > box.max[2] + radius) {
                    continue;
                }
                append(p, local, box);
            }
        }
    }
    return out;
}

bool CollisionResolver::is_aabb_solid(const AABB& aabb) const {
    constexpr float kPad = 1.0f;
    auto lock = chunk_map_->lock_keys(chunk_keys_for_box(
        chunk_map_, aabb.position - Vector3(kPad, kPad, kPad),
        aabb.position + aabb.size + Vector3(kPad, kPad, kPad)));
    return is_aabb_solid_fast(aabb);
}

CollisionResolver::CollisionResult CollisionResolver::resolve(
    const Vector3& position,
    const Vector3& motion,
    const Vector3& size,
    float step_height
) const {
    // `position` is Minecraft-style: X/Z at the CENTER of the footprint,
    // Y at the feet (min corner). Godot's AABB(origin, size) wants a min-corner
    // origin, so shift into "corner space" for the sweep/step math below,
    // then shift back out before returning.
    const Vector3 half_xz(size.x * 0.5f, 0.0f, size.z * 0.5f);
    Vector3 result = position - half_xz;
    CollisionResult out;

    // Conservative swept volume (corner space) that every solid probe below can
    // touch: the 3-axis sweep over `motion`, the floor probe (y-0.05), and the
    // step-up assist (raised body + settle down). Shared-lock only the shards of
    // chunks intersecting this box instead of the whole map.
    constexpr float kPad = 1.0f;
    Vector3 box_lo = result;
    Vector3 box_hi = result + size;
    box_lo.x = std::min(box_lo.x, box_lo.x + motion.x) - kPad;
    box_hi.x = std::max(box_hi.x, box_hi.x + motion.x) + kPad;
    box_lo.z = std::min(box_lo.z, box_lo.z + motion.z) - kPad;
    box_hi.z = std::max(box_hi.z, box_hi.z + motion.z) + kPad;
    box_lo.y = std::min(box_lo.y, box_lo.y + motion.y) - 0.05f - kPad;
    box_hi.y = std::max(box_hi.y, box_hi.y + motion.y) + step_height + kPad;

    auto lock = chunk_map_->lock_keys(chunk_keys_for_box(chunk_map_, box_lo, box_hi));

    auto is_solid = [this](const AABB& aabb) { return is_aabb_solid_fast(aabb); };

    // Vanilla: Y first, then whichever horizontal axis has the LARGER motion magnitude.
    int axis_order[3] = {1, 0, 2};
    if (std::abs(motion.x) < std::abs(motion.z)) {
        axis_order[1] = 2;
        axis_order[2] = 0;
    }
    Vector3 result_after_y;

    for (int i = 0; i < 3; ++i) {
        int axis = axis_order[i];
        bool collided = false;
        resolve_axis(position, motion, size, axis, result, collided, is_solid);
        if (axis == 1) {
            out.collided_y = collided;
            result_after_y = result;
        } else if (axis == 0) {
            out.collided_x = collided;
        } else {
            out.collided_z = collided;
        }
    }

    // Floor probe: check final resolved position only (matches the vanilla grounded state)
    AABB floor_aabb(result, size);
    floor_aabb.position.y -= 0.05f;
    out.on_floor = is_solid(floor_aabb);

    // Step-up assist: if grounded and collided horizontally, try raising position.
    // Only accept the step if it actually lets the player travel further horizontally
    // than not stepping — this correctly rejects full-block walls (step doesn't help)
    // while allowing slabs, snow layers, etc. (step clears the obstruction).
    if (step_height > 0.0f && out.on_floor && (out.collided_x || out.collided_z)) {
        // Raised-body check: the player's FULL AABB moved up by step_height must be
        // clear. (The old probe of just [feet, feet+step_height) always contained the
        // obstruction being stepped onto — and the floor beneath — so step-up could
        // never succeed; the raised body clears both once the feet pass the top.)
        Vector3 stepped_pos = result_after_y;
        stepped_pos.y += step_height;
        AABB raised(stepped_pos, size);

        if (!is_solid(raised)) {
            Vector3 stepped_result = stepped_pos;
            bool sx = false, sz = false;
            resolve_axis(result_after_y, motion, size, 0, stepped_result, sx, is_solid);
            resolve_axis(result_after_y, motion, size, 2, stepped_result, sz, is_solid);

            float unstepped_d2 = (result.x - result_after_y.x) * (result.x - result_after_y.x)
                                + (result.z - result_after_y.z) * (result.z - result_after_y.z);
            float stepped_d2 = (stepped_result.x - result_after_y.x) * (stepped_result.x - result_after_y.x)
                              + (stepped_result.z - result_after_y.z) * (stepped_result.z - result_after_y.z);
            bool made_progress = stepped_d2 > unstepped_d2 + 0.0001f;

            if (made_progress) {
                AABB player_at_stepped(stepped_result, size);
                if (!is_solid(player_at_stepped)) {
                    Vector3 settle_result = stepped_result;
                    bool settled = false;
                    resolve_axis(stepped_result, Vector3(0.0f, -step_height, 0.0f), size, 1,
                                 settle_result, settled, is_solid);

                    out.position = settle_result + half_xz;
                    out.collided_x = sx;
                    out.collided_z = sz;
                    out.stepped_up = true;
                    AABB stepped_floor(settle_result, size);
                    stepped_floor.position.y -= 0.05f;
                    out.on_floor = is_solid(stepped_floor);
                    return out;
                }
            }
        }
    }

    out.position = result + half_xz;
    return out;
}

bool CollisionResolver::is_aabb_solid_fast(const AABB& aabb) const {
    if (!chunk_map_) return false;
    const BlockRegistry& registry = BlockRegistry::get_instance();
    int32_t min_x = static_cast<int32_t>(std::floor(aabb.position.x));
    int32_t min_y = static_cast<int32_t>(std::floor(aabb.position.y));
    int32_t min_z = static_cast<int32_t>(std::floor(aabb.position.z));
    int32_t max_x = static_cast<int32_t>(std::floor(aabb.position.x + aabb.size.x));
    int32_t max_y = static_cast<int32_t>(std::floor(aabb.position.y + aabb.size.y));
    int32_t max_z = static_cast<int32_t>(std::floor(aabb.position.z + aabb.size.z));

    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t z = min_z; z <= max_z; ++z) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                BlockID bid = static_cast<BlockID>(chunk_map_->get_block_world_fast(x, y, z));
                if (bid == BlockIDs::AIR) continue;
                const BlockType& bt = registry.get_block_fast(bid);
                // A liquid's shape is a surface height, not a wall: a body swims
                // through it. Skipping before the full-cube test covers both
                // registries, since the built-in defaults describe water as a
                // full cube while the JSON gives it a lowered shape.
                if (!bt.stops_bodies()) continue;
                if (bt.is_full_cube()) return true;
                // A neighbour-dependent shape collides only with the parts that
                // are present: an isolated fence post is a post, a run is a wall.
                // Shapes without parts borrow their static list, so the common
                // path costs no resolution at all.
                CollisionShapeContext shape_ctx{chunk_map_, x, y, z};
                ShapeBoxes collision_boxes;
                resolve_shape_boxes(bt, registry,
                                    ShapeNeighborFn{&collision_shape_neighbor, &shape_ctx},
                                    ShapeBoxKind::Collision, collision_boxes);
                for (uint8_t bi = 0; bi < collision_boxes.count(); ++bi) {
                    const BlockAABB& box = collision_boxes[bi];
                    AABB cell_aabb(
                        Vector3(x + box.min[0], y + box.min[1], z + box.min[2]),
                        Vector3(box.max[0] - box.min[0], box.max[1] - box.min[1], box.max[2] - box.min[2]));
                    if (aabb.intersects(cell_aabb)) return true;
                }
            }
        }
    }
    return false;
}

bool CollisionResolver::is_solid_at(int32_t wx, int32_t wy, int32_t wz) const {
    if (!chunk_map_) return false;
    const BlockID id = static_cast<BlockID>(chunk_map_->get_block_world(wx, wy, wz));
    if (id == BlockIDs::AIR) return false;
    // "Solid" here means "stops a body", not "is not air": both callers want to
    // know what the body collides with (the sneak edge-guard above a drop and the
    // third-person camera's clear distance), and a liquid is neither footing nor
    // a camera wall.
    return BlockRegistry::get_instance().get_block(id).stops_bodies();
}

bool CollisionResolver::is_liquid_at(int32_t wx, int32_t wy, int32_t wz) const {
    if (!chunk_map_) return false;
    const BlockID id = static_cast<BlockID>(chunk_map_->get_block_world(wx, wy, wz));
    if (id == BlockIDs::AIR) return false;
    return BlockRegistry::get_instance().get_block(id).is_liquid();
}

float CollisionResolver::get_slipperiness_at(int32_t wx, int32_t wy, int32_t wz) const {
    if (!chunk_map_) return 0.6f;
    BlockID block = static_cast<BlockID>(chunk_map_->get_block_world(wx, wy, wz));
    return BlockRegistry::get_instance().get_block(block).slipperiness;
}

} // namespace VoxelEngine
