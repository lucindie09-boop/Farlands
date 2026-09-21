#ifndef FARLANDS_WORLD_UPDATER_HPP
#define FARLANDS_WORLD_UPDATER_HPP
#include "core/chunk_types.hpp"
#include "core/terrain_params.hpp"
#include "core/frame_budgets.hpp"
#include "core/frustum.hpp"
#include "fluids/fluid_sim.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine { class ChunkGenerator; }
// The fluid sink batches its writes per chunk, so it needs the real EditCell type
// rather than a forward declaration.
#include "world/chunk_world.hpp"
#include <array>
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

    // Fluid simulation. The state table must be built (from the loaded block
    // registry) before the first update; a world with no fluid states leaves the
    // simulation disabled and every entry point a no-op.
    void set_fluid_state_table(fluids::FluidStateTable* table);
    void notify_block_edit(int32_t x, int32_t y, int32_t z) {
        fluid_sim.notify_block_changed(x, y, z);
    }
    // The worker-thread form (see ChunkWorld::set_worker_edit_listener): queues the
    // wake for the next fluid tick instead of applying it here.
    void post_block_edit(int32_t x, int32_t y, int32_t z) {
        fluid_sim.post_block_changed(x, y, z);
    }
    [[nodiscard]] const fluids::FluidSim& get_fluid_sim() const { return fluid_sim; }

    void update(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, double delta);
    bool generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, uint64_t epoch);
    void try_unload(uint64_t key);
    void queue_unload(uint64_t key);

    void clear();
    void reset();

    // Nearest column of the given biome within max_radius_blocks of
    // (center_x, center_z), using the internal height-estimator generator so
    // the search agrees with chunk generation. Returns true and fills
    // out_x / out_z / out_height (macro surface height at the hit).
    bool find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                            int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                            float& out_height);

    // True when the chunk at (cx, cy, cz) would be entirely solid if it were
    // generated — every column's surface sits above the chunk's top, so the
    // density field has no air there. Used by mesh culling to treat an
    // ungenerated underground neighbor as opaque instead of rendering a box
    // wall into the void. Out-of-world chunks return false so boundary faces
    // at the world edges still render.
    bool chunk_would_be_solid(int32_t cx, int32_t cy, int32_t cz);

    int32_t get_last_player_chunk_x() const { return last_player_chunk_x; }
    int32_t get_last_player_chunk_y() const { return last_player_chunk_y; }
    int32_t get_last_player_chunk_z() const { return last_player_chunk_z; }
    double get_initial_loading_duration() const { return budgets.loading_duration; }

    // Counters for the generation sweep, read by /genstats.
    //
    // Why they exist: the candidate offset list spans the entire world height
    // (WORLD_HEIGHT_Y / CHUNK_HEIGHT slices per column) while only the
    // near-surface band of a column can ever pass the filters, so each pass
    // spends most of its per-frame check budget on entries that are guaranteed
    // rejections. These counters say how many entries a pass actually touches,
    // why each one was rejected, and how many became real generations — which
    // is the only way to tell whether reordering or shrinking the candidate set
    // would buy anything, rather than guessing from the list size.
    struct GenerationStats {
        uint64_t frames           = 0;  // update_generation calls
        uint64_t candidate_offsets = 0; // built offset list size (last rebuild)
        uint64_t cursor_resets    = 0;  // walks restarted by a chunk crossing

        // Phase 2, the distance-ordered sweep. `checks` is the number of
        // candidate offsets it examined; the reject counters sum to
        // checks - band_pass, since every examined offset takes one path.
        uint64_t checks            = 0;
        uint64_t band_pass         = 0;  // passed every filter, generation attempted
        uint64_t generations       = 0;  // generate_chunk enqueued it
        uint64_t generate_refused  = 0;  // generate_chunk declined (in flight / backlog)
        uint64_t reject_loaded     = 0;  // chunk already in the map
        uint64_t reject_above      = 0;  // entirely above the column's content
        uint64_t reject_below      = 0;  // entirely below the band (band-only columns)
        uint64_t reject_oob        = 0;  // outside [0, kWorldChunkSlices)
        uint64_t sweeps_completed  = 0;  // full walks that found nothing left to do

        // Phase 1, the frustum pass.
        uint64_t frustum_checks      = 0;
        uint64_t frustum_visible     = 0;  // inside the frustum (BEFORE the loaded test)
        uint64_t frustum_loaded      = 0;  // inside the frustum and already resident
        uint64_t frustum_band_pass   = 0;
        uint64_t frustum_generations = 0;

        // Urgent requests (a paste waiting on chunks). These bypass the sweep's
        // filters, so they are counted separately rather than as sweep work.
        uint64_t urgent_requested = 0;
        uint64_t urgent_generated = 0;

        // Rolling window of the last kWindowFrames frames. A session total is
        // dominated by the initial load; mid-flight the question is what the
        // sweep costs *now*, and the two are wildly different numbers.
        static constexpr size_t kWindowFrames = 120;
        std::array<uint32_t, kWindowFrames> window_checks{};
        std::array<uint32_t, kWindowFrames> window_generations{};
        size_t   window_head   = 0;
        uint32_t window_frames = 0;

        // Wall time inside update_generation, milliseconds.
        double total_ms = 0.0;
        double last_ms  = 0.0;
        double max_ms   = 0.0;
    };

    [[nodiscard]] const GenerationStats& get_generation_stats() const { return generation_stats; }
    void reset_generation_stats() {
        // candidate_offsets is the size of the list that is currently built, not
        // a count of anything that happened, so it survives a reset. Zeroing it
        // would read as "no candidates" for the rest of the session, since the
        // list is only rebuilt when the render distance changes.
        const uint64_t offsets = generation_stats.candidate_offsets;
        generation_stats = GenerationStats{};
        generation_stats.candidate_offsets = offsets;
    }

private:
    // Puts a tick's fluid writes back where they belong: into the edit map (what
    // survives a save) and into the remesh queue. The blocks themselves and the
    // render-dirty flags are already applied by the simulation's chunk adapter;
    // this is only the part that needs game-side objects.
    class FluidSink final : public fluids::FluidWriteSink {
    public:
        explicit FluidSink(WorldUpdater* owner) : owner_(owner) {}
        void on_chunk_updated(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                              const std::vector<fluids::FluidWriteRecord>& writes) override;
    private:
        WorldUpdater* owner_;
        // Reused across calls so a tick's writes do not allocate per chunk.
        std::vector<ChunkWorld::EditCell> batch;
    };

    fluids::FluidSim fluid_sim;
    FluidSink fluid_sink{ this };

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

    GenerationStats generation_stats;

    std::vector<ChunkPos> pre_sorted_offsets;
    int32_t current_render_distance = 64;
    std::vector<uint64_t> unload_queue;
    std::unordered_set<uint64_t> unload_pending;
    int32_t last_player_chunk_x = INT32_MIN;
    int32_t last_player_chunk_y = INT32_MIN;
    int32_t last_player_chunk_z = INT32_MIN;

    double dirty_flush_accumulator = 0.0;

    // Surface-aware generation: cached per-column content bounds, derived from
    // the generator's rigorous chunk height range (all lattice nodes over the
    // 16x16 chunk area, padded by the density margin). land_h is the lowest
    // column surface in the chunk — everything below it is solid rock, so the
    // scheduler can skip chunks entirely below it. top_h is the highest
    // content the chunk can reach — macro surface or water level for ocean
    // chunks — so chunks entirely above it are air and can be skipped.
    struct ColumnSurfaceBounds {
        float land_h = 0.0f;
        float top_h  = 0.0f;
    };
    std::unique_ptr<ChunkGenerator> height_estimator;
    std::unordered_map<uint64_t, ColumnSurfaceBounds> column_height_cache;
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

    // Columns within this Chebyshev radius (chunks) of the player generate
    // their FULL column — the near-surface band PLUS the solid underground
    // fill from the surface down to the world floor. Everywhere else only the
    // near-surface band generates. The fill exists so rock under the player is
    // genuinely solid: without it, the band ends in open void and its
    // underside + the rock mass's side walls render as floating chunk-border
    // faces when seen from underground. The radius stays well under the render
    // distance because only near geometry is visible (player light / fog
    // margin) — beyond it the band-only columns' undersides are too far to
    // see, so they keep the cheap band-only window.
    static constexpr int32_t kUndergroundFillRadius = 8;

    // Resumable cursor for the unload scan (bucket index into ChunkMap's
    // internal unordered_map). Persisted across frames so the scan actually
    // walks the whole map over time instead of re-checking the same ~500
    // entries forever. See ChunkMap::for_each_limited_resumable.
    size_t unload_scan_bucket_cursor = 0;

    ColumnSurfaceBounds get_column_surface_bounds(int32_t cx, int32_t cz);
    void invalidate_height_cache();

    void initialize_view_distance(int32_t horizontal_rd);
    void update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void process_mesh_budgets(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, int32_t active_render_distance, double delta);
    void flush_dirty(double delta);
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_UPDATER_HPP