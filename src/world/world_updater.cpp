#include "world/world_updater.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "core/thread_pool.hpp"
#include "core/performance_timer.hpp"
#include "render/material_manager.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>

namespace VoxelEngine {
using namespace godot;

WorldUpdater::WorldUpdater() = default;
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
}

void WorldUpdater::set_vegetation_config(const VegetationConfig& c) {
    vegetation_config = c;
    if (height_estimator) height_estimator->set_vegetation_config(c);
}

void WorldUpdater::set_vegetation_enabled(bool enabled) {
    vegetation_enabled = enabled;
    if (chunk_world) chunk_world->set_vegetation_enabled(enabled);
}

void WorldUpdater::update(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, double delta) {
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
        mesh_manager->reprioritize(player_chunk_x, player_chunk_y, player_chunk_z,
                                   frustum.is_initialized() ? &frustum : nullptr);
    }

    update_generation(is_editor, active_render_distance, epoch, player_chunk_x, player_chunk_y, player_chunk_z, chunk_changed);
    update_unload(active_render_distance, player_chunk_x, player_chunk_y, player_chunk_z, chunk_changed);
    // Fluids tick after generation/unload (so the cells it looks at are the ones
    // that exist now) and before the mesh budgets (so the chunks it dirties can
    // remesh this same frame).
    fluid_sim.advance(delta);
    process_mesh_budgets(is_editor, epoch, chunks_processed_total, active_render_distance, delta);
    flush_dirty(delta);
}

void WorldUpdater::update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch,
                                     int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    ScopedTimer t(*perf_timer, TimerID::ChunkLoadUnload);
    const auto sweep_start = std::chrono::steady_clock::now();
    ++generation_stats.frames;
    // Per-frame halves of the rolling window, written once at the end.
    uint32_t frame_checks = 0;
    uint32_t frame_generations = 0;
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    // The list holds player-relative columns with ABSOLUTE slice ranges, so it
    // goes stale when either half moves: the render distance, the terrain the
    // bands were read from, or the player's chunk in the horizontal axes. A
    // vertical crossing does not (the bands are absolute in chunk y), which is
    // why this compares x/z rather than using the `chunk_changed` flag it is
    // handed. Rebuilding is cheap now — ~3,200 entries against the 208,585 the
    // old list held — where a per-crossing rebuild of the old shape would have
    // been a stall.
    const bool columns_moved = pcx != sweep_origin_cx || pcz != sweep_origin_cz;
    if (sweep_columns.empty() || current_render_distance != active_render_distance ||
        columns_moved || sweep_bands_dirty) {
        rebuild_sweep_columns(active_render_distance, pcx, pcz);
    }
    service_sweep_bands();

    size_t generating_count = chunk_world->get_scheduler().generating_count();
    size_t worker_queue_size = thread_pool ? thread_pool->get_queue_size() : 0;
    size_t worker_count = thread_pool ? thread_pool->get_worker_count() : 0;
    const double worker_pressure =
        budgets.worker_queue_backlog > 0 ? static_cast<double>(worker_queue_size) / static_cast<double>(budgets.worker_queue_backlog) : 0.0;
    const double generating_pressure =
        worker_count > 0 ? static_cast<double>(generating_count) / static_cast<double>(worker_count * budgets.generating_per_worker) : 0.0;
    const double thread_pool_pressure = std::max(worker_pressure, generating_pressure);

    int32_t dynamic_max_generations = budgets.chunk_generations;
    if (thread_pool_pressure > 0.75) {
        const double generation_scale = std::clamp(1.5 - thread_pool_pressure, 0.25, 1.0);
        dynamic_max_generations = std::max(1, static_cast<int32_t>(
            std::round(static_cast<double>(budgets.chunk_generations) * generation_scale)));
    }

    if (chunk_changed) {
        generation_cursor          = SweepCursor{};
        generation_pass_complete   = false;
        generation_sweep_generated = false;
        ++generation_stats.cursor_resets;
    }

    // --- Urgent requests, before anything else ------------------------------
    // A caller (a paste) is waiting on these chunks, and the sweep would mostly
    // refuse them anyway: the sky above a build sits outside the per-column band
    // and is never generated on its own, and a build can reach past the render
    // distance. They are generated here rather than by the sweep because the
    // sweep's filters are exactly what is in the way. The allowance is small and
    // separate from the sweep's, so a huge request streams in over frames
    // instead of stalling this one.
    {
        constexpr size_t kMaxUrgentPerFrame = 8;
        size_t allowance = kMaxUrgentPerFrame;
        // Same backlog guard the sweep uses: if the completed queue is already
        // full, the main thread is behind and generating more would only grow it.
        if (!chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog)) {
            allowance = 0;
        }
        if (allowance > 0) {
            const std::vector<ChunkPos> urgent =
                chunk_world->take_urgent_chunk_requests(allowance);
            for (const ChunkPos& pos : urgent) {
                ++generation_stats.urgent_requested;
                if (pos.y < 0 || pos.y >= kWorldChunkSlices) continue;
                if (generate_chunk(pos.x, pos.y, pos.z, epoch)) {
                    ++generation_stats.urgent_generated;
                }
            }
        }
    }

    const bool frustum_active = frustum.is_initialized();
    const size_t total_offsets = sweep_columns.size();
    const size_t max_checks_per_frame = static_cast<size_t>(
        std::max(dynamic_max_generations * 2, 512));

    // --- Phase 1: Frustum-priority pass ---
    // Walk the sweep list and generate the candidates that are in the camera
    // frustum. Allocates up to half the generation budget to visible chunks.
    // Also counts visible-vs-total candidates to estimate viewport load.
    if (frustum_active && !frustum_pass_complete && total_offsets > 0) {
        size_t   frustum_checks          = 0;
        int32_t  frustum_generations     = 0;
        const size_t max_frustum_checks = std::max(max_checks_per_frame / 2, size_t(64));
        int32_t  visible_in_sweep        = 0;
        int32_t  total_in_sweep          = 0;

        while (frustum_checks < max_frustum_checks &&
               frustum_generations < std::max(dynamic_max_generations / 2, 1) &&
               chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog)) {

            SweepCandidate candidate;
            if (!advance_sweep(frustum_cursor, pcy, candidate)) {
                // "Spent" only if the cursor left the list. Stopping on a band the
                // frontier has not read yet is not the end of anything: the pass
                // resumes next frame, and marking it complete here would retire
                // the frustum pass for good the first time it outran the frontier.
                if (frustum_cursor.column >= sweep_columns.size()) frustum_pass_complete = true;
                break;
            }
            const int32_t cx = candidate.x;
            const int32_t cy = candidate.y;
            const int32_t cz = candidate.z;
            ++frustum_checks;

            ++generation_stats.frustum_checks;
            ++total_in_sweep;
            if (!frustum.is_chunk_visible(cx, cy, cz)) continue;
            ++visible_in_sweep;
            ++generation_stats.frustum_visible;

            uint64_t key = chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz);
            if (chunk_world->get_chunk_map().contains(key)) {
                // Counted, not just skipped: in a settled world almost everything
                // in the frustum is already resident, and without this number the
                // pass reads as if it were finding candidate work it never acts on.
                ++generation_stats.frustum_loaded;
                continue;
            }

            // Safety net, and it should never fire: the list was built from these
            // same bounds, so a reject here means the bands went stale mid-walk
            // (terrain or render distance changed without a rebuild). Cheap
            // enough to keep, and it is what makes the band reject counters in
            // /genstats a check on the pruning rather than a restatement of it.
            const bool fill_column = candidate.fill_column;
            const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
            if (!sweep::chunk_in_band(cy, surface.land_h, surface.top_h, fill_column)) continue;
            // World bounds: never generate chunks outside [0, kSlices) — the
            // fill path no longer has a bottom height filter to catch them.
            if (cy < 0 || cy >= kWorldChunkSlices) continue;

            ++generation_stats.frustum_band_pass;
            if (generate_chunk(cx, cy, cz, epoch)) {
                ++frustum_generations;
                ++generation_stats.frustum_generations;
                generation_sweep_generated = true;
            } else {
                ++generation_stats.frustum_refused;
            }
        }
        if (total_in_sweep > 0) {
            // NOTE: the denominator used to be every slice of every column, sky
            // and bedrock included, which pinned this near 0 and the mesh budgets
            // it scales (0.5x-1.0x) near their floor. It now measures the share of
            // REAL candidates in view, so the mesh budget will sit higher. If
            // meshing becomes the bottleneck, this is where to look.
            visible_chunk_ratio_ = static_cast<float>(visible_in_sweep) / static_cast<float>(total_in_sweep);
        }
    }

    // --- Phase 2: Normal ring-ordered pass ---
    if (!generation_pass_complete && !sweep_columns.empty()) {
        size_t   checks              = 0;
        int32_t  generations_this_frame = 0;

        while (checks < max_checks_per_frame &&
               generations_this_frame < dynamic_max_generations &&
               chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog)) {

            SweepCandidate candidate;
            if (!advance_sweep(generation_cursor, pcy, candidate)) {
                if (generation_cursor.column < sweep_columns.size()) {
                    // Stopped at an unread band: the list is not spent, this frame
                    // has simply reached the frontier. Resetting the cursor here
                    // (as the spent case does) would re-walk the same prefix every
                    // frame and never advance the frontier it is waiting on.
                    break;
                }
                // The whole list has been offered. If none of it generated
                // anything there is nothing left to do until something
                // invalidates the list, so stop walking it every frame.
                generation_cursor = SweepCursor{};
                if (!generation_sweep_generated) {
                    generation_pass_complete = true;
                    ++generation_stats.sweeps_completed;
                    break;
                }
                generation_sweep_generated = false;
                continue;
            }
            const int32_t cx = candidate.x;
            const int32_t cy = candidate.y;
            const int32_t cz = candidate.z;
            ++checks;
            ++generation_stats.checks;
            ++frame_checks;

            uint64_t key = chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz);
            if (chunk_world->get_chunk_map().contains(key)) {
                ++generation_stats.reject_loaded;
                continue;
            }

            int32_t chunk_bottom = cy * CHUNK_HEIGHT;
            int32_t chunk_top    = (cy + 1) * CHUNK_HEIGHT;
            ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
            // Same window the list was built from: full column (band +
            // underground fill) within kUndergroundFillRadius of the player,
            // near-surface band only beyond it.
            const bool fill_column = candidate.fill_column;
            if (static_cast<float>(chunk_bottom) > surface.top_h + 32.0f) {
                ++generation_stats.reject_above;
                continue;
            }
            if (!fill_column && static_cast<float>(chunk_top) < surface.land_h - 32.0f) {
                ++generation_stats.reject_below;
                continue;
            }
            // World bounds: never generate chunks outside [0, kSlices) — the
            // fill path no longer has a bottom height filter to catch them.
            if (cy < 0 || cy >= kWorldChunkSlices) {
                ++generation_stats.reject_oob;
                continue;
            }

            ++generation_stats.band_pass;
            if (generate_chunk(cx, cy, cz, epoch)) {
                ++generations_this_frame;
                ++generation_stats.generations;
                ++frame_generations;
                generation_sweep_generated = true;
            } else {
                ++generation_stats.generate_refused;
            }
        }
    }

    {
        const std::chrono::duration<double, std::milli> elapsed =
            std::chrono::steady_clock::now() - sweep_start;
        generation_stats.last_ms = elapsed.count();
        generation_stats.total_ms += elapsed.count();
        generation_stats.max_ms = std::max(generation_stats.max_ms, generation_stats.last_ms);
        generation_stats.window_checks[generation_stats.window_head] = frame_checks;
        generation_stats.window_generations[generation_stats.window_head] = frame_generations;
        generation_stats.window_head =
            (generation_stats.window_head + 1) % GenerationStats::kWindowFrames;
        if (generation_stats.window_frames < GenerationStats::kWindowFrames) {
            ++generation_stats.window_frames;
        }
    }
}

void WorldUpdater::update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    if (chunk_changed || (++unload_scan_skip_counter >= kUnloadScanSkipFrames)) {
        unload_scan_skip_counter = 0;
        int32_t unload_hrd  = active_render_distance + 2;
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
        int32_t unload_hrd = active_render_distance + 2;
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
    const bool is_initial_loading = loaded_count_post < budgets.loading_threshold;

    const size_t generating_count = chunk_world->get_scheduler().generating_count();
    const bool pipelines_busy =
        generating_count > 0 ||
        chunk_world->get_scheduler().completed_chunk_count() > 0 ||
        (mesh_manager && mesh_manager->has_pending_mesh_work());

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
    // Scale budgets by viewport load: few visible chunks → less urgency, save CPU.
    // Many visible chunks → keep full budget for visible-area quality.
    if (!is_initial_loading) {
        const float visibility_scale = 0.5f + visible_chunk_ratio_ * 0.5f;
        mesh_rebuild_budget = std::max(1, static_cast<int32_t>(static_cast<float>(mesh_rebuild_budget) * visibility_scale));
        upload_budget       = std::max(1, static_cast<int32_t>(static_cast<float>(upload_budget) * visibility_scale));
    }

    {
        ScopedTimer t(*perf_timer, TimerID::ProcessCompletedChunks);
        int32_t active_max = is_initial_loading ? budgets.chunk_completions_initial : budgets.chunk_completions_gameplay;
        int32_t scaled_completion_budget = std::min(active_max + 16, active_render_distance + 16);
        int32_t installed = chunk_world->process_completed_chunks(
            epoch,
            budgets.processing_budget_ms,
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
    {
        ScopedTimer t(*perf_timer, TimerID::ProcessCompletedMeshes);
        mesh_manager->process_completed_meshes(
            epoch,
            budgets.mesh_completion_budget_ms,
            upload_budget,
            material_manager->get_material(),
            material_manager->get_water_material()
        );
    }

    {
        ScopedTimer t(*perf_timer, TimerID::DirtyMeshQueue);
        mesh_manager->process_queue(
            budgets.mesh_rebuilds_immediate,
            mesh_rebuild_budget,
            budgets.processing_budget_ms
        );
    }
}

void WorldUpdater::flush_dirty(double delta) {
    dirty_flush_accumulator += delta;
    if (dirty_flush_accumulator >= budgets.flush_interval) {
        chunk_world->flush_dirty_chunks();
        dirty_flush_accumulator = 0.0;
    }
}

bool WorldUpdater::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch) {
    return chunk_world->generate_chunk(chunk_x, chunk_y, chunk_z, epoch, terrain_params,
                                       biome_config, vegetation_config);
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
    return chunk_would_be_fully_solid(*height_estimator, cx, cy, cz);
}

WorldUpdater::ColumnSurfaceBounds WorldUpdater::get_column_surface_bounds(int32_t cx, int32_t cz) {
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(cx)) << 32)
                 | static_cast<uint64_t>(static_cast<uint32_t>(cz));
    auto it = column_height_cache.find(key);
    if (it != column_height_cache.end()) {
        return it->second;
    }
    // FIFO eviction: evict oldest entries when cache is full.
    if (column_height_cache.size() >= 65536) {
        uint64_t oldest = column_height_fifo.front();
        column_height_fifo.pop_front();
        column_height_cache.erase(oldest);
    }
    // Rigorous content bounds over the WHOLE chunk area (all 4-block lattice
    // nodes, not just the center column). On steep terrain a biome border or
    // mountain wall can cross a chunk while the center column sits far below
    // the local high side — a single center sample then understates the top,
    // and the scheduler permanently skips chunks that genuinely contain the
    // wall, leaving invisible-solid holes (no mesh, no data to place into).
    // This range already pads by ChunkGenerator::density_margin(), so it bounds every column's
    // real content; land_h is the lowest possible surface (everything below is
    // solid rock), top_h the highest (air above, with water to sea level).
    const ChunkGenerator::HeightRange range = height_estimator->get_chunk_height_range(cx, cz);
    ColumnSurfaceBounds b;
    b.land_h = range.min_h;
    b.top_h  = std::max(range.max_h, range.max_water_h);
    column_height_cache[key] = b;
    column_height_fifo.push_back(key);
    return b;
}

void WorldUpdater::invalidate_height_cache() {
    column_height_cache.clear();
    column_height_fifo.clear();
    // The sweep list carries a slice range per column, read from this cache, so a
    // change of seed, sea level, terrain params or biome config invalidates the
    // list with it — otherwise the sweep would keep generating against bands from
    // the previous terrain.
    sweep_bands_dirty = true;
}

void WorldUpdater::service_sweep_bands() {
    if (sweep_band_frontier >= sweep_columns.size()) return;  // caught up
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    // Two milliseconds of a 16 ms frame, always at least one column so the
    // frontier can never stall on a column slower than the budget. The list is
    // nearest-first, so this always banks the terrain the player is standing in
    // before anything far away.
    constexpr double kBandBudgetMs = 2.0;
    const auto start = std::chrono::steady_clock::now();
    double band_ms = 0.0;
    while (sweep_band_frontier < sweep_columns.size()) {
        SweepColumn& column = sweep_columns[sweep_band_frontier];
        const int32_t cx = sweep_origin_cx + column.dx;
        const int32_t cz = sweep_origin_cz + column.dz;
        const ColumnSurfaceBounds surface = get_column_surface_bounds(cx, cz);
        const bool fill_column = std::abs(static_cast<int32_t>(column.dx)) <= kUndergroundFillRadius &&
                                 std::abs(static_cast<int32_t>(column.dz)) <= kUndergroundFillRadius;
        column.band = sweep::band_for_column(surface.land_h, surface.top_h, fill_column, kWorldChunkSlices);
        column.band_ready = true;
        // Decided here, where the band was just computed, rather than by the walk
        // that would otherwise look up every slice of it on every pass. A chunk in
        // flight is not resident yet, so such a column stays walkable until a
        // rebuild — a wasted walk, never a hole.
        if (sweep::band_fully_resident(column.band, [&](int32_t cy) {
                return chunk_world->get_chunk_map().contains(
                    chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz));
            })) {
            built_columns.insert(chunk_world->get_chunk_map().get_chunk_key(cx, 0, cz));
            ++generation_stats.columns_built;
        }
        generation_stats.candidate_offsets += static_cast<uint64_t>(sweep::count(column.band));
        ++generation_stats.band_reads;
        ++sweep_band_frontier;
        band_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - start).count();
        if (band_ms >= kBandBudgetMs) break;
    }
    // The elapsed value is written straight into the stats so the timing costs
    // nothing per column; `max` is the worst frame the budget allowed to slip.
    generation_stats.total_band_ms += band_ms;
    generation_stats.max_band_ms = std::max(generation_stats.max_band_ms, band_ms);
}

bool WorldUpdater::advance_sweep(SweepCursor& cursor, int32_t pcy, SweepCandidate& out) {
    while (cursor.column < sweep_columns.size()) {
        const SweepColumn& column = sweep_columns[cursor.column];
        // Not read yet: the walk stops here rather than reading it, so the cost
        // stays on service_sweep_bands' budget and both stay in ring order.
        if (!column.band_ready) return false;
        // Already fully built: skip the whole column for one lookup instead of
        // re-confirming each of its chunks. Both passes share this walk, so the
        // frustum pass gets the same skip.
        if (!built_columns.empty()) {
            const uint64_t column_key = chunk_world->get_chunk_map().get_chunk_key(
                sweep_origin_cx + column.dx, 0, sweep_origin_cz + column.dz);
            if (built_columns.count(column_key) != 0) {
                ++generation_stats.columns_skipped;
                ++cursor.column;
                cursor.slice = 0;
                continue;
            }
        }
        int32_t cy = 0;
        if (sweep::slice_cy(column.band, pcy, cursor.slice, cy)) {
            ++cursor.slice;
            out.x = sweep_origin_cx + column.dx;
            out.y = cy;
            out.z = sweep_origin_cz + column.dz;
            out.fill_column = std::abs(static_cast<int32_t>(column.dx)) <= kUndergroundFillRadius &&
                              std::abs(static_cast<int32_t>(column.dz)) <= kUndergroundFillRadius;
            return true;
        }
        ++cursor.column;
        cursor.slice = 0;
    }
    return false;
}

void WorldUpdater::rebuild_sweep_columns(int32_t horizontal_rd, int32_t pcx, int32_t pcz) {
    const auto rebuild_start = std::chrono::steady_clock::now();
    ++generation_stats.rebuilds;
    // Created once, never reconfigured here. Every setter that changes the
    // terrain (seed, sea level, biome size, terrain params, biome config) already
    // pushes the new settings into the estimator AND invalidates the height cache
    // — which is what sends us back through this function — so reconfiguring it
    // again is not just redundant: it was 19 ms per chunk crossing, which is a
    // dropped frame every time the player crossed into a new chunk while flying.
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
        height_estimator->set_biome_config(biome_config);
        height_estimator->set_vegetation_config(vegetation_config);
    }
    if (column_height_cache.empty()) column_height_cache.reserve(65536);
    // No vertical render distance: a column whose terrain sits far above or
    // below the player is still reachable, because what bounds generation is the
    // column's own content band, not a window around the player. The band is
    // computed ONCE per column here (see world/sweep_band.hpp) instead of being
    // re-tested for all 65 slices of every column while walking — which is where
    // 84% of a 208,585-entry walk was being thrown away.
    constexpr int32_t kWorldChunkSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    current_render_distance = horizontal_rd;
    sweep_origin_cx = pcx;
    sweep_origin_cz = pcz;
    sweep_bands_dirty = false;
    sweep_columns.clear();
    // Every band in the new list is unknown, and a column's contents may have
    // changed under the old marks, so the whole set goes with the list and is
    // re-earned as the frontier reads each new band.
    built_columns.clear();
    sweep_band_frontier = 0;
    // Every band in the new list is unknown, so the candidate total starts over
    // and climbs as the frontier reads them (it reaches the true total within a
    // frame or two once the heights are cached, and over a couple of seconds on
    // the very first build, where every one of them is cold).
    generation_stats.candidate_offsets = 0;
    unload_queue.clear();

    for (int32_t dx = -horizontal_rd; dx <= horizontal_rd; ++dx) {
        for (int32_t dz = -horizontal_rd; dz <= horizontal_rd; ++dz) {
            if (dx * dx + dz * dz > horizontal_rd * horizontal_rd) continue;
            // A column's band is read later, in list order (service_sweep_bands).
            // Columns whose band turns out to be empty are simply walked past.
            sweep_columns.push_back(SweepColumn{ static_cast<int16_t>(dx),
                                                 static_cast<int16_t>(dz),
                                                 sweep::ChunkBand{},
                                                 false });
        }
    }

    // Nearest columns first — rings expand outward around the player. This is now
    // the whole ordering: the old list also sorted by vertical distance from the
    // player's slice, which only existed because it carried 62 unusable slices
    // per column. With those gone, the slice order inside a single column is the
    // only vertical question left, and the walk answers it (centre-out from the
    // player, see advance_sweep).
    std::sort(sweep_columns.begin(), sweep_columns.end(),
        [](const SweepColumn& a, const SweepColumn& b) {
            const int32_t ha = static_cast<int32_t>(a.dx) * a.dx + static_cast<int32_t>(a.dz) * a.dz;
            const int32_t hb = static_cast<int32_t>(b.dx) * b.dx + static_cast<int32_t>(b.dz) * b.dz;
            if (ha != hb) return ha < hb;
            if (a.dx != b.dx) return a.dx < b.dx;
            return a.dz < b.dz;
        }
    );

    generation_stats.candidate_columns = sweep_columns.size();
    const std::chrono::duration<double, std::milli> rebuild_elapsed =
        std::chrono::steady_clock::now() - rebuild_start;
    generation_stats.last_rebuild_ms = rebuild_elapsed.count();
    generation_stats.total_rebuild_ms += rebuild_elapsed.count();
    generation_stats.max_rebuild_ms = std::max(generation_stats.max_rebuild_ms, generation_stats.last_rebuild_ms);
}

void WorldUpdater::clear() {
    // Nothing to carry over a world reload: the fluid states themselves are the
    // record of a flood (they are in the edit maps), so the schedule is derived
    // again when those chunks load.
    fluid_sim.clear();
    sweep_columns.clear();
    built_columns.clear();
    sweep_bands_dirty = true;
    sweep_origin_cx = INT32_MIN;
    sweep_origin_cz = INT32_MIN;
    unload_queue.clear();
    unload_pending.clear();
    invalidate_height_cache();
    frustum_cursor             = SweepCursor{};
    generation_cursor          = SweepCursor{};
    frustum_pass_complete      = false;
    generation_pass_complete   = false;
    generation_sweep_generated = false;
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
