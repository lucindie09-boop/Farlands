#include "mesh/mesh_manager_internal.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include <godot_cpp/classes/rendering_server.hpp>

namespace VoxelEngine {

using namespace godot;
void MeshManager::clear() {
    mesh_queue.clear();
    // The world (or its seed) is going away: the memoised "would be solid"
    // answers described the old one.
    solid_probe_cache_.clear();
    // Stop, drain and free in-flight region uploads while the RenderingServer
    // is still up. Without this, an upload finishing after the world reset
    // publishes into a region that no longer exists.
    shutdown_region_uploads();
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
    active_far_detail_chunks_.clear();
    far_visibility_keys_.clear();
    far_visibility_cursor_ = 0;
    far_visibility_pending_ = false;
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
    mark_far_region_dirty_for_chunk(cx, cy, cz, std::shared_ptr<CachedFarChunkMesh>{});
}


void MeshManager::mark_all_chunks_dirty() {
    if (!chunk_map) return;
    // The queue entry is built from the VISITED chunk rather than by calling
    // queue_dirty_chunk(): that would re-look the same chunk up through the
    // LOCKING accessor, a nested shared acquisition on a shard this thread already
    // holds, which SRW answers by blocking forever once a writer is queued on it.
    // Running the walk under lock_all() did not make that safe, only rare (any
    // queued chunk insert is enough). Everything queue_dirty_chunk() needs is
    // already in hand here: the key, the render data, and the far-region flag.
    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (!render_data) return;
        render_data->is_mesh_dirty = true;
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        if (render_data->far_mesh_cache) {
            mark_far_region_dirty_for_chunk(cx, cy, cz, render_data->far_mesh_cache);
        }
        const int32_t dx = cx - last_player_chunk_x;
        const int32_t dy = cy - last_player_chunk_y;
        const int32_t dz = cz - last_player_chunk_z;
        mesh_queue.queue_dirty_chunk(key, dx * dx + dy * dy + dz * dz, false);
    });
}

bool MeshManager::has_pending_mesh_work() const {
    bool far_region_dirty = false;
    for (const auto& entry : far_regions) {
        const FarRegionRenderData& region = entry.second;
        if (region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0 ||
            region.pending_uploads > 0) {
            far_region_dirty = true;
            break;
        }
    }
    // The incremental far-visibility pass is part of the initial fill too. If it
    // still has members to walk, the world is not quiet even when the mesh queues
    // are empty; the startup latch must not switch back to idle budgets yet.
    if (far_visibility_pending_) return true;
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
    RenderingServer* rs = RenderingServer::get_singleton();

    // Active membership WITHOUT the target-detail check: a full-detail chunk
    // can still be part of the previous region mesh during the debounced
    // handoff. This is the predicate that tells whether the frame can be
    // drawing that chunk twice.
    std::unordered_set<uint64_t> visible_far_region_keys;
    auto active_region_contains = [this](int32_t cx, int32_t cy, int32_t cz) -> bool {
        const auto it = far_regions.find(get_far_region_key(cx, cy, cz));
        if (it == far_regions.end() || !it->second.active) {
            return false;
        }
        const uint64_t key = chunk_map->get_chunk_key(cx, cy, cz);
        const auto& members = it->second.active_chunk_keys;
        return std::find(members.begin(), members.end(), key) != members.end();
    };

    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        if (should_use_far_region_for_chunk(cx, cy, cz)) {
            ++stats.eligible_far_chunks;
            if (render_data->far_mesh_cache) {
                ++stats.cached_far_chunks;
            }
        }

        // The liquid invariant, checked against the live state rather than at
        // upload time (see note_liquid_geometry). Non-zero means water is missing
        // somewhere; liquid_geometry_repairs is how many remeshes that took.
        if (render_data->data && render_data->data->liquid_count() > 0 &&
            render_data->uploaded_solid_vertices > 0 &&
            render_data->uploaded_water_vertices == 0) {
            ++stats.chunks_with_liquid_but_no_water_mesh;
        }

        // Geometry on the GPU that nothing draws: within the render distance, not
        // covered by a far region, and with no instance of its own. The far-region
        // handoff is the only thing that can hide a chunk legitimately, and it
        // does so as a member of an active region — so this should be 0 too.
        if (is_chunk_within_render_distance(cx, cy, cz) &&
            !is_far_region_active_for_chunk(cx, cy, cz) &&
            !render_data->instance_rid.is_valid() &&
            (render_data->uploaded_solid_vertices > 0 || render_data->uploaded_water_vertices > 0)) {
            ++stats.chunks_with_geometry_but_no_instance;
        }

        if (render_data->instance_visible) {
            ++stats.visible_chunk_instances_actual;
            const float uploaded = render_data->uploaded_detail_level;
            if (uploaded >= 0.75f) {
                ++stats.visible_chunk_full;
            } else if (uploaded >= 0.375f) {
                ++stats.visible_chunk_mid;
            } else if (uploaded > 0.0f) {
                ++stats.visible_chunk_far;
            } else {
                ++stats.visible_chunk_detail_unknown;
            }
            const bool region_covered = active_region_contains(cx, cy, cz);
            if (region_covered) {
                ++stats.visible_chunk_region_overlap;
            }
            if (uploaded > 0.0f && uploaded < 0.375f) {
                visible_far_region_keys.insert(get_far_region_key(cx, cy, cz));
                const auto region_it = far_regions.find(get_far_region_key(cx, cy, cz));
                if (region_it == far_regions.end()) {
                    ++stats.visible_far_no_region;
                } else if (region_it->second.active) {
                    if (!region_covered) {
                        ++stats.visible_far_active_uncovered;
                    }
                } else if (region_it->second.pending_builds.load(std::memory_order_relaxed) > 0) {
                    ++stats.visible_far_pending_region;
                } else if (region_it->second.dirty) {
                    ++stats.visible_far_debounced_region;
                } else {
                    ++stats.visible_far_inactive_region;
                }
            }
            const float target = compute_chunk_detail_level(cx, cy, cz);
            if (uploaded > 0.0f && std::fabs(uploaded - target) > 0.01f) {
                ++stats.visible_chunk_detail_mismatch;
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
            if (region.active && rs != nullptr && rs->mesh_get_surface_count(region.mesh_rid) == 0) {
                ++stats.far_region_meshes_without_surfaces;
            }
        }
        if (region.instance_rid.is_valid() && region.active) {
            ++stats.visible_instances;
            ++stats.far_region_instances;
            stats.active_region_member_chunks += static_cast<int32_t>(region.active_chunk_keys.size());
            for (uint64_t member_key : region.active_chunk_keys) {
                int32_t cx, cy, cz;
                ChunkMap::decode_chunk_key(member_key, cx, cy, cz);
                const float target = compute_chunk_detail_level(cx, cy, cz);
                if (target >= 0.75f) {
                    ++stats.region_member_full;
                } else if (target >= 0.375f) {
                    ++stats.region_member_mid;
                } else if (target > 0.0f) {
                    ++stats.region_member_far;
                } else {
                    ++stats.region_member_unknown;
                }
            }
        }
    }

    stats.visible_far_region_keys = static_cast<int32_t>(visible_far_region_keys.size());
    stats.regions_partial_missing_cache = far_regions_partial_missing_cache_last;
    stats.liquid_geometry_repairs = liquid_geometry_repairs;
    stats.mesh_uploads = mesh_uploads;
    stats.mesh_upload_dedup_skips = mesh_upload_dedup_skips;
    stats.mesh_upload_swallowed_water_changes = mesh_upload_swallowed_water_changes;

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
