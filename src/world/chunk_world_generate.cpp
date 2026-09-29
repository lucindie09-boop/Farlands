#include "world/chunk_world.hpp"

#include <memory>

#include "worldgen/chunk_generator.hpp"
#include "core/edit_map.hpp"

namespace VoxelEngine {

using namespace godot;

// Worker-side half of ChunkWorld: the generation closure that runs on the
// thread pool (terrain, deferred cross-chunk vegetation, the chunk's own edit
// map, sky/block light). The main-thread half — installing the results, light
// fix-ups, dirty-mesh fan-out and the pending-queue pruning — is in
// chunk_world.cpp.
bool ChunkWorld::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                const TerrainParams& params, const BiomeConfig& biomes,
                                const VegetationConfig& veg_config) {
    if (!thread_pool) return false;
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    return chunk_scheduler.enqueue_generation(
        thread_pool, chunk_x, chunk_y, chunk_z, epoch, key,
        [this](uint64_t k) { return chunk_map.contains(k); },
        [this, params = params, biomes = biomes, veg_config = veg_config](int32_t cx, int32_t cy, int32_t cz, bool& loaded) {
            auto chunk_data = std::make_unique<ChunkData>();
            
            // Always generate - edit maps are applied as a diff on top
            thread_local ChunkGenerator generator;
            generator.set_params(params);
            generator.set_biome_config(biomes);
            generator.set_vegetation_config(veg_config);

            // Uniform-chunk fast paths (all air / all bedrock / all solid
            // subsurface) live in ChunkGenerator::generate_fast_path so the
            // fully-solid bookkeeping is shared and testable. Handled chunks
            // skip the lattice/density/material passes below. The member call
            // reuses this configured generator (no per-call construction).
            if (generator.generate_fast_path(*chunk_data, cx, cy, cz)) {
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
            
            // Load and apply edit map BEFORE light propagation so that sky/block
            // light is computed on the final terrain (edits remove/add blocks that
            // affect shadow/light columns).
            uint64_t key = chunk_map.get_chunk_key(cx, cy, cz);
            bool edit_map_known = false;
            {
                std::lock_guard<std::mutex> lock(edit_maps_mutex);
                edit_map_known = chunk_edit_maps.find(key) != chunk_edit_maps.end();
            }
            if (!edit_map_known) {
                // Read and PARSE outside the map's mutex. This is a file read plus a
                // deserialize, and holding `edit_maps_mutex` across it put one disk
                // access in the way of every other chunk's generation: generation
                // workers queue on this mutex, so a stream of chunks into a world with
                // a big pasted build waited on one .edit file at a time. That is what
                // "loading the saved player edits is laggy" turns out to be — the
                // edits themselves apply in microseconds.
                EditMap loaded;
                if (load_edit_map_from_disk(cx, cy, cz, loaded, BlockRegistry::get_instance())) {
                    std::lock_guard<std::mutex> lock(edit_maps_mutex);
                    // Two workers can now load the same chunk at once. It is the same
                    // file either way, so the first one in wins and the other is
                    // dropped rather than overwriting a map a third thread may already
                    // be reading out of.
                    chunk_edit_maps.emplace(key, std::move(loaded));
                }
            }
            apply_edit_map_to_chunk(key, cx, cy, cz, *chunk_data);

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

} // namespace VoxelEngine
