#ifndef FUK_MINECRAFT_CHUNK_WORLD_HPP
#define FUK_MINECRAFT_CHUNK_WORLD_HPP
#include "core/chunk_map.hpp"
#include "core/terrain_params.hpp"
#include "core/chunk_types.hpp"
#include "core/inventory.hpp"
#include "core/edit_map.hpp"
#include "world/chunk_scheduler.hpp"
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

    bool generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch, const TerrainParams& params);
    int32_t process_completed_chunks(uint64_t epoch, double budget_ms, int32_t max_installs, int32_t max_lighting, int32_t max_dirties, int32_t player_cx, int32_t player_cy, int32_t player_cz, int32_t render_distance);
    void save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    void save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData* chunk_data);
    void mark_chunk_dirty(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    // Snapshots every dirty chunk's edit map on the caller's thread and hands the
    // snapshots to the thread pool for serialization + write. With wait_for_completion=true
    // (quit path) it blocks until all outstanding saves finish (or timeout).
    void flush_dirty_chunks(bool wait_for_completion = false, double timeout_sec = 5.0);
    bool is_chunk_dirty(uint64_t key) const;
    bool load_chunk_from_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& out_chunk_data);
    void free_loaded_chunks();
    bool try_unload_chunk(uint64_t key, MeshManager* mesh_mgr);
    void clear();
    void queue_pending_placement(int32_t world_x, int32_t world_y, int32_t world_z, int block_id);
    void apply_pending_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data);
    void queue_vegetation_placement(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    void apply_vegetation_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data);
    
    // Edit map methods
    void add_block_edit(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, int32_t local_x, int32_t local_y, int32_t local_z, BlockID block_id);
    void apply_edit_map_to_chunk(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& chunk_data);
    bool load_edit_map_from_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, EditMap& out_edit_map);
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

private:
    // Writes the edit map file. Caller MUST hold file_access_mutex.
    void write_edit_map_file_locked(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, const EditMap& edit_map);
    // Deep-copies the edit map into the background-save queue. Supersedes any
    // in-flight save of the same chunk via a generation bump.
    void enqueue_edit_map_save(uint64_t key, int32_t cx, int32_t cy, int32_t cz, std::unique_ptr<EditMap> snapshot);
    // Background worker: epoch-gated, supersession-gated, writes the file.
    void save_edit_map_snapshot(uint64_t key, int32_t cx, int32_t cy, int32_t cz,
                               std::unique_ptr<EditMap> snapshot, uint64_t epoch, uint64_t generation);
    bool is_save_in_flight(uint64_t key) const;
    void wait_for_saves(double timeout_sec);

    godot::Node* owner = nullptr;
    MeshManager* mesh_manager = nullptr;
    LightPropagator* light_propagator = nullptr;
    ThreadPool* thread_pool = nullptr;
    ChunkMap chunk_map;
    ChunkScheduler chunk_scheduler;
    std::deque<CompletedChunk> pending_chunk_installs;
    std::deque<PendingChunkStage> pending_chunk_lighting;
    std::deque<PendingChunkStage> pending_chunk_dirty_mesh;
    std::unordered_set<uint64_t> light_propagated_chunks;
    std::unordered_map<uint64_t, std::vector<PendingBlockPlacement>> pending_block_placements;
    std::mutex pending_placement_mutex;
    std::unordered_map<uint64_t, std::vector<PendingBlockPlacement>> pending_vegetation_placements;
    std::mutex vegetation_placement_mutex;
    std::mutex file_access_mutex;
    std::atomic<uint64_t> async_epoch{0};

    std::unordered_set<uint64_t> dirty_chunks;
    mutable std::mutex dirty_chunks_mutex;

    // Per-chunk edit maps: sparse block edits that get persisted instead of full chunks
    std::unordered_map<uint64_t, EditMap> chunk_edit_maps;
    mutable std::mutex edit_maps_mutex;

    // Background chunk saves: key -> save generation. At most one live task per
    // key; a newer snapshot supersedes an older one by bumping the generation.
    std::unordered_map<uint64_t, uint64_t> saves_in_flight;
    mutable std::mutex saves_in_flight_mutex;
    std::atomic<uint32_t> outstanding_saves{0};
    std::atomic<uint64_t> next_save_generation{1};
    std::mutex saves_cv_mutex;
    std::condition_variable saves_cv;

    // Cross-boundary vegetation writes: neighbor chunks modified during generation
    std::vector<ChunkPos> pending_cross_boundary_remesh;
    mutable     std::mutex cross_boundary_mutex;

    bool vegetation_enabled = true;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_WORLD_HPP