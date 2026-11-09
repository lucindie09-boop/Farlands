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
    // Horizontal-only: no vertical render distance, so chunks stay visible
    // however far above or below the player they are.
    return (dx * dx + dz * dz) <= (mesh_render_distance * mesh_render_distance);
}

bool MeshManager::should_use_far_region_for_chunk(int32_t cx, int32_t cy, int32_t cz) const {
    return compute_chunk_detail_level(cx, cy, cz) < 1.0f && is_chunk_within_render_distance(cx, cy, cz);
}

uint64_t MeshManager::get_far_region_key(int32_t cx, int32_t cy, int32_t cz) const {
    if (!chunk_map) {
        return 0;
    }
    // Always use 8x8 for region keys to ensure stability across LOD transitions
    const int32_t region_size = kFarRegionSizeXZ;
    const int32_t rx = floor_div(cx, region_size);
    const int32_t rz = floor_div(cz, region_size);
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
    if (rs != nullptr) {
        if (region.instance_rid.is_valid()) {
            rs->free_rid(region.instance_rid);
            region.instance_rid = RID();
        }
        if (region.mesh_rid.is_valid()) {
            rs->free_rid(region.mesh_rid);
            region.mesh_rid = RID();
        }
    }
    // Invalidate any upload still in flight: it must not publish its mesh into
    // a region whose resources (and members) were just released.
    ++region.revision;
    region.active = false;
    region.uploaded_content_hash = 0;
    region.member_sync_cursor = 0;
    region.member_sync_pending = false;
    // far_sources is membership, not a GPU resource: it must survive a region
    // dropping below two sources, or the next cache would replace the previous
    // one and the region could never accumulate enough to build again. The
    // whole map is destroyed by far_regions.clear() on a world reset.
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
            Vector3(region.region_size_xz * CHUNK_WIDTH, CHUNK_HEIGHT, region.region_size_xz * CHUNK_DEPTH));
        const Vector3 region_origin(
            rx * region.region_size_xz * CHUNK_WIDTH,
            ry * CHUNK_HEIGHT,
            rz * region.region_size_xz * CHUNK_DEPTH);
        // A far region is one instance covering a block of chunks, so it has the
        // same culling problem over a much wider box - and the same fix.
        cull_apply(region.instance_rid, region.cull_growth, region_aabb, region_origin);

        Transform3D transform;
        transform.origin = region_origin;
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

void MeshManager::sync_far_region_member(FarRegionRenderData& region, uint64_t chunk_key) {
    if (!chunk_map) return;
    int32_t cx, cy, cz;
    ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (!render_data || !render_data->instance_rid.is_valid()) {
        return;
    }
    // What the region DRAWS decides, not what the chunk would like to be. A
    // chunk that has just promoted to full detail is still listed in the
    // region's active mesh until that mesh is rebuilt and uploaded; asking
    // should_use_far_region_for_chunk (which is already false for a full-detail
    // chunk) showed its own instance at the same time and the two water
    // surfaces overlapped and z-fought. If the region lists the chunk, the
    // region draws it and the chunk's own instance stays hidden; when a later
    // region build drops the chunk, the upload reports it through
    // visibility_extra_keys and this same function is called again with it no
    // longer a member, so the full-detail instance appears.
    const bool in_region_mesh =
        region.active &&
        std::find(region.active_chunk_keys.begin(), region.active_chunk_keys.end(), chunk_key) !=
            region.active_chunk_keys.end();
    const bool show_chunk = is_chunk_within_render_distance(cx, cy, cz) && !in_region_mesh;
    if (show_chunk) {
        show_chunk_instance(render_data, cx, cy, cz);
    } else {
        hide_chunk_instance(render_data);
    }
}

void MeshManager::sync_visibility_extra_member(uint64_t chunk_key) {
    if (!chunk_map) return;
    int32_t cx, cy, cz;
    ChunkMap::decode_chunk_key(chunk_key, cx, cy, cz);
    ChunkRenderData* render_data = chunk_map->get_chunk_render_data(cx, cy, cz);
    if (!render_data || !render_data->instance_rid.is_valid()) return;
    if (is_chunk_within_render_distance(cx, cy, cz)) {
        show_chunk_instance(render_data, cx, cy, cz);
    } else {
        hide_chunk_instance(render_data);
    }
}

void MeshManager::sync_far_region_members_visibility(FarRegionRenderData& region) {
    region.member_sync_cursor = 0;
    region.member_sync_pending = false;
    if (!chunk_map) return;
    for (uint64_t chunk_key : region.active_chunk_keys) {
        sync_far_region_member(region, chunk_key);
    }
    region.member_sync_pending = false;
    region.member_sync_cursor = 0;
}

void MeshManager::request_far_region_visibility_refresh() {
    if (far_visibility_pending_) return;
    far_visibility_keys_.clear();
    far_visibility_keys_.reserve(far_regions.size());
    for (const auto& entry : far_regions) {
        far_visibility_keys_.push_back(entry.first);
    }
    far_visibility_cursor_ = 0;
    far_visibility_pending_ = !far_visibility_keys_.empty();
}

void MeshManager::process_far_region_visibility(double budget_ms) {
    if (!far_visibility_pending_ || budget_ms <= 0.0) return;
    const auto start = std::chrono::steady_clock::now();
    while (far_visibility_cursor_ < far_visibility_keys_.size()) {
        const uint64_t region_key = far_visibility_keys_[far_visibility_cursor_++];
        auto it = far_regions.find(region_key);
        if (it != far_regions.end()) {
            FarRegionRenderData& region = it->second;
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
            if (!region.member_sync_pending) {
                region.member_sync_cursor = 0;
                region.member_sync_pending = true;
            }
            const double member_start_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            while (region.member_sync_cursor < region.active_chunk_keys.size()) {
                sync_far_region_member(region, region.active_chunk_keys[region.member_sync_cursor]);
                ++region.member_sync_cursor;
                const double member_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (member_ms - member_start_ms >= budget_ms) break;
            }
            if (region.member_sync_cursor >= region.active_chunk_keys.size()) {
                region.member_sync_pending = false;
                region.member_sync_cursor = 0;
            } else {
                // Same region next frame; do not advance past it.
                --far_visibility_cursor_;
                return;
            }
            const double extra_start_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - start).count();
            while (region.visibility_extra_cursor < region.visibility_extra_keys.size()) {
                sync_visibility_extra_member(
                    region.visibility_extra_keys[region.visibility_extra_cursor]);
                ++region.visibility_extra_cursor;
                const double extra_ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - start).count();
                if (extra_ms - extra_start_ms >= budget_ms) break;
            }
            if (region.visibility_extra_cursor < region.visibility_extra_keys.size()) {
                --far_visibility_cursor_;
                return;
            }
            region.visibility_extra_keys.clear();
            region.visibility_extra_cursor = 0;
        }
        const double elapsed_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (elapsed_ms >= budget_ms) break;
    }
    if (far_visibility_cursor_ >= far_visibility_keys_.size()) {
        far_visibility_pending_ = false;
        far_visibility_keys_.clear();
        far_visibility_cursor_ = 0;
    }
}

void MeshManager::mark_far_region_dirty_for_chunk(int32_t cx, int32_t cy, int32_t cz,
                                                  const std::shared_ptr<CachedFarChunkMesh>& cache) {
    // Debounce: record when the region became dirty and bump the revision.
    // process_far_region_queue skips regions marked recently so a burst of
    // child-cache arrivals (initial streaming) coalesces into one rebuild
    // instead of one rebuild per child.
    FarRegionRenderData& region = far_regions[get_far_region_key(cx, cy, cz)];
    const uint64_t chunk_key = chunk_map ? chunk_map->get_chunk_key(cx, cy, cz) : 0;
    // The only place that learns a far cache appeared, changed or went away.
    // The scheduler reads this map instead of rescanning all 64 chunks of the
    // region, which cost ~3 ms on a crossing frame.
    if (cache && should_use_far_region_for_chunk(cx, cy, cz)) {
        region.far_sources[chunk_key] = cache;
    } else {
        region.far_sources.erase(chunk_key);
    }
    region.last_dirty_at = std::chrono::steady_clock::now();
    region.dirty = true;
    ++region.revision;

    // A far chunk that is not in a region's current mesh has no merged draw
    // covering it yet: its own instance is the only thing on screen. Waiting
    // 250 ms for the debounce means a whole newly-far shell draws individually.
    // Build this region on the next mesh-queue pass instead. Inactive regions
    // count as missing: there is no mesh at all to wait for.
    //
    // The reverse handoff is just as urgent: a chunk that has promoted back to
    // full detail is still in the region mesh until that mesh is rebuilt, and
    // its own full-detail instance is already visible. Leaving it for the
    // debounce draws the same chunk twice for 250 ms.
    // New regions have no entry in the in-flight visibility snapshot; asking for
    // another pass here is what brings them into it (no-op while one is running).
    request_far_region_visibility_refresh();
    const bool wants_far_region = should_use_far_region_for_chunk(cx, cy, cz);
    const auto& members = region.active_chunk_keys;
    const bool in_active_mesh = region.active &&
                                std::find(members.begin(), members.end(), chunk_key) != members.end();
    if ((wants_far_region && !in_active_mesh) || (in_active_mesh && !wants_far_region)) {
        region.needs_immediate_rebuild = true;
    }
    // All far regions are now 8x8
    region.region_size_xz = kFarRegionSizeXZ;
}

void MeshManager::process_far_region_queue(int32_t max_rebuilds, double budget_ms) {
    if (max_rebuilds <= 0 || budget_ms <= 0.0 || !thread_pool || !chunk_map) {
        far_regions_partial_missing_cache_last = 0;
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    std::vector<std::pair<int32_t, uint64_t>> candidates;
    candidates.reserve(far_regions.size());
    for (const auto& [region_key, region] : far_regions) {
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0 ||
            region.pending_uploads > 0) {
            continue;
        }
        // Debounce: give a burst of cache arrivals time to settle before
        // rebuilding, so streaming a region does not schedule N rebuilds.
        const double elapsed_ms = std::chrono::duration<double, std::milli>(now - region.last_dirty_at).count();
        if (elapsed_ms < kFarRegionDebounceMs && !region.needs_immediate_rebuild) {
            continue;
        }
        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t region_size = region.region_size_xz;
        const int32_t center_x = rx * region_size + region_size / 2;
        const int32_t center_z = rz * region_size + region_size / 2;
        const int32_t dx = center_x - last_player_chunk_x;
        const int32_t dy = ry - last_player_chunk_y;
        const int32_t dz = center_z - last_player_chunk_z;
        candidates.emplace_back(dx * dx + dy * dy + dz * dz, region_key);
    }

    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.first < b.first;
    });

    int32_t scheduled = 0;
    const auto schedule_start = std::chrono::steady_clock::now();
    for (const auto& candidate : candidates) {
        const uint64_t region_key = candidate.second;
        if (scheduled >= max_rebuilds) {
            break;
        }
        const double scheduled_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - schedule_start).count();
        if (scheduled_ms >= budget_ms) {
            break;
        }

        auto it = far_regions.find(region_key);
        if (it == far_regions.end()) {
            continue;
        }
        FarRegionRenderData& region = it->second;
        if (!region.dirty || region.pending_builds.load(std::memory_order_relaxed) > 0 ||
            region.pending_uploads > 0) {
            continue;
        }

        int32_t rx, ry, rz;
        ChunkMap::decode_chunk_key(region_key, rx, ry, rz);
        const int32_t region_size = region.region_size_xz;
        const int32_t base_cx = rx * region_size;
        const int32_t base_cz = rz * region_size;

        struct BuildSource {
            int32_t local_chunk_x = 0;
            int32_t local_chunk_z = 0;
            uint64_t chunk_key = 0;
            std::shared_ptr<CachedFarChunkMesh> cache;
        };

        // The region's sources are maintained by the dirty marks: no chunk-map
        // scan, no resumable cursor. This is a map iteration over the caches
        // the region already owns, and the build starts immediately.
        std::vector<BuildSource> sources;
        std::vector<uint64_t> member_chunk_keys;
        sources.reserve(region.far_sources.size());
        member_chunk_keys.reserve(region.far_sources.size());
        for (const auto& [chunk_key, cache] : region.far_sources) {
            if (!cache) continue;
            int32_t scx, scy, scz;
            ChunkMap::decode_chunk_key(chunk_key, scx, scy, scz);
            if (!should_use_far_region_for_chunk(scx, scy, scz)) continue;
            sources.push_back({scx - base_cx, scz - base_cz, chunk_key, cache});
            member_chunk_keys.push_back(chunk_key);
        }

        if (sources.size() < 2) {
            ensure_far_region_instance(region, region_key, false);
            // Do not walk the members here: the incremental visibility pass
            // owns that walk now. Keeping the keys lets it show them.
            request_far_region_visibility_refresh();
            free_far_region_resources(region);
            region.dirty = false;
            region.needs_immediate_rebuild = false;
            continue;
        }

        region.dirty = false;
        region.needs_immediate_rebuild = false;
        const uint64_t revision = region.revision;
        region.pending_builds.fetch_add(1, std::memory_order_relaxed);

        const uint64_t build_epoch = async_epoch ? async_epoch->load(std::memory_order_acquire) : 0;
        const float spike_detail = far_lod_detail_level;
        const bool spike_smooth = smooth_lighting_enabled;
        thread_pool->fire_and_forget([this, sources = std::move(sources), member_chunk_keys = std::move(member_chunk_keys),
                                      region_key, revision, build_epoch, region_size = region.region_size_xz,
                                      spike_detail, spike_smooth]() mutable {
            int32_t rx_local, ry_local, rz_local;
            ChunkMap::decode_chunk_key(region_key, rx_local, ry_local, rz_local);
            const int32_t region_origin_x = rx_local * region_size * CHUNK_WIDTH;
            const int32_t region_origin_z = rz_local * region_size * CHUNK_DEPTH;

            region_snapshot_spike(member_chunk_keys, spike_detail, spike_smooth);

            CompletedRegionMesh completed;
            completed.region_key = region_key;
            completed.epoch = build_epoch;
            completed.revision = revision;
            completed.member_chunk_keys = std::move(member_chunk_keys);

            uint64_t region_hash = 1469598103934665603ull;
            for (const BuildSource& source : sources) {
                const int32_t offset_x = source.local_chunk_x * CHUNK_WIDTH;
                const int32_t offset_z = source.local_chunk_z * CHUNK_DEPTH;
                append_packed_mesh_data(completed.mesh_data, source.cache->mesh_data, offset_x, 0, offset_z);
                append_packed_mesh_data(completed.water_mesh_data, source.cache->water_mesh_data, offset_x, 0, offset_z);
                // Order-independent: the source map is unordered and the merged
                // geometry does not depend on the append order.
                uint64_t child = source.cache->content_hash;
                child ^= source.chunk_key + 0x9e3779b97f4a7c15ull + (child << 6) + (child >> 2);
                region_hash ^= child;
            }
            region_hash ^= static_cast<uint64_t>(sources.size()) * 0x9e3779b97f4a7c15ull;
            completed.content_hash = region_hash != 0 ? region_hash : 1;

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

    far_regions_partial_missing_cache_last = 0;
}

} // namespace VoxelEngine
