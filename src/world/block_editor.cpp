#include "world/block_editor.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "core/block_types.hpp"
#include <godot_cpp/core/class_db.hpp>
#include <algorithm>
#include <vector>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

namespace {
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

BlockEditor::BlockEditor(ChunkWorld* cw, MeshManager* mm, LightPropagator* lp)
    : chunk_world(cw), mesh_manager(mm), light_propagator(lp) {}

int BlockEditor::query_block(int32_t world_x, int32_t world_y, int32_t world_z) const {
    return chunk_world->get_block_world(world_x, world_y, world_z);
}

String BlockEditor::get_block_name(int block_id) const {
    const auto& block = BlockRegistry::get_instance().get_block(static_cast<BlockID>(block_id));
    return String(block.name);
}

void BlockEditor::place_block(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);

    ChunkMap& cm = chunk_world->get_chunk_map();

    // Quick check: if chunk not loaded, queue pending placement and bail.
    // Uses shared lock only — safe before exclusive scope.
    {
        auto sl = cm.lock_chunk(chunk_x, chunk_y, chunk_z);
        if (!cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z)) {
            chunk_world->queue_pending_placement(world_x, world_y, world_z, static_cast<int>(new_block));
            return;
        }
    }

    // Track mud variant chunk for post-lock edit map write.
    int32_t mud_cx = 0, mud_cy = 0, mud_cz = 0;
    int32_t mud_lx = 0, mud_ly = 0, mud_lz = 0;
    BlockID mud_variant = BlockIDs::AIR;
    bool did_mud = false;

    // Calculate mud variant local coordinates once (world_to_chunk_local needs chunk coords)
    int32_t below_world_y = world_y - 1;
    int32_t bw_cx, bw_cy, bw_cz;
    world_to_chunk_local(world_x, below_world_y, world_z, bw_cx, bw_cy, bw_cz, mud_lx, mud_ly, mud_lz);

    {
        // Block change + light propagation can reach at most 1 chunk in each
        // direction (max light level 15 < chunk size 32). Lock 3×3×3 around
        // the center chunk — covers block write, light BFS, and mud variant.
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = cm.get_chunk_key(chunk_x + dx, chunk_y + dy, chunk_z + dz);
        auto lock = cm.lock_keys_exclusive(keys);
        ChunkData* chunk_data = cm.get_chunk_data_fast(chunk_x, chunk_y, chunk_z);
        if (!chunk_data) return;
        if (!is_local_in_bounds(local_x, local_y, local_z)) return;

        const BlockID old_block = chunk_data->get_block_unsafe(local_x, local_y, local_z);
        if (old_block != BlockIDs::AIR && new_block != BlockIDs::AIR) return;
        if (old_block == new_block) return;

        const BlockRegistry& registry = BlockRegistry::get_instance();
        const BlockType& old_type = registry.get_block(old_block);
        const BlockType& new_type = registry.get_block(new_block);
        const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
        const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);

        const uint8_t old_r = chunk_data->get_light_r(local_x, local_y, local_z);
        const uint8_t old_g = chunk_data->get_light_g(local_x, local_y, local_z);
        const uint8_t old_b = chunk_data->get_light_b(local_x, local_y, local_z);

        chunk_data->set_block(local_x, local_y, local_z, new_block);

        if (old_opaque != new_opaque) {
            ChunkData* above = cm.get_chunk_data_fast(chunk_x, chunk_y + 1, chunk_z);
            chunk_data->propagate_sky_light_column(local_x, local_z, above);
        }

        light_propagator->update_block_light_incremental_locked(
            chunk_x, chunk_y, chunk_z, chunk_x, chunk_y, chunk_z,
            local_x, local_y, local_z,
            old_block, new_block, old_r, old_g, old_b);

        ChunkRenderData* render_data = cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z);
        if (render_data) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(local_x, local_y, local_z));
            render_data->mark_block_dirty(local_x, local_y, local_z);
        }

        // Mud variant: inline update_mud_variants logic while lock is held.
        if (world_y > 0) {
            ChunkData* below_chunk = cm.get_chunk_data_fast(bw_cx, bw_cy, bw_cz);
            if (below_chunk && is_local_in_bounds(mud_lx, mud_ly, mud_lz)) {
                const BlockID below = below_chunk->get_block_unsafe(mud_lx, mud_ly, mud_lz);
                BlockID variant = BlockIDs::AIR;
                if (new_block != BlockIDs::AIR) {
                    if (below == BlockIDs::MUD) variant = BlockIDs::MUD_FULL;
                    else if (below == BlockIDs::WET_SAND) variant = BlockIDs::WET_SAND_FULL;
                } else {
                    if (below == BlockIDs::MUD_FULL) variant = BlockIDs::MUD;
                    else if (below == BlockIDs::WET_SAND_FULL) variant = BlockIDs::WET_SAND;
                }
                if (variant != BlockIDs::AIR && below != variant) {
                    below_chunk->set_block(mud_lx, mud_ly, mud_lz, variant);
                    ChunkRenderData* bw_rd = cm.get_chunk_render_data_fast(bw_cx, bw_cy, bw_cz);
                    if (bw_rd) {
                        bw_rd->is_mesh_dirty = true;
                        bw_rd->mesh_version++;
                        bw_rd->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(mud_lx, mud_ly, mud_lz));
                        bw_rd->mark_block_dirty(mud_lx, mud_ly, mud_lz);
                    }
                    mud_cx = bw_cx;
                    mud_cy = bw_cy;
                    mud_cz = bw_cz;
                    mud_variant = variant;
                    did_mud = true;
                }
            }
        }
    }
    // Lock released — auto-locking accessors are safe now.

    // Persist the edit in the edit map instead of marking the whole chunk dirty
    chunk_world->add_block_edit(chunk_x, chunk_y, chunk_z, local_x, local_y, local_z, new_block);
    // A block change (and any light removal) can touch every chunk in the
    // 3×3×3 neighborhood — including diagonals when a light block near a chunk
    // corner spills light across two boundaries at once. Bump mesh_version on
    // the whole neighborhood so rebuild_rendering_server_mesh's version gate
    // lets every affected chunk remesh (the old queue_player_edit_chunk_refresh
    // only set is_mesh_dirty, which the gate silently discarded).
    mesh_manager->mark_chunks_dirty_for_light(chunk_x, chunk_y, chunk_z);
    if (did_mud) {
        // Persist the mud variant edit as well
        chunk_world->add_block_edit(mud_cx, mud_cy, mud_cz, mud_lx, mud_ly, mud_lz, mud_variant);
        mesh_manager->queue_dirty_chunk(mud_cx, mud_cy, mud_cz);
    }
}

Dictionary BlockEditor::raycast(godot::Node* chunk_manager, const NodePath& player_path, double max_distance) const {
    Dictionary result;
    result["success"] = false;

    if (player_path.is_empty()) return result;
    Node* player_node = chunk_manager->get_node_or_null(player_path);
    if (!player_node) return result;
    Node3D* player = Object::cast_to<Node3D>(player_node);
    if (!player) return result;

    Camera3D* camera = nullptr;
    if (!player_path.is_empty()) {
        if (player_path != last_camera_path) {
            last_camera_path = player_path;
            cached_camera = Object::cast_to<Camera3D>(player->get_node_or_null(NodePath("Camera3D")));
        }
        camera = cached_camera;
    }
    if (!camera) return result;

    Vector3 ray_origin = camera->get_global_position();
    Vector3 ray_dir = -camera->get_global_transform().basis.get_column(2).normalized();

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
    const int32_t end_x = static_cast<int32_t>(std::floor(ray_origin.x + ray_dir.x * (max_distance * 3.0 + 2.0)));
    const int32_t end_y = static_cast<int32_t>(std::floor(ray_origin.y + ray_dir.y * (max_distance * 3.0 + 2.0)));
    const int32_t end_z = static_cast<int32_t>(std::floor(ray_origin.z + ray_dir.z * (max_distance * 3.0 + 2.0)));

    std::vector<uint64_t> keys;
    {
        int32_t min_cx, min_cy, min_cz, max_cx, max_cy, max_cz, dummy;
        world_to_chunk_local(std::min(current_x, end_x), std::min(current_y, end_y), std::min(current_z, end_z),
                             min_cx, min_cy, min_cz, dummy, dummy, dummy);
        world_to_chunk_local(std::max(current_x, end_x), std::max(current_y, end_y), std::max(current_z, end_z),
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
                result["success"] = true;
                result["position"] = Vector3(current_x, current_y, current_z);
                result["place_position"] = Vector3(prev_x, prev_y, prev_z);
                result["block_id"] = static_cast<int>(bid);
                return result;
            }
            // Non-full block: test ray against each selection_box
            bool hit_any = false;
            double closest_t = max_distance;
            Vector3 hit_normal;
            for (const auto& box : bt.selection_boxes) {
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
                result["success"] = true;
                result["position"] = Vector3(current_x, current_y, current_z);
                result["place_position"] = Vector3(current_x, current_y, current_z) + hit_normal;
                result["block_id"] = static_cast<int>(bid);
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

// -------------------------------------------------------------------------
// Internal helpers
// -------------------------------------------------------------------------

void BlockEditor::update_mud_variants(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    if (world_y <= 0) return;
    const BlockID below = static_cast<BlockID>(chunk_world->get_block_world(world_x, world_y - 1, world_z));
    if (new_block != BlockIDs::AIR) {
        if (below == BlockIDs::MUD) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::MUD_FULL);
        } else if (below == BlockIDs::WET_SAND) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::WET_SAND_FULL);
        }
    } else {
        if (below == BlockIDs::MUD_FULL) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::MUD);
        } else if (below == BlockIDs::WET_SAND_FULL) {
            set_block_variant(world_x, world_y - 1, world_z, BlockIDs::WET_SAND);
        }
    }
}

void BlockEditor::post_block_change(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    update_mud_variants(world_x, world_y, world_z, new_block);
}

void BlockEditor::set_block_variant(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);

    ChunkMap& cm = chunk_world->get_chunk_map();
    {
        uint64_t key = cm.get_chunk_key(chunk_x, chunk_y, chunk_z);
        auto lock = cm.lock_keys_exclusive({key});
        ChunkData* chunk_data = cm.get_chunk_data_fast(chunk_x, chunk_y, chunk_z);
        if (!chunk_data) return;
        if (!is_local_in_bounds(local_x, local_y, local_z)) return;
        const BlockID old_block = chunk_data->get_block_unsafe(local_x, local_y, local_z);
        if (old_block == block_id) return;
        chunk_data->set_block(local_x, local_y, local_z, block_id);
        ChunkRenderData* render_data = cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z);
        if (render_data) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(local_x, local_y, local_z));
            render_data->mark_block_dirty(local_x, local_y, local_z);
        }
    }
    chunk_world->mark_chunk_dirty(chunk_x, chunk_y, chunk_z);
    mesh_manager->queue_dirty_chunk(chunk_x, chunk_y, chunk_z);
}

} // namespace VoxelEngine
