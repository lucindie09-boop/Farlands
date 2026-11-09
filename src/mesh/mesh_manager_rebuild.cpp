#include "mesh/mesh_manager_internal.hpp"
#include "core/performance_timer.hpp"

#include <godot_cpp/classes/rendering_server.hpp>

namespace VoxelEngine {

using namespace godot;
void MeshManager::rebuild_rendering_server_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                                   ChunkRenderData* render_data,
                                                   ChunkRenderData* d_x_neg,
                                                   ChunkRenderData* d_x_pos,
                                                   ChunkRenderData* d_y_neg,
                                                   ChunkRenderData* d_y_pos,
                                                   ChunkRenderData* d_z_neg,
                                                   ChunkRenderData* d_z_pos) {
    ++mesh_pop_outcomes_.total;
    if (!render_data) {
        ++mesh_pop_outcomes_.missing;
        return;
    }
    if (!render_data->is_mesh_dirty) {
        ++mesh_pop_outcomes_.clean;
        return;
    }
    if (!thread_pool || !chunk_scheduler || !chunk_map) {
        ++mesh_pop_outcomes_.no_pipeline;
        return;
    }

    // Skip mesh builds for chunks beyond render distance + 2 (allows unload to proceed).
    // Horizontal-only: no vertical render distance, so any chunk within the
    // horizontal disc is meshable regardless of height.
    if (mesh_render_distance > 0 && last_player_chunk_x != INT32_MIN) {
        int32_t dx = chunk_x - last_player_chunk_x;
        int32_t dz = chunk_z - last_player_chunk_z;
        if (dx*dx + dz*dz > (mesh_render_distance + 2) * (mesh_render_distance + 2)) {
            ++mesh_pop_outcomes_.distance;
            render_data->is_mesh_dirty = false;
            render_data->dirty_subchunks = 0;
            render_data->reset_dirty_bbox();
            return;
        }
    }

    // 1.5 Version check: skip enqueuing if versions match
    bool needs_enqueue = false;
    bool own_version_changed = false;
    bool neighbor_version_changed = false;
    {
    ScopedTimer early_timer(*perf_timer, TimerID::MeshPopEarly);
    if (render_data->mesh_version != render_data->last_built_version) {
        needs_enqueue = true;
        own_version_changed = true;
    } else {
        ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
        for (int i = 0; i < 6; ++i) {
            uint32_t n_ver = neighbors[i] ? neighbors[i]->mesh_version : 0;
            if (n_ver != render_data->last_built_neighbor_versions[i]) {
                needs_enqueue = true;
                neighbor_version_changed = true;
                break;
            }
        }
    }

    if (!needs_enqueue) {
        ++mesh_pop_outcomes_.clean;
        render_data->is_mesh_dirty = false;
        render_data->dirty_subchunks = 0;
        render_data->reset_dirty_bbox();
        return;
    }
    }

    // Occlusion culling: skip mesh generation for completely invisible chunks
    if (!render_data->data || render_data->data->is_all_air()) {
        ++mesh_pop_outcomes_.air;
        if (render_data->mesh_rid.is_valid()) {
            RenderingServer* rs = RenderingServer::get_singleton();
            RID old_mesh = render_data->mesh_rid;
            rs->free_rid(old_mesh);
            render_data->mesh_rid = RID();
        }
        if (render_data->instance_rid.is_valid()) {
            RenderingServer* rs = RenderingServer::get_singleton();
            rs->free_rid(render_data->instance_rid);
            render_data->instance_rid = RID();
        }
        render_data->is_mesh_dirty = false;
        render_data->dirty_subchunks = 0;
        render_data->reset_dirty_bbox();
        render_data->cached_quads.clear();
        render_data->light_checksums = {};
        render_data->uploaded_solid_vertices = 0;
        render_data->uploaded_water_vertices = 0;

        render_data->last_built_version = render_data->mesh_version;
        ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
        for (int i = 0; i < 6; ++i) {
            render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
        }
        return;
    }

    if (render_data->data && render_data->data->fully_solid()) {
        ScopedTimer buried_timer(*perf_timer, TimerID::MeshPopBuried);
        // A missing neighbor is treated as opaque when it would itself be
        // entirely solid if generated (deep underground). That keeps fully
        // buried chunks from rendering box walls into the ungenerated void
        // below the surface band — the "random chunk borders underground"
        // artifact. Real neighbors still require real solid data, so chunks
        // next to actual air (cliffs, excavations, world edges) keep
        // rendering their faces.
        const auto neighbor_solid = [this](ChunkRenderData* n, int32_t nx, int32_t ny, int32_t nz) {
            if (n && n->data) {
                ++solid_probe_resident_;
                return n->data->fully_solid();
            }
            if (!chunk_would_be_solid_fn_) return false;
            // The answer depends only on the world seed and the coordinate, and
            // the frontier asks for the same six coordinates for chunk after
            // chunk. One sample measured ~25 us on the main thread, and these
            // probes were 1.07 s of the fill's 1.18 s mesh queue.
            const uint64_t nkey = chunk_map->get_chunk_key(nx, ny, nz);
            const auto cached = solid_probe_cache_.find(nkey);
            if (cached != solid_probe_cache_.end()) {
                ++solid_probe_hits_;
                return cached->second != 0;
            }
            ++solid_probe_misses_;
            const bool solid = chunk_would_be_solid_fn_(nx, ny, nz);
            if (solid_probe_cache_.size() >= kSolidProbeCacheLimit) solid_probe_cache_.clear();
            solid_probe_cache_.emplace(nkey, solid ? uint8_t{1} : uint8_t{0});
            return solid;
        };
        // The four sub-timers split this branch's measured 0.15 ms a pop: the test
        // that decides not to build is the drain's largest term, and 0.8 s of a
        // 4.1 s tail sits here. They nest inside MeshPopBuried, so the parts sum to
        // the whole rather than double-counting it.
        bool buried = false;
        {
            ScopedTimer probes_timer(*perf_timer, TimerID::MeshPopBuriedProbes);
            buried =
                neighbor_solid(d_x_neg, chunk_x - 1, chunk_y,     chunk_z) &&
                neighbor_solid(d_x_pos, chunk_x + 1, chunk_y,     chunk_z) &&
                neighbor_solid(d_y_pos, chunk_x,     chunk_y + 1, chunk_z) &&
                neighbor_solid(d_z_neg, chunk_x,     chunk_y,     chunk_z - 1) &&
                neighbor_solid(d_z_pos, chunk_x,     chunk_y,     chunk_z + 1) &&
                (neighbor_solid(d_y_neg, chunk_x, chunk_y - 1, chunk_z) || chunk_y == 0);
        }
        if (buried) {
            ++mesh_pop_outcomes_.buried;
            {
                ScopedTimer frees_timer(*perf_timer, TimerID::MeshPopBuriedFrees);
                if (render_data->mesh_rid.is_valid()) {
                    RenderingServer* rs = RenderingServer::get_singleton();
                    rs->free_rid(render_data->mesh_rid);
                    render_data->mesh_rid = RID();
                }
                if (render_data->instance_rid.is_valid()) {
                    RenderingServer* rs = RenderingServer::get_singleton();
                    rs->free_rid(render_data->instance_rid);
                    render_data->instance_rid = RID();
                }
            }
            // Drop stale LOD/far caches referencing this chunk so the detail
            // sweeps and far regions don't keep drawing the old box geometry.
            const uint64_t key = chunk_map->get_chunk_key(chunk_x, chunk_y, chunk_z);
            active_full_detail_chunks_.erase(key);
            active_mid_detail_chunks_.erase(key);
            active_far_detail_chunks_.erase(key);
            if (render_data->far_mesh_cache) {
                ScopedTimer mark_timer(*perf_timer, TimerID::MeshPopBuriedFarMark);
                render_data->far_mesh_cache.reset();
                { ScopedTimer far_timer(*perf_timer, TimerID::MeshPopFarMark);
                  mark_far_region_dirty_for_chunk(chunk_x, chunk_y, chunk_z, render_data->far_mesh_cache); }
            }
            {
                ScopedTimer rest_timer(*perf_timer, TimerID::MeshPopBuriedRest);
                render_data->is_mesh_dirty = false;
                render_data->dirty_subchunks = 0;
                render_data->reset_dirty_bbox();
                render_data->cached_quads.clear();
                render_data->light_checksums = {};
                render_data->uploaded_solid_vertices = 0;
                render_data->uploaded_water_vertices = 0;

                render_data->last_built_version = render_data->mesh_version;
                ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
                for (int i = 0; i < 6; ++i) {
                    render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
                }
            }
            return;
        }
    }

    if (render_data->pending_mesh_builds.load(std::memory_order_acquire) > 0) {
        ++mesh_pop_outcomes_.in_flight;
        { ScopedTimer requeue_timer(*perf_timer, TimerID::MeshPopRequeue);
          queue_immediate_dirty_chunk(chunk_x, chunk_y, chunk_z); }
        return;
    }
    render_data->is_mesh_dirty = false;
    render_data->pending_mesh_builds.fetch_add(1, std::memory_order_relaxed);
    render_data->pending_mesh_uploads.fetch_add(1, std::memory_order_relaxed);
    const uint64_t mesh_job_serial = render_data->mesh_job_serial.fetch_add(1, std::memory_order_acq_rel) + 1;
    float detail = 0.0f;
    {
        ScopedTimer detail_timer(*perf_timer, TimerID::MeshPopDetail);
        detail = compute_chunk_detail_level(chunk_x, chunk_y, chunk_z);
    }
    render_data->last_built_detail_level = detail;
    uint64_t key = chunk_map->get_chunk_key(chunk_x, chunk_y, chunk_z);
    if (detail >= 1.0f) {
        active_mid_detail_chunks_.erase(key);
        if (render_data->far_mesh_cache) {
            // Back at full detail: the far region's merged mesh still contains
            // this chunk until the region is rebuilt, and its water box draws
            // over the full-detail surface (the two z-fight). The mark erases
            // the source from far_sources and, because the chunk is still an
            // active member that no longer wants a far region, asks for the
            // rebuild on the next pass instead of waiting out the debounce.
            mark_far_region_dirty_for_chunk(chunk_x, chunk_y, chunk_z, render_data->far_mesh_cache);
        }
        if (lod_distance > 0 && last_player_chunk_x != INT32_MIN) {
            int32_t dx = chunk_x - last_player_chunk_x;
            int32_t dy = chunk_y - last_player_chunk_y;
            int32_t dz = chunk_z - last_player_chunk_z;
            int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
            if (dist <= lod_distance + 2) {
                active_full_detail_chunks_.insert(key);
            }
        }
    } else if (detail == lod_detail_level) {
        active_full_detail_chunks_.erase(key);
        if (far_lod_distance > 0 && last_player_chunk_x != INT32_MIN) {
            int32_t dx = chunk_x - last_player_chunk_x;
            int32_t dy = chunk_y - last_player_chunk_y;
            int32_t dz = chunk_z - last_player_chunk_z;
            int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
            if (dist <= far_lod_distance + 2) {
                active_mid_detail_chunks_.insert(key);
            }
        }
    } else {
        active_full_detail_chunks_.erase(key);
        active_mid_detail_chunks_.erase(key);
        // Every far-detail chunk is tracked, however far away it was built.
        // The old distance gate left a chunk that was built beyond
        // far_lod_distance + 2 in NO set, so when the player flew toward it the
        // active-set sweeps could never see it and the only other path -- the
        // transition-shell scan -- runs solely on frames where the sweeps
        // queued nothing, which at speed is never. That is the LOD chunk that
        // stays at far detail until something unrelated dirties it.
        active_far_detail_chunks_.insert(key);
    }
    const bool high_priority = mesh_queue.erase_urgent(key);

    // Update last built versions
    render_data->last_built_version = render_data->mesh_version;
    ChunkRenderData* neighbors[6] = { d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos };
    for (int i = 0; i < 6; ++i) {
        render_data->last_built_neighbor_versions[i] = neighbors[i] ? neighbors[i]->mesh_version : 0;
    }

    const int32_t player_bx = last_player_block_x;
    const int32_t player_by = last_player_block_y;
    const int32_t player_bz = last_player_block_z;

    // Greedy vs per-face fallback (mirrors the worker's old decision).
    const bool player_known = player_bx != INT32_MIN && player_by != INT32_MIN && player_bz != INT32_MIN;
    int32_t pdx = 0, pdy = 0, pdz = 0;
    if (player_known) {
        int32_t cmin_x = chunk_x * CHUNK_WIDTH;
        int32_t cmax_x = cmin_x + CHUNK_WIDTH - 1;
        int32_t cmin_y = chunk_y * CHUNK_HEIGHT;
        int32_t cmax_y = cmin_y + CHUNK_HEIGHT - 1;
        int32_t cmin_z = chunk_z * CHUNK_DEPTH;
        int32_t cmax_z = cmin_z + CHUNK_DEPTH - 1;
        pdx = player_bx < cmin_x ? cmin_x - player_bx : player_bx > cmax_x ? player_bx - cmax_x : 0;
        pdy = player_by < cmin_y ? cmin_y - player_by : player_by > cmax_y ? player_by - cmax_y : 0;
        pdz = player_bz < cmin_z ? cmin_z - player_bz : player_bz > cmax_z ? player_bz - cmax_z : 0;
    }
    const bool greedy_enabled = !(player_known && std::max(pdx, std::max(pdy, pdz)) <= kGreedyDisableBlockRadius);

    // Only near-player chunks record quad caches (they are the only ones edited).
    const bool record_quads =
        player_known &&
        std::max(std::abs(chunk_x - last_player_chunk_x),
                 std::max(std::abs(chunk_y - last_player_chunk_y),
                          std::abs(chunk_z - last_player_chunk_z))) <= 2;

    // Partial remesh eligibility: own edits only (no neighbor edits), a partial
    // dirty mask, an existing quad cache, and unchanged mode/detail from the
    // build that produced that cache.
    const bool partial =
        record_quads && own_version_changed && !neighbor_version_changed &&
        render_data->dirty_subchunks != 0 && render_data->dirty_subchunks != 0xFF &&
        render_data->has_dirty_bbox() &&
        !render_data->cached_quads.empty() &&
        detail >= 1.0f &&
        render_data->last_built_detail_level == detail &&
        render_data->last_built_greedy_mode == greedy_enabled;

    // Do NOT capture raw neighbor pointers. The worker looks up neighbors by coordinate
    // at build time. If a neighbor was unloaded, the mesh builder sees nullptr (air).
    // Only the center chunk is pinned via pending_mesh_builds.
    ScopedTimer construct_timer(*perf_timer, TimerID::MeshPopConstruct);
    auto mesh_task = std::make_unique<MeshBuildTask>();
    mesh_task->chunk_map = chunk_map;
    mesh_task->chunk_scheduler = chunk_scheduler;
    mesh_task->async_epoch = async_epoch;
    mesh_task->render_data = render_data;
    mesh_task->chunk_x = chunk_x;
    mesh_task->chunk_y = chunk_y;
    mesh_task->chunk_z = chunk_z;
    mesh_task->epoch = epoch;
    mesh_task->mesh_job_serial = mesh_job_serial;
    mesh_task->player_bx = player_bx;
    mesh_task->player_by = player_by;
    mesh_task->player_bz = player_bz;
    mesh_task->high_priority = high_priority;
    mesh_task->smooth_lighting = smooth_lighting_enabled;
    mesh_task->detail_level = detail;
    mesh_task->greedy_enabled = greedy_enabled;
    mesh_task->record_quads = record_quads;
    mesh_task->partial = partial;
    mesh_task->prev_quads = render_data->cached_quads;
    mesh_task->prev_light_checksums = render_data->light_checksums;
    mesh_task->dirty_subchunks = render_data->dirty_subchunks;
    // NOTE: render_data->dirty_subchunks is NOT cleared here. It is cleared at
    // apply time (process_completed_meshes) with the snapshot mask, so edits that
    // land while the build is in flight stay marked for the next build.

    // Block-level dirty bbox: snapshot into the task, then clear it so later
    // edits accumulate their own bbox. The bitmask + mesh_version deferral above
    // still catches edits that land mid-build (they re-open their own bbox and
    // force a rebuild via the version mismatch).
    mesh_task->have_dirty_bounds = render_data->has_dirty_bbox();
    if (mesh_task->have_dirty_bounds) {
        mesh_task->dirty_bounds = {
            render_data->dirty_min_x,
            render_data->dirty_max_x + 1,
            render_data->dirty_min_y,
            render_data->dirty_max_y + 1,
            render_data->dirty_min_z,
            render_data->dirty_max_z + 1};
    }
    render_data->reset_dirty_bbox();

    ++mesh_pop_outcomes_.built;
    thread_pool->enqueue_task(std::move(mesh_task), high_priority);
}

void MeshManager::rebuild_chunk_mesh(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch) {
    ScopedTimer pop_timer(*perf_timer, TimerID::MeshPopCallback);
    ++mesh_pop_outcomes_.callbacks;
    if (!chunk_map) return;
    ChunkRenderData* chunk_render_data = nullptr;
    ChunkRenderData* neighbors[6] = {};
    {
        ScopedTimer lookup_timer(*perf_timer, TimerID::MeshPopLookup);
        chunk_render_data = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z);
        if (!chunk_render_data) {
            ++mesh_pop_outcomes_.missing;
            return;
        }
        chunk_map->get_neighbors(chunk_x, chunk_y, chunk_z, neighbors);
    }
    chunk_render_data->is_mesh_dirty = true;

    rebuild_rendering_server_mesh(chunk_x, chunk_y, chunk_z, epoch, chunk_render_data,
                                  neighbors[0], neighbors[1],
                                  neighbors[2], neighbors[3],
                                  neighbors[4], neighbors[5]);
}

void MeshManager::rebuild_all_meshes_with_neighbors(uint64_t epoch) {
    if (!chunk_map) return;
    std::vector<std::tuple<int32_t, int32_t, int32_t>> dirty_chunks;
    chunk_map->for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (render_data->is_mesh_dirty) {
            int32_t chunk_x, chunk_y, chunk_z;
            ChunkMap::decode_chunk_key(key, chunk_x, chunk_y, chunk_z);
            dirty_chunks.emplace_back(chunk_x, chunk_y, chunk_z);
        }
    });
    // By reference: the three coordinates are read, never written, and the tuple
    // they are bound from lives in the vector for the whole walk.
    for (const auto& [chunk_x, chunk_y, chunk_z] : dirty_chunks) {
        ChunkRenderData* render_data = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z);
        ChunkRenderData* d_x_neg = chunk_map->get_chunk_render_data(chunk_x - 1, chunk_y, chunk_z);
        ChunkRenderData* d_x_pos = chunk_map->get_chunk_render_data(chunk_x + 1, chunk_y, chunk_z);
        ChunkRenderData* d_y_neg = chunk_map->get_chunk_render_data(chunk_x, chunk_y - 1, chunk_z);
        ChunkRenderData* d_y_pos = chunk_map->get_chunk_render_data(chunk_x, chunk_y + 1, chunk_z);
        ChunkRenderData* d_z_neg = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z - 1);
        ChunkRenderData* d_z_pos = chunk_map->get_chunk_render_data(chunk_x, chunk_y, chunk_z + 1);
        if (render_data) {
            rebuild_rendering_server_mesh(chunk_x, chunk_y, chunk_z, epoch, render_data,
                                          d_x_neg, d_x_pos, d_y_neg, d_y_pos, d_z_neg, d_z_pos);
        }
    }
}

} // namespace VoxelEngine
