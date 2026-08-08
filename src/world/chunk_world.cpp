#include "world/chunk_world.hpp"

#include <godot_cpp/classes/rendering_server.hpp>

#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "lighting/block_light_region.hpp"
#include "worldgen/chunk_generator.hpp"
#include "mesh/chunk_boundary_dirty.hpp"
#include "core/crc32.hpp"
#include "core/edit_map.hpp"

namespace VoxelEngine {

using namespace godot;

bool ChunkWorld::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch, const TerrainParams& params) {
    if (!thread_pool) return false;
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    return chunk_scheduler.enqueue_generation(
        thread_pool, chunk_x, chunk_y, chunk_z, epoch, key,
        [this](uint64_t k) { return chunk_map.contains(k); },
        [this, params = params](int32_t cx, int32_t cy, int32_t cz, bool& loaded) {
            auto chunk_data = std::make_unique<ChunkData>();
            
            // Always generate - edit maps are applied as a diff on top
            thread_local ChunkGenerator generator;
            generator.set_params(params);

            int32_t world_y_start = cy * CHUNK_HEIGHT;
            int32_t world_y_end = world_y_start + CHUNK_HEIGHT;

if (world_y_start >= WORLD_HEIGHT_Y || world_y_end <= 0) {
chunk_data->clear();
chunk_data->propagate_sky_light(nullptr);
chunk_data->compute_fully_solid();
return chunk_data;
}

            // Fast estimation: skip chunks that are entirely air or entirely solid
            auto height_range = generator.get_chunk_height_range(cx, cz);
            float margin = 3.0f; // safety margin for intra-chunk height variation
float top_content_h = std::max(height_range.max_h, height_range.max_water_h);

            // Entirely above surface: all air
            if (world_y_start > static_cast<int32_t>(top_content_h + margin)) {
                chunk_data->clear();
                chunk_data->propagate_sky_light(nullptr); // sky light = 15 for all air
                chunk_data->compute_fully_solid();
                return chunk_data;
            }

            // Entirely below surface (and below bedrock): all bedrock
            if (world_y_end <= params.bedrock_height) {
                chunk_data->fill_blocks(BlockIDs::BEDROCK);
                chunk_data->propagate_sky_light(nullptr); // first block is opaque → all light = 0
                return chunk_data;
            }

            // Caves only form inside [bedrock_height+3, sea_level+10]
            // (see ChunkGenerator::is_cave). A chunk overlapping that range is
            // not automatically solid even when it sits below the surface.
            const int32_t cave_min_y = params.bedrock_height + 3;
            const int32_t cave_max_y = static_cast<int32_t>(params.sea_level) + 10;
            const bool may_contain_caves =
                world_y_end > cave_min_y && world_y_start < cave_max_y;

            // Entirely below surface but above bedrock: all solid subsurface block.
            if (!may_contain_caves && world_y_end < static_cast<int32_t>(height_range.min_h - margin)) {
                BlockID solid_block = generator.get_chunk_subsurface_block(cx, cz);
                chunk_data->fill_blocks(solid_block);
                chunk_data->propagate_sky_light(nullptr); // first block is opaque → all light = 0
                return chunk_data;
            }

            // Surface chunk: generate normally
            {
                // Cross-chunk vegetation writes are deferred: the worker only pushes to
                // vegetation queues (no shard locking). The main thread applies them
                // during process_completed_chunks. These are NOT persisted.
                auto cross_writer = [this](int32_t wx, int32_t wy, int32_t wz, BlockID block) {
                    queue_vegetation_placement(wx, wy, wz, static_cast<int>(block));
                    int32_t tc_x, tc_y, tc_z, lx, ly, lz;
                    world_to_chunk_local(wx, wy, wz, tc_x, tc_y, tc_z, lx, ly, lz);
                    {
                        std::lock_guard<std::mutex> lock(cross_boundary_mutex);
                        pending_cross_boundary_remesh.push_back({tc_x, tc_y, tc_z});
                    }
                };
                generator.generate_chunk(*chunk_data, cx, cy, cz,
                                         ChunkGenerator::CrossChunkWriter(std::move(cross_writer)),
                                         vegetation_enabled);
            }
            
            chunk_data->propagate_sky_light(chunk_map.get_chunk_data(cx, cy + 1, cz));
            if (chunk_data->get_emissive_count() > 0) {
                chunk_data->propagate_light();
            }
            chunk_data->compute_fully_solid();
            
            // Load edit map from disk if not already in memory, then apply it
            uint64_t key = chunk_map.get_chunk_key(cx, cy, cz);
            {
                std::lock_guard<std::mutex> lock(edit_maps_mutex);
                if (chunk_edit_maps.find(key) == chunk_edit_maps.end()) {
                    EditMap loaded;
                    if (load_edit_map_from_disk(cx, cy, cz, loaded)) {
                        chunk_edit_maps[key] = std::move(loaded);
                    }
                }
            }
            apply_edit_map_to_chunk(key, cx, cy, cz, *chunk_data);
            
            return chunk_data;
        },
        [this]() { return async_epoch.load(std::memory_order_acquire); }
    );
}

int32_t ChunkWorld::process_completed_chunks(uint64_t epoch, double budget_ms, int32_t max_installs, int32_t max_lighting, int32_t max_dirties, int32_t player_cx, int32_t player_cy, int32_t player_cz, int32_t render_distance) {
    // Fast path: skip the loop entirely if nothing is pending and the scheduler is empty.
    if (pending_chunk_lighting.empty() && pending_chunk_dirty_mesh.empty() && pending_chunk_installs.empty() &&
        chunk_scheduler.completed_chunk_count() == 0) {
        return 0;
    }

    int32_t dynamic_max_installs = max_installs;
    int32_t dynamic_max_lighting = max_lighting;
    int32_t dynamic_max_dirty = max_dirties;

    auto start_time = std::chrono::high_resolution_clock::now();
    int32_t installs_this_frame = 0;
    int32_t lights_this_frame = 0;
    int32_t dirties_this_frame = 0;
    int32_t installed_count = 0;

    while (true) {
        auto current_time = std::chrono::high_resolution_clock::now();
        double elapsed_ms = std::chrono::duration<double, std::milli>(current_time - start_time).count();
        if (elapsed_ms >= budget_ms) break;

        // Poll completed light propagations (fire-and-forget worker results)
        // Fast-path: skip mutex entirely when queue is empty (atomic counter check)
        if (chunk_scheduler.completed_light_count() != 0) {
            CompletedLightPropagation completed;
            while (chunk_scheduler.poll_completed_light_propagation(completed)) {
                if (completed.epoch != epoch) continue;
                if (mesh_manager) {
                    mesh_manager->mark_chunks_dirty_for_light(completed.chunk_x, completed.chunk_y, completed.chunk_z);
                }
                pending_chunk_dirty_mesh.push_back({completed.chunk_x, completed.chunk_y, completed.chunk_z, completed.epoch});
            }
        }

        if (lights_this_frame < dynamic_max_lighting && !pending_chunk_lighting.empty()) {
            PendingChunkStage stage = pending_chunk_lighting.front();
            pending_chunk_lighting.pop_front();
            lights_this_frame++;

            if (stage.epoch != epoch) {
                continue;
            }

            uint64_t key = chunk_map.get_chunk_key(stage.chunk_x, stage.chunk_y, stage.chunk_z);
            bool any_emissive_in_region = false;
            bool took_fire_and_forget = false;
            {
                uint64_t keys[28];
                keys[0] = key;
                int idx = 1;
                for (int dz = -1; dz <= 1; dz++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            keys[idx++] = chunk_map.get_chunk_key(stage.chunk_x + dx, stage.chunk_y + dy, stage.chunk_z + dz);
                auto lock = chunk_map.lock_keys_exclusive(keys);
                if (!chunk_map.contains_fast(key)) continue;

                if (light_propagated_chunks.find(key) != light_propagated_chunks.end()) {
                    continue;
                }
                light_propagated_chunks.insert(key);

                bool any_emissive = false;
                for (int dz = -1; dz <= 1 && !any_emissive; dz++) {
                    for (int dy = -1; dy <= 1 && !any_emissive; dy++) {
                        for (int dx = -1; dx <= 1 && !any_emissive; dx++) {
                            ChunkData* n = chunk_map.get_chunk_data_fast(stage.chunk_x + dx, stage.chunk_y + dy, stage.chunk_z + dz);
                            if (n && n->get_emissive_count() > 0) {
                                any_emissive = true;
                            }
                        }
                    }
                }
                any_emissive_in_region = any_emissive;

                if (any_emissive && thread_pool) {
                    took_fire_and_forget = true;
                    int32_t cx = stage.chunk_x;
                    int32_t cy = stage.chunk_y;
                    int32_t cz = stage.chunk_z;
                    thread_pool->fire_and_forget([this, cx, cy, cz, epoch]() {
                        {
                            uint64_t keys[27];
                            int idx = 0;
                            for (int dz = -1; dz <= 1; dz++)
                                for (int dy = -1; dy <= 1; dy++)
                                    for (int dx = -1; dx <= 1; dx++)
                                        keys[idx++] = chunk_map.get_chunk_key(cx + dx, cy + dy, cz + dz);
                            auto wlock = chunk_map.lock_keys_exclusive(keys);
                            ChunkData* region_grid[3][3][3] = {};
                            for (int dz = -1; dz <= 1; dz++) {
                                for (int dy = -1; dy <= 1; dy++) {
                                    for (int dx = -1; dx <= 1; dx++) {
                                        region_grid[dx + 1][dy + 1][dz + 1] = chunk_map.get_chunk_data_fast(cx + dx, cy + dy, cz + dz);
                                    }
                                }
                            }
                            BlockLightRegion light_region(region_grid);
                            std::vector<EmissiveSource> sources;
                            light_region.collect_emissive_sources(sources);
                            light_region.clear_block_light();
                            light_region.propagate_additive(sources);
                        }
                        chunk_scheduler.push_completed_light_propagation({cx, cy, cz, epoch});
                    });
                } else {
                    pending_chunk_dirty_mesh.push_back({stage.chunk_x, stage.chunk_y, stage.chunk_z, stage.epoch});
                }
            }

            if (!took_fire_and_forget && any_emissive_in_region && light_propagator) {
                light_propagator->propagate_block_light_region(stage.chunk_x, stage.chunk_y, stage.chunk_z);
            }

            continue;
        }

        if (dirties_this_frame < dynamic_max_dirty && !pending_chunk_dirty_mesh.empty()) {
            PendingChunkStage stage = pending_chunk_dirty_mesh.front();
            pending_chunk_dirty_mesh.pop_front();
            dirties_this_frame++;

            if (stage.epoch != epoch) {
                continue;
            }

            uint64_t key = chunk_map.get_chunk_key(stage.chunk_x, stage.chunk_y, stage.chunk_z);
            {
                // Batch-lock the chunk + its 6 neighbors
                uint64_t neighbor_keys[7] = {
                    key,
                    chunk_map.get_chunk_key(stage.chunk_x - 1, stage.chunk_y,     stage.chunk_z    ),
                    chunk_map.get_chunk_key(stage.chunk_x + 1, stage.chunk_y,     stage.chunk_z    ),
                    chunk_map.get_chunk_key(stage.chunk_x,     stage.chunk_y - 1, stage.chunk_z    ),
                    chunk_map.get_chunk_key(stage.chunk_x,     stage.chunk_y + 1, stage.chunk_z    ),
                    chunk_map.get_chunk_key(stage.chunk_x,     stage.chunk_y,     stage.chunk_z - 1),
                    chunk_map.get_chunk_key(stage.chunk_x,     stage.chunk_y,     stage.chunk_z + 1)
                };
                auto lock = chunk_map.lock_keys(std::vector<uint64_t>(neighbor_keys, neighbor_keys + 7));
                if (!chunk_map.contains_fast(key)) continue;

                if (mesh_manager) {
                    mesh_manager->queue_dirty_chunk(stage.chunk_x,     stage.chunk_y,     stage.chunk_z);
                    ChunkData* installed_chunk = chunk_map.get_chunk_data_fast(stage.chunk_x, stage.chunk_y, stage.chunk_z);
                    if (installed_chunk) {
                        ChunkData* neighbor = nullptr;
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x - 1, stage.chunk_y, stage.chunk_z);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Left, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x - 1, stage.chunk_y,     stage.chunk_z);
                        }
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x + 1, stage.chunk_y, stage.chunk_z);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Right, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x + 1, stage.chunk_y,     stage.chunk_z);
                        }
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x, stage.chunk_y - 1, stage.chunk_z);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Bottom, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x,     stage.chunk_y - 1, stage.chunk_z);
                        }
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x, stage.chunk_y + 1, stage.chunk_z);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Top, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x,     stage.chunk_y + 1, stage.chunk_z);
                        }
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x, stage.chunk_y, stage.chunk_z - 1);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Back, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x,     stage.chunk_y,     stage.chunk_z - 1);
                        }
                        
                        neighbor = chunk_map.get_chunk_data_fast(stage.chunk_x, stage.chunk_y, stage.chunk_z + 1);
                        if (should_dirty_neighbor(neighbor, FaceDirection::Front, installed_chunk)) {
                            mesh_manager->queue_dirty_chunk(stage.chunk_x,     stage.chunk_y,     stage.chunk_z + 1);
                        }
                    }
                }
            }
            continue;
        }

        if (installs_this_frame < dynamic_max_installs) {
            if (pending_chunk_installs.empty()) {
                CompletedChunk completed;
                if (chunk_scheduler.poll_completed_chunk(completed)) {
                    if (completed.chunk_data) {
                        pending_chunk_installs.push_back(std::move(completed));
                    }
                }
            }

            if (pending_chunk_installs.empty()) {
                break;
            }

            CompletedChunk completed = std::move(pending_chunk_installs.front());
            pending_chunk_installs.pop_front();
            installs_this_frame++;

            if (completed.epoch != epoch) {
                continue;
            }

            uint64_t key = chunk_map.get_chunk_key(completed.chunk_x, completed.chunk_y, completed.chunk_z);

            if (chunk_map.contains(key)) continue;

            auto render_data = std::make_unique<ChunkRenderData>();
            render_data->data = std::move(completed.chunk_data);
            render_data->is_mesh_dirty = true;
            // NOTE: mesh_rid and instance_rid are created lazily in MeshManager
            // when a chunk actually needs a visible mesh. This avoids creating
            // 83,000+ Godot resources for invisible chunks.

apply_pending_placements(key, completed.chunk_x, completed.chunk_y, completed.chunk_z, *render_data);
apply_vegetation_placements(key, completed.chunk_x, completed.chunk_y, completed.chunk_z, *render_data);

            chunk_map.insert(key, std::move(render_data));

if (mesh_manager) {
    // Newly loaded chunk may provide boundary data that changes neighbor meshes.
    // Queue dirtied neighbor chunks for remesh so they don't retain holes where
    // boundary faces were skipped at first build (when this chunk was missing).
    const int32_t ncx = completed.chunk_x;
    const int32_t ncy = completed.chunk_y;
    const int32_t ncz = completed.chunk_z;
    uint64_t neighbor_keys[7] = {
        key,
        chunk_map.get_chunk_key(ncx - 1, ncy,     ncz    ),
        chunk_map.get_chunk_key(ncx + 1, ncy,     ncz    ),
        chunk_map.get_chunk_key(ncx,     ncy - 1, ncz    ),
        chunk_map.get_chunk_key(ncx,     ncy + 1, ncz    ),
        chunk_map.get_chunk_key(ncx,     ncy,     ncz - 1),
        chunk_map.get_chunk_key(ncx,     ncy,     ncz + 1)
    };
    auto lock = chunk_map.lock_keys(std::vector<uint64_t>(neighbor_keys, neighbor_keys + 7));
    ChunkData* installed_chunk = chunk_map.get_chunk_data_fast(ncx, ncy, ncz);
    if (installed_chunk) {
        const int32_t kOff[6][3] = {
            {-1, 0, 0}, {1, 0, 0},
            {0,-1, 0}, {0, 1, 0},
            {0, 0,-1}, {0, 0, 1}
        };
        const FaceDirection kDirs[6] = {
            FaceDirection::Left, FaceDirection::Right,
            FaceDirection::Bottom, FaceDirection::Top,
            FaceDirection::Back, FaceDirection::Front
        };
        for (int i = 0; i < 6; ++i) {
            ChunkData* neighbor = chunk_map.get_chunk_data_fast(
                ncx + kOff[i][0], ncy + kOff[i][1], ncz + kOff[i][2]);
            if (should_dirty_neighbor(neighbor, kDirs[i], installed_chunk)) {
                mesh_manager->queue_dirty_chunk(
                    ncx + kOff[i][0], ncy + kOff[i][1], ncz + kOff[i][2]);
            }
        }
    }
}

if (light_propagator) {
light_propagator->try_fixup_chunk(key, completed.chunk_x, completed.chunk_y, completed.chunk_z);
}

            const int32_t dx = completed.chunk_x - player_cx;
            const int32_t dy = completed.chunk_y - player_cy;
            const int32_t dz = completed.chunk_z - player_cz;
            if (player_cx != INT32_MIN && player_cy != INT32_MIN && player_cz != INT32_MIN &&
                std::abs(dx) <= 1 && std::abs(dy) <= 1 && std::abs(dz) <= 1) {
                if (mesh_manager) {
                    mesh_manager->mark_chunk_urgent(completed.chunk_x, completed.chunk_y, completed.chunk_z);
                }
            }

            pending_chunk_lighting.push_back({completed.chunk_x, completed.chunk_y, completed.chunk_z, completed.epoch});

            installed_count++;
            continue;
        }

        break;
    }

    // Process cross-boundary vegetation modifications (neighbor chunks that need re-mesh).
    // Vegetation overflow is NOT persisted - it's regenerated on each load.
    {
        std::vector<ChunkPos> cross_remeshes;
        {
            std::lock_guard<std::mutex> lock(cross_boundary_mutex);
            cross_remeshes.swap(pending_cross_boundary_remesh);
        }
        for (const auto& pos : cross_remeshes) {
            uint64_t key = chunk_map.get_chunk_key(pos.x, pos.y, pos.z);
            ChunkRenderData* rd = chunk_map.get_chunk_render_data(pos.x, pos.y, pos.z);
            if (rd && rd->data) {
                apply_vegetation_placements(key, pos.x, pos.y, pos.z, *rd);
            }
            if (mesh_manager) {
                mesh_manager->queue_dirty_chunk(pos.x, pos.y, pos.z);
            }
        }
    }

    // Prune pending_block_placements for chunks beyond render distance.
    // Cross-chunk vegetation writes to never-generated neighbor chunks
    // would otherwise accumulate forever (but they're not persisted, so
    // this is purely a memory/queue-size concern, not disk growth).
    if (player_cx != INT32_MIN && player_cy != INT32_MIN && player_cz != INT32_MIN) {
        const int32_t hrd_sq = (render_distance + 1) * (render_distance + 1);
        const int32_t vertical_limit = 128;
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        for (auto it = pending_block_placements.begin(); it != pending_block_placements.end(); ) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(it->first, cx, cy, cz);
            const int32_t dx = cx - player_cx;
            const int32_t dy = cy - player_cy;
            const int32_t dz = cz - player_cz;
            if (dx * dx + dz * dz > hrd_sq || std::abs(dy) > vertical_limit) {
                it = pending_block_placements.erase(it);
            } else {
                ++it;
            }
        }
        
        // Also prune vegetation placements
        std::lock_guard<std::mutex> vlock(vegetation_placement_mutex);
        for (auto it = pending_vegetation_placements.begin(); it != pending_vegetation_placements.end(); ) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(it->first, cx, cy, cz);
            const int32_t dx = cx - player_cx;
            const int32_t dy = cy - player_cy;
            const int32_t dz = cz - player_cz;
            if (dx * dx + dz * dz > hrd_sq || std::abs(dy) > vertical_limit) {
                it = pending_vegetation_placements.erase(it);
            } else {
                ++it;
            }
        }
    }

    return installed_count;
}

} // namespace VoxelEngine
