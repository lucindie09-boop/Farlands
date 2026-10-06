#ifndef FARLANDS_ENVIRONMENT_CONTROLLER_HPP
#define FARLANDS_ENVIRONMENT_CONTROLLER_HPP
#include <algorithm>
#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/color.hpp>

#include "world/day_night_cycle.hpp"
#include "world/player_light.hpp"
#include "render/material_manager.hpp"
#include "render/sky_controller.hpp"
#include "render/fog_controller.hpp"

namespace VoxelEngine {

class EnvironmentController {
public:
    EnvironmentController() = default;

    // Pushes the time of day and the player-follow glow into the materials. The
    // world objects the voxel player-light used to be marched through are gone
    // with that path (see world/player_light.hpp), so this needs no chunk map,
    // light propagator or mesh manager.
    void update(double delta, const godot::Vector3& player_pos);

    // Pushes the world's lighting state into one ITEM material -- a held block,
    // a dropped item, the first-person arm. The values are the ones last sent to
    // the terrain material, so an item can never disagree with the ground it is
    // standing on; the item shader runs the terrain's own light model
    // (shaders/item_lighting.gdshaderinc) instead of the engine's sky/ambient.
    void apply_item_shader_lighting(const godot::Ref<godot::ShaderMaterial>& material) const;

    // The sky warmth the player's BODY is lit with, kept beside the world's own
    // (item_sky_warmth) because the two are two views of the same instant: the
    // body takes the sun's own colour -- white overhead, warm at the horizon --
    // where everything else takes the world's midday cream
    // (DayNightCycle::get_sun_color_neutral). A chest-height pale model is the
    // one sky-lit surface in the world with nothing between the light and its
    // albedo to hide a cast, and a cream that reads as nothing on grass reads as
    // orange on skin, so scripts/player_model.gd overwrites `sky_light_warmth`
    // with this on the body's material after apply_item_shader_lighting has
    // written the world's.
    godot::Color get_body_sky_warmth() const { return item_body_sky_warmth; }

    DayNightCycle& get_day_night_cycle() { return day_night; }
    const DayNightCycle& get_day_night_cycle() const { return day_night; }
    MaterialManager& get_material_manager() { return material_manager; }
    SkyController& get_sky_controller() { return sky_controller; }
    FogController& get_fog_controller() { return fog_controller; }
    const FogController& get_fog_controller() const { return fog_controller; }

    void set_day_time(double t) { day_night.set_time(t); update_shader_parameters(); }
    double get_day_time() const { return day_night.get_time(); }
    godot::Vector3 get_sun_direction() const { return day_night.get_sun_direction(); }

    void set_player_light_enabled(bool enabled) { player_light.set_enabled(enabled); }
    bool get_player_light_enabled() const { return player_light.get_enabled(); }
    void set_player_light_level(int32_t level) { player_light.set_level(static_cast<uint8_t>(std::clamp(level, 0, 15))); }
    int32_t get_player_light_level() const { return static_cast<int32_t>(player_light.get_level()); }
    void set_player_light_color(const godot::Color& color) { player_light.set_color(color); }
    godot::Color get_player_light_color() const { return player_light.get_color(); }

    // The world's midday cream, as a setting. `sky_light_warmth` is the tint every
    // sky-lit surface is multiplied by (shaders/item_lighting.gdshaderinc), and the
    // sun's own colour carries a warm CREAM at the zenith -- which hides in grass,
    // dirt and stone and does not hide on a pale skin. ON is that cream, the look
    // every other lighting default here was tuned against; OFF is the sun's neutral
    // curve instead: white overhead and still warm at the horizon, so a sunset
    // lights the ground exactly as it would have. It moves the terrain, the water
    // and every item together -- they have to agree on the colour of the light --
    // and it leaves the sky, the fog and the sun light alone, which are their own
    // settings (see the Day/Night Sky Color rows). probes/probe_sky_tint.gd holds
    // both ends of it: the cream while ON, white overhead while OFF, and the two
    // curves meeting at the horizon.
    void set_sky_tint_enabled(bool enabled);
    bool get_sky_tint_enabled() const { return sky_tint_enabled; }

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
    // The far mode's fog range: begin at the loaded world's edge, end at the outer
    // tile ring. Set by the controller each frame the mode is on, because the fog
    // the world uses is tuned to the loaded radius and would erase the horizon this
    // mode exists to draw.
    void set_lod_grid_fog_range(int32_t begin_blocks, int32_t end_blocks);
    // While the far mode is on, the WORLD's terrain wears that range too. Its own
    // fog is tuned to the loaded radius, which with the mode on means the loaded
    // chunks fade out at their border while the seed-grid field beyond them does
    // not -- a bright ring around the near world, which is the one place the two
    // halves of the same terrain should agree about distance.
    void set_lod_grid_fog_active(bool active);
    [[nodiscard]] bool get_lod_grid_fog_active() const { return lod_grid_fog_active; }
    void set_fog_mode(int32_t mode) { fog_controller.set_fog_mode(static_cast<FogController::FogMode>(mode)); update_shader_parameters(); }
    int32_t get_fog_mode() const { return static_cast<int32_t>(fog_controller.get_fog_mode()); }

    void set_mipmaps_enabled(bool enabled);
    bool get_mipmaps_enabled() const { return mipmaps_enabled; }
    void set_mipmap_bias(double bias) { mipmap_bias = static_cast<float>(bias); update_shader_parameters(); }
    double get_mipmap_bias() const { return static_cast<double>(mipmap_bias); }
    void set_textures_enabled(bool enabled);
    bool get_textures_enabled() const { return textures_enabled; }
    void set_compression_enabled(bool enabled);
    bool get_compression_enabled() const;

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

    // The copy of the world lighting that item materials are fed from. Kept
    // beside the pushes to the terrain material rather than recomputed, so the
    // two can never drift: whatever the ground was last lit with is what the
    // item next to it is lit with.
    float item_sky_intensity = 1.0f;
    godot::Color item_sky_color = godot::Color(1.0f, 1.0f, 1.0f, 1.0f);
    godot::Color item_sky_warmth = godot::Color(1.0f, 1.0f, 1.0f, 1.0f);
    // The same instant, without the world's midday cream: see get_body_sky_warmth.
    godot::Color item_body_sky_warmth = godot::Color(1.0f, 1.0f, 1.0f, 1.0f);
    godot::Vector3 item_player_light_position;
    float item_player_light_radius = 8.0f;
    float item_player_light_intensity = 0.0f;
    godot::Color item_player_light_color = PlayerLight::default_color();

    bool mipmaps_enabled = true;
    float mipmap_bias = 0.1f;
    bool textures_enabled = true;
    bool sky_tint_enabled = true;
    void apply_lod_grid_lighting(float sky_intensity, const godot::Color& sky_color,
                                 const godot::Color& sky_warmth, const godot::Color& fog_color);

    // The material's own defaults until the controller sets a range: a fog range of
    // zero would erase every far tile, so an unset one must not mean "fade out at
    // the player's feet".
    float lod_grid_fog_begin = 512.0f;
    float lod_grid_fog_end = 4096.0f;
    float cached_lod_grid_fog_begin = -1.0f;
    float cached_lod_grid_fog_end = -1.0f;
    bool lod_grid_fog_active = false;

    // Dirty tracking for shader parameters (avoid redundant Godot API calls)
    float cached_blend = -1.0f;
    godot::Color cached_sky_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);
    godot::Color cached_sky_warmth = godot::Color(1.0f, 1.0f, 1.0f, 1.0f);
    godot::Vector3 cached_sun_dir = godot::Vector3(0.0f, 1.0f, 0.0f);
    float cached_contrast = -1.0f;
    float cached_saturation = -1.0f;
    godot::Color cached_ao_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);
    float cached_ao_strength = -1.0f;
    godot::Color cached_darkness_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);
    float cached_mipmap_bias = -1.0f;
    float cached_fog_begin = -1.0f;
    float cached_fog_end = -1.0f;
    godot::Color cached_fog_color = godot::Color(0.0f, 0.0f, 0.0f, 1.0f);
    float cached_fog_scatter = -1.0f;
    int32_t cached_fog_mode = -1;

    // Epsilon threshold for detecting meaningful changes (sky/fog transitions are smooth)
    static constexpr float PARAM_EPSILON = 0.001f;

    void update_shader_parameters();
};

} // namespace VoxelEngine

#endif // FARLANDS_ENVIRONMENT_CONTROLLER_HPP
