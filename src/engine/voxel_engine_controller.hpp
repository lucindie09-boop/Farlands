#ifndef FARLANDS_VOXEL_ENGINE_CONTROLLER_HPP
#define FARLANDS_VOXEL_ENGINE_CONTROLLER_HPP
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/chunk_types.hpp"
#include "core/inventory.hpp"
#include "fluids/fluid_state_table.hpp"
#include "core/crafting.hpp"
#include "core/frustum.hpp"
#include "core/performance_timer.hpp"
#include "world/world_updater.hpp"
#include "world/chunk_world.hpp"
#include "world/block_editor.hpp"
#include "render/environment_controller.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "engine/collision_resolver.hpp"
#include "schematic/mc_palette.hpp"

namespace VoxelEngine {

class ThreadPool;

class VoxelEngineController {
public:
    VoxelEngineController();
    ~VoxelEngineController();

    VoxelEngineController(const VoxelEngineController&) = delete;
    VoxelEngineController& operator=(const VoxelEngineController&) = delete;

    void initialize();
    void shutdown();
    void reset_runtime_state(bool restart_thread_pool);
    void set_owner(godot::Node* node);

    void update(double delta, bool is_editor, const godot::Vector3& player_position);
    void update_frustum(const std::array<godot::Plane, 6>& planes);
    void update_chunks(bool is_editor);

    void set_block_world(int32_t world_x, int32_t world_y, int32_t world_z, int block_id);
    int  get_block_world(int32_t world_x, int32_t world_y, int32_t world_z);

    // Block files from other tools: hand it the bytes of a .schematic (or bare
    // NBT) and an origin, and it decodes, translates and writes in one go.
    //
    // The bytes come from the caller rather than a path because the caller is
    // the side that knows about res:// and user:// paths; nothing here touches
    // the filesystem except the translation table, which it reads once and
    // caches. The returned Dictionary always has "ok", and on failure "error";
    // on success the counters described in AGENTS.md (cells, replaced,
    // substituted, skipped, unknown, chunks, undo_cells, ...).
    godot::Dictionary paste_schematic_bytes(const godot::PackedByteArray& bytes,
                                            int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                            const godot::Dictionary& options);
    // Decodes and plans without writing anything: what the file is, and what it
    // would become. This is what a preview needs before it has an anchor, and
    // what tells a caller the file's size without a throwaway paste.
    godot::Dictionary inspect_schematic(const godot::PackedByteArray& bytes,
                                        const godot::Dictionary& options);
    // The same plan, anchored, plus the cells a preview has to draw: a flat
    // PackedByteArray of four int32 per cell (x, y, z, block id), stride-sampled
    // down to `preview_cells` so a 900k-cell build is still one buffer a frame
    // can upload. Nothing is written; the counts are the WHOLE plan's, so a
    // preview can say "showing 20,000 of 937,143 cells" without lying. Cell
    // sampling is even (every Nth cell in the file's own order), which is what
    // keeps the sampled shape readable as a ghost of the real one.
    godot::Dictionary preview_schematic(const godot::PackedByteArray& bytes, int32_t origin_x,
                                        int32_t origin_y, int32_t origin_z,
                                        const godot::Dictionary& options);
    // Puts the last paste back through the same writer. Answers "ok" false when
    // there is nothing to undo. A paste still waiting on chunks is abandoned
    // first, and whatever did land is reverted with it.
    godot::Dictionary undo_paste();
    // Cells the next undo would restore, 0 when there is no paste to undo.
    int64_t paste_undo_cells() const;

    // --- A paste that had to wait for chunks --------------------------------
    // Cells are written as their chunks arrive rather than being skipped: the
    // chunks are requested, pinned so the unload pass leaves them alone, and the
    // write is retried every frame until the plan is empty. `get_pending_paste`
    // is the live state (for a progress line), and `take_paste_completion` hands
    // over the one-shot report of the last job that finished — consumed, so a
    // caller polling every frame announces it once.
    godot::Dictionary get_pending_paste();
    godot::Dictionary take_paste_completion();
    void tick_pending_paste(double delta);

    bool is_aabb_solid(const godot::AABB& aabb) {
        return collision_resolver.is_aabb_solid(aabb);
    }

    CollisionResolver::CollisionResult resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size) {
        return collision_resolver.resolve(position, motion, size);
    }

    void unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    void clear_editor_chunks();

    void generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);

    godot::String get_performance_report();

    // The flow simulation, for stats and probes. Const: callers read it, they do
    // not drive it (WorldUpdater::update owns that).
    [[nodiscard]] const VoxelEngine::fluids::FluidSim& get_fluid_sim() const {
        return world_updater.get_fluid_sim();
    }
    void print_debug_info(double delta);

    void set_seed(int32_t s);
    int32_t get_seed() const;
    void set_render_distance(int32_t rd);
    int32_t get_render_distance() const;
    void set_editor_render_distance(int32_t rd);
    int32_t get_editor_render_distance() const;
    void set_player_position(const godot::Vector3& pos);
    godot::Vector3 get_player_position() const;
    void set_sea_level(float level);
    float get_sea_level() const;
    void set_biome_size(float size);
    float get_biome_size() const;
    void set_auto_update(bool enabled);
    bool get_auto_update() const;
    void set_editor_enabled(bool enabled);
    bool get_editor_enabled() const;

    void set_smooth_lighting(bool enabled);
    bool get_smooth_lighting() const;

    void set_lod_distance(int32_t d);
    int32_t get_lod_distance() const;
    void set_lod_detail_level(float l);
    float get_lod_detail_level() const;
    void set_far_lod_distance(int32_t d);
    int32_t get_far_lod_distance() const;
    void set_far_lod_detail_level(float l);
    float get_far_lod_detail_level() const;

    void set_player_light_enabled(bool enabled);
    bool get_player_light_enabled() const;
    void set_player_light_level(int32_t level);
    int32_t get_player_light_level() const;
    void set_player_light_color(const godot::Color& color);
    godot::Color get_player_light_color() const;
    void set_day_time(double t);
    double get_day_time() const;
    void set_time(double t);
    double get_time() const;
    godot::Vector3 get_sun_direction() const;
    void set_day_night_cycle_enabled(bool enabled);
    bool get_day_night_cycle_enabled() const;
    void toggle_day_night_cycle();
    void set_day_duration(double duration);
    double get_day_duration() const;
    void set_day_sky_intensity(double intensity);
    double get_day_sky_intensity() const;
    void set_night_sky_intensity(double intensity);
    double get_night_sky_intensity() const;
    void set_day_sky_color(const godot::Color& color);
    godot::Color get_day_sky_color() const;
    void set_night_sky_color(const godot::Color& color);
    godot::Color get_night_sky_color() const;
    void set_contrast(double contrast);
    double get_contrast() const;
    void set_saturation(double saturation);
    double get_saturation() const;
    void set_ao_color(const godot::Color& color);
    godot::Color get_ao_color() const;
    void set_ao_strength(double strength);
    double get_ao_strength() const;
    void set_darkness_color(const godot::Color& color);
    godot::Color get_darkness_color() const;

    void set_fog_density(double density);
    double get_fog_density() const;
    void set_fog_mode(int32_t mode);
    int32_t get_fog_mode() const;
    void set_mipmaps_enabled(bool enabled);
    bool get_mipmaps_enabled() const;
    void set_mipmap_bias(double bias);
    double get_mipmap_bias() const;
    void set_textures_enabled(bool enabled);
    bool get_textures_enabled() const;
    void set_compression_enabled(bool enabled);
    bool get_compression_enabled() const;
    void set_render_distance_blocks(float blocks);
    float get_render_distance_blocks() const;
    void set_vegetation_enabled(bool enabled);
    bool is_vegetation_enabled() const;

    void save_world_metadata();
    bool load_world_metadata();
    bool world_metadata_exists() const;
    
    void save_inventory(const Inventory& inventory);
    bool load_inventory(Inventory& inventory);
    void flush_dirty_chunks(bool wait_for_completion = false, double timeout_sec = 5.0);

    ChunkWorld& get_chunk_world() { return chunk_world; }

    // The pool the generation/mesh pipeline runs on. Borrowed by callers that
    // want to schedule their own work (the debug path planner); null before
    // initialize() or after shutdown().
    ThreadPool* get_thread_pool() { return thread_pool.get(); }
    WorldUpdater& get_world_updater() { return world_updater; }
    EnvironmentController& get_environment_controller() { return environment_controller; }
    CollisionResolver& get_collision_resolver() { return collision_resolver; }
    BlockEditor& get_block_editor() { return block_editor; }

    // Locates the nearest column of the named biome (case-insensitive:
    // ocean/hills) within max_radius_blocks of (center_x, center_z).
    // Returns {found: bool, x, y, z} where y is the macro surface height at
    // the hit (the sea bed for ocean columns).
    godot::Dictionary find_biome(const godot::String& biome_name, int32_t center_x,
                                 int32_t center_z, int32_t max_radius_blocks);

    // Loaded once at startup from res://data/recipes.json.
    const RecipeBook& get_recipe_book() const { return recipe_book; }

    static PerformanceTimer& get_perf_timer();

private:
    void create_thread_pool();
    void shutdown_thread_pool();
    void clear_async_queues();
    void free_loaded_chunks();

    // Loads data/terrain_config.json, biomes.json, vegetation.json and
    // recipes.json at startup. Missing files keep built-in defaults.
    void load_world_configs();

    // Subsystems
    ChunkWorld chunk_world;
    MeshManager mesh_manager;
    LightPropagator light_propagator;
    // The part of a paste that is still waiting for chunks. One at a time: a
    // second paste replaces the first (and cancels its waiting cells), because
    // two half-landed buildings and one undo record is worse than a refusal.
    struct PendingPaste {
        std::vector<schematic::PastePlan::Cell> remaining;
        schematic::PasteOptions options;
        // Chunk keys asked for and not yet resident (also the pinned set).
        std::vector<uint64_t> requested;
        PasteWriteResult totals;
        size_t planned = 0;
        double waited_ms = 0.0;
        int32_t batches = 0;
        // True once this job has an undo record of its own to add to. Until
        // then its first write REPLACES whatever the last paste left, so those
        // wait-batches can never merge two pastes into one undo.
        bool undo_started = false;
    };
    PendingPaste pending_paste_storage;
    bool has_pending_paste = false;
    // The report of a finished job, handed over once. `pending_paste_abandoned`
    // means the wait timed out (or nothing was left to ask for) and the cells
    // that never landed are `pending_paste_leftover`.
    bool paste_report_ready = false;
    bool paste_report_abandoned = false;
    size_t paste_report_cells = 0;
    size_t paste_report_leftover = 0;
    size_t paste_report_chunks = 0;
    double paste_report_waited_ms = 0.0;
    // The rest of the job's totals, so a caller can account for every cell: a
    // finished paste has written + unchanged + covered + out_of_bounds == planned.
    size_t paste_report_unchanged = 0;
    size_t paste_report_covered = 0;
    size_t paste_report_out_of_bounds = 0;
    size_t paste_report_planned = 0;
    void cancel_pending_paste();
    void finish_pending_paste(bool abandoned);

    WorldUpdater world_updater;
    BlockEditor block_editor;
    // Built once from whatever block registry loaded, then handed to the world
    // updater. It is the only place fluid states and block ids meet.
    VoxelEngine::fluids::FluidStateTable fluid_state_table;
    RecipeBook recipe_book;
    CollisionResolver collision_resolver{&chunk_world.get_chunk_map()};
    EnvironmentController environment_controller;

    std::unique_ptr<ThreadPool> thread_pool;

    // State
    int32_t last_player_block_x = INT32_MIN;
    int32_t last_player_block_y = INT32_MIN;
    int32_t last_player_block_z = INT32_MIN;
    godot::Vector3 player_position;

    int32_t seed = 12345;
    int32_t render_distance = 8;
    int32_t editor_render_distance = 4;
    bool auto_update = true;
    bool editor_enabled = false;
bool smooth_lighting = false;
    int32_t lod_distance = 0;
    float lod_detail_level = 0.5f;
    int32_t far_lod_distance = 16;
    float far_lod_detail_level = 0.25f;
    float sea_level = 200.0f;
    float biome_size = 1.0f;
    bool vegetation_enabled = true;
    double runtime_elapsed = 0.0;
    uint64_t frame_count = 0;
    double frame_time_accumulator = 0.0;
    uint64_t chunks_processed_last_interval = 0;
    uint64_t chunks_processed_total = 0;
    double last_delta = 0.0;

    // The legacy-id translation table, loaded from res://data/minecraft_blocks.json
    // on first use. Kept here rather than in the block registry because it is an
    // import concern: nothing else in the engine wants to know that an old id 53
    // is oak stairs.
    schematic::McPalette minecraft_palette_;
    bool minecraft_palette_loaded_ = false;
    // Names the palette uses, resolved to this build's ids once per load.
    std::unordered_map<std::string, BlockID> minecraft_name_ids_;
    // Loads the table if it has not been loaded; false with a reason in
    // `minecraft_palette_error_` when the file is missing or malformed.
    bool ensure_minecraft_palette();
    std::string minecraft_palette_error_;
    // Shared front half of inspect/paste: decode the bytes, read the options and
    // build the plan. False with a reason when any of that fails.
    // Decodes the file (or reuses the cached decode) and plans where it lands.
    // `out_file` is borrowed rather than copied: the decoded build is held by the
    // cache below, and copying a half-million-cell file per preview would put back
    // the cost the cache exists to remove.
    bool decode_and_plan(const godot::PackedByteArray& bytes,
                         const godot::Dictionary& options,
                         int32_t origin_x, int32_t origin_y, int32_t origin_z,
                         schematic::PasteOptions& out_options,
                         const schematic::SchematicData*& out_file,
                         schematic::PastePlan& out_plan, std::string& error);

    // The decoded build the wand is working on. `preview_schematic` runs at every
    // re-aim, and decoding a large file (a gzip inflate plus a parse) is the
    // expensive half of that, while the plan depends on where you aim and has to be
    // redone anyway. One entry on purpose: the wand works on one file at a time, so
    // a map would add management for no extra hits. Keyed by a hash of the bytes, so
    // a different build — or the same file edited — re-decodes.
    struct DecodedBuild {
        uint64_t fingerprint = 0;
        schematic::SchematicData file;
    };
    DecodedBuild decoded_build_;
    bool have_decoded_build_ = false;
    // The counters both entry points report, from the plan and (when there was a
    // write) what the world actually took.
    static godot::Dictionary plan_counters(const schematic::PastePlan& plan);

    static PerformanceTimer perf_timer;
};

} // namespace VoxelEngine

#endif // FARLANDS_VOXEL_ENGINE_CONTROLLER_HPP