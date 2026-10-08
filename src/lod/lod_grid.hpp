#ifndef FARLANDS_LOD_GRID_HPP
#define FARLANDS_LOD_GRID_HPP
#include "core/terrain_params.hpp"
#include "lod/lod_node_cache.hpp"
#include "lod/lod_surface.hpp"
#include "lod/lod_tile_policy.hpp"
#include "worldgen/biome_config.hpp"

#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <utility>
#include <vector>

namespace godot {
class Camera3D;
class Node;
}

namespace VoxelEngine {

class ThreadPool;

// The seed-grid far mode (docs/lod-modes.md): terrain beyond the loaded world,
// sampled from the seed instead of generated. Nothing in it is a chunk — no block
// storage, no light pass, no save file — which is what lets the horizon cost a
// function call per sample rather than a generation per column.
//
// Ground rules, all of them the mode's, not decoration:
//   * Off by default, and off means no tiles exist at all.
//   * It owns only the ring BEYOND the loaded world (`set_inner_radius_blocks`),
//     so generation, lighting, collision, fluids and the save format are untouched
//     and the mode can be deleted by deleting this class.
//   * Tiles are world-aligned and sized so their edges fall on the same global
//     lattice as their neighbours at the same spacing: two tiles share their edge
//     nodes exactly, and no crack opens between them.
//   * Sampling runs on the thread pool, one generator per task (a generator is
//     cheap next to the columns it samples, and a per-task one has no state to
//     share), and the results are drained and uploaded on the main thread under a
//     per-frame budget, nearest tile first.
class LodGrid {
public:
    // What a tile build did, as the engine reports it. `columns_sampled` is the
    // cost model's unit: one column is the sampler's whole price.
    struct Stats {
        bool enabled = false;
        int32_t tiles_live = 0;
        int32_t tiles_built = 0;
        int32_t tiles_dropped = 0;
        int32_t tiles_failed = 0;
        int32_t uploads = 0;
        // Columns the SAMPLER actually ran for (a cache miss), and the asks the table
        // answered instead. The ratio is what the shared table is worth: without it
        // every tile samples its own four corners, so a 27 km reach asks for 185,017
        // columns where 46,656 exist.
        int64_t columns_sampled = 0;
        int64_t cache_hits = 0;
        int32_t spacing_blocks = 0;
        // What the OUTERMOST level samples at: always the tile size, whatever the base
        // spacing is (lod_spacing_for_level), which is the number that says the reach
        // is a distance rather than a quality setting.
        int32_t outer_spacing_blocks = 0;
        int32_t outer_radius_blocks = 0;
        int32_t quads = 0;
        // What the far field costs to draw: one instance per non-empty level, not
        // one per tile.
        int32_t draw_calls = 0;
        int32_t vertices = 0;
        // Live tiles that straddle the loaded world's edge, and the radius the
        // shader clips them against. Both are the mode's answer to the question
        // "is the ground beyond the world filled in?": the tiles are the wedges
        // the world's disc cannot reach, and the clip is why the part of them the
        // world already draws is not drawn twice.
        int32_t straddling_tiles = 0;
        int32_t clip_radius_blocks = 0;
        double last_build_ms = 0.0;
        double last_schedule_ms = 0.0;
        // What one frame of merging costs, averaged over the frames since the last
        // gather (including the frames that merged nothing, which is every frame
        // once the reach has settled). The far field's only main-thread work.
        double merge_ms_per_frame = 0.0;
    };

    LodGrid();
    ~LodGrid();

    LodGrid(const LodGrid&) = delete;
    LodGrid& operator=(const LodGrid&) = delete;

    void set_config(int32_t seed, const TerrainParams& params, const BiomeConfig& biomes);
    void set_thread_pool(ThreadPool* pool) { thread_pool = pool; }
    void set_owner(godot::Node* node) { owner = node; }
    void set_material(const godot::Ref<godot::ShaderMaterial>& mat) { material = mat; }
    void set_enabled(bool value);
    [[nodiscard]] bool is_enabled() const { return enabled; }
    // Blocks between samples in the innermost ring. Every level out from it doubles
    // up to a tile, so the geometry per tile falls as fast as the tile count rises;
    // the outermost level is the tile size whatever this is, which keeps the reach's
    // distance and its cost out of the near detail's hands (lod_spacing_for_level).
    void set_base_spacing(int32_t blocks);
    [[nodiscard]] int32_t get_base_spacing() const { return base_spacing; }
    // Tile rings per spacing level: how much of the annulus each level covers.
    void set_rings_per_level(int32_t rings);
    [[nodiscard]] int32_t get_rings_per_level() const { return rings_per_level; }
    // How many rings the OUTERMOST spacing level covers -- the reach, and the one
    // distance knob that is cheap: at that level a tile is a single quad, so extra
    // horizon costs one quad per tile, where the same ring at the innermost spacing
    // is six figures of them. 0 keeps the ladder's own reach
    // (`rings_per_level` there), which is what every setting the old builds saved
    // meant. Changing it only ADDS or DROPS the outer ring's tiles: the levels
    // inside it do not move, so a drag of the slider does not re-sample the world.
    void set_outer_rings(int32_t rings);
    [[nodiscard]] int32_t get_outer_rings() const { return outer_rings; }
    // Where the far field starts: the loaded world's own radius in blocks.
    void set_inner_radius_blocks(int32_t blocks);
    void set_player_position(const godot::Vector3& position) { player_position = position; }
    void set_epoch(uint64_t value);
    void update(double delta);
    // Drops every tile and every in-flight result: a world change. In-flight
    // results are refused by epoch rather than waited for.
    void reset();
    [[nodiscard]] int32_t get_outer_radius_blocks() const;
    [[nodiscard]] Stats gather_stats();
    // The two probe readouts (lod/lod_grid_debug.cpp), for the questions a frame
    // cannot answer: what the sampler gives one column, and where the water quads
    // that have been built actually are. probes/probe_lod_grid_water.gd is the
    // caller; neither is on a path the mode itself runs.
    [[nodiscard]] godot::Dictionary debug_column(int32_t x, int32_t z) const;
    [[nodiscard]] godot::Dictionary debug_water_heights() const;
    // The scenario every tile instance is registered in (the owner's world).
    [[nodiscard]] godot::RID scenario() const;
    // Per-frame ceilings, injected from FrameBudgets by the engine controller.
    void set_budgets(int32_t max_builds_in_flight, int32_t max_uploads_per_frame);

private:
    struct Tile {
        int32_t tx = 0;
        int32_t tz = 0;
        int32_t spacing = 0;
        int32_t level = 0;        // which merge bucket this tile's geometry belongs to
        // The geometry, in world coordinates, kept until the bucket it belongs to
        // is next merged. It is the same data the mesh would hold, so nothing is
        // duplicated by holding it: a merged ArrayMesh is built from these and the
        // per-tile one is never built at all.
        lod::TileMesh raw;
        int32_t quads = 0;
        bool has_mesh = false;
        // A build has answered for this tile, with geometry or with nothing. An
        // empty answer is FINAL, not a failure to retry: the sampler is a pure
        // function of the configuration, so a second pass would return the same
        // hole. Without this the tiles of a level whose spacing the builder
        // refuses stayed pending for the whole session and were re-sampled every
        // frame, and -- because the scheduler works nearest first -- they spent the
        // in-flight budget on themselves and starved the reach's own outer rings of
        // ever being built at all.
        bool settled = false;
        bool in_flight = false;
        uint64_t request = 0;   // bumped on every (re)build request for this tile
        // When that request was fired, in seconds. A build that has not come back
        // in kBuildTimeoutSeconds is treated as lost and asked for again: a task
        // can be dropped by a pool shutdown, and a finished result can be thrown
        // away by a world reset, and either one used to leave this tile unbuilt
        // for the rest of the session.
        double in_flight_since = 0.0;
    };

    // One instance (and so one draw call) per spacing level. Draw calls are the
    // expensive thing in Godot, not vertices, and the far field is a horizon: what
    // matters is how many calls it costs, not which part of it is culled. Every
    // tile shares one material and one set of uniforms, and the vertices are in
    // world coordinates, so merging them is an append -- which is exactly what the
    // world-coordinate convention was for.
    struct Bucket {
        godot::RID instance_rid;
        godot::RID mesh_rid;
        godot::Ref<godot::ArrayMesh> mesh;
        bool dirty = true;
        // Frames left before this level may be merged again (see kMergeIntervalFrames).
        int32_t cooldown = 0;
        int32_t tiles = 0;
        int32_t quads = 0;
        int32_t vertices = 0;
    };

    struct CompletedTile {
        int32_t tx = 0;
        int32_t tz = 0;
        int32_t spacing = 0;
        // The spacing of the tile across each edge (-x, +x, -z, +z; 0 for none),
        // decided here rather than on the worker: it depends on the player's tile,
        // which is a main-thread fact, and reading it from the grid would race the
        // settings that change it.
        std::array<int32_t, 4> neighbour_spacing{};
        uint64_t epoch = 0;
        uint64_t request = 0;
        int32_t columns_sampled = 0;
        double build_ms = 0.0;
        lod::TileMesh mesh;
    };

    // 256 blocks a side, from the tile policy: the unit of build, drop and
    // instance, and the width of the lattice the disc's edge is measured in.
    static constexpr int32_t kTileBlocks = lod::kTileBlocks;

    // Everything a worker touches, owned jointly with the grid. A tile build can
    // outlive the grid (a world reset, or shutdown), and a task that wrote into a
    // freed queue would be a crash a long way from the cause: the sink keeps the
    // queue and the mutex alive for as long as one task is still running, and the
    // grid simply stops reading. Same reasoning (and the same shape) as
    // ColumnPrefetch's shared state.
    //
    // There is deliberately no count of builds in flight here. There was one, and
    // it was a second source of truth: free_all_tiles() throws away the results
    // that have finished but not been drained yet, and a counter that is only
    // decremented by a task or a drain silently kept counting them, so the ceiling
    // stopped being satisfied and the mode never scheduled another tile -- a hole
    // in the ground for the rest of the session, with no failure anywhere to see.
    // The count is taken from the tiles themselves instead (schedule_builds).
    struct Sink {
        std::mutex mutex;
        std::queue<CompletedTile> completed;
        std::atomic<int64_t> columns_sampled{0};
        // The column samples every task shares (lod_node_cache.hpp). It lives here
        // rather than on the grid because this is the half that outlives the grid: a
        // task still running through a world reset reads its table, not a freed one.
        lod::NodeCache cache;
        // What the merge cost the frame it ran in, summed over the frames since the
        // last gather, and how many frames that was. A fill is the only time this
        // mode touches the main thread at all, so this pair is where its frame cost
        // is measured rather than guessed at.
        std::atomic<double> merge_ms_window{0.0};
        std::atomic<int64_t> merge_frames{0};
    };

    static uint64_t tile_key(int32_t tx, int32_t tz);
    [[nodiscard]] int32_t spacing_for_level(int32_t level) const {
        return lod::lod_spacing_for_level(base_spacing, level);
    }
    int32_t level_for_distance(int32_t tile_distance) const;
    int32_t player_tile_x() const;
    int32_t player_tile_z() const;
    // The spacing across each of a tile's four edges (-x, +x, -z, +z), from the
    // same level rule that decides the tiles themselves: 0 where no tile is wanted
    // (the loaded world, or past the outer ring). A neighbour with a larger
    // spacing is what makes the shared edge snap to its chord.
    std::array<int32_t, 4> neighbour_spacing_for(int32_t tx, int32_t tz, int32_t ptx,
                                                 int32_t ptz) const;
    // Pushes the disc the far field is clipped against (the loaded world's own
    // radius) into the material. Cheap and idempotent: it only writes when the
    // centre or the radius moved.
    void push_clip_uniforms();
    // The camera's far plane has to reach the horizon, or the horizon is not there:
    // Godot's default is 4000 blocks, and the outer rings past it would be clipped
    // away -- geometry the mode spent quads on, invisible, with the reach slider's
    // top half doing nothing. The plane is raised to the mode's own horizon while it
    // is on (never lowered: a shorter reach leaves it where it was, which costs
    // nothing but a wider depth range) and the value found before the first raise is
    // put back when the mode goes off.
    void push_camera_far();
    void restore_camera_far();
    // The viewport's active camera, or null outside a world: the far-plane pair
    // above is a no-op without one.
    [[nodiscard]] godot::Camera3D* find_camera() const;
    // The tile's texture layers, resolved once per configuration change: the
    // biome's surface block for terrain and the water block for the liquid plane.
    void refresh_layers();

    void refresh_wanted_tiles();
    void schedule_builds(double delta);
    void drain_completed(int32_t max_uploads);
    // Merges a level's tiles into the one mesh its one instance draws. Rebuilt
    // rather than patched when any of its tiles arrives or leaves, which is what
    // keeps the cost in the frame it happens: a level is a horizon, and the whole
    // merge is one pass over the tiles it owns.
    void rebuild_bucket(int32_t level);
    void build_tile(const std::shared_ptr<CompletedTile>& result);
    void free_tile(Tile& tile);
    void free_all_tiles();

    TerrainParams terrain_params;
    BiomeConfig biome_config;
    int32_t seed = 0;
    ThreadPool* thread_pool = nullptr;
    godot::Node* owner = nullptr;
    godot::Ref<godot::ShaderMaterial> material;
    godot::Vector3 player_position;

    bool enabled = false;
    int32_t base_spacing = 32;
    int32_t rings_per_level = 2;
    // 0 = the ladder's own rings at the outermost level (see lod_outer_rings).
    int32_t outer_rings = 0;
    int32_t inner_radius_blocks = 512;
    // Uploading a tile is a move of its geometry now -- the mesh merge happens once
    // per level per frame -- so the ceilings can sit where the build cost is, which
    // is what keeps a ring from arriving as a slow trickle while the player walks.
    // This is the FILL RATE, and it is a per-frame count rather than a concurrency
    // ceiling, which is what it was mistaken for: schedule_builds tops the in-flight
    // count up to it once a frame, so the reach arrived at 12 x the frame rate and not
    // at the pool's throughput at all. Measured at the setting's far end by
    // probes/probe_lod_grid_fill.gd: 45,369 tiles in 7,365 frames over 97.2 s, which is
    // 6 tiles a frame and 467 a second -- the pool's fifteen workers idle for most of
    // it, and "the reach takes forty seconds to arrive" is that arithmetic with nothing
    // else in it (the same probe reads 20.5 s at 24 tiles a frame with the numbers
    // below). The pool runs the mode's tasks beside the
    // world's, so what this wants to be is more than the workers can run: a queue deep
    // enough that no worker waits on the next frame's dispatch, and still a bounded
    // one, so a burst cannot hold the pool away from generation and meshing.
    int32_t max_builds_in_flight = 64;
    // A drained tile is a move of its geometry plus a dirty mark now (the merge
    // happens once per level per frame), so what this budget really controls is how
    // long a freshly moved reach takes to APPEAR. It was 4, which is 4 tiles a frame:
    // the ladder's 289 tiles came up in a second or two, while the 45,369 tiles of the
    // setting's far end would have taken three minutes. 64 was picked when the build
    // side was the bound at 600 tiles a second; with the dispatch rate above it is the
    // drain that binds next, and it follows the same rule -- far more than one frame's
    // worth, because the merge is charged once per level per frame whether one tile
    // arrived or two hundred, so draining more often is nearly free.
    int32_t max_uploads_per_frame = 256;
    // How many frames a level waits between merges once it has been merged. A merge is
    // the whole level rebuilt and re-uploaded -- every vertex of it, into a fresh
    // ArrayMesh -- and during a fill every level is dirty every frame, so the frame was
    // paying four full re-uploads of up to 321k vertices. Measured, that is most of what
    // a filling frame costs: the fill ran at 49 fps with the tiles arriving at 31 a
    // frame, and both numbers are the merge rather than the mode. Three frames between
    // merges is 20 Hz at 60 fps, which on a horizon two kilometres out is not a rate
    // anybody can see, and it hands two frames in three back to the rest of the engine.
    static constexpr int32_t kMergeIntervalFrames = 3;
    // Long enough that no healthy build is re-asked for (the innermost tile is
    // tens of milliseconds even on a busy pool) and short enough that a lost task
    // is a retry rather than a hole.
    static constexpr double kBuildTimeoutSeconds = 10.0;
    // Levels 0..3, doubling from the setting's base and capped at the tile size
    // (see lod::lod_spacing_for_level), so the default base of 32 is 32 / 64 / 128 /
    // 256 blocks. The last level is always the cap and always puts a single quad on a
    // 256-block tile, which is what keeps a horizon several kilometres out from
    // costing anything -- at every setting, not only at the default.
    static constexpr int32_t kLevels = lod::kLevels;
    // The reach ceiling, in rings of the outermost level: 100 * 256 blocks = 25.6 km
    // past the world, which is what the setting offers. The ceiling is real (the tile
    // scan, the tile map and the first build-out all scale with it) but it is not
    // small: at that level a tile is one quad, so the last ring is ~850 of them and
    // the whole band 45,369 -- geometry that is cheap, and builds that are not
    // (docs/lod-modes.md carries the measured rate).
    static constexpr int32_t kMaxOuterRings = 100;

    uint64_t epoch = 0;
    int32_t last_player_tile_x = INT32_MIN;
    int32_t last_player_tile_z = INT32_MIN;
    int32_t last_inner_blocks = INT32_MIN;
    // The clip the material currently carries, so the per-frame push is a compare.
    godot::Vector3 last_clip_center;
    int32_t last_clip_radius = INT32_MIN;
    // The far plane the camera had before the mode raised it, and whether it did.
    float camera_far_original = 0.0f;
    bool camera_far_raised = false;
    bool tiles_dirty = true;

    std::unordered_map<uint64_t, Tile> tiles;
    // The scheduler's per-frame worklist, kept between frames: at the setting's far
    // end it holds one entry per live tile (tens of thousands), and a fresh vector
    // per frame was an allocation and a sort of that size every frame while the reach
    // filled.
    std::vector<std::pair<int32_t, uint64_t>> pending_scratch;
    std::array<Bucket, static_cast<size_t>(lod::kLevels)> buckets;
    std::shared_ptr<Sink> sink;
    uint64_t next_request = 1;

    uint8_t biome_layers[static_cast<size_t>(BiomeType::Count)] = {0, 0, 0};
    uint8_t underwater_layer = 0;
    uint8_t water_layer = 0;
    // The liquid layer the material currently carries, so the per-frame push is a
    // compare. -1 means "never pushed", which is not a layer any array has.
    int32_t last_water_layer = -1;

    Stats stats;
};

} // namespace VoxelEngine

#endif // FARLANDS_LOD_GRID_HPP
