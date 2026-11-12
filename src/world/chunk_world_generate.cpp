#include "world/chunk_world.hpp"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "worldgen/chunk_generator.hpp"
#include "worldgen/generation_slice_profile.hpp"
#include "core/edit_map.hpp"

namespace VoxelEngine {

using namespace godot;

namespace {

// Measurement-only in-RAM memo at the generation boundary. `FARLANDS_GEN_MEMO=1`
// stores each chunk's generated terrain (before the edit map, light and solid
// pass) so a second fill of the same world can skip the expensive generator.
// The read path still applies the edit map, light and the solid pass, so it is
// the cache hit path a codec would have, not a replay of the whole task.
//
// It is keyed by chunk key and never evicted: the loaded set is ~20k chunks and
// this holds one copy of it plus the chunk's cross-chunk spill list, which is
// the price of the decider. A hit replays the spill through the same writer
// closure a miss uses, so a second fill after a reset has the same canopies and
// boundary remeshes; the replay is part of the hit cost.
bool generation_memo_enabled() {
    // Presence used to be enough, so FARLANDS_GEN_MEMO=0 still turned the memo
    // on and an "off" control silently measured the hit path. The off values
    // are matched in full ("off" is not an f-prefix guess); any other value
    // enables it.
    static const bool enabled = [] {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        const char* raw = std::getenv("FARLANDS_GEN_MEMO");
        if (raw == nullptr || raw[0] == '\0') return false;
        std::string value(raw);
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return value != "0" && value != "false" && value != "no" && value != "off";
    }();
    return enabled;
}

// Batch a column's band into one generation task. Every vertical slice of a
// column runs the identical macro column pass (1,024 sample_column_with_climate
// calls plus their weirdness masks), and the scheduler has no worker affinity, so
// a cache cannot share it: the slices are dequeued by different workers at the
// same instant and all miss. Generating the band in one task shares the pass for
// real -- one pass for 2-6 slices instead of one each -- at the cost of a coarser
// unit of work. On by default, because the paired arm at 15 workers said so 3/3
// (settle 1.858/1.918/1.932 s against 2.114/2.140/2.127 with it off, installs
// identical at 20,230); `FARLANDS_GEN_BAND=0` is the arm that turns it off.
// How many slices of a column one band task may take. 0 is the whole accepted
// band (the shipped default). A smaller span trades part of the shared column
// pass for a finer unit of work: the six-slice task is coarser than the per-chunk
// tasks it replaced, and the tail of the fill is only as fine as the last task.
int32_t band_span() {
    static const int32_t span = [] {
        const char* raw = std::getenv("FARLANDS_GEN_BAND_SPAN");
        return raw != nullptr ? std::atoi(raw) : 0;
    }();
    return span;
}

bool band_generation_enabled() {
    static const bool enabled = [] {
        const char* raw = std::getenv("FARLANDS_GEN_BAND");
        return !(raw != nullptr && raw[0] == '0');
    }();
    return enabled;
}

struct GenerationMemoEntry {
    ChunkData data;
    std::vector<TerrainCacheSpill> spill;
};

struct GenerationMemo {
    std::mutex mutex;
    std::unordered_map<uint64_t, GenerationMemoEntry> chunks;
};

GenerationMemo& generation_memo() {
    static GenerationMemo memo;  // no godot types, built on first use
    return memo;
}

} // namespace

// Worker-side half of ChunkWorld: the generation closure that runs on the
// thread pool (terrain, deferred cross-chunk vegetation, the chunk's own edit
// map, sky/block light). The main-thread half — installing the results, light
// fix-ups, dirty-mesh fan-out and the pending-queue pruning — is in
// chunk_world.cpp.
bool ChunkWorld::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                                const TerrainParams& params, const BiomeConfig& biomes,
                                const VegetationConfig& veg_config,
                                int32_t band_lo, int32_t band_hi) {
    if (!thread_pool) return false;
    // Main-thread setup: the manifest and the pack indexes are loaded once per
    // key. With the switch off this is one cached boolean; with a missing or
    // malformed cache it leaves the reader closed and generation proceeds.
    terrain_cache.ensure_open(params);
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    // The keys are computed HERE, on the main thread: the scheduler claims every
    // one of them before the task starts, and it has no chunk map to build them.
    std::vector<int32_t> band_ys;
    std::vector<uint64_t> band_keys;
    if (band_hi >= band_lo && band_generation_enabled()) {
        // The task starts at the requested slice and takes at most `span` of them
        // (0 = the rest of the band).
        const int32_t span = band_span();
        const int32_t last = span > 0 ? std::min(band_hi, chunk_y + span - 1) : band_hi;
        band_ys.reserve(static_cast<size_t>(last - chunk_y + 1));
        band_keys.reserve(band_ys.capacity());
        for (int32_t cy = chunk_y; cy <= last; ++cy) {
            band_ys.push_back(cy);
            band_keys.push_back(chunk_map.get_chunk_key(chunk_x, cy, chunk_z));
        }
    }
    const uint64_t path_id = GenerationPathProfile::new_attempt();
    const uint64_t admit_ns = path_id != 0 ? generation_path_now_ns() : 0;
    const bool enqueued = chunk_scheduler.enqueue_generation(
        thread_pool, chunk_x, chunk_y, chunk_z, epoch, key, path_id,
        [this](uint64_t k) { return chunk_map.contains(k); },
        [this, params = params, biomes = biomes, veg_config = veg_config, key, path_id, epoch](int32_t cx, int32_t cy, int32_t cz, bool& loaded) {
            generation_path_profile_.started(path_id);
            gen_slice_count_chunk();
            auto chunk_data = std::make_unique<ChunkData>();
            const bool memo_enabled = generation_memo_enabled();
            bool memo_hit = false;
            std::vector<TerrainCacheSpill> memo_spill;
            bool cache_hit = false;
            std::vector<TerrainCacheSpill> cache_spill;
            if (terrain_cache.is_open()) {
                cache_hit = terrain_cache.read_chunk(cx, cy, cz,
                                                     BlockRegistry::get_instance(),
                                                     *chunk_data, cache_spill);
            }
            if (!cache_hit && memo_enabled) {
                GenerationMemo& memo = generation_memo();
                std::lock_guard<std::mutex> lock(memo.mutex);
                const auto it = memo.chunks.find(key);
                if (it != memo.chunks.end()) {
                    *chunk_data = it->second.data;
                    memo_spill = it->second.spill;
                    memo_hit = true;
                }
            }
            
            // Generate on a miss; edit maps are applied as a diff on top of either path.
            thread_local ChunkGenerator generator;
            generator.set_params(params);
            generator.set_biome_config(biomes);
            generator.set_vegetation_config(veg_config);

            // Uniform-chunk fast paths (all air / all bedrock / all solid
            // subsurface) live in ChunkGenerator::generate_fast_path so the
            // fully-solid bookkeeping is shared and testable. Handled chunks
            // skip the lattice/density/material passes below. The member call
            // reuses this configured generator (no per-call construction).
            // The uniform-chunk classification lives inside generate_chunk now:
            // the old pre-check rebuilt the climate, land-shape and amplification
            // lattices from scratch, only for the writer to build them again from
            // its own copy. Its light and fully-solid bookkeeping moved to the
            // same branch, which already runs on the exact column heights.
            {
                // Cross-chunk vegetation writes are deferred: the worker only pushes to
                // vegetation queues (no shard locking). The main thread applies them
                // during process_completed_chunks. These are NOT persisted.
                // A memo hit replays the recorded writes through the same closure, so
                // the second fill queues the same canopies and boundary remeshes.
                std::vector<TerrainCacheSpill> spill;
                auto cross_writer = [this, &spill, record_spill = !memo_hit && !cache_hit](int32_t wx, int32_t wy, int32_t wz, BlockID block) {
                    if (record_spill) {
                        spill.push_back({wx, wy, wz, block});
                    }
                    queue_vegetation_placement(wx, wy, wz, static_cast<int>(block));
                    int32_t tc_x, tc_y, tc_z, lx, ly, lz;
                    world_to_chunk_local(wx, wy, wz, tc_x, tc_y, tc_z, lx, ly, lz);
                    pending_cross_boundary_remesh.push({tc_x, tc_y, tc_z});
                };
                if (cache_hit) {
                    for (const TerrainCacheSpill& write : cache_spill) {
                        cross_writer(write.x, write.y, write.z, write.block);
                    }
                } else if (memo_hit) {
                    for (const TerrainCacheSpill& write : memo_spill) {
                        cross_writer(write.x, write.y, write.z, write.block);
                    }
                } else {
                    generator.generate_chunk(*chunk_data, cx, cy, cz,
                                             ChunkGenerator::CrossChunkWriter(cross_writer),
                                             vegetation_enabled);
                    // Cache capture boundary: the generator's own answer, before
                    // the edit map, light and solid flags, with the full spill
                    // list from this same call. An attempt whose epoch was retired
                    // is not worth a pack, and the memo below takes the spill list
                    // only after the cache has copied it.
                    if (async_epoch.load(std::memory_order_acquire) == epoch) {
                        terrain_cache.stage_chunk(cx, cy, cz, *chunk_data, spill);
                    }
                    if (memo_enabled) {
                        GenerationMemo& memo = generation_memo();
                        std::lock_guard<std::mutex> lock(memo.mutex);
                        memo.chunks.emplace(key, GenerationMemoEntry{*chunk_data, std::move(spill)});
                    }
                }
            }
            
            // Load and apply edit map BEFORE light propagation so that sky/block
            // light is computed on the final terrain (edits remove/add blocks that
            // affect shadow/light columns).
            bool edit_map_known = false;
            {
                std::lock_guard<std::mutex> lock(edit_maps_mutex);
                edit_map_known = chunk_edit_maps.find(key) != chunk_edit_maps.end();
            }
            if (!edit_map_known && edit_file_exists_on_disk(key)) {
                GenSliceTimer t(GenSlice::EditDisk);
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
            {
                GenSliceTimer t(GenSlice::EditMap);
                apply_edit_map_to_chunk(key, cx, cy, cz, *chunk_data);
            }

            {
                GenSliceTimer t(GenSlice::Light);
                chunk_data->propagate_sky_light(chunk_map.get_chunk_data(cx, cy + 1, cz));
                if (chunk_data->get_emissive_count() > 0) {
                    chunk_data->propagate_light();
                }
            }
            {
                GenSliceTimer t(GenSlice::Solid);
                chunk_data->compute_fully_solid();
            }
            generation_path_profile_.finished(path_id);
            return chunk_data;
        },
        [this]() { return async_epoch.load(std::memory_order_acquire); },
        std::move(band_ys), std::move(band_keys)
    );
    if (enqueued) {
        generation_path_profile_.admitted(path_id, admit_ns);
    }
    return enqueued;
}

} // namespace VoxelEngine
