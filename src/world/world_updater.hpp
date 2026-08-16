#ifndef FUK_MINECRAFT_WORLD_UPDATER_HPP
#define FUK_MINECRAFT_WORLD_UPDATER_HPP
#include "core/chunk_types.hpp"
#include "core/terrain_params.hpp"
#include "core/frame_budgets.hpp"
#include "core/frustum.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine { class ChunkGenerator; }
#include <deque>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include <memory>
#include <cstdint>

namespace godot {
class Node;
}

namespace VoxelEngine {

class ChunkWorld;
class MeshManager;
class ThreadPool;
class PerformanceTimer;
class MaterialManager;

// -------------------------------------------------------------------------
// WorldUpdater — owns the per-frame chunk scheduling logic.
// -------------------------------------------------------------------------
class WorldUpdater {
public:
    WorldUpdater();
    ~WorldUpdater();

    void set_chunk_world(ChunkWorld* cw) { chunk_world = cw; }
    void set_mesh_manager(MeshManager* mm) { mesh_manager = mm; }
    void set_thread_pool(ThreadPool* tp) { thread_pool = tp; }
    void set_performance_timer(PerformanceTimer* pt) { perf_timer = pt; }
    void set_material_manager(MaterialManager* mm) { material_manager = mm; }
    void set_owner(godot::Node* node) { owner = node; }

    void set_seed(int32_t s);
    void set_sea_level(float level);
    void set_biome_size(float size);
    void set_terrain_params(const TerrainParams& p);
    void set_biome_config(const BiomeConfig& c);
    void set_vegetation_config(const VegetationConfig& c);
    void set_render_distance(int32_t rd) { render_distance = rd; }
    void set_editor_render_distance(int32_t rd) { editor_render_distance = rd; }
    void set_player_position(const godot::Vector3& pos) { player_position = pos; }
    void set_lod_distance(int32_t d) { lod_distance = d; }
    int32_t get_lod_distance() const { return lod_distance; }
    void set_lod_detail_level(float l) { lod_detail_level = l; }
    float get_lod_detail_level() const { return lod_detail_level; }
    void set_far_lod_distance(int32_t d) { far_lod_distance = d; }
    int32_t get_far_lod_distance() const { return far_lod_distance; }
    void set_far_lod_detail_level(float l) { far_lod_detail_level = l; }
    float get_far_lod_detail_level() const { return far_lod_detail_level; }
    void set_vegetation_enabled(bool enabled);
    bool is_vegetation_enabled() const { return vegetation_enabled; }
    const TerrainParams& get_terrain_params() const { return terrain_params; }
    void set_frustum(const Frustum& f) {
        frustum = f;
        frustum_cursor = 0;
        frustum_pass_complete = false;
    }
    const Frustum& get_frustum() const { return frustum; }

    void update(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, double delta);
    bool generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch);
    void try_unload(uint64_t key);
    void queue_unload(uint64_t key);

    void clear();
    void reset();

    int32_t get_last_player_chunk_x() const { return last_player_chunk_x; }
    int32_t get_last_player_chunk_y() const { return last_player_chunk_y; }
    int32_t get_last_player_chunk_z() const { return last_player_chunk_z; }
    double get_initial_loading_duration() const { return budgets.loading_duration; }

private:
    ChunkWorld* chunk_world = nullptr;
    MeshManager* mesh_manager = nullptr;
    ThreadPool* thread_pool = nullptr;
    PerformanceTimer* perf_timer = nullptr;
    MaterialManager* material_manager = nullptr;
    godot::Node* owner = nullptr;

    TerrainParams terrain_params;
    BiomeConfig biome_config;
    VegetationConfig vegetation_config;
    godot::Vector3 player_position;
    int32_t render_distance = 8;
    int32_t editor_render_distance = 4;
    int32_t lod_distance = 0;
    float lod_detail_level = 0.5f;
    int32_t far_lod_distance = 16;
    float far_lod_detail_level = 0.25f;
    bool vegetation_enabled = true;

    FrameBudgets budgets;

    std::vector<ChunkPos> pre_sorted_offsets;
    int32_t current_render_distance = 64;
    std::vector<uint64_t> unload_queue;
    std::unordered_set<uint64_t> unload_pending;
    int32_t last_player_chunk_x = INT32_MIN;
    int32_t last_player_chunk_y = INT32_MIN;
    int32_t last_player_chunk_z = INT32_MIN;

    double dirty_flush_accumulator = 0.0;

    // Surface-aware generation: cached per-column height estimates.
    std::unique_ptr<ChunkGenerator> height_estimator;
    std::unordered_map<uint64_t, float> column_height_cache;
    std::deque<uint64_t> column_height_fifo;

    Frustum frustum;
    size_t frustum_cursor = 0;
    bool frustum_pass_complete = false;
    float visible_chunk_ratio_ = 1.0f;

    // Resumable generation cursor — amortises the pre_sorted_offsets scan across frames.
    // Reset when player changes chunks; set pass_complete when a full sweep finds nothing.
    size_t  generation_cursor          = 0;
    bool    generation_pass_complete   = false;  // true = all chunks loaded, skip scan
    bool    generation_sweep_generated = false;  // tracks if current sweep generated any chunk

    // Unload scan throttle — avoids holding shared_lock for 500 iterations every frame.
    // Scan runs when player changes chunks, or every kUnloadScanSkipFrames frames otherwise.
    int32_t unload_scan_skip_counter   = 0;
    static constexpr int32_t kUnloadScanSkipFrames = 15;

    // Resumable cursor for the unload scan (bucket index into ChunkMap's
    // internal unordered_map). Persisted across frames so the scan actually
    // walks the whole map over time instead of re-checking the same ~500
    // entries forever. See ChunkMap::for_each_limited_resumable.
    size_t unload_scan_bucket_cursor = 0;

    float get_column_surface_height(int32_t cx, int32_t cz);
    void invalidate_height_cache();

    void initialize_view_distance(int32_t horizontal_rd);
    void update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void process_mesh_budgets(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, int32_t active_render_distance, double delta);
    void flush_dirty(double delta);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_WORLD_UPDATER_HPP