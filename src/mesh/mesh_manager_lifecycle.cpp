#include "mesh/mesh_manager_internal.hpp"

#include <algorithm>

namespace VoxelEngine {

using namespace godot;
void MeshManager::clear() {
    mesh_queue.clear();
    for (auto& entry : far_regions) {
        FarRegionRenderData& region = entry.second;
        free_far_region_resources(region);
    }
    far_regions.clear();
    {
        std::lock_guard<std::mutex> lock(completed_far_region_meshes_mutex);
        while (!completed_far_region_meshes.empty()) {
            completed_far_region_meshes.pop();
        }
    }
    completed_far_region_mesh_count.store(0, std::memory_order_relaxed);
    active_full_detail_chunks_.clear();
    far_regions_partial_missing_cache_last = 0;
    last_player_chunk_x = INT32_MIN;
    last_player_chunk_y = INT32_MIN;
    last_player_chunk_z = INT32_MIN;
    last_player_block_x = INT32_MIN;
    last_player_block_y = INT32_MIN;
    last_player_block_z = INT32_MIN;
}

void MeshManager::notify_chunk_unloaded(int32_t cx, int32_t cy, int32_t cz, const ChunkRenderData* render_data) {
    if (!render_data || !render_data->far_mesh_cache) {
        return;
    }
    const uint64_t region_key = get_far_region_key(cx, cy, cz);
    auto it = far_regions.find(region_key);
    if (it != far_regions.end()) {
        ensure_far_region_instance(it->second, region_key, false);
        sync_far_region_members_visibility(it->second);
    }
    mark_far_region_dirty_for_chunk(cx, cy, cz);
}


void MeshManager::mark_all_chunks_dirty() {
if (!chunk_map) return;
chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
if (render_data->data && !render_data->data->is_all_air()) {
}
render_data->is_mesh_dirty = true;
int32_t cx, cy, cz;
ChunkMap::decode_chunk_key(key, cx, cy, cz);
queue_dirty_chunk(cx, cy, cz);
});
}

bool MeshManager::has_pending_mesh_work() const {
    bool far_region_dirty = false;
    for (const auto& entry : far_regions) {
        const FarRegionRenderData& region = entry.second;
        if (region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            far_region_dirty = true;
            break;
        }
    }
    if (!chunk_scheduler) {
        return mesh_queue.size() > 0 ||
               mesh_queue.immediate_size() > 0 ||
               far_region_dirty ||
               completed_far_region_mesh_count.load(std::memory_order_relaxed) > 0;
    }
    return chunk_scheduler->completed_mesh_count() > 0 ||
           mesh_queue.size() > 0 ||
           mesh_queue.immediate_size() > 0 ||
           far_region_dirty ||
           completed_far_region_mesh_count.load(std::memory_order_relaxed) > 0;
}

WorldRenderStats MeshManager::gather_render_stats() {
    WorldRenderStats stats;
    if (!chunk_map) {
        return stats;
    }

    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        if (should_use_far_region_for_chunk(cx, cy, cz)) {
            ++stats.eligible_far_chunks;
            if (render_data->far_mesh_cache) {
                ++stats.cached_far_chunks;
            }
        }

        if (render_data->mesh_rid.is_valid()) {
            ++stats.mesh_rids;
            ++stats.chunk_mesh_rids;
        }
        if (!render_data->instance_rid.is_valid()) {
            return;
        }
        if (!is_chunk_within_render_distance(cx, cy, cz) ||
            is_far_region_active_for_chunk(cx, cy, cz)) {
            return;
        }
        if (render_data->mesh_rid.is_valid()) {
            ++stats.visible_instances;
            ++stats.chunk_instances;
        }
    });

    for (const auto& entry : far_regions) {
        const FarRegionRenderData& region = entry.second;
        if (region.mesh_rid.is_valid()) {
            ++stats.mesh_rids;
            ++stats.far_region_mesh_rids;
        }
        if (region.instance_rid.is_valid() && region.active) {
            ++stats.visible_instances;
            ++stats.far_region_instances;
            stats.active_region_member_chunks += static_cast<int32_t>(region.active_chunk_keys.size());
        }
    }

    stats.regions_partial_missing_cache = far_regions_partial_missing_cache_last;

    return stats;
}

float MeshManager::compute_chunk_detail_level(int32_t cx, int32_t cy, int32_t cz) const {
    if (lod_distance <= 0) return 1.0f;
    if (last_player_chunk_x == INT32_MIN) return 1.0f;
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});

    // Two-tier LOD: chunks within lod_distance+1 are always full-res.
    // The +1 ring acts as a skirt so that the first coarse ring (dist =
    // lod_distance+2) always borders a stride-1 neighbor, preventing
    // T-junction cracks at the transition boundary.
    // TODO: if more LOD tiers are added, replace with resolved-stride
    // propagation (compute nearest-first, store in ChunkRenderData, check
    // neighbor's actual resolved stride instead of recomputing from distance).
    if (dist <= lod_distance + 1) return 1.0f;

    // Third tier: beyond lod_far_distance, chunks drop to the far-mode
    // heightmap-only mesh (see is_chunk_far_mode / MeshBuilder::set_far_mode).
    if (lod_far_distance > lod_distance + 1 && dist > lod_far_distance) {
        return far_detail_level;
    }

    return lod_detail_level;
}

bool MeshManager::is_chunk_far_mode(int32_t cx, int32_t cy, int32_t cz) const {
    if (lod_far_distance <= 0 || lod_distance <= 0 || last_player_chunk_x == INT32_MIN) {
        return false;
    }
    int32_t dx = cx - last_player_chunk_x;
    int32_t dy = cy - last_player_chunk_y;
    int32_t dz = cz - last_player_chunk_z;
    int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
    return dist > lod_far_distance;
}

} // namespace VoxelEngine
