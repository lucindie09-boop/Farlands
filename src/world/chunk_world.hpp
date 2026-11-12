#ifndef FARLANDS_CHUNK_WORLD_HPP
#define FARLANDS_CHUNK_WORLD_HPP
#include "core/chunk_map.hpp"
#include "core/terrain_params.hpp"
#include "core/chunk_types.hpp"
#include "core/inventory.hpp"
#include "core/edit_map.hpp"
#include "world/chunk_scheduler.hpp"
#include "world/cross_boundary_queue.hpp"
#include "world/light_band_index.hpp"
#include "world/light_profile.hpp"
#include "world/install_profile.hpp"
#include "world/generation_path_profile.hpp"
#include "world/terrain_cache.hpp"
#include "worldgen/generation_slice_profile.hpp"
#include "core/thread_pool.hpp"
#include "worldgen/chunk_generator.hpp"
#include <godot_cpp/classes/node.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <cmath>
#include <deque>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <thread>

namespace VoxelEngine {

class MeshManager;
class LightPropagator;

class ChunkWorld {
public:
    void set_owner(godot::Node* node) { owner = node; }
    godot::Node* get_owner() const { return owner; }
    void set_mesh_manager(MeshManager* mm) { mesh_manager = mm; }
    void set_light_propagator(LightPropagator* lp) { light_propagator = lp; }
    void set_thread_pool(ThreadPool* tp) { thread_pool = tp; }
    // Called on the main thread for every chunk installed into the map, with the
    // chunk's coordinates and whether its data holds any non-air block. The
    // generation chain (WorldUpdater) seeds its frontier from this.
    void set_install_listener(std::function<void(int32_t, int32_t, int32_t, bool)> listener) {
        install_listener = std::move(listener);
    }

    bool generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch,
                        const TerrainParams& params, const BiomeConfig& biomes,
                        const VegetationConfig& veg_config,
                        int32_t band_lo = 0, int32_t band_hi = -1);
    int32_t process_completed_chunks(uint64_t epoch, double budget_ms, int32_t max_installs, int32_t max_lighting, int32_t max_dirties, int32_t player_cx, int32_t player_cy, int32_t player_cz, int32_t render_distance);
    void save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    void mark_chunk_dirty(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    // Snapshots every dirty chunk's edit map on the caller's thread and hands the
    // snapshots to the thread pool for serialization + write. With wait_for_completion=true
    // (quit path) it blocks until all outstanding saves finish (or timeout).
    void flush_dirty_chunks(bool wait_for_completion = false, double timeout_sec = 5.0);
    bool is_chunk_dirty(uint64_t key) const;
    void free_loaded_chunks();
    bool try_unload_chunk(uint64_t key, MeshManager* mesh_mgr);
    void clear();
    void queue_pending_placement(int32_t world_x, int32_t world_y, int32_t world_z, int block_id);
    void apply_pending_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data);

    // --- Chunks a caller needs NOW -------------------------------------------
    // The streaming sweep decides what to generate from the player's position and
    // a per-column surface band, which means the chunks a caller is waiting on are
    // often not in it at all: the sky above a build is deliberately never
    // generated, and anything past the render distance is out of scope entirely.
    // An URGENT request is a chunk that must exist whatever those rules say — the
    // updater generates it ahead of the sweep, and takes it out of the queue when
    // it does. A PIN is a chunk the unload pass leaves alone until the caller is
    // finished with it, so a chunk that was just loaded is not evicted before the
    // write that asked for it lands. Both are keyed by chunk key and both are
    // the caller's to clean up; nothing expires them on its own.
    void request_urgent_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    // Takes up to `max` pending requests, oldest first, and forgets them.
    std::vector<ChunkPos> take_urgent_chunk_requests(size_t max);
    [[nodiscard]] size_t urgent_chunk_count() const;
    void pin_chunk(uint64_t key);
    void unpin_chunk(uint64_t key);
    [[nodiscard]] bool is_chunk_pinned(uint64_t key) const;
    void unpin_all_chunks();
    void queue_vegetation_placement(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    void apply_vegetation_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data);
    
    // Every block edit that lands in an edit map reports the world position it
    // edited here: player edits, vegetation, pending placements, and the fluid
    // simulation's own writes. The fluid simulation uses it to wake the edited
    // cell and its neighbours; nothing else listens. Always called on the main
    // thread, and never while the edit-map lock is held (a listener reads the
    // world, and reading the world under that lock is how a deadlock starts).
    void set_edit_listener(std::function<void(int32_t, int32_t, int32_t)> listener) {
        edit_listener = std::move(listener);
    }

    // The same notification from a WORKER thread, where the main-thread listener
    // must not be called: applying a chunk's edit map happens on the thread pool
    // during generation, and the listener's real work (waking the fluid sim)
    // mutates a queue the main thread ticks. The receiver of this one has to be
    // something that is safe to call from anywhere — a post, not an apply.
    void set_worker_edit_listener(std::function<void(int32_t, int32_t, int32_t)> listener) {
        worker_edit_listener = std::move(listener);
    }

    // Edit map methods
    // `notify` is false for a write the FLUID SIMULATION already knows about:
    // it schedules what it writes itself, so telling it again would only mean
    // seven block reads per cell for nothing.
    void add_block_edit(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, int32_t local_x, int32_t local_y, int32_t local_z, BlockID block_id, bool notify = true);

    // One cell of a batched write, in LOCAL chunk coordinates (a batch belongs to
    // one chunk, so the caller already has them).
    struct EditCell {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        BlockID block = 0;
    };

    // add_block_edit for a whole chunkful at once: one edit-map lock, one chunk
    // lookup, one dirty mark and one hash-table reserve for the run instead of one
    // of each per block. A schematic paste writes hundreds of thousands of cells
    // through here, and the per-cell version was paying three mutex acquisitions
    // and two hash lookups per BLOCK.
    //
    // Deliberately does NOT notify the edit listener: the caller has the volume in
    // hand and can pick the cells worth waking, and waking per written cell would
    // put the fluid simulation's locked neighbour scan on the critical path for
    // every block of a build. Use notify_block_change for the ones that matter.
    void add_block_edits(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                         const std::vector<EditCell>& edits);

    // Wakes the edit listener (the fluid simulation) for one world cell, if one is
    // installed. Separate from persisting so a bulk writer can write in batches and
    // wake only the cells that can change a fluid's answer.
    void notify_block_change(int32_t world_x, int32_t world_y, int32_t world_z);
    void apply_edit_map_to_chunk(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& chunk_data);
    bool load_edit_map_from_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, EditMap& out_edit_map, const BlockRegistry& registry);
    void save_edit_map_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, const EditMap& edit_map);

    // World metadata (seed, terrain params, version)
    void save_world_metadata(const TerrainParams& params);
    bool load_world_metadata(TerrainParams& out_params, int32_t& out_version);
    
    // Inventory persistence
    void save_inventory(const Inventory& inventory);
    bool load_inventory(Inventory& inventory);
    bool world_metadata_exists() const;

    void set_vegetation_enabled(bool enabled) { vegetation_enabled = enabled; }
    bool is_vegetation_enabled() const { return vegetation_enabled; }

    ChunkMap& get_chunk_map() { return chunk_map; }
    ChunkMap* get_chunk_map_ptr() { return &chunk_map; }
    ChunkScheduler& get_scheduler() { return chunk_scheduler; }
    [[nodiscard]] TerrainCacheReaderStats get_terrain_cache_stats() const {
        return terrain_cache.stats();
    }
    [[nodiscard]] TerrainCacheWriterStats get_terrain_cache_writer_stats() const {
        return terrain_cache.writer_stats();
    }
    void reset_terrain_cache_stats() { terrain_cache.reset_stats(); }
    [[nodiscard]] bool is_terrain_cache_open() const { return terrain_cache.is_open(); }

    uint64_t get_epoch() const { return async_epoch.load(std::memory_order_acquire); }
    std::atomic<uint64_t>* get_epoch_ptr() { return &async_epoch; }
    uint64_t increment_epoch() { return async_epoch.fetch_add(1, std::memory_order_acq_rel); }
    
    // Test accessors for edit maps (test-only)
    std::mutex& get_edit_maps_mutex() { return edit_maps_mutex; }
    std::unordered_map<uint64_t, EditMap>& get_chunk_edit_maps() { return chunk_edit_maps; }

    ChunkData* get_chunk_data(int32_t cx, int32_t cy, int32_t cz) { return chunk_map.get_chunk_data(cx, cy, cz); }
    ChunkRenderData* get_chunk_render_data(int32_t cx, int32_t cy, int32_t cz) { return chunk_map.get_chunk_render_data(cx, cy, cz); }
    bool has_loaded_chunk(int32_t cx, int32_t cy, int32_t cz) const { return chunk_map.has_loaded_chunk(cx, cy, cz); }
    bool is_block_solid(int32_t world_x, int32_t world_y, int32_t world_z) { return chunk_map.is_block_solid(world_x, world_y, world_z); }
    int get_block_world(int32_t wx, int32_t wy, int32_t wz) { return chunk_map.get_block_world(wx, wy, wz); }
    uint16_t get_light_world(int32_t wx, int32_t wy, int32_t wz) { return chunk_map.get_light_world(wx, wy, wz); }

    // Main-thread completion-drain depths: what is waiting for its light stage,
    // what a worker light pass has finished, and what is waiting to be marked
    // dirty. Read on the main thread only.
    [[nodiscard]] size_t pending_lighting_count() const { return pending_chunk_lighting.size(); }
    [[nodiscard]] size_t pending_dirty_stage_count() const { return pending_chunk_dirty_mesh.size(); }

    // Reset diagnostics: these are the queues that can survive a world clear by
    // accident. They are read only by the report/probe, on the main thread.
    [[nodiscard]] size_t pending_vegetation_chunk_count() {
        std::lock_guard<std::mutex> lock(vegetation_placement_mutex);
        return pending_vegetation_placements.size();
    }
    [[nodiscard]] size_t pending_vegetation_placement_count() const {
        return pending_vegetation_count.load(std::memory_order_relaxed);
    }
    [[nodiscard]] size_t pending_cross_remesh_count() {
        return pending_cross_boundary_remesh.size();
    }

    // Per-chunk generation pipeline: admission (enqueue) -> worker start ->
    // worker finish -> install. Collected only when FARLANDS_GEN_PROFILE=1;
    // the startup probe prints this readout.
    [[nodiscard]] std::string get_generation_path_report() const {
        return generation_path_profile_.report() + " | tail: " +
               generation_path_profile_.report_tail(150) + " | " + generation_slice_report();
    }

    // Worker-side light-pass timing. The counters are collected only when
    // FARLANDS_LIGHT_PROFILE=1; the startup probe prints this readout.
    [[nodiscard]] std::string get_light_profile_report() const { return light_profile_.report(); }

    // Main-thread install breakdown; "disabled" unless FARLANDS_INSTALL_PROFILE=1.
    [[nodiscard]] std::string get_install_profile_report() const { return install_profile_.report(); }

private:
    // Writes the edit map file. Caller MUST hold file_access_mutex.
    void write_edit_map_file_locked(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, const EditMap& edit_map);
    // One directory scan decides whether ANY chunk in this world has saved edits;
    // a world with none skips the per-chunk edit load entirely.
    bool edit_file_exists_on_disk(uint64_t key);
    // Deep-copies the edit map into the background-save queue. Supersedes any
    // in-flight save of the same chunk via a generation bump.
    void enqueue_edit_map_save(uint64_t key, int32_t cx, int32_t cy, int32_t cz, std::unique_ptr<EditMap> snapshot);
    // Background worker: epoch-gated, supersession-gated, writes the file.
    void save_edit_map_snapshot(uint64_t key, int32_t cx, int32_t cy, int32_t cz,
                               std::unique_ptr<EditMap> snapshot, uint64_t epoch, uint64_t generation);
    bool is_save_in_flight(uint64_t key) const;
    void wait_for_saves(double timeout_sec);
    // Tail of process_completed_chunks: cross-boundary vegetation remeshes and
    // the placement-queue prune. Split out so that TU stays under the size cap.
    void drain_cross_boundary_and_prune(int32_t player_cx, int32_t player_cy, int32_t player_cz, int32_t render_distance);

    godot::Node* owner = nullptr;
    MeshManager* mesh_manager = nullptr;
    LightPropagator* light_propagator = nullptr;
    ThreadPool* thread_pool = nullptr;
    ChunkMap chunk_map;
    ChunkScheduler chunk_scheduler;
    TerrainCache terrain_cache;
    std::deque<CompletedChunk> pending_chunk_installs;
    std::deque<PendingChunkStage> pending_chunk_lighting;
    std::deque<PendingChunkStage> pending_chunk_dirty_mesh;
    std::unordered_set<uint64_t> light_propagated_chunks;
    // Event-maintained 3x3x3 light-relevance summary; see light_band_index.hpp.
    LightBandIndex light_band_index;
    std::unordered_map<uint64_t, std::vector<PendingBlockPlacement>> pending_block_placements;
    std::mutex pending_placement_mutex;
    std::unordered_map<uint64_t, std::vector<PendingBlockPlacement>> pending_vegetation_placements;
    // Entry counts for the two maps above, so an install can skip their mutexes
    // and hash lookups when nothing is waiting — the fill's normal state. Only
    // over-counts (a prune leaves the count high), which costs a lock, never work.
    std::atomic<size_t> pending_placement_count{0};
    std::atomic<size_t> pending_vegetation_count{0};
    std::mutex vegetation_placement_mutex;
    // Chunks a caller asked for urgently, in request order (the deque is what
    // keeps the order; the set is what keeps the queue free of duplicates).
    std::deque<uint64_t> urgent_chunk_requests;
    std::unordered_set<uint64_t> urgent_chunk_set;
    mutable std::mutex urgent_chunk_mutex;
    // Chunks the unload pass must not touch yet.
    std::unordered_set<uint64_t> pinned_chunks;
    mutable std::mutex pinned_chunk_mutex;
    std::function<void(int32_t, int32_t, int32_t)> edit_listener;
    std::function<void(int32_t, int32_t, int32_t)> worker_edit_listener;
    std::function<void(int32_t, int32_t, int32_t, bool)> install_listener;
    std::mutex file_access_mutex;
    std::atomic<uint64_t> async_epoch{0};

    std::unordered_set<uint64_t> dirty_chunks;
    mutable std::mutex dirty_chunks_mutex;

    // Per-chunk edit maps: sparse block edits that get persisted instead of full chunks
    std::unordered_map<uint64_t, EditMap> chunk_edit_maps;
    mutable std::mutex edit_maps_mutex;
    // 0 = not scanned, 1 = scanned and nothing found, 2 = scanned and the set
    // below is not empty. Published once under file_access_mutex; every
    // generation worker reads it and then the set.
    std::atomic<uint8_t> edit_scan_{0};
    // The chunk keys the scan found. Remembering only "this world has some
    // edits" made every other chunk probe the filesystem twice to learn its own
    // file is missing - `GenSlice::EditDisk` 0.13 ms across 19,848 of 20,230
    // chunks in a fill. Merged (never cleared) by the scan so an edit made
    // before the first scan survives it, and reset with the world.
    mutable std::mutex edit_keys_mutex;
    std::unordered_set<uint64_t> edit_file_keys;

    // Background chunk saves: key -> save generation. At most one live task per
    // key; a newer snapshot supersedes an older one by bumping the generation.
    std::unordered_map<uint64_t, uint64_t> saves_in_flight;
    mutable std::mutex saves_in_flight_mutex;
    std::atomic<uint32_t> outstanding_saves{0};
    std::atomic<uint64_t> next_save_generation{1};
    std::mutex saves_cv_mutex;
    std::condition_variable saves_cv;

    // Cross-boundary vegetation writes: neighbor chunks modified during generation
    CrossBoundaryRemeshQueue pending_cross_boundary_remesh;

    LightProfile light_profile_;
    InstallProfile install_profile_;
    GenerationPathProfile generation_path_profile_;
    bool vegetation_enabled = true;
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_WORLD_HPP