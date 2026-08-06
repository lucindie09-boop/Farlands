#ifndef FUK_MINECRAFT_ENVIRONMENT_CONTROLLER_HPP
#define FUK_MINECRAFT_ENVIRONMENT_CONTROLLER_HPP
#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/classes/world_environment.hpp>
#include <godot_cpp/classes/directional_light3d.hpp>

#include "world/day_night_cycle.hpp"
#include "world/player_light.hpp"
#include "render/material_manager.hpp"
#include "render/sky_controller.hpp"
#include "render/fog_controller.hpp"

namespace godot {
class Node;
}

namespace VoxelEngine {

class ChunkWorld;
class LightPropagator;
class MeshManager;

class EnvironmentController {
public:
    EnvironmentController() = default;

    void update(double delta, double runtime_elapsed, const godot::Vector3& player_pos,
                ChunkWorld& cw, LightPropagator& lp, MeshManager& mm,
                double initial_loading_duration);

    void update_environment(godot::Node* parent);

    DayNightCycle& get_day_night_cycle() { return day_night; }
    MaterialManager& get_material_manager() { return material_manager; }
    PlayerLight& get_player_light() { return player_light; }

void set_day_time(double t) { day_night.set_time(t); update_shader_parameters(); }
double get_day_time() const { return day_night.get_time(); }
godot::Vector3 get_sun_direction() const { return day_night.get_sun_direction(); }

    void set_player_light_enabled(bool enabled) { player_light.set_enabled(enabled); }
    bool get_player_light_enabled() const { return player_light.get_enabled(); }
    void set_player_light_level(int32_t level) { player_light.set_level(static_cast<uint8_t>(std::clamp(level, 0, 15))); }
    int32_t get_player_light_level() const { return static_cast<int32_t>(player_light.get_level()); }

    void set_day_night_cycle_enabled(bool enabled) { day_night.set_enabled(enabled); update_shader_parameters(); }
    bool get_day_night_cycle_enabled() const { return day_night.get_enabled(); }
    void set_day_duration(double duration) { day_night.set_duration(duration); }
    double get_day_duration() const { return day_night.get_duration(); }
    void set_day_sky_intensity(double intensity) { day_night.set_day_intensity(static_cast<float>(intensity)); update_shader_parameters(); }
    double get_day_sky_intensity() const { return day_night.get_day_intensity(); }
    void set_night_sky_intensity(double intensity) { day_night.set_night_intensity(static_cast<float>(intensity)); update_shader_parameters(); }
    double get_night_sky_intensity() const { return day_night.get_night_intensity(); }
    void set_day_sky_color(const godot::Color& color) { day_night.set_day_color(color); update_shader_parameters(); }
    godot::Color get_day_sky_color() const { return day_night.get_day_color(); }
    void set_night_sky_color(const godot::Color& color) { day_night.set_night_color(color); update_shader_parameters(); }
    godot::Color get_night_sky_color() const { return day_night.get_night_color(); }

    void set_contrast(double value) { contrast = static_cast<float>(value); update_shader_parameters(); }
    double get_contrast() const { return contrast; }
    void set_saturation(double value) { saturation = static_cast<float>(value); update_shader_parameters(); }
    double get_saturation() const { return saturation; }
    void set_ao_color(const godot::Color& color) { ao_color = color; update_shader_parameters(); }
    godot::Color get_ao_color() const { return ao_color; }
    void set_ao_strength(double value) { ao_strength = static_cast<float>(value); update_shader_parameters(); }
    double get_ao_strength() const { return ao_strength; }
    void set_darkness_color(const godot::Color& color) { darkness_color = color; update_shader_parameters(); }
    godot::Color get_darkness_color() const { return darkness_color; }

    void set_fog_density(double density) { fog_controller.set_fog_density(static_cast<float>(density)); update_shader_parameters(); }
    double get_fog_density() const { return static_cast<double>(fog_controller.get_fog_density()); }
    void set_render_distance_blocks(float blocks) { fog_controller.set_render_distance_blocks(blocks); update_shader_parameters(); }
    float get_render_distance_blocks() const { return fog_controller.get_render_distance_blocks(); }
    void set_fog_mode(int32_t mode) { fog_controller.set_fog_mode(static_cast<FogController::FogMode>(mode)); update_shader_parameters(); }
    int32_t get_fog_mode() const { return static_cast<int32_t>(fog_controller.get_fog_mode()); }

private:
    DayNightCycle day_night;
    PlayerLight player_light;
    MaterialManager material_manager;
    SkyController sky_controller;
    FogController fog_controller;

    float contrast = 1.0f;
    float saturation = 1.0f;
    godot::Color ao_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);
    float ao_strength = 1.0f;
    godot::Color darkness_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);

    godot::Node* cached_parent = nullptr;
    godot::WorldEnvironment* cached_world_env = nullptr;
    godot::DirectionalLight3D* cached_sun_light = nullptr;

    void update_shader_parameters();
    void update_player_light(const godot::Vector3& player_pos, double runtime_elapsed,
                             ChunkWorld& cw, LightPropagator& lp, MeshManager& mm,
                             double initial_loading_duration);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_ENVIRONMENT_CONTROLLER_HPP
