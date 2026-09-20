#ifndef FARLANDS_CHUNK_MANAGER_HPP
#define FARLANDS_CHUNK_MANAGER_HPP
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <memory>

#include "core/performance_timer.hpp"
#include "core/block_types.hpp"

namespace godot {
class Node;
class Node3D;
class WorldEnvironment;
class DirectionalLight3D;
class Image;
}

namespace VoxelEngine {
class VoxelEngineController;
class CollisionResolver;
class Inventory;
namespace nav {
class PathService;
}
}

namespace VoxelEngine {

// -------------------------------------------------------------------------
// ChunkManager — Godot-facing Node3D that delegates to VoxelEngineController.
// All engine logic lives in the controller; this class is a thin wrapper
// for Godot lifecycle, property binding, and player node resolution.
// -------------------------------------------------------------------------
class ChunkManager : public godot::Node3D {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(ChunkManager, godot::Node3D)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    ChunkManager();
    ~ChunkManager() override;

    void _ready() override;
    void _enter_tree() override;
    void _exit_tree() override;
    void _process(double delta) override;

    // Godot-bound API (thin wrappers)
    void set_seed(int32_t p_seed);
    int32_t get_seed() const;

    // When this shared library was COMPILED. Reported in game, because the one
    // question that has cost the most time here is "is the build I just made the one
    // that is running?" — Godot does not hot-reload a GDExtension, and an editor left
    // open holds the image it loaded at startup. `/version` prints this.
    godot::String engine_build_stamp() const;

    void set_render_distance(int32_t distance);
    int32_t get_render_distance() const;

    void set_player_position(const godot::Vector3& position);
    godot::Vector3 get_player_position() const;

    void set_player_path(const godot::NodePath& path);
    godot::NodePath get_player_path() const;

    void set_auto_update(bool enabled);
    bool get_auto_update() const;

    void update_chunks();

    void generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    void unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);

    void set_sea_level(float level);
    float get_sea_level() const;


    void set_biome_size(float size);
    float get_biome_size() const;

    godot::String get_performance_report();

    void set_chunk_scenario(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    void clear_editor_chunks();


    void set_editor_enabled(bool enabled);
    bool get_editor_enabled() const;

    void set_editor_render_distance(int32_t distance);
    int32_t get_editor_render_distance() const;

    godot::Dictionary raycast_from_camera(double max_distance);

    void set_block(int32_t world_x, int32_t world_y, int32_t world_z, int block_id);
    int get_block(int32_t world_x, int32_t world_y, int32_t world_z);
    godot::String get_block_name(int block_id);
    godot::Array get_selection_boxes(int block_id);

    // Nearest column of the named biome (ocean/hills) within max_radius
    // blocks of (center_x, center_z). Returns {found: bool, x, y, z}.
    // Block files from other tools, from the bytes of a .schematic to blocks in
    // the world. GDScript reads the file (it is the side that knows about res://
    // and user://) and gets back the counters that say what landed and what the
    // table could not place. Options: fluids, substitutes, replace_solid,
    // write_air, max_cells.
    godot::Dictionary inspect_schematic(const godot::PackedByteArray& bytes,
                                        const godot::Dictionary& options);
    // The same plan anchored at a world position, plus the cells to draw it with:
    // four int32 per cell (x, y, z, block) in `cells`, sampled down to
    // options["preview_cells"]. Writes nothing — this is what the wand's ghost is
    // built from, and what a menu uses to say how big the paste would be.
    godot::Dictionary preview_schematic(const godot::PackedByteArray& bytes, int32_t origin_x,
                                        int32_t origin_y, int32_t origin_z,
                                        const godot::Dictionary& options);
    godot::Dictionary paste_schematic(const godot::PackedByteArray& bytes, int32_t origin_x,
                                      int32_t origin_y, int32_t origin_z,
                                      const godot::Dictionary& options);
    godot::Dictionary undo_paste();
    // Live state of a paste that is waiting for chunks to generate, and the
    // one-shot report of the last one that finished (empty when nothing is
    // waiting and nothing has finished since the last call).
    godot::Dictionary get_pending_paste();
    godot::Dictionary take_paste_completion();
    int64_t paste_undo_cells();

    godot::Dictionary find_biome(const godot::String& biome_name, int32_t center_x,
                                 int32_t center_z, int32_t max_radius);

    godot::Dictionary resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size);

    // Debug pathfinding. request_path queues a ground route between two feet
    // positions on a worker thread and returns its job id (0 when the planner
    // is unavailable); poll_paths drains every result finished since the last
    // call, each carrying its id plus the route as support-block coordinates.
    // Either cap being hit returns a truncated partial route instead of failing,
    // and both are overridable per call: `max_expansions` bounds the work,
    // `max_ms` the wall-clock time (0 disables the time cap). The time cap is a
    // ceiling for a contended machine, deliberately above what a cross-country
    // route costs (a few milliseconds), so it does not cut long routes short; the
    // expansion cap is the real work bound.
    int64_t request_path(const godot::Vector3& from, const godot::Vector3& to,
                         int32_t max_expansions = 20000, double max_ms = 32.0);

    godot::Array poll_paths();

    // The flow simulation's ticking: last tick's cells/writes/milliseconds, the
    // work still queued, and how many cells were left alone this tick because a
    // chunk their window needs is not loaded.
    godot::Dictionary get_fluid_stats();

    // Jobs still running. Cheap enough to poll per frame.
    int32_t get_pending_paths() const;

    VoxelEngine::CollisionResolver* get_collision_resolver();

void set_smooth_lighting(bool enabled);
bool get_smooth_lighting() const;

    void set_lod_distance(int32_t distance);
    int32_t get_lod_distance() const;
    void set_lod_detail_level(float level);
    float get_lod_detail_level() const;
    void set_far_lod_distance(int32_t distance);
    int32_t get_far_lod_distance() const;
    void set_far_lod_detail_level(float level);
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

    // --- Liquid Texture Lab live preview ----------------------------------
    // The lab generates an animated strip in C++ and then hands the world one
    // frame at a time; these three are the whole interface for that. The layer
    // is looked up by texture name ("water", "lava", "acid"), so a name the
    // active block set does not use reports found=false instead of writing over
    // the fallback layer.
    //
    // Shape/state of the layer that push_texture_frame would write into.
    godot::Dictionary get_texture_layer_info(const godot::String& texture_name);
    // The layer's current pixels (null when the array has no such layer).
    // Best effort only: Godot 4.7 answers null even for an array it just built.
    godot::Ref<godot::Image> get_texture_layer_image(const godot::String& texture_name);
    // The frame as it would actually be pushed: RGBA8, the array's resolution,
    // with mipmaps if the array has them. null when the name has no layer or the
    // array is compressed. This is the half of the live path that is worth
    // testing — update_layer() itself is Godot's.
    godot::Ref<godot::Image> fit_texture_frame(const godot::String& texture_name, const godot::Ref<godot::Image>& frame);
    // Overwrites that layer with `frame`. The frame is converted to RGBA8,
    // resized to the array's resolution and given mipmaps if the array has
    // them, so any lab resolution lands safely. false when the name has no
    // layer or the array is compressed (a compressed layer cannot take an
    // uncompressed frame).
    bool push_texture_frame(const godot::String& texture_name, const godot::Ref<godot::Image>& frame);
    // Puts the original image back: the active texture pack's override if there
    // is one, else the built-in PNG.
    bool restore_texture_layer(const godot::String& texture_name);

    void set_mipmaps_enabled(bool enabled);
    bool get_mipmaps_enabled() const;
    void set_mipmap_bias(double bias);
    double get_mipmap_bias() const;
    void set_textures_enabled(bool enabled);
    bool get_textures_enabled() const;
    void set_compression_enabled(bool enabled);
    bool get_compression_enabled() const;

    void set_vegetation_enabled(bool enabled);
    bool get_vegetation_enabled() const;

    void set_move_speed_multiplier(float multiplier);
    float get_move_speed_multiplier() const;

    void save_world_metadata();
    bool load_world_metadata();
    bool world_metadata_exists() const;
    void flush_dirty_chunks();
    
    VoxelEngineController* get_controller() { return controller.get(); }
    const VoxelEngineController* get_controller() const { return controller.get(); }

    static PerformanceTimer& get_perf_timer();

protected:
    static void _bind_methods();

private:
    void update_mouse_mode();
    void update_environment();

    std::unique_ptr<VoxelEngineController> controller;
    // Created on first use: it needs the controller's thread pool and chunk map.
    std::unique_ptr<VoxelEngine::nav::PathService> path_service;
    godot::NodePath player_path = godot::NodePath("../Player");
    godot::Node3D* cached_player = nullptr;
    godot::Camera3D* cached_camera = nullptr;
    godot::WorldEnvironment* cached_world_env = nullptr;
    godot::DirectionalLight3D* cached_sun_light = nullptr;
    godot::Node* cached_env_parent = nullptr;
    bool ready_for_auto_update = false;
    float move_speed_multiplier_ = 1.0f;
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_MANAGER_HPP
