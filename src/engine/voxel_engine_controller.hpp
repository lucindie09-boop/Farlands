#ifndef FUK_MINECRAFT_VOXEL_ENGINE_CONTROLLER_HPP
#define FUK_MINECRAFT_VOXEL_ENGINE_CONTROLLER_HPP
#include <cstdint>
#include <memory>

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/node_path.hpp>

#include "core/chunk_types.hpp"
#include "core/inventory.hpp"
#include "core/frustum.hpp"
#include "core/performance_timer.hpp"
#include "world/world_updater.hpp"
#include "world/chunk_world.hpp"
#include "world/block_editor.hpp"
#include "render/environment_controller.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "engine/collision_resolver.hpp"

namespace godot {
class Node;
}

namespace VoxelEngine {

class ThreadPool;

class VoxelEngineController {
public:
    VoxelEngineController();
    ~VoxelEngineController();

    VoxelEngineController(const VoxelEngineController&) = delete;
    VoxelEngineController& operator=(const VoxelEngineController&) = delete;

    void initialize();
    void shutdown(godot::Node* parent);
    void reset_runtime_state(bool restart_thread_pool);
    void set_owner(godot::Node* node);

    void update(double delta, bool is_editor, const godot::Vector3& player_position, godot::Node* owner);
    void update_frustum(const std::array<godot::Plane, 6>& planes);
    void update_chunks(bool is_editor);

    void set_block_world(int32_t world_x, int32_t world_y, int32_t world_z, int block_id);
    int  get_block_world(int32_t world_x, int32_t world_y, int32_t world_z);
    godot::String get_block_name(int block_id);
    godot::Dictionary raycast_from_camera(godot::Node* owner, const godot::NodePath& player_path, double max_distance);

    bool is_aabb_solid(const godot::AABB& aabb) {
        return collision_resolver.is_aabb_solid(aabb);
    }

    CollisionResolver::CollisionResult resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size) {
        return collision_resolver.resolve(position, motion, size);
    }

    void set_chunk_scenario(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, godot::Node* owner);
    void clear_editor_chunks(godot::Node* parent);
    void unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);

    void generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);

    godot::String get_performance_report();
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
    void set_lod_far_distance(int32_t d);
    int32_t get_lod_far_distance() const;
    void set_far_detail_level(float l);
    float get_far_detail_level() const;

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
    EnvironmentController& get_environment_controller() { return environment_controller; }
    CollisionResolver& get_collision_resolver() { return collision_resolver; }

    static PerformanceTimer& get_perf_timer();

private:
    void create_thread_pool();
    void shutdown_thread_pool();
    void clear_async_queues();
    void free_loaded_chunks();

    // Subsystems
    ChunkWorld chunk_world;
    MeshManager mesh_manager;
    LightPropagator light_propagator;
    WorldUpdater world_updater;
    BlockEditor block_editor;
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
    int32_t lod_far_distance = 0;
    float far_detail_level = 0.25f;
    float sea_level = 200.0f;
    float biome_size = 1.0f;
    bool vegetation_enabled = true;
    double runtime_elapsed = 0.0;
    uint64_t frame_count = 0;
    double frame_time_accumulator = 0.0;
    uint64_t chunks_processed_last_interval = 0;
    uint64_t chunks_processed_total = 0;
    double last_delta = 0.0;

    static PerformanceTimer perf_timer;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_VOXEL_ENGINE_CONTROLLER_HPP