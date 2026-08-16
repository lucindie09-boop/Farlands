#include "world/world_updater.hpp"

#include "worldgen/chunk_generator.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "core/thread_pool.hpp"
#include "core/performance_timer.hpp"
#include "render/material_manager.hpp"
#include <godot_cpp/classes/engine.hpp>
#include <cmath>
#include <algorithm>

namespace VoxelEngine {
using namespace godot;

WorldUpdater::WorldUpdater() = default;
WorldUpdater::~WorldUpdater() = default;

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
        mesh_manager->reprioritize(player_chunk_x, player_chunk_y, player_chunk_z,
                                   frustum.is_initialized() ? &frustum : nullptr);
        mesh_manager->set_player_chunk(player_chunk_x, player_chunk_y, player_chunk_z);
        mesh_manager->set_mesh_render_distance(active_render_distance);
        mesh_manager->set_lod_distance(lod_distance);
        mesh_manager->set_lod_detail_level(lod_detail_level);
        mesh_manager->set_far_lod_distance(far_lod_distance);
        mesh_manager->set_far_lod_detail_level(far_lod_detail_level);
    }

    update_generation(is_editor, active_render_distance, epoch, player_chunk_x, player_chunk_y, player_chunk_z, chunk_changed);
    update_unload(active_render_distance, player_chunk_x, player_chunk_y, player_chunk_z, chunk_changed);
    process_mesh_budgets(is_editor, epoch, chunks_processed_total, active_render_distance, delta);
    flush_dirty(delta);
}

void WorldUpdater::update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch,
                                     int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    ScopedTimer t(*perf_timer, TimerID::ChunkLoadUnload);
    if (pre_sorted_offsets.empty() ||
        current_render_distance != active_render_distance) {
        initialize_view_distance(active_render_distance);
    }

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
        generation_cursor          = 0;
        generation_pass_complete   = false;
        generation_sweep_generated = false;
    }

    const bool frustum_active = frustum.is_initialized();
    const size_t total_offsets = pre_sorted_offsets.size();
    const size_t max_checks_per_frame = static_cast<size_t>(
        std::max(dynamic_max_generations * 2, 512));

    // --- Phase 1: Frustum-priority pass ---
    // Walk the offset list and generate chunks that are in the camera frustum.
    // Allocates up to half the generation budget to visible chunks.
    // Also counts visible-vs-total offsets to estimate viewport load.
    if (frustum_active && !frustum_pass_complete && total_offsets > 0) {
        size_t   frustum_checks          = 0;
        int32_t  frustum_generations     = 0;
        const size_t max_frustum_checks = std::max(max_checks_per_frame / 2, size_t(64));
        int32_t  visible_in_sweep        = 0;
        int32_t  total_in_sweep          = 0;

        while (frustum_checks < max_frustum_checks &&
               frustum_generations < std::max(dynamic_max_generations / 2, 1) &&
               chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog)) {

            if (frustum_cursor >= total_offsets) {
                frustum_pass_complete = true;
                break;
            }

            const ChunkPos& offset = pre_sorted_offsets[frustum_cursor++];
            ++frustum_checks;

            int32_t cx = pcx + offset.x;
            int32_t cy = pcy + offset.y;
            int32_t cz = pcz + offset.z;

            ++total_in_sweep;
            if (!frustum.is_chunk_visible(cx, cy, cz)) continue;
            ++visible_in_sweep;

            uint64_t key = chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz);
            if (chunk_world->get_chunk_map().contains(key)) continue;

            int32_t chunk_bottom = cy * CHUNK_HEIGHT;
            int32_t chunk_top    = (cy + 1) * CHUNK_HEIGHT;
            float   surface_h    = get_column_surface_height(cx, cz);
            bool near_player = std::abs(offset.x) <= 1 && std::abs(offset.z) <= 1 && std::abs(offset.y) <= 1;
            if (!near_player) {
                if (static_cast<float>(chunk_top)   < surface_h - 32.0f) continue;
                if (static_cast<float>(chunk_bottom) > surface_h + 32.0f) continue;
            }

            if (generate_chunk(cx, cy, cz, epoch)) {
                ++frustum_generations;
                generation_sweep_generated = true;
            }
        }
        if (total_in_sweep > 0) {
            visible_chunk_ratio_ = static_cast<float>(visible_in_sweep) / static_cast<float>(total_in_sweep);
        }
    }

    // --- Phase 2: Normal distance-sorted pass ---
    if (!generation_pass_complete && !pre_sorted_offsets.empty()) {
        size_t   checks              = 0;
        int32_t  generations_this_frame = 0;

        while (checks < max_checks_per_frame &&
               generations_this_frame < dynamic_max_generations &&
               chunk_world->get_scheduler().can_enqueue(budgets.completed_queue_backlog)) {

            if (generation_cursor >= total_offsets) {
                generation_cursor = 0;
                if (!generation_sweep_generated) {
                    generation_pass_complete = true;
                    break;
                }
                generation_sweep_generated = false;
            }

            const ChunkPos& offset = pre_sorted_offsets[generation_cursor++];
            ++checks;

            int32_t cx = pcx + offset.x;
            int32_t cy = pcy + offset.y;
            int32_t cz = pcz + offset.z;

            uint64_t key = chunk_world->get_chunk_map().get_chunk_key(cx, cy, cz);
            if (chunk_world->get_chunk_map().contains(key)) {
                continue;
            }

            int32_t chunk_bottom = cy * CHUNK_HEIGHT;
            int32_t chunk_top    = (cy + 1) * CHUNK_HEIGHT;
            float   surface_h    = get_column_surface_height(cx, cz);
            bool near_player = std::abs(offset.x) <= 1 && std::abs(offset.z) <= 1 && std::abs(offset.y) <= 1;
            if (!near_player) {
                if (static_cast<float>(chunk_top)   < surface_h - 32.0f) continue;
                if (static_cast<float>(chunk_bottom) > surface_h + 32.0f) continue;
            }

            if (generate_chunk(cx, cy, cz, epoch)) {
                ++generations_this_frame;
                generation_sweep_generated = true;
            }
        }
    }
}

void WorldUpdater::update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed) {
    if (chunk_changed || (++unload_scan_skip_counter >= kUnloadScanSkipFrames)) {
        unload_scan_skip_counter = 0;
        int32_t unload_hrd  = active_render_distance + 2;
        int32_t unload_hrd2 = unload_hrd * unload_hrd;
int32_t unload_vrd = active_render_distance + 2;

        chunk_world->get_chunk_map().for_each_limited_resumable([&](uint64_t key, const std::unique_ptr<ChunkRenderData>&) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            int32_t dx = cx - pcx;
            int32_t dz = cz - pcz;
            int32_t horiz_dist2 = dx * dx + dz * dz;
int32_t dy = cy - pcy;
            if (horiz_dist2 > unload_hrd2 || std::abs(dy) > unload_vrd) {
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
int32_t unload_vrd = active_render_distance + 2;
        const bool frustum_active = frustum.is_initialized();
        // Frustum-aware unload: prefer unloading non-visible chunks first.
        // Collect visible chunks and defer them, unload non-visible chunks now.
        std::vector<uint64_t> visible_deferred;
        while (!unload_queue.empty() && unloads_this_frame < budgets.unloads_per_frame) {
            uint64_t key = unload_queue.back();
            unload_queue.pop_back();
            if (chunk_world->get_chunk_map().contains(key)) {
                int32_t cx = 0, cy = 0, cz = 0;
                ChunkMap::decode_chunk_key(key, cx, cy, cz);
                int32_t dx = cx - pcx;
                int32_t dz = cz - pcz;
                int32_t horiz_dist2 = dx * dx + dz * dz;
int32_t dy = cy - pcy;
                if (horiz_dist2 > unload_hrd2 || std::abs(dy) > unload_vrd) {
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
    }
}

void WorldUpdater::queue_unload(uint64_t key) {
    if (unload_pending.insert(key).second) {
        unload_queue.push_back(key);
    }
}

float WorldUpdater::get_column_surface_height(int32_t cx, int32_t cz) {
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
    float h = height_estimator->get_terrain_height(
        cx * CHUNK_WIDTH + CHUNK_WIDTH / 2,
        cz * CHUNK_DEPTH + CHUNK_DEPTH / 2);
    column_height_cache[key] = h;
    column_height_fifo.push_back(key);
    return h;
}

void WorldUpdater::invalidate_height_cache() {
    column_height_cache.clear();
    column_height_fifo.clear();
}

void WorldUpdater::initialize_view_distance(int32_t horizontal_rd) {
    if (!height_estimator) {
        height_estimator = std::make_unique<ChunkGenerator>(terrain_params);
    } else {
        height_estimator->set_params(terrain_params);
    }
    height_estimator->set_biome_config(biome_config);
    height_estimator->set_vegetation_config(vegetation_config);
    column_height_cache.reserve(65536);
    // Vertical range covers terrain variation up to ~500-block mountain peaks.
    constexpr int32_t VERTICAL_BUFFER = 10;
    current_render_distance = horizontal_rd;
    pre_sorted_offsets.clear();
    unload_queue.clear();

    for (int32_t x = -horizontal_rd; x <= horizontal_rd; ++x) {
        for (int32_t y = -VERTICAL_BUFFER; y <= VERTICAL_BUFFER; ++y) {
            for (int32_t z = -horizontal_rd; z <= horizontal_rd; ++z) {
                int32_t horiz_dist2 = x * x + z * z;
                if (horiz_dist2 <= (horizontal_rd * horizontal_rd)) {
                    pre_sorted_offsets.push_back(ChunkPos{x, y, z});
                }
            }
        }
    }

    std::sort(pre_sorted_offsets.begin(), pre_sorted_offsets.end(),
        [](const ChunkPos& a, const ChunkPos& b) {
            return (a.x * a.x + a.y * a.y + a.z * a.z) < (b.x * b.x + b.y * b.y + b.z * b.z);
        }
    );
}

void WorldUpdater::clear() {
    pre_sorted_offsets.clear();
    unload_queue.clear();
    unload_pending.clear();
    invalidate_height_cache();
    generation_cursor          = 0;
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
