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
    active_mid_detail_chunks_.clear();
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

    // Three-tier LOD, each tier using the same stride-reduction mechanism:
    //   Tier 0 (full res): dist <= lod_distance + 1
    //   Tier 1 (mid):      lod_distance + 2 <= dist <= far_lod_distance + 1
    //   Tier 2 (far):      dist >= far_lod_distance + 2
    // Each tier keeps a +1 "skirt" ring at its render start (full res at
    // lod_distance+1, mid detail at far_lod_distance+1) so the first reduced
    // ring of a tier always borders a neighbor one detail step finer,
    // preventing T-junction cracks at the transition boundary.
    if (dist <= lod_distance + 1) return 1.0f;
    if (dist <= far_lod_distance + 1) return lod_detail_level;
    return far_lod_detail_level;
}

} // namespace VoxelEngine
