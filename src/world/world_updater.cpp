#include "world/world_updater.hpp"
#include "world/world_updater_internal.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "core/performance_timer.hpp"
#include "render/material_manager.hpp"
#include <algorithm>
#include <cstdlib>

namespace VoxelEngine {
using namespace godot;

WorldUpdater::WorldUpdater()
    : initial_loading_latch_(
          latch_seconds_from_env("FARLANDS_LOADING_LATCH_MAX_SEC", 30.0),
          latch_seconds_from_env("FARLANDS_LOADING_LATCH_QUIET_SEC", 1.0)),
      column_prefetch(std::make_shared<ColumnPrefetch>()) {
    // The workers' first act is to copy the published configuration, so it has to
    // be published from the start: a worker computing with a default config would
    // hand the sweep bands from terrain that is not the terrain it is rendering.
    refresh_prefetch_config();

    // Measurement-only overrides for the post-load clocks, the mesh hold, the
    // admission controller, the held drain and the install clock. They live in
    // world_updater_budgets.cpp to keep this file under the line cap; the call
    // returns the hold's ceiling for the latch this constructor owns.
    initial_mesh_hold_.set_max_sec(apply_measurement_overrides(budgets));
}
WorldUpdater::~WorldUpdater() = default;

void WorldUpdater::set_fluid_state_table(fluids::FluidStateTable* table) {
    // No fluid states in this registry (a test registry, or a data file without
    // any) means no simulation at all, and every fluid entry point stays a no-op.
    if (table == nullptr || !table->any() || chunk_world == nullptr) return;
    fluid_sim.set_context(chunk_world->get_chunk_map(), BlockRegistry::get_instance(), *table, &fluid_sink);
}

void WorldUpdater::FluidSink::on_chunk_updated(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                                               const std::vector<fluids::FluidWriteRecord>& writes) {
    // One batched write per chunk per tick, rather than a lock, a chunk lookup and a
    // dirty mark per cell. notify=false either way: the simulation already scheduled
    // everything it wrote — it has to, because that is what makes the flood advance.
    batch.clear();
    batch.reserve(writes.size());
    for (const fluids::FluidWriteRecord& write : writes) {
        batch.push_back(ChunkWorld::EditCell{write.local_x, write.local_y, write.local_z, write.block});
    }
    owner_->chunk_world->add_block_edits(chunk_x, chunk_y, chunk_z, batch);
    if (owner_->mesh_manager != nullptr) {
        // One remesh per chunk per tick, rather than one per cell.
        owner_->mesh_manager->queue_dirty_chunk(chunk_x, chunk_y, chunk_z);
    }
}

void WorldUpdater::set_seed(int32_t s) { terrain_params.seed = s; if (height_estimator) height_estimator->set_params(terrain_params); invalidate_height_cache(); }
void WorldUpdater::set_sea_level(float level) { terrain_params.sea_level = level; if (height_estimator) height_estimator->set_params(terrain_params); invalidate_height_cache(); }
void WorldUpdater::set_biome_size(float size) {
    terrain_params.biome_size = size;
    terrain_params.climate_temp_scale = terrain_params.climate_temp_base_scale / size;
    terrain_params.climate_humidity_scale = terrain_params.climate_humidity_base_scale / size;
    if (height_estimator) height_estimator->set_params(terrain_params);
    invalidate_height_cache();
}

void WorldUpdater::set_terrain_params(const TerrainParams& p) {
    terrain_params = p;
    if (height_estimator) height_estimator->set_params(terrain_params);
    invalidate_height_cache();
}

void WorldUpdater::set_biome_config(const BiomeConfig& c) {
    biome_config = c;
    if (height_estimator) height_estimator->set_biome_config(c);
    // Biome amplification is part of what a column's bounds are derived from, so
    // republish it here even though the column cache is left alone (these setters
    // are setup-time today, but the prefetch must not outlive the config it was
    // asked to compute with either way).
    refresh_prefetch_config();
}

void WorldUpdater::set_vegetation_config(const VegetationConfig& c) {
    vegetation_config = c;
    if (height_estimator) height_estimator->set_vegetation_config(c);
    refresh_prefetch_config();
}

void WorldUpdater::set_vegetation_enabled(bool enabled) {
    vegetation_enabled = enabled;
    if (chunk_world) chunk_world->set_vegetation_enabled(enabled);
}

void WorldUpdater::update(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, double delta) {
    last_frame_ms = delta * 1000.0;
    int32_t player_chunk_x = 0, player_chunk_y = 0, player_chunk_z = 0;
    chunk_world->get_chunk_map().get_chunk_coords(player_position, player_chunk_x, player_chunk_y, player_chunk_z);

    int32_t active_render_distance = is_editor ? editor_render_distance : render_distance;

    bool chunk_changed = (player_chunk_x != last_player_chunk_x ||
                          player_chunk_y != last_player_chunk_y ||
                          player_chunk_z != last_player_chunk_z);
    if (chunk_changed) {
        last_player_chunk_x = player_chunk_x;
        last_player_chunk_y = player_chunk_y;
        last_player_chunk_z = player_chunk_z;
        mesh_manager->set_mesh_render_distance(active_render_distance);
        mesh_manager->set_lod_distance(lod_distance);
        mesh_manager->set_lod_detail_level(lod_detail_level);
        mesh_manager->set_far_lod_distance(far_lod_distance);
        mesh_manager->set_far_lod_detail_level(far_lod_detail_level);
        mesh_manager->set_player_chunk(player_chunk_x, player_chunk_y, player_chunk_z);
        {
            ScopedTimer t(*perf_timer, TimerID::MeshReprioritize);
            mesh_manager->reprioritize(player_chunk_x, player_chunk_y, player_chunk_z,
                                       frustum.is_initialized() ? &frustum : nullptr);
        }
    }

    update_generation(is_editor, active_render_distance, epoch, player_chunk_x, player_chunk_y, player_chunk_z);
    update_unload(active_render_distance, player_chunk_x, player_chunk_y, player_chunk_z, chunk_changed);
    // Fluids tick after generation/unload (so the cells it looks at are the ones
    // that exist now) and before the mesh budgets (so the chunks it dirties can
    // remesh this same frame).
    {
        ScopedTimer t(*perf_timer, TimerID::FluidSimAdvance);
        fluid_sim.advance(delta);
    }
    process_mesh_budgets(is_editor, epoch, chunks_processed_total, active_render_distance, delta);
    {
        ScopedTimer t(*perf_timer, TimerID::FarRegionVisibility);
        mesh_manager->process_far_region_visibility(
            initial_loading_latch_.active() ? budgets.far_region_visibility_loading_budget_ms
                                            : budgets.far_region_visibility_budget_ms);
    }
    {
        ScopedTimer t(*perf_timer, TimerID::FlushDirty);
        flush_dirty(delta);
    }
}

void WorldUpdater::update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    ScopedTimer t(*perf_timer, TimerID::ChunkUnload);
    if (chunk_changed || (++unload_scan_skip_counter >= kUnloadScanSkipFrames)) {
        unload_scan_skip_counter = 0;
        int32_t unload_hrd  = active_render_distance + kChunkRetentionChunks;
        int32_t unload_hrd2 = unload_hrd * unload_hrd;

        // Unload is purely horizontal: chunks stay loaded no matter how far
        // above or below the player they are (no vertical render distance).
        chunk_world->get_chunk_map().for_each_limited_resumable([&](uint64_t key, const std::unique_ptr<ChunkRenderData>&) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            int32_t dx = cx - pcx;
            int32_t dz = cz - pcz;
            int32_t horiz_dist2 = dx * dx + dz * dz;
            if (horiz_dist2 > unload_hrd2) {
                queue_unload(key);
            } else {
                unload_pending.erase(key);
            }
        }, budgets.unload_checks_per_frame, unload_scan_bucket_cursor);
    }

    if (!unload_queue.empty()) {
        int32_t unload_hrd = active_render_distance + kChunkRetentionChunks;
        int32_t unload_hrd2 = unload_hrd * unload_hrd;
        int32_t unloads_this_frame = 0;
        const bool frustum_active = frustum.is_initialized();
        // Frustum-aware unload: prefer unloading non-visible chunks first.
        // Collect visible chunks and defer them, unload non-visible chunks now.
        std::vector<uint64_t> visible_deferred;
        while (!unload_queue.empty() && unloads_this_frame < budgets.unloads_per_frame) {
            uint64_t key = unload_queue.back();
            unload_queue.pop_back();
            // Pinned: a caller asked for this chunk and has not finished with it.
            // Dropped from the pending set rather than retried, so it is not
            // re-queued every frame; the next scan re-adds it once the pin is
            // gone and it is still out of range.
            if (chunk_world->is_chunk_pinned(key)) {
                unload_pending.erase(key);
                continue;
            }
            if (chunk_world->get_chunk_map().contains(key)) {
                int32_t cx = 0, cy = 0, cz = 0;
                ChunkMap::decode_chunk_key(key, cx, cy, cz);
                int32_t dx = cx - pcx;
                int32_t dz = cz - pcz;
                int32_t horiz_dist2 = dx * dx + dz * dz;
                if (horiz_dist2 > unload_hrd2) {
                    // Beyond render distance: unload now, non-visible first
                    if (frustum_active && frustum.is_chunk_visible(cx, cy, cz)) {
                        // Visible beyond render distance: defer if budget available
                        if (unloads_this_frame < budgets.unloads_per_frame / 2) {
                            visible_deferred.push_back(key);
                            continue;
                        }
                    }
                    try_unload(key);
                } else {
                    unload_pending.erase(key);
                }
            } else {
                unload_pending.erase(key);
            }
            unloads_this_frame++;
        }
        // Re-queue deferred visible chunks for next frame
        for (uint64_t k : visible_deferred) {
            unload_queue.push_back(k);
        }
    }
}

void WorldUpdater::process_mesh_budgets(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total,
                                        int32_t active_render_distance, double delta) {
    const int32_t loaded_count_post = static_cast<int32_t>(chunk_world->get_chunk_map().size());
    const size_t generating_count = chunk_world->get_scheduler().generating_count();
    const size_t completed_count = chunk_world->get_scheduler().completed_chunk_count();
    const bool pending_mesh = mesh_manager && mesh_manager->has_pending_mesh_work();

    // Phase one is the chunk load. Keep the gameplay mesh budgets there: the
    // loading mesh budget competes with the completion/light work on the main
    // thread and made the chunk count settle later (9.86 s vs 6.01 s measured).
    // Phase two is the mesh/far tail, after generation has gone quiet; that is
    // where the larger budgets are useful and cannot delay the chunk map.
    // The fill is settled when the walk has spent its list and nothing is left
    // in generation or completion. This is exact at any render distance, unlike
    // the old `loaded >= loading_threshold` gate: at RD 4 the whole disc is
    // below 500 chunks, so that gate never fired and the hold never released.
    const bool queues_idle = generating_count == 0 && completed_count == 0;
    fill_settled_ = loaded_count_post > 0 && queues_idle &&
                    generation_pass_complete &&
                    sweep_band_frontier >= sweep_columns.size();
    const bool fill_settled = fill_settled_;
    // The latch's transitions live in LoadingLatch (tested without a world);
    // this pass supplies the frame's inputs and reads the budget it selects.
    const bool is_initial_loading =
        initial_loading_latch_.tick(delta, fill_settled, pending_mesh);
    const bool pipelines_busy = generating_count > 0 || completed_count > 0 || pending_mesh;

    const size_t worker_count = thread_pool ? thread_pool->get_worker_count() : 0;
    const bool hold_allowed = budgets.hold_mesh_force ||
        worker_count >= static_cast<size_t>(budgets.hold_mesh_min_workers);
    const bool hold_mesh =
        initial_mesh_hold_.tick(delta, budgets.hold_mesh_until_load_done && hold_allowed,
                                fill_settled);
    int32_t mesh_rebuild_budget = 0;
    int32_t upload_budget = 0;
    if (is_initial_loading) {
        mesh_rebuild_budget = budgets.mesh_rebuilds_loading;
        upload_budget = budgets.mesh_uploads_loading;
    } else if (pipelines_busy) {
        mesh_rebuild_budget = budgets.mesh_rebuilds_active;
        upload_budget = budgets.mesh_uploads_active;
    } else {
        mesh_rebuild_budget = budgets.mesh_rebuilds_idle;
        upload_budget = budgets.mesh_uploads_idle;
    }
    // A viewport-load budget scale used to sit here, driven by a visibility ratio
    // the retired frustum pass was meant to write. Nothing ever assigned it, so
    // the scale was a constant 1.0 — the budgets were never actually scaled, and
    // the dead computation is gone with the ratio (see set_frustum).

    // NOTE: a backlog-proportional mesh budget was tried here and reverted. The
    // reasoning was sound (the queue is saturated while the player moves, so a
    // bigger count drains it sooner) but the cost is on the main thread, and this
    // pass is not cheap per rebuild: every rebuild does 7 chunk lookups, and the
    // far-region share of the budget walks 64 chunks per scheduled region. Scaling
    // the count 4x took `dirty_mesh_queue` from a ~1.0 ms median to ~2.7 ms in a
    // live session with no visible gain, because the extra rebuilds are not more
    // terrain — they arrive in the same order, just sooner (see ARCHITECTURE.md).
    // The starvation fix (the queue's reserve) does not depend on this.

    {
        ScopedTimer t(*perf_timer, TimerID::ProcessCompletedChunks);
        // The fill is `!fill_settled`, not the latch: LoadingLatch is the
        // post-fill mesh drain, so keying the loading counts on it left the fill
        // running on the gameplay cap (the same trap the install clock hit).
        const bool loading_drain = !fill_settled || is_initial_loading;
        int32_t active_max = loading_drain ? budgets.chunk_completions_initial
                                           : budgets.chunk_completions_gameplay;
        // The render-distance clamp is a gameplay rule: a frame should not install
        // more chunks than the disc it is about to draw. While the fill runs there
        // is no disc to protect -- the fill is installing that disc -- and at RD 32
        // the clamp held installs to 48 a frame while the pool was finishing 80 and
        // the drain had 6 ms of clock to spend. 48 -> 128 with the in-flight set
        // opened measured load 2.72 s against 2.89-2.93 (11 workers), and 2.13 s at
        // 15 workers once all three gates were open. Shipped together with the
        // loading in-flight term: three pairs put load 2.66/2.61/2.68 s against
        // 3.18/3.15/3.13 and settle 2.84/2.81/2.72 against 3.22/3.17/3.09.
        int32_t scaled_completion_budget = loading_drain
            ? active_max
            : std::min(active_max + 16, active_render_distance + 16);
        if (loading_drain && budgets.chunk_completions_loading > 0) {
            scaled_completion_budget = budgets.chunk_completions_loading;
        }
        int32_t installed = chunk_world->process_completed_chunks(
            epoch,
            (!fill_settled || is_initial_loading) ? budgets.processing_budget_loading_ms
                                                  : budgets.processing_budget_ms,
            scaled_completion_budget,
            scaled_completion_budget,
            scaled_completion_budget,
            last_player_chunk_x,
            last_player_chunk_y,
            last_player_chunk_z,
            active_render_distance
        );
        chunks_processed_total += installed;
    }
    if (!hold_mesh) {
        {
            ScopedTimer t(*perf_timer, TimerID::ProcessCompletedMeshes);
            mesh_manager->process_completed_meshes(
                epoch,
                is_initial_loading ? budgets.mesh_completion_budget_loading_ms
                                   : budgets.mesh_completion_budget_ms,
                upload_budget,
                material_manager->get_material(),
                material_manager->get_water_material()
            );
        }

        {
            ScopedTimer t(*perf_timer, TimerID::DirtyMeshQueue);
            const double mesh_queue_budget_ms = is_initial_loading
                ? budgets.mesh_queue_loading_budget_ms
                : budgets.mesh_queue_budget_ms;
            mesh_manager->process_queue(
                is_initial_loading ? budgets.mesh_rebuilds_immediate_loading
                                   : budgets.mesh_rebuilds_immediate,
                mesh_rebuild_budget,
                mesh_queue_budget_ms
            );
        }
    } else if (budgets.hold_mesh_partial > 0) {
        // The hold's one-shot batch makes the first terrain wait for the whole
        // chunk set. This arm lets the nearest chunks through under a tiny
        // budget; the queue is already visibility/distance ordered, so N spends
        // the frame on what the player sees first. It is deliberately much
        // smaller than the post-load 512 (the full overlap was slower).
        const int32_t partial = budgets.hold_mesh_partial;
        {
            ScopedTimer t(*perf_timer, TimerID::ProcessCompletedMeshes);
            mesh_manager->process_completed_meshes(
                epoch,
                budgets.mesh_completion_budget_ms,
                partial,
                material_manager->get_material(),
                material_manager->get_water_material()
            );
        }
        {
            ScopedTimer t(*perf_timer, TimerID::DirtyMeshQueue);
            mesh_manager->process_queue(0, partial, budgets.mesh_queue_budget_ms);
        }
    }
}

void WorldUpdater::flush_dirty(double delta) {
    dirty_flush_accumulator += delta;
    if (dirty_flush_accumulator >= budgets.flush_interval) {
        chunk_world->flush_dirty_chunks();
        dirty_flush_accumulator = 0.0;
    }
}

bool WorldUpdater::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                  int32_t band_lo, int32_t band_hi) {
    return chunk_world->generate_chunk(chunk_x, chunk_y, chunk_z, epoch, terrain_params,
                                       biome_config, vegetation_config, band_lo, band_hi);
}

void WorldUpdater::try_unload(uint64_t key) {
    if (!chunk_world->try_unload_chunk(key, mesh_manager)) {
        // Retry later: push back to queue. The chunk stays in unload_pending
        // so for_each won't add a duplicate.
        unload_queue.push_back(key);
    } else {
        unload_pending.erase(key);
        // The column is no longer complete, and skipping it would leave exactly
        // the hole the sweep exists to fill, so it has to become walkable again.
        int32_t cx = 0, cy = 0, cz = 0;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        built_columns.erase(chunk_world->get_chunk_map().get_chunk_key(cx, 0, cz));
    }
}

void WorldUpdater::queue_unload(uint64_t key) {
    if (unload_pending.insert(key).second) {
        unload_queue.push_back(key);
    }
}

bool WorldUpdater::find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                                      int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                                      float& out_height) {
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    return height_estimator->find_nearest_biome(target, center_x, center_z,
                                                max_radius_blocks, out_x, out_z, out_height);
}

bool WorldUpdater::chunk_would_be_solid(int32_t cx, int32_t cy, int32_t cz) {
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    // One height sample per COLUMN, not per neighbour. The six around a popped
    // chunk span five columns (the two vertical neighbours share the chunk's own),
    // and neighbouring pops share columns too, so the chunk-keyed memo above this
    // predicate missed 87% of the time - 26,090 misses to 3,757 hits, each miss a
    // ~25 us lattice sample - because it keyed the sample that is per-column. These
    // are the same bounds the sweep's band filter derived (or a worker prefetched)
    // for every column in the band, and `land_h` is exactly HeightRange::min_h, so
    // the tail's probe is a hash lookup.
    const float column_min_h = get_column_surface_bounds(cx, cz).land_h;
    return chunk_would_be_fully_solid_at_column(*height_estimator, cy, column_min_h);
}

void WorldUpdater::clear() {
    // Nothing to carry over a world reload: the fluid states themselves are the
    // record of a flood (they are in the edit maps), so the schedule is derived
    // again when those chunks load.
    fluid_sim.clear();
    initial_loading_latch_.reset();
    initial_mesh_hold_.reset();
    sweep_columns.clear();
    carry_bands.clear();
    carry_ready.clear();
    built_columns.clear();
    chain_queue.clear();
    chain_queued.clear();
    sweep_bands_dirty = true;
    sweep_origin_cx = INT32_MIN;
    sweep_origin_cz = INT32_MIN;
    sweep_band_budget_ms = 2.0;
    unload_queue.clear();
    unload_pending.clear();
    invalidate_height_cache();
    generation_cursor        = SweepCursor{};
    generation_pass_complete = false;
    fill_settled_            = false;
    unload_scan_skip_counter   = 0;
    unload_scan_bucket_cursor  = 0;
}

void WorldUpdater::reset() {
    clear();
    last_player_chunk_x = INT32_MIN;
    last_player_chunk_y = INT32_MIN;
    last_player_chunk_z = INT32_MIN;
    current_render_distance = 64;
}

} // namespace VoxelEngine
