#ifndef FUK_MINECRAFT_CHUNK_MANAGER_HPP
#define FUK_MINECRAFT_CHUNK_MANAGER_HPP
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/node_path.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <memory>

#include "core/performance_timer.hpp"
#include "core/block_types.hpp"

namespace godot {
class Node;
class Node3D;
}

namespace VoxelEngine {
class VoxelEngineController;
class CollisionResolver;
class Inventory;
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

    godot::Dictionary resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size);

    VoxelEngine::CollisionResolver* get_collision_resolver();

void set_smooth_lighting(bool enabled);
bool get_smooth_lighting() const;

    void set_lod_distance(int32_t distance);
    int32_t get_lod_distance() const;
    void set_lod_detail_level(float level);
    float get_lod_detail_level() const;

    void set_player_light_enabled(bool enabled);
    bool get_player_light_enabled() const;

    void set_player_light_level(int32_t level);
    int32_t get_player_light_level() const;

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
    std::unique_ptr<VoxelEngineController> controller;
    godot::NodePath player_path = godot::NodePath("../Player");
    godot::Node3D* cached_player = nullptr;
    godot::Camera3D* cached_camera = nullptr;
    bool ready_for_auto_update = false;
    float move_speed_multiplier_ = 1.0f;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_MANAGER_HPP
