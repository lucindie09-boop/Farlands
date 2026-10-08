#ifndef FARLANDS_WORLD_UPDATER_HPP
#define FARLANDS_WORLD_UPDATER_HPP
#include "core/chunk_coords.hpp"
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
#include "world/world_updater_types.hpp"
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

// The hysteresis the unload pass keeps: a chunk survives until it is this many chunks
// past the render distance (see WorldUpdater::update_unload). It is also what says how far
// the world DRAWS, because a retained chunk keeps its mesh and its mesh is what is on
// screen -- so the far mode's inner disc is cut from that drawn radius rather than from
// the streaming one (see VoxelEngineController::set_render_distance). A disc inside the
// world's own edge is a disc of far-field cells laid over real terrain, at a height the
// two agree on only where the coarse sampling happens to cross the real surface, and what
// that looks like is chunks hidden under a cell and cells cutting through chunks at once.
inline constexpr int32_t kChunkRetentionChunks = 2;

// The radius the loaded world draws to, in blocks, from a render distance in CHUNKS: the
// render distance, the retained rings, and one more chunk for the width of the last ring
// -- a chunk centred on the far edge spans a whole chunk past it.
inline int32_t world_drawn_radius_blocks(int32_t render_distance_chunks) {
    return (render_distance_chunks + kChunkRetentionChunks + 1) * CHUNK_WIDTH;
}

// -------------------------------------------------------------------------
// WorldUpdater — owns the per-frame chunk scheduling logic.
// -------------------------------------------------------------------------
class WorldUpdater {
public:
    WorldUpdater();
    ~WorldUpdater();

    void set_chunk_world(ChunkWorld* cw) {
        chunk_world = cw;
        // The chain queue is the chunk-selection priority: every install tells
        // the updater what to spread to. Both are controller members, so the
        // listener cannot outlive the updater that owns the lambda.
        if (cw) {
            cw->set_install_listener(
                [this](int32_t cx, int32_t cy, int32_t cz, bool has_blocks) {
                    on_chunk_installed(cx, cy, cz, has_blocks);
                });
        }
    }
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
    int32_t get_render_distance() const { return render_distance; }
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
    void set_frustum(const Frustum& f) { frustum = f; }
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

    // Counters for the generation sweep, read by /genstats. Defined at namespace
    // scope in world_updater_types.hpp; the alias keeps the nested spelling
    // (`WorldUpdater::GenerationStats`) that the bindings use.
    using GenerationStats = ::VoxelEngine::GenerationStats;

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

    // --- The generation chain -------------------------------------------
    // The ONLY chunk-selection priority (experiment, replacing the frustum
    // pass): when a chunk installs and its data holds any non-air block, its
    // four horizontal neighbours are queued here. update_generation drains this
    // queue BEFORE the ring walk, and every drained candidate still goes through
    // the same filters (in-flight, loaded, band, world bounds) and the same
    // generation budget as any other candidate, so the chain can only reorder,
    // never bypass. Deliberately unbounded as a queue — it is a SET of positions
    // waiting to be offered, and generation is bounded elsewhere; a dedupe set
    // keeps a popular chunk from being offered repeatedly by its many neighbours.
    std::deque<uint64_t> chain_queue;
    std::unordered_set<uint64_t> chain_queued;
    void on_chunk_installed(int32_t cx, int32_t cy, int32_t cz, bool has_blocks);
    // Drains up to `budget` candidates from the chain queue through the shared
    // filters. Returns how many generations it enqueued.
    int32_t drain_chain_queue(uint64_t epoch, int32_t budget, int32_t pcy, bool& generated_anything);

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

    // The frustum the unload pass tests against. The retired frustum *generation*
    // pass kept a cursor and a pass-complete flag here, and the visibility ratio
    // it fed; with that pass gone the frustum is read-only state: nothing writes
    // any of the three any more, so they are not kept.
    Frustum frustum;

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

    // The slack the band filter adds around a column's content. Zero when
    // squished: the squished height range is already pinned to the kept region
    // (worldgen/terrain_squish.hpp), and the usual one-slice pad would put the
    // slices just above and below that region back — generated to hold nothing.
    [[nodiscard]] float band_pad() const {
        return terrain_params.squish_enabled ? 0.0f : 32.0f;
    }

    // Whether a column generates its FULL column (band + underground fill).
    // Never when squished: the terrain occupies the kept region only, and
    // filling from the surface to the world floor under the player would
    // restore exactly the vertical work the squish exists to take out of the
    // test (and with the kept region low, that is most of the world).
    [[nodiscard]] bool column_fill_enabled(int32_t dx, int32_t dz) const {
        return !terrain_params.squish_enabled &&
               std::abs(dx) <= kUndergroundFillRadius &&
               std::abs(dz) <= kUndergroundFillRadius;
    }

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
    // The cached bounds for a column, or nullptr when they are not in the cache.
    // For callers that must not COMPUTE a cold range as a side effect of asking —
    // the chain's seeding is per-install and must stay a few hash lookups.
    [[nodiscard]] const ColumnSurfaceBounds* peek_column_surface_bounds(int32_t cx, int32_t cz) const;
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