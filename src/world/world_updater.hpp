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
#include "world/sweep_band.hpp"
#include "world/column_prefetch.hpp"
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
    // The frustum is stored as given, and the frustum PASS is re-armed on every
    // call — deliberately, and only after measuring the alternative.
    //
    // The pass looks wasteful from the counters: in one real session it spent
    // 3,545,274 checks to the distance walk's 196,553 (95% of every check in the
    // sweep), 2,406,288 of its 2,426,296 in-frustum candidates were already
    // loaded, and it re-armed from ring zero on every frame because ChunkManager
    // feeds the camera in every frame. Gating the re-arm on a real view change
    // (a position/angle threshold on the frustum) was implemented and A/B'd on
    // one build with `.freebuff/probe_stream_bench.gd`: it removed the idle cost
    // exactly (standing still for 10 s: 306,845 checks and 88.5 ms -> 0 checks
    // and 0.6 ms) and cost more than it saved (flight throughput 1,903 ->
    // 1,750-1,798 chunks/s, because this pass is a SECOND, view-ordered consumer
    // of the generation budget and its work is not waste the walk picks up: the
    // walk refuses 11% of its own checks on chunks already in flight, so it has
    // no headroom to absorb them). 88.5 ms per 10 s idle is 0.15 ms per frame —
    // the pass's checks are cheap and its ordering is what puts terrain in front
    // of the player first. What its numbers DO justify is making it cheaper per
    // check, not running it less.
    void set_frustum(const Frustum& f) {
        frustum = f;
        frustum_cursor = SweepCursor{};
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
        uint64_t candidate_offsets = 0; // candidate CHUNKS in the built sweep list
        uint64_t candidate_columns = 0; // columns that list covers
        uint64_t cursor_resets    = 0;  // walks restarted by a chunk crossing

        // Phase 2, the distance-ordered sweep. `checks` is the number of
        // candidate offsets it examined; the reject counters sum to
        // checks - band_pass, since every examined offset takes one path.
        uint64_t checks            = 0;
        uint64_t band_pass         = 0;  // passed every filter, generation attempted
        uint64_t generations       = 0;  // generate_chunk enqueued it
        uint64_t generate_refused  = 0;  // generate_chunk declined (in flight / backlog)
        uint64_t reject_loaded     = 0;  // chunk already in the map
        // Candidates the walk passed over because the chunk is ALREADY being
        // generated, learned from ChunkScheduler's lock-free filter instead of by
        // asking (which costs a global mutex plus a shard lock, and is the reason
        // these used to show up as `generate_refused`). Counted separately from
        // `checks` because they do not consume the check budget: the point is that
        // those checks go to candidates that are not already on their way.
        uint64_t reject_inflight   = 0;
        uint64_t reject_above      = 0;  // entirely above the column's content
        uint64_t reject_below      = 0;  // entirely below the band (band-only columns)
        uint64_t reject_oob        = 0;  // outside [0, kWorldChunkSlices)
        uint64_t sweeps_completed  = 0;  // full walks that found nothing left to do

        // Columns whose whole band was already resident when the band was read,
        // and how many times the walk has since skipped one. `skipped` growing
        // while `checks` stays flat is the shape of the waste this removes: the
        // walk re-confirming chunks that exist (97.8% of phase 2 before it).
        uint64_t columns_built   = 0;
        uint64_t columns_skipped = 0;

        // Phase 1, the frustum pass.
        uint64_t frustum_checks      = 0;
        uint64_t frustum_visible     = 0;  // inside the frustum (BEFORE the loaded test)
        uint64_t frustum_loaded      = 0;  // inside the frustum and already resident
        uint64_t frustum_band_pass   = 0;
        uint64_t frustum_generations = 0;
        // Passed every filter and was refused anyway — the chunk is already in
        // flight. Counted because it is the one refusal that is NOT waste
        // discovered by a bug: it is this pass asking again for something it
        // asked for on a previous frame, so it measures exactly how much of the
        // pass's budget goes on work already under way.
        uint64_t frustum_refused     = 0;
        uint64_t frustum_inflight    = 0;  // skipped without the locks (see reject_inflight)

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

        // Of that, the part spent rebuilding the sweep list. The rebuild reads one
        // band per column, so it is the only part of a walk whose cost is per
        // COLUMN rather than per candidate, and the only part that can hitch.
        uint64_t rebuilds         = 0;
        double   last_rebuild_ms  = 0.0;
        double   total_rebuild_ms = 0.0;
        double   max_rebuild_ms   = 0.0;

        // Of the walk, the part spent reading column bands (the frontier), and
        // how many columns that covers. This is the only per-COLUMN cost in the
        // sweep and the only one that needs a per-frame budget.
        uint64_t band_reads       = 0;
        double   total_band_ms    = 0.0;
        double   max_band_ms      = 0.0;
        // How many columns the worst band frame read. The budget is checked after
        // every column, so a frame cannot overshoot by more than ONE column's cost
        // — which makes this the number that says whether a fat band frame is many
        // columns (the budget is not doing its job) or a single column that stalled
        // (something outside this loop held the thread).
        uint64_t max_band_columns = 0;
        // The slowest single column ever read. If the worst band frame is large
        // AND this is large, one column stalled (the budget is fine and something
        // outside this loop took the thread); if the worst frame is large and this
        // is small, thousands of cheap columns ran past the budget.
        double   max_band_column_ms = 0.0;
        // What that slowest column was DOING, recorded together because the parts
        // only mean something for the column that was slow: its bounds read (cache
        // hit, cold range, or claimed prefetch answer) and its resident lookups
        // (count(band) shared-lock `contains` calls). Whatever is left over is
        // neither of those — the bookkeeping is arithmetic, so a large remainder is
        // this thread being taken away rather than any code in this loop.
        double   max_band_bounds_ms   = 0.0;
        double   max_band_resident_ms = 0.0;
        // The slowest single resident lookup, in any column. A value near the
        // column's whole cost is a lock wait (the generation workers hold the same
        // shards); a small value beside a large column means the stall was between
        // the statements, not inside one.
        double   max_contains_ms      = 0.0;
        // The slowest single cold bounds derivation. If a fat band frame coincides
        // with a fat value here, the rigorous height range is what stalled; if not,
        // that frame was spent after the range (the resident lookups) or this thread
        // was taken away entirely.
        double   max_cold_bounds_ms = 0.0;

        // Columns whose bounds a worker produced ahead of the frontier, and the
        // band reads that consumed one. With the prefetch running, `band_reads`
        // stays where it is while the cold part of each one moves off this
        // thread, so `total_band_ms`/`max_band_ms` are the measurement that says
        // whether that happened; `prefetch_taken` is the counter that proves a
        // band read was taken from a ready answer rather than computed here.
        uint64_t prefetch_taken   = 0;
        // Columns whose bounds THIS thread had to derive, which is the number the
        // prefetch exists to drive to zero. A column is counted once, on the pass
        // that first needed it: later passes hit the column cache instead.
        uint64_t cold_bounds      = 0;
    };

    [[nodiscard]] const GenerationStats& get_generation_stats() const { return generation_stats; }
    [[nodiscard]] ColumnPrefetch::Stats get_prefetch_stats() const {
        return column_prefetch ? column_prefetch->stats() : ColumnPrefetch::Stats{};
    }
    void reset_generation_stats() {
        // candidate_offsets/candidate_columns describe the list that is currently
        // built rather than counting anything that happened, so they survive a
        // reset. Zeroing them would read as "no candidates" for the rest of the
        // session, since the list is only rebuilt on a movement or a change.
        const uint64_t offsets = generation_stats.candidate_offsets;
        const uint64_t columns = generation_stats.candidate_columns;
        generation_stats = GenerationStats{};
        generation_stats.candidate_offsets = offsets;
        generation_stats.candidate_columns = columns;
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

    // One column of the generation sweep: where it sits relative to the player's
    // chunk, and the only slices of it that can pass the band filter.
    //
    // Replacing a flat vector of (dx, dy, dz) offsets — 65 slices per column,
    // 208,585 entries at render distance 32, ~84% of which the filter rejected
    // while it walked them — with ~3,200 column entries covering ~18,000
    // candidate chunks. The slice range is ABSOLUTE in chunk y (the filter is),
    // so only dx/dz go stale as the player moves, which is why the list is
    // rebuilt on a horizontal chunk crossing and not on a vertical one.
    struct SweepColumn {
        int16_t dx = 0;
        int16_t dz = 0;
        sweep::ChunkBand band;
        // Read on demand by service_sweep_bands, in list order, rather than for
        // the whole disc in the frame the list was built. Reading one column's
        // band costs a rigorous chunk height range over its lattice — measured at
        // ~235 us, so all 3,209 of them is ~755 ms of work that has to be spread
        // over frames or paid as one stall.
        bool band_ready = false;
    };

    // A resumable position in the sweep list: which column, and how far into that
    // column's slice range. `column == sweep_columns.size()` means the list is
    // spent.
    struct SweepCursor {
        size_t column = 0;
        uint32_t slice = 0;
    };

    struct SweepCandidate {
        int32_t x = 0;
        int32_t y = 0;
        int32_t z = 0;
        bool fill_column = false;
    };

    std::vector<SweepColumn> sweep_columns;
    // Columns whose whole band is resident, keyed by the chunk-map key at slice 0
    // (same packing, so one convention covers both and a negative coordinate
    // cannot collide with a positive one). A column lands here when its band is
    // computed and every slice of it exists, and is removed when any of its
    // chunks is unloaded (`try_unload`) or when the list is rebuilt. This is what
    // `advance_sweep` skips: one lookup instead of count(band) of them.
    std::unordered_set<uint64_t> built_columns;
    // First column whose band is still unknown. Because the list is sorted nearest
    // first and the bands are read in that order, the walk (which also goes
    // nearest first) can always run up to this frontier and never past it.
    size_t  sweep_band_frontier = 0;
    int32_t sweep_origin_cx = INT32_MIN;  // player chunk the list was built around
    int32_t sweep_origin_cz = INT32_MIN;
    bool sweep_bands_dirty = true;        // terrain changed under the bands
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
    SweepCursor frustum_cursor;
    bool frustum_pass_complete = false;
    float visible_chunk_ratio_ = 1.0f;

    // Resumable generation cursor — amortises the sweep list scan across frames.
    // Reset when player changes chunks; set pass_complete when a full sweep finds nothing.
    SweepCursor generation_cursor;
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

    // Off-thread producer of the per-column content bounds the band frontier
    // consumes. See column_prefetch.hpp: the bounds are ~181 us of pure
    // computation each and 70% of the sweep's wall time lived in them, so they
    // are the one part of the sweep that belongs on a worker rather than inside
    // a 2 ms frame budget.
    std::shared_ptr<ColumnPrefetch> column_prefetch;
    // Where the request scan has got to in the sweep list. The list is nearest
    // first and a crossing replaces its outer ring, so a bounded walk that
    // requests whatever has no bounds yet (independent of the frontier) is what
    // keeps the ring about to enter the disc already computed when it arrives.
    size_t prefetch_scan = 0;
    // Requests made so far by the pass the cursor is in, and whether a whole pass
    // came up empty. An empty pass means every column of this list either has
    // bounds or is already asked for, and only a rebuild (or a cache clear, which
    // forces one) can change that — so the scan stops until then instead of
    // re-reading 3,209 positions every frame forever.
    size_t prefetch_pass_requests = 0;
    bool   prefetch_idle = false;
    // True until the first pass over a freshly built list has run. That pass
    // covers the whole list in one frame, because the frontier reaches the far end
    // (where the ring that just entered sits) before the next one.
    bool   prefetch_pass_fresh = true;

    // Resumable cursor for the unload scan (bucket index into ChunkMap's
    // internal unordered_map). Persisted across frames so the scan actually
    // walks the whole map over time instead of re-checking the same ~500
    // entries forever. See ChunkMap::for_each_limited_resumable.
    size_t unload_scan_bucket_cursor = 0;

    ColumnSurfaceBounds get_column_surface_bounds(int32_t cx, int32_t cz);
    // The one definition of a column's bounds from a generator answer, shared by
    // this thread's fallback path and a worker's prefetched answer so the two
    // cannot disagree about them.
    static ColumnSurfaceBounds bounds_of_height_range(float min_h, float max_h, float max_water_h);
    // Inserts into the column cache with its FIFO eviction, in one place so the
    // fallback path and a claimed prefetch answer cannot diverge on ownership.
    void store_column_bounds(uint64_t key, const ColumnSurfaceBounds& bounds);
    void invalidate_height_cache();

    // Builds the sweep list: one entry per column in the render distance that has
    // any slice the band filter can accept, each carrying that slice range.
    void rebuild_sweep_columns(int32_t horizontal_rd, int32_t pcx, int32_t pcz);
    // Offers the next candidate of a list walk, in ring order then outward from
    // the player's own slice. False once the list is spent.
    bool advance_sweep(SweepCursor& cursor, int32_t pcy, SweepCandidate& out);
    // Reads the bands of the not-yet-known columns nearest the player, up to a
    // per-frame time budget. Always reads at least one, so the frontier cannot
    // stall. Costs nothing once it has caught up with the list.
    void service_sweep_bands();
    // Requests bounds for a bounded slice of the sweep list, so a column entering
    // the disc has its band answer already waiting instead of being computed on
    // this thread the moment the frontier reaches it.
    void pump_column_prefetch();
    // Hands one column to the pool against `epoch`, if a request for it was not
    // already out. True when a task was queued.
    bool enqueue_column_prefetch(int32_t cx, int32_t cz, uint32_t epoch);
    // Republishes the terrain configuration the prefetch workers compute with,
    // and drops their answers, because those bounds are derived from it.
    void refresh_prefetch_config();
    void update_generation(bool is_editor, int32_t active_render_distance, uint64_t epoch, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void update_unload(int32_t active_render_distance, int32_t pcx, int32_t pcy, int32_t pcz, bool chunk_changed);
    void process_mesh_budgets(bool is_editor, uint64_t epoch, uint64_t& chunks_processed_total, int32_t active_render_distance, double delta);
    void flush_dirty(double delta);
};

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_UPDATER_HPP