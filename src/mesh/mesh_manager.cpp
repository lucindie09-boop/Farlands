#include "mesh/mesh_manager_internal.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

using namespace godot;
void MeshManager::set_player_chunk(int32_t cx, int32_t cy, int32_t cz) {
    last_player_chunk_x = cx;
    last_player_chunk_y = cy;
    last_player_chunk_z = cz;
    refresh_far_region_visibility();
}

void MeshManager::hide_chunk_instance(ChunkRenderData* render_data) {
    if (!render_data || !render_data->instance_rid.is_valid()) {
        return;
    }
    RenderingServer::get_singleton()->instance_set_visible(render_data->instance_rid, false);
}

void MeshManager::show_chunk_instance(ChunkRenderData* render_data, int32_t cx, int32_t cy, int32_t cz) {
    if (!render_data || !render_data->mesh_rid.is_valid()) {
        return;
    }

    RenderingServer* rs = RenderingServer::get_singleton();
    if (!render_data->instance_rid.is_valid()) {
        render_data->instance_rid = rs->instance_create();
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
        rs->instance_geometry_set_cast_shadows_setting(
            render_data->instance_rid,
            RenderingServer::SHADOW_CASTING_SETTING_OFF);
        rs->instance_set_custom_aabb(
            render_data->instance_rid,
            AABB(Vector3(0, 0, 0), Vector3(CHUNK_WIDTH, CHUNK_HEIGHT, CHUNK_DEPTH)));
        Transform3D transform;
        transform.origin = Vector3(cx * CHUNK_WIDTH, cy * CHUNK_HEIGHT, cz * CHUNK_DEPTH);
        rs->instance_set_transform(render_data->instance_rid, transform);
        if (owner) {
            Node3D* owner3d = Object::cast_to<Node3D>(owner);
            if (owner3d) {
                Ref<World3D> world = owner3d->get_world_3d();
                if (world.is_valid()) {
                    rs->instance_set_scenario(render_data->instance_rid, world->get_scenario());
                }
            }
        }
    } else {
        rs->instance_set_base(render_data->instance_rid, render_data->mesh_rid);
    }
    rs->instance_set_visible(render_data->instance_rid, true);
}

void MeshManager::queue_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist_sq = dx * dx + dy * dy + dz * dz;
    mesh_queue.queue_dirty_chunk(chunk_map->get_chunk_key(cx, cy, cz), dist_sq, false);
}

void MeshManager::queue_immediate_dirty_chunk(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (render_data) {
        render_data->is_mesh_dirty = true;
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz);
        }
    }
    uint64_t key = chunk_map->get_chunk_key(cx, cy, cz);
    mesh_queue.queue_immediate_dirty_chunk(key, mesh_queue.is_pending(key));
}

void MeshManager::mark_chunk_urgent(int32_t cx, int32_t cy, int32_t cz) {
    if (!chunk_map) return;
    mesh_queue.mark_urgent(chunk_map->get_chunk_key(cx, cy, cz));
}

void MeshManager::reprioritize(int32_t player_cx, int32_t player_cy, int32_t player_cz, const Frustum* frustum) {
    mesh_queue.reprioritize(player_cx, player_cy, player_cz, frustum);

    if (lod_distance <= 0 || !chunk_map) {
        refresh_far_region_visibility();
        return;
    }

    const int32_t lod = lod_distance;
    const int32_t lod_shell_min = lod > 0 ? lod - 1 : 0;
    const int32_t lod_shell_max = lod + 1;
    const int32_t vert_range = 10;

    int32_t queued = 0;
    constexpr int32_t kMaxLodRemeshPerFrame = 128;

    for (int32_t dx = -lod_shell_max; dx <= lod_shell_max && queued < kMaxLodRemeshPerFrame; ++dx) {
        for (int32_t dz = -lod_shell_max; dz <= lod_shell_max && queued < kMaxLodRemeshPerFrame; ++dz) {
            for (int32_t dy = -vert_range; dy <= vert_range && queued < kMaxLodRemeshPerFrame; ++dy) {
                int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                if (dist < lod_shell_min || dist > lod_shell_max) continue;

                int32_t cx = player_cx + dx;
                int32_t cy = player_cy + dy;
                int32_t cz = player_cz + dz;

                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                if (!render_data) continue;

                float target = compute_chunk_detail_level(cx, cy, cz);
                if (target != render_data->last_built_detail_level && !render_data->is_mesh_dirty) {
                    render_data->is_mesh_dirty = true;
                    render_data->mesh_version++;
                    mark_far_region_dirty_for_chunk(cx, cy, cz);
                    queue_dirty_chunk(cx, cy, cz);
                    ++queued;
                }
            }
        }
    }

    // Far shell: chunks crossing the third-tier boundary (dist ==
    // lod_far_distance..lod_far_distance+1) must switch between the mid-LOD
    // mesh and the far-mode heightmap-only mesh. The distance classifier only
    // crosses a shell ring when the player moves one chunk, so scanning these
    // two rings keeps the far tier in sync without a full-world sweep.
    if (lod_far_distance > lod + 1) {
        const int32_t far_shell_min = lod_far_distance;
        const int32_t far_shell_max = lod_far_distance + 1;
        for (int32_t dx = -far_shell_max; dx <= far_shell_max && queued < kMaxLodRemeshPerFrame; ++dx) {
            for (int32_t dz = -far_shell_max; dz <= far_shell_max && queued < kMaxLodRemeshPerFrame; ++dz) {
                for (int32_t dy = -vert_range; dy <= vert_range && queued < kMaxLodRemeshPerFrame; ++dy) {
                    const int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                    if (dist < far_shell_min || dist > far_shell_max) continue;

                    const int32_t cx = player_cx + dx;
                    const int32_t cy = player_cy + dy;
                    const int32_t cz = player_cz + dz;

                    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                    if (!render_data) continue;

                    const float target = compute_chunk_detail_level(cx, cy, cz);
                    const bool target_far_mode = is_chunk_far_mode(cx, cy, cz);
                    if ((target != render_data->last_built_detail_level || target_far_mode != render_data->last_built_far_mode) &&
                        !render_data->is_mesh_dirty) {
                        render_data->is_mesh_dirty = true;
                        render_data->mesh_version++;
                        mark_far_region_dirty_for_chunk(cx, cy, cz);
                        queue_dirty_chunk(cx, cy, cz);
                        ++queued;
                    }
                }
            }
        }
    }

    // Downgrade chunks that were built at full detail but are now beyond LOD threshold.
    for (auto it = active_full_detail_chunks_.begin(); it != active_full_detail_chunks_.end() && queued < kMaxLodRemeshPerFrame;) {
        uint64_t chunk_key = *it;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        float target = compute_chunk_detail_level(cx, cy, cz);
        if (target >= 1.0f) {
            ++it;
            continue;
        }
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (render_data && !render_data->is_mesh_dirty) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            mark_far_region_dirty_for_chunk(cx, cy, cz);
            queue_dirty_chunk(cx, cy, cz);
            ++queued;
        }
        it = active_full_detail_chunks_.erase(it);
    }

    refresh_far_region_visibility();
}

void MeshManager::mark_chunks_dirty_for_light(int32_t center_cx, int32_t center_cy, int32_t center_cz) {
    if (!chunk_map) return;
    for (int32_t dy = -1; dy <= 1; dy++) {
        for (int32_t dz = -1; dz <= 1; dz++) {
            for (int32_t dx = -1; dx <= 1; dx++) {
                const int32_t cx = center_cx + dx;
                const int32_t cy = center_cy + dy;
                const int32_t cz = center_cz + dz;
                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
                if (render_data) {
                    render_data->is_mesh_dirty = true;
                    render_data->mesh_version++;
                }
                queue_dirty_chunk(cx, cy, cz);
            }
        }
    }
}

void MeshManager::process_queue(int32_t max_immediate, int32_t max_rebuilds, double budget_ms) {
    if (max_rebuilds <= 0) {
        return;
    }
    int32_t far_region_budget = max_rebuilds / kFarRegionBuildDivisor;
    if (max_rebuilds > 1) {
        far_region_budget = std::max(1, far_region_budget);
    }
    far_region_budget = std::min(far_region_budget, max_rebuilds);
    const int32_t chunk_budget = std::max(0, max_rebuilds - far_region_budget);

    int32_t mesh_rd_sq = mesh_render_distance ? mesh_render_distance * mesh_render_distance : INT32_MAX;
    int32_t pcx = last_player_chunk_x;
    int32_t pcy = last_player_chunk_y;
    int32_t pcz = last_player_chunk_z;
        mesh_queue.process(
            [this, mesh_rd_sq, pcx, pcy, pcz](int32_t cx, int32_t cy, int32_t cz) {
                int32_t dx = cx - pcx;
                int32_t dy = cy - pcy;
                int32_t dz = cz - pcz;
                if (dx*dx + dz*dz > mesh_rd_sq || std::abs(dy) > 10) {
                    return;
                }
                rebuild_chunk_mesh(cx, cy, cz, async_epoch ? async_epoch->load(std::memory_order_acquire) : 0);
            },
        max_immediate,
        chunk_budget,
        budget_ms
    );
    process_far_region_queue(far_region_budget);
}

} // namespace VoxelEngine
