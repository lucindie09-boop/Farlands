#include "mesh/mesh_manager_internal.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <algorithm>

namespace VoxelEngine {

using namespace godot;
bool MeshManager::is_chunk_within_render_distance(int32_t cx, int32_t cy, int32_t cz) const {
    if (mesh_render_distance <= 0 || last_player_chunk_x == INT32_MIN) {
        return true;
    }
    const int32_t dx = cx - last_player_chunk_x;
    const int32_t dz = cz - last_player_chunk_z;
    const int32_t dy = std::abs(cy - last_player_chunk_y);
    return (dx * dx + dz * dz) <= (mesh_render_distance * mesh_render_distance) && dy <= 10;
}

bool MeshManager::should_use_far_region_for_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    return compute_chunk_detail_level(cx, cy, cz) < 1.0f && is_chunk_within_render_distance(cx, cy, cz);
}

uint64_t MeshManager::get_far_region_key(int32_t cx, int32_t cy, int32_t cz) const {
    if (!chunk_map) {
        return 0;
    }
    const int32_t rx = floor_div(cx, kFarRegionSizeXZ);
    const int32_t rz = floor_div(cz, kFarRegionSizeXZ);
    return chunk_map->get_chunk_key(rx, cy, rz);
}

bool MeshManager::is_far_region_active_for_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    if (!should_use_far_region_for_chunk(cx, cy, cz)) {
        return false;
    }
    const uint64_t chunk_key = chunk_map ? chunk_map->get_chunk_key(cx, cy, cz) : 0;
    const auto it = far_regions.find(get_far_region_key(cx, cy, cz));
    if (it == far_regions.end() || !it->second.active) {
        return false;
    }
    const auto& members = it->second.active_chunk_keys;
    return std::find(members.begin(), members.end(), chunk_key) != members.end();
}

void MeshManager::free_far_region_resources(FarRegionRenderData& region) {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (region.instance_rid.is_valid()) {
        rs->free_rid(region.instance_rid);
        region.instance_rid = RID();
    }
    if (region.mesh_rid.is_valid()) {
        rs->free_rid(region.mesh_rid);
        region.mesh_rid = RID();
    }
    region.active = false;
}

void MeshManager::ensure_far_region_instance(FarRegionRenderData& region, uint64_t region_key, bool visible) {
    RenderingServer* rs = RenderingServer::get_singleton();
    if (!visible || !region.mesh_rid.is_valid()) {
        if (region.instance_rid.is_valid()) {
            rs->instance_set_visible(region.instance_rid, false);
        }
        region.active = false;
        return;
    }

    if (!region.instance_rid.is_valid()) {
        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);

        region.instance_rid = rs->instance_create();
        rs->instance_set_base(region.instance_rid, region.mesh_rid);
        rs->instance_geometry_set_cast_shadows_setting(
            region.instance_rid,
            RenderingServer::SHADOW_CASTING_SETTING_OFF);
        AABB region_aabb(
            Vector3(0, 0, 0),
            Vector3(kFarRegionSizeXZ * CHUNK_WIDTH, CHUNK_HEIGHT, kFarRegionSizeXZ * CHUNK_DEPTH));
        rs->instance_set_custom_aabb(region.instance_rid, region_aabb);

        Transform3D transform;
        transform.origin = Vector3(
            rx * kFarRegionSizeXZ * CHUNK_WIDTH,
            ry * CHUNK_HEIGHT,
            rz * kFarRegionSizeXZ * CHUNK_DEPTH);
        rs->instance_set_transform(region.instance_rid, transform);

        if (owner) {
            Node3D* owner3d = Object::cast_to<Node3D>(owner);
            if (owner3d) {
                Ref<World3D> world = owner3d->get_world_3d();
                if (world.is_valid()) {
                    rs->instance_set_scenario(region.instance_rid, world->get_scenario());
                }
            }
        }
    }

    rs->instance_set_visible(region.instance_rid, true);
    region.active = true;
}

void MeshManager::sync_far_region_members_visibility(FarRegionRenderData& region) {
    if (!chunk_map) {
        return;
    }
    for (uint64_t chunk_key : region.active_chunk_keys) {
        int32_t cx, cy, cz;
        ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
        if (!render_data || !render_data->instance_rid.is_valid()) {
            continue;
        }
        const bool show_chunk = is_chunk_within_render_distance(cx, cy, cz) &&
                                !(region.active && should_use_far_region_for_chunk(cx, cy, cz));
        if (show_chunk) {
            show_chunk_instance(render_data, cx, cy, cz);
        } else {
            hide_chunk_instance(render_data);
        }
    }
}

void MeshManager::refresh_far_region_visibility() {
    for (auto& [region_key, region] : far_regions) {
        bool any_visible_members = false;
        for (uint64_t chunk_key : region.active_chunk_keys) {
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
            if (should_use_far_region_for_chunk(cx, cy, cz)) {
                any_visible_members = true;
                break;
            }
        }
        ensure_far_region_instance(region, region_key, any_visible_members);
        sync_far_region_members_visibility(region);
    }
}

void MeshManager::mark_far_region_dirty_for_chunk(int32_t cx, int32_t cy, int32_t cz) {
    // Debounce: record when the region became dirty and bump the revision.
    // process_far_region_queue skips regions marked recently so a burst of
    // child-cache arrivals (initial streaming) coalesces into one rebuild
    // instead of one rebuild per child.
    FarRegionRenderData& region = far_regions[get_far_region_key(cx, cy, cz)];
    region.last_dirty_at = std::chrono::steady_clock::now();
    region.dirty = true;
    ++region.revision;
}

void MeshManager::process_far_region_queue(int32_t max_rebuilds) {
    if (max_rebuilds <= 0 || !thread_pool || !chunk_map) {
        far_regions_partial_missing_cache_last = 0;
        return;
    }

    int32_t partial_missing_cache = 0;
    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<int32_t, uint64_t>> candidates;
    candidates.reserve(far_regions.size());
    for (const auto& [region_key, region] : far_regions) {
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            continue;
        }
        // Debounce: give a burst of cache arrivals time to settle before
        // rebuilding, so streaming a region does not schedule N rebuilds.
        const double elapsed_ms = std::chrono::duration<double, std::milli>(now - region.last_dirty_at).count();
        if (elapsed_ms < kFarRegionDebounceMs) {
            continue;
        }
        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t center_x = rx * kFarRegionSizeXZ + kFarRegionSizeXZ / 2;
        const int32_t center_z = rz * kFarRegionSizeXZ + kFarRegionSizeXZ / 2;
        const int32_t dx = center_x - last_player_chunk_x;
        const int32_t dy = ry - last_player_chunk_y;
        const int32_t dz = center_z - last_player_chunk_z;
        candidates.emplace_back(dx * dx + dy * dy + dz * dz, region_key);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    int32_t scheduled = 0;
    for (const auto& candidate : candidates) {
        const uint64_t region_key = candidate.second;
        if (scheduled >= max_rebuilds) {
            break;
        }

        auto it = far_regions.find(region_key);
        if (it == far_regions.end()) {
            continue;
        }
        FarRegionRenderData& region = it->second;
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0) {
            continue;
        }

        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t base_cx = rx * kFarRegionSizeXZ;
        const int32_t base_cz = rz * kFarRegionSizeXZ;

        struct BuildSource {
            int32_t local_chunk_x = 0;
            int32_t local_chunk_z = 0;
            uint64_t chunk_key = 0;
            std::shared_ptr<CachedFarChunkMesh> cache;
        };

        std::vector<BuildSource> sources;
        std::vector<uint64_t> member_chunk_keys;
        bool missing_far_cache = false;
        int32_t eligible_chunk_count = 0;

        for (int32_t local_z = 0; local_z < kFarRegionSizeXZ; ++local_z) {
            for (int32_t local_x = 0; local_x < kFarRegionSizeXZ; ++local_x) {
                const int32_t cx = base_cx + local_x;
                const int32_t cz = base_cz + local_z;
                if (!should_use_far_region_for_chunk(cx, ry, cz)) {
                    continue;
                }

                ++eligible_chunk_count;
                ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, ry, cz);
                if (!render_data) {
                    continue;
                }
                if (!render_data->far_mesh_cache) {
                    missing_far_cache = true;
                    continue;
                }

                const uint64_t chunk_key = chunk_map->get_chunk_key(cx, ry, cz);
                sources.push_back({local_x, local_z, chunk_key, render_data->far_mesh_cache});
                member_chunk_keys.push_back(chunk_key);
            }
        }

        if (missing_far_cache) {
            ++partial_missing_cache;
        }

        if (eligible_chunk_count < 2 || sources.size() < 2) {
            ensure_far_region_instance(region, region_key, false);
            sync_far_region_members_visibility(region);
            region.active_chunk_keys.clear();
            free_far_region_resources(region);
            region.dirty = false;
            continue;
        }

        region.dirty = false;
        const uint64_t revision = region.revision;
        region.pending_builds.fetch_add(1, std::memory_order_relaxed);

        const uint64_t build_epoch = async_epoch ? async_epoch->load(std::memory_order_acquire) : 0;
        thread_pool->fire_and_forget([this, sources = std::move(sources), member_chunk_keys = std::move(member_chunk_keys),
                                      region_key, revision, build_epoch]() mutable {
            int32_t rx_local, ry_local, rz_local;
            ChunkMap::decode_chunk_key(region_key, rx_local, ry_local, rz_local);
            const int32_t region_origin_x = rx_local * kFarRegionSizeXZ * CHUNK_WIDTH;
            const int32_t region_origin_z = rz_local * kFarRegionSizeXZ * CHUNK_DEPTH;

            CompletedRegionMesh completed;
            completed.region_key = region_key;
            completed.epoch = build_epoch;
            completed.revision = revision;
            completed.member_chunk_keys = std::move(member_chunk_keys);

            for (const BuildSource& source : sources) {
                const int32_t offset_x = source.local_chunk_x * CHUNK_WIDTH;
                const int32_t offset_z = source.local_chunk_z * CHUNK_DEPTH;
                append_packed_mesh_data(completed.mesh_data, source.cache->mesh_data, offset_x, 0, offset_z);
                append_packed_mesh_data(completed.water_mesh_data, source.cache->water_mesh_data, offset_x, 0, offset_z);
            }

            {
                std::lock_guard<std::mutex> lock(completed_far_region_meshes_mutex);
                completed_far_region_meshes.push(std::move(completed));
                completed_far_region_mesh_count.fetch_add(1, std::memory_order_relaxed);
            }
            (void)region_origin_x;
            (void)region_origin_z;
        });
        ++scheduled;
    }

    far_regions_partial_missing_cache_last = partial_missing_cache;
}

} // namespace VoxelEngine
