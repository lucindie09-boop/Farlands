#include "world/chunk_world.hpp"

#include <godot_cpp/classes/rendering_server.hpp>

#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "lighting/block_light_region.hpp"
#include "worldgen/chunk_generator.hpp"
#include "mesh/chunk_boundary_dirty.hpp"
#include "core/crc32.hpp"
#include "core/rle_codec.hpp"
#include "core/chunk_persistence.hpp"

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
            loaded = load_chunk_from_disk(cx, cy, cz, *chunk_data);
            if (!loaded) {
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
                    // Cross-chunk writes are deferred: the worker only pushes to
                    // pending queues (no shard locking). The main thread applies
                    // them during process_completed_chunks.
                    auto cross_writer = [this](int32_t wx, int32_t wy, int32_t wz, BlockID block) {
                        queue_pending_placement(wx, wy, wz, static_cast<int>(block));
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
            }
            chunk_data->propagate_sky_light(chunk_map.get_chunk_data(cx, cy + 1, cz));
            if (chunk_data->get_emissive_count() > 0) {
                chunk_data->propagate_light();
            }
            chunk_data->compute_fully_solid();
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

    // Process cross-boundary vegetation modifications (neighbor chunks that need re-mesh)
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
                apply_pending_placements(key, pos.x, pos.y, pos.z, *rd);
            }
            if (mesh_manager) {
                mesh_manager->queue_dirty_chunk(pos.x, pos.y, pos.z);
            }
        }
    }

    // Prune pending_block_placements for chunks beyond render distance.
    // Cross-chunk vegetation writes to never-generated neighbor chunks
    // would otherwise accumulate forever.
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
    }

    return installed_count;
}

void ChunkWorld::mark_chunk_dirty(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
    dirty_chunks.insert(key);
}

void ChunkWorld::flush_dirty_chunks() {
    std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
    for (uint64_t key : dirty_chunks) {
        int32_t cx = 0, cy = 0, cz = 0;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);
        save_chunk_to_disk(cx, cy, cz);
    }
    dirty_chunks.clear();
}

bool ChunkWorld::is_chunk_dirty(uint64_t key) const {
    std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
    return dirty_chunks.find(key) != dirty_chunks.end();
}

void ChunkWorld::save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    ChunkData* chunk_data = chunk_map.get_chunk_data(chunk_x, chunk_y, chunk_z);
    if (!chunk_data) return;
    save_chunk_to_disk(chunk_x, chunk_y, chunk_z, chunk_data);
}

void ChunkWorld::save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData* chunk_data) {
    if (!chunk_data) return;

    String save_dir = "user://chunks/";
    String filename = save_dir + "chunk_" + String::num_int64(chunk_x) + "_" + String::num_int64(chunk_y) + "_" + String::num_int64(chunk_z) + ".chunk";
    String temp_filename = filename + ".tmp";
    String backup_filename = filename + ".bak";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }

    // Build RLE body into a byte buffer so we can CRC32 it before writing.
    std::vector<uint8_t> body;
    encode_chunk_rle(*chunk_data, body);

    uint32_t checksum = crc32(body.data(), body.size());

    // Write to temporary file first (atomic write pattern)
    Ref<FileAccess> file = FileAccess::open(temp_filename, FileAccess::WRITE);
    if (!file.is_valid()) return;

    // Header: dimensions + version + CRC32
    file->store_32(CHUNK_WIDTH);
    file->store_32(CHUNK_HEIGHT);
    file->store_32(CHUNK_DEPTH);
    file->store_32(3);             // version = 3 (v2 + CRC32)
    file->store_32(checksum);

    // Body
    file->store_buffer(body.data(), body.size());
    file->close();

    // Create backup of existing file before overwriting
    if (dir.is_valid() && FileAccess::file_exists(filename)) {
        // Remove old backup if it exists
        if (FileAccess::file_exists(backup_filename)) {
            dir->remove(backup_filename);
        }
        // Move current file to backup
        dir->rename(filename, backup_filename);
    }

    // Atomic rename: temp -> target
    // On POSIX this is atomic; on Windows it's atomic if both files are on the same volume
    if (dir.is_valid()) {
        dir->rename(temp_filename, filename);
    }
}

bool ChunkWorld::load_chunk_from_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& out_chunk_data) {
    String save_dir = "user://chunks/";
    String filename = save_dir + "chunk_" + String::num_int64(chunk_x) + "_" + String::num_int64(chunk_y) + "_" + String::num_int64(chunk_z) + ".chunk";
    String old_filename = save_dir + "chunk_" + String::num_int64(chunk_x) + "_" + String::num_int64(chunk_z) + ".chunk";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    String backup_filename = filename + ".bak";

    bool use_old_format = false;
    bool use_backup = false;
    
    if (!FileAccess::file_exists(filename)) {
        // Primary file missing - try backup first
        if (FileAccess::file_exists(backup_filename)) {
            use_backup = true;
        } else if (FileAccess::file_exists(old_filename)) {
            use_old_format = true;
        } else {
            return false;
        }
    }

    String target = use_old_format ? old_filename : (use_backup ? backup_filename : filename);
    Ref<FileAccess> file = FileAccess::open(target, FileAccess::READ);
    if (!file.is_valid()) return false;

    int32_t saved_width  = file->get_32();
    int32_t saved_height = file->get_32();
    int32_t saved_depth  = file->get_32();

    if (saved_width != CHUNK_WIDTH || saved_height != CHUNK_HEIGHT || saved_depth != CHUNK_DEPTH) {
        file->close();
        return false;
    }

    if (use_old_format) {
        int32_t version = file->get_32();
        if (version != 1) {
            file->close();
            return false;
        }
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
                for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
                    BlockID block_id = static_cast<BlockID>(file->get_16());
                    out_chunk_data.set_block(x, y, z, block_id);
                }
            }
        }
    } else {
        int32_t version = file->get_32();
        if (version == 3) {
            // v3: RLE + CRC32 checksum — read primary body
            uint32_t expected_crc = file->get_32();
            int64_t body_start = file->get_position();
            int64_t body_end = file->get_length();
            size_t body_size = static_cast<size_t>(body_end - body_start);

            std::vector<uint8_t> body(body_size);
            file->get_buffer(body.data(), body_size);
            file->close();

            // Try to read backup body (may be empty if no backup exists)
            std::vector<uint8_t> bk_body;
            uint32_t bk_crc = 0;
            String backup_filename = target + ".bak";
            if (FileAccess::file_exists(backup_filename)) {
                Ref<FileAccess> bk_file = FileAccess::open(backup_filename, FileAccess::READ);
                if (bk_file.is_valid()) {
                    int32_t bk_w = bk_file->get_32();
                    int32_t bk_h = bk_file->get_32();
                    int32_t bk_d = bk_file->get_32();
                    int32_t bk_v = bk_file->get_32();
                    bk_crc = bk_file->get_32();
                    if (bk_w == CHUNK_WIDTH && bk_h == CHUNK_HEIGHT &&
                        bk_d == CHUNK_DEPTH && bk_v == 3) {
                        int64_t bk_body_start = bk_file->get_position();
                        int64_t bk_body_end = bk_file->get_length();
                        size_t bk_size = static_cast<size_t>(bk_body_end - bk_body_start);
                        bk_body.resize(bk_size);
                        bk_file->get_buffer(bk_body.data(), bk_size);
                    }
                    bk_file->close();
                }
            }

            ChunkLoadOutcome outcome = decode_v3_chunk(
                body.data(), body.size(), expected_crc,
                bk_body.empty() ? nullptr : bk_body.data(),
                bk_body.size(), bk_crc,
                out_chunk_data);

            if (outcome == ChunkLoadOutcome::RECOVERED_FROM_BACKUP) {
                // Backup loaded successfully — restore it as the main file
                Ref<DirAccess> recovery_dir = DirAccess::open("user://");
                if (recovery_dir.is_valid()) {
                    recovery_dir->rename(backup_filename, target);
                }
                return true;
            } else if (outcome == ChunkLoadOutcome::BOTH_CORRUPTED) {
                // No valid backup — delete corrupted file to prevent future load attempts
                Ref<DirAccess> cleanup_dir = DirAccess::open("user://");
                if (cleanup_dir.is_valid()) {
                    cleanup_dir->remove(target);
                }
                return false;
            }
            return true;
        } else if (version == 2) {
            // v2: RLE, no checksum (legacy)
            for (int32_t z = 0; z < CHUNK_DEPTH; z++) {
                for (int32_t x = 0; x < CHUNK_WIDTH; x++) {
                    uint16_t num_runs = file->get_16();
                    for (uint16_t r = 0; r < num_runs; r++) {
                        uint16_t start_y = file->get_16();
                        uint16_t length = file->get_16();
                        uint16_t block_id = file->get_16();
                        for (uint16_t y = 0; y < length; y++) {
                            int32_t wy = static_cast<int32_t>(start_y) + y;
                            if (wy < CHUNK_HEIGHT) {
                                out_chunk_data.set_block(x, wy, z, static_cast<BlockID>(block_id));
                            }
                        }
                    }
                }
            }
        } else {
            file->close();
            return false;
        }
    }
    file->close();
    out_chunk_data.compute_fully_solid();
    return true;
}

void ChunkWorld::save_world_metadata(const TerrainParams& params) {
    String filename = "user://chunks/world.meta";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }

    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::WRITE);
    if (!file.is_valid()) return;

    // Header: magic + version
    file->store_32(0x574F524C); // "WORL" magic
    file->store_32(2);           // metadata version

    // Terrain params
    file->store_32(params.seed);
    file->store_float(params.sea_level);
    file->store_32(params.bedrock_height);
    file->store_float(params.cave_threshold);
    file->store_float(params.cave_scale);
    file->store_float(params.continentalness_scale);
    file->store_float(params.ocean_threshold);
    file->store_float(params.land_threshold);
    file->store_float(params.shelf_width);
    file->store_float(params.shelf_depth);
    file->store_float(params.deep_ocean_depth);
    file->store_float(params.beach_width);
    file->store_32(params.subsurface_cover_depth);
    file->store_float(params.climate_temp_scale);
    file->store_float(params.climate_humidity_scale);
    file->store_float(params.biome_size);

    // Chunk save format version (for future compatibility)
    file->store_32(3); // current chunk format version

    file->close();
}

bool ChunkWorld::load_world_metadata(TerrainParams& out_params, int32_t& out_version) {
    String filename = "user://chunks/world.meta";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    if (!FileAccess::file_exists(filename)) return false;

    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::READ);
    if (!file.is_valid()) return false;

    // Header: magic + version
    uint32_t magic = file->get_32();
    if (magic != 0x574F524C) { // "WORL"
        file->close();
        return false;
    }

    int32_t meta_version = file->get_32();
    if (meta_version < 1 || meta_version > 2) {
        file->close();
        return false;
    }

    // Terrain params
    out_params.seed = file->get_32();
    out_params.sea_level = file->get_float();
    if (meta_version == 1) {
        // Skip old fields for backward compatibility
        file->get_float(); // base_height
        file->get_float(); // height_scale
        file->get_float(); // mountain_scale
    }
    out_params.bedrock_height = file->get_32();
    out_params.cave_threshold = file->get_float();
    out_params.cave_scale = file->get_float();
    out_params.continentalness_scale = file->get_float();
    out_params.ocean_threshold = file->get_float();
    out_params.land_threshold = file->get_float();
    out_params.shelf_width = file->get_float();
    out_params.shelf_depth = file->get_float();
    out_params.deep_ocean_depth = file->get_float();
    out_params.beach_width = file->get_float();
    out_params.subsurface_cover_depth = file->get_32();
    out_params.climate_temp_scale = file->get_float();
    out_params.climate_humidity_scale = file->get_float();
    out_params.biome_size = file->get_float();

    // Chunk save format version
    out_version = file->get_32();

    file->close();
    return true;
}

void ChunkWorld::save_inventory(const Inventory& inventory) {
    String filename = "user://chunks/inventory.bin";
    
    std::lock_guard<std::mutex> lock(file_access_mutex);
    
    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }
    
    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::WRITE);
    if (!file.is_valid()) return;
    
    // Header: magic + version
    file->store_32(0x494E5645); // "INVE" magic
    file->store_32(1);           // inventory version
    
    // Save hotbar
    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory.get_hotbar_slot(i);
        file->store_32(slot.block_id);
        file->store_32(slot.count);
    }
    
    // Save main inventory
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        const auto& slot = inventory.get_inventory_slot(i);
        file->store_32(slot.block_id);
        file->store_32(slot.count);
    }
    
    // Save selected slot
    file->store_32(inventory.get_selected_slot());
    
    file->close();
}

bool ChunkWorld::load_inventory(Inventory& inventory) {
    String filename = "user://chunks/inventory.bin";
    
    std::lock_guard<std::mutex> lock(file_access_mutex);
    
    if (!FileAccess::file_exists(filename)) return false;
    
    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::READ);
    if (!file.is_valid()) return false;
    
    // Header: magic + version
    uint32_t magic = file->get_32();
    if (magic != 0x494E5645) { // "INVE"
        file->close();
        return false;
    }
    
    int32_t inv_version = file->get_32();
    if (inv_version != 1) {
        file->close();
        return false;
    }
    
    // Clear existing inventory
    inventory.clear();
    
    // Load hotbar (restore exact slot positions)
    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        BlockID block_id = file->get_32();
        int count = file->get_32();
        inventory.set_hotbar_slot(i, block_id, count);
    }
    
    // Load main inventory (restore exact slot positions)
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        BlockID block_id = file->get_32();
        int count = file->get_32();
        inventory.set_inventory_slot(i, block_id, count);
    }
    
    // Load selected slot
    int selected_slot = file->get_32();
    inventory.select_slot(selected_slot);
    
    file->close();
    return true;
}

bool ChunkWorld::world_metadata_exists() const {
    String filename = "user://chunks/world.meta";
    return FileAccess::file_exists(filename);
}

void ChunkWorld::free_loaded_chunks() {
    RenderingServer* rs = RenderingServer::get_singleton();
    chunk_map.for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (render_data->instance_rid.is_valid()) {
            rs->free_rid(render_data->instance_rid);
        }
        if (render_data->mesh_rid.is_valid()) {
            rs->free_rid(render_data->mesh_rid);
        }
    });
    chunk_map.clear();
}

bool ChunkWorld::try_unload_chunk(uint64_t key, MeshManager* mesh_mgr) {
    int32_t cx = 0, cy = 0, cz = 0;
    ChunkMap::decode_chunk_key(key, cx, cy, cz);
bool needs_save = false;
    auto render_data = chunk_map.find_and_erase_if(key, [this, key, &needs_save](const ChunkRenderData& rd) {
        if (rd.pending_mesh_builds.load(std::memory_order_relaxed) != 0) {
            return false;
        }
        if (rd.pending_mesh_uploads.load(std::memory_order_relaxed) != 0) {
            return false;
        }
needs_save = is_chunk_dirty(key);
        return true;
    });

    if (!render_data) {
        return !chunk_map.contains(key);
    }

if (needs_save) {
save_chunk_to_disk(cx, cy, cz, render_data->data.get());
}
    if (mesh_mgr) {
        mesh_mgr->notify_chunk_unloaded(cx, cy, cz, render_data.get());
        mesh_mgr->erase_urgent(key);
        ChunkRenderData* below = chunk_map.get_chunk_render_data(cx, cy - 1, cz);
        if (below && below->data && !below->data->is_all_air()) {
            below->is_mesh_dirty = true;
            mesh_mgr->queue_dirty_chunk(cx, cy - 1, cz);
        }
    }
    light_propagated_chunks.erase(key);
    {
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        pending_block_placements.erase(key);
    }
    RenderingServer* rs = RenderingServer::get_singleton();
    if (render_data->instance_rid.is_valid()) {
        rs->free_rid(render_data->instance_rid);
    }
    if (render_data->mesh_rid.is_valid()) {
        rs->free_rid(render_data->mesh_rid);
    }
    return true;
}

void ChunkWorld::clear() {
    {
        std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
        dirty_chunks.clear();
    }
    chunk_scheduler.clear();
    pending_chunk_installs.clear();
    pending_chunk_lighting.clear();
    pending_chunk_dirty_mesh.clear();
    light_propagated_chunks.clear();
    pending_block_placements.clear();
    chunk_map.clear();
    async_epoch.store(0, std::memory_order_release);
}

void ChunkWorld::queue_pending_placement(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);
    std::lock_guard<std::mutex> lock(pending_placement_mutex);
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    pending_block_placements[key].push_back({world_x, world_y, world_z, block_id});
}

void ChunkWorld::apply_pending_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data) {
    std::lock_guard<std::mutex> lock(pending_placement_mutex);
    auto it = pending_block_placements.find(key);
    if (it != pending_block_placements.end()) {
        for (const auto& placement : it->second) {
            int32_t lx = placement.world_x - chunk_x * CHUNK_WIDTH;
            int32_t ly = placement.world_y - chunk_y * CHUNK_HEIGHT;
            int32_t lz = placement.world_z - chunk_z * CHUNK_DEPTH;
            if (lx >= 0 && lx < CHUNK_WIDTH &&
                ly >= 0 && ly < CHUNK_HEIGHT &&
                lz >= 0 && lz < CHUNK_DEPTH) {
                render_data.data->set_block(lx, ly, lz, static_cast<BlockID>(placement.block_id));
            }
        }
        pending_block_placements.erase(it);
        render_data.data->compute_section_flags();
        render_data.data->compute_fully_solid();
    }
}

} // namespace VoxelEngine
