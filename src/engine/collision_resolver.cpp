#include "engine/collision_resolver.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_coords.hpp"
#include "core/block_types.hpp"
#include <cmath>
#include <algorithm>
#include <vector>

namespace VoxelEngine {

using namespace godot;

namespace {

// Shared-lock only the shards of the chunks intersecting a block-space box,
// instead of the whole map. Collision probes only ever touch the swept volume
// of the player's body, so the key set is tiny (usually 1-8 chunks).
std::vector<uint64_t> chunk_keys_for_box(const ChunkMap* chunk_map,
                                         const godot::Vector3& box_lo,
                                         const godot::Vector3& box_hi) {
    int32_t min_cx, min_cy, min_cz, max_cx, max_cy, max_cz;
    int32_t dummy;
    world_to_chunk_local(static_cast<int32_t>(std::floor(box_lo.x)),
                         static_cast<int32_t>(std::floor(box_lo.y)),
                         static_cast<int32_t>(std::floor(box_lo.z)),
                         min_cx, min_cy, min_cz, dummy, dummy, dummy);
    world_to_chunk_local(static_cast<int32_t>(std::floor(box_hi.x)),
                         static_cast<int32_t>(std::floor(box_hi.y)),
                         static_cast<int32_t>(std::floor(box_hi.z)),
                         max_cx, max_cy, max_cz, dummy, dummy, dummy);
    std::vector<uint64_t> keys;
    keys.reserve(static_cast<size_t>(max_cx - min_cx + 1) *
                 static_cast<size_t>(max_cy - min_cy + 1) *
                 static_cast<size_t>(max_cz - min_cz + 1));
    for (int32_t cx = min_cx; cx <= max_cx; ++cx)
        for (int32_t cy = min_cy; cy <= max_cy; ++cy)
            for (int32_t cz = min_cz; cz <= max_cz; ++cz)
                keys.push_back(chunk_map->get_chunk_key(cx, cy, cz));
    return keys;
}

} // namespace

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

    // Floor probe: check final resolved position only (matches vanilla onGround)
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
                if (bt.is_full_cube()) return true;
                for (const auto& box : bt.selection_boxes) {
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

bool CollisionResolver::is_aabb_solid(const AABB& aabb) const {
    constexpr float kPad = 1.0f;
    auto lock = chunk_map_->lock_keys(chunk_keys_for_box(
        chunk_map_, aabb.position - Vector3(kPad, kPad, kPad),
        aabb.position + aabb.size + Vector3(kPad, kPad, kPad)));
    return is_aabb_solid_fast(aabb);
}

bool CollisionResolver::is_solid_at(int32_t wx, int32_t wy, int32_t wz) const {
    if (!chunk_map_) return false;
    return chunk_map_->is_block_solid(wx, wy, wz);
}

float CollisionResolver::get_slipperiness_at(int32_t wx, int32_t wy, int32_t wz) const {
    if (!chunk_map_) return 0.6f;
    BlockID block = static_cast<BlockID>(chunk_map_->get_block_world(wx, wy, wz));
    return BlockRegistry::get_instance().get_block(block).slipperiness;
}

} // namespace VoxelEngine
