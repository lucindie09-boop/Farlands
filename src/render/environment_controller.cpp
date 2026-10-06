#include "render/environment_controller.hpp"
#include <algorithm>
#include <cmath>

namespace VoxelEngine {

void EnvironmentController::update(double delta, const godot::Vector3& player_pos) {
    day_night.update(delta);
    update_shader_parameters();

    // The colour is whatever the light is set to, not a constant: a held torch
    // owns both the level and the tint (see PlayerController::update_held_light),
    // so the glow around the player matches the item being carried.
    const float player_light_intensity = player_light.get_enabled() ? player_light.get_level() / 15.0f : 0.0f;
    const godot::Color player_light_color = player_light.get_color();
    material_manager.update_player_light(
        player_pos,
        8.0f,
        player_light_intensity,
        player_light_color
    );

    // The same glow for the object-level lighting (items held or dropped near
    // the player are inside this radius), kept so apply_item_shader_lighting can
    // hand over exactly what the terrain just got.
    item_player_light_position = player_pos;
    item_player_light_radius = 8.0f;
    item_player_light_intensity = player_light_intensity;
    item_player_light_color = player_light_color;
}

void EnvironmentController::apply_item_shader_lighting(const godot::Ref<godot::ShaderMaterial>& material) const {
    if (material.is_null()) return;
    material->set_shader_parameter("sky_light_intensity", item_sky_intensity);
    material->set_shader_parameter("sky_light_color", item_sky_color);
    material->set_shader_parameter("sky_light_warmth", item_sky_warmth);
    // No AO: an item is a convex body in open air, and the item shader's own
    // face shading is all the occlusion it has (see the include).
    material->set_shader_parameter("darkness_color", darkness_color);
    material->set_shader_parameter("saturation", saturation);
    material->set_shader_parameter("contrast", contrast);
    material->set_shader_parameter("player_light_position", item_player_light_position);
    material->set_shader_parameter("player_light_radius", item_player_light_radius);
    material->set_shader_parameter("player_light_intensity", item_player_light_intensity);
    material->set_shader_parameter("player_light_color", item_player_light_color);
}

void EnvironmentController::set_lod_grid_fog_active(bool active) {
    if (lod_grid_fog_active == active) return;
    lod_grid_fog_active = active;
    // The world's fog range changes with it, and the gate below compares the range
    // it will push, so a plain repush is what makes the change land.
    update_shader_parameters();
}

void EnvironmentController::set_lod_grid_fog_range(int32_t begin_blocks, int32_t end_blocks) {
    const float begin = static_cast<float>(begin_blocks);
    const float end = static_cast<float>(std::max(begin_blocks + 1, end_blocks));
    if (std::abs(begin - lod_grid_fog_begin) <= PARAM_EPSILON &&
        std::abs(end - lod_grid_fog_end) <= PARAM_EPSILON) {
        return;
    }
    lod_grid_fog_begin = begin;
    lod_grid_fog_end = end;
    // Straight to the material: this is a range change, and the dirty gate below
    // would otherwise swallow it (none of its other inputs move when the mode's
    // radius does) -- the same reason the sky tint pushes here.
    update_shader_parameters();
}

void EnvironmentController::apply_lod_grid_lighting(float sky_intensity,
                                                   const godot::Color& sky_color,
                                                   const godot::Color& sky_warmth,
                                                   const godot::Color& fog_color) {
    const godot::Ref<godot::ShaderMaterial> material = material_manager.get_lod_grid_material();
    if (material.is_null()) return;
    material->set_shader_parameter("sky_light_intensity", sky_intensity);
    material->set_shader_parameter("sky_light_color",
                                   godot::Vector3(sky_color.r, sky_color.g, sky_color.b));
    material->set_shader_parameter("sky_light_warmth",
                                   godot::Vector3(sky_warmth.r, sky_warmth.g, sky_warmth.b));
    material->set_shader_parameter("darkness_color",
                                   godot::Vector3(darkness_color.r, darkness_color.g, darkness_color.b));
    material->set_shader_parameter("saturation", saturation);
    material->set_shader_parameter("contrast", contrast);
    material->set_shader_parameter("fog_color",
                                   godot::Vector3(fog_color.r, fog_color.g, fog_color.b));
    material->set_shader_parameter("fog_begin", lod_grid_fog_begin);
    material->set_shader_parameter("fog_end", lod_grid_fog_end);
    material->set_shader_parameter("mipmap_bias", mipmaps_enabled ? mipmap_bias : 0.0f);
}

void EnvironmentController::set_sky_tint_enabled(bool enabled) {
    if (sky_tint_enabled == enabled) return;
    sky_tint_enabled = enabled;
    // Straight to the materials rather than waiting for the next frame: this is a
    // setting, and the dirty gate below is what would otherwise swallow it (none of
    // its other inputs move when the tint does) -- cached_sky_warmth is checked
    // there so this call is the one that gets through.
    update_shader_parameters();
}

void EnvironmentController::set_mipmaps_enabled(bool enabled) {
    if (mipmaps_enabled == enabled) return;
    mipmaps_enabled = enabled;
    material_manager.set_texture_filter_mode(enabled ? static_cast<int>(TextureFilterMode::NearestMipmap) : static_cast<int>(TextureFilterMode::Nearest));
    update_shader_parameters();
}

void EnvironmentController::set_textures_enabled(bool enabled) {
    if (textures_enabled == enabled) return;
    textures_enabled = enabled;
    material_manager.set_textures_enabled(enabled);
}

void EnvironmentController::set_compression_enabled(bool enabled) {
    material_manager.set_compression_enabled(enabled);
}

bool EnvironmentController::get_compression_enabled() const {
    return material_manager.get_compression_enabled();
}

void EnvironmentController::update_shader_parameters() {
    const float blend = day_night.get_blend();
    const float sky_intensity = day_night.get_sky_intensity();
    const godot::Color sky_color = day_night.get_sky_color();
    const godot::Vector3 sun_dir = day_night.get_sun_direction();

    const float elevation = day_night.get_sun_elevation();
    const godot::Color sun_color = day_night.get_sun_color();
    // The world's surface tint: the cream, or the sun's own curve when the setting
    // is off (see set_sky_tint_enabled). Both are warm at the horizon, so the two
    // differ only where the sun is high.
    const godot::Color sky_warmth = sky_tint_enabled ? sun_color : day_night.get_sun_color_neutral();
    const float sky_turbidity = day_night.get_sky_turbidity();

    // Item materials are fed from these every frame, independent of the dirty
    // gate below: it only exists to keep Godot API calls off redundant frames,
    // and the item pushes are the caller's own decision. The body's warmth rides
    // with them: same instant, same curve, no midday cream (see
    // get_body_sky_warmth in the header).
    item_sky_intensity = sky_intensity;
    item_sky_color = sky_color;
    item_sky_warmth = sky_warmth;
    item_body_sky_warmth = day_night.get_sun_color_neutral();

    const godot::Vector3 sky_horizon_color = sky_controller.get_horizon_color(blend, elevation, sun_color, sky_turbidity);
    const godot::Vector3 sky_zenith_color = sky_controller.get_zenith_color(blend);

    // Check if any values changed meaningfully (epsilon-gated dirty check)
    bool needs_update = false;
    
    if (std::abs(blend - cached_blend) > PARAM_EPSILON) needs_update = true;
    if (sky_color != cached_sky_color) needs_update = true;
    if (sky_warmth != cached_sky_warmth) needs_update = true;
    if (sun_dir != cached_sun_dir) needs_update = true;
    if (std::abs(contrast - cached_contrast) > PARAM_EPSILON) needs_update = true;
    if (std::abs(saturation - cached_saturation) > PARAM_EPSILON) needs_update = true;
    if (ao_color != cached_ao_color) needs_update = true;
    if (std::abs(ao_strength - cached_ao_strength) > PARAM_EPSILON) needs_update = true;
    if (darkness_color != cached_darkness_color) needs_update = true;
    if (std::abs(mipmap_bias - cached_mipmap_bias) > PARAM_EPSILON) needs_update = true;

    // Fog parameters
    const float fog_begin = fog_controller.get_fog_begin();
    const float fog_end = fog_controller.get_fog_end();
    // With the far mode on, the world's terrain is fogged over the same range as
    // the field that continues it, so the world's own border is not the one place
    // the fog disagrees. Off, the world keeps the range its render distance implies.
    const float world_fog_begin = lod_grid_fog_active ? lod_grid_fog_begin : fog_begin;
    const float world_fog_end = lod_grid_fog_active ? lod_grid_fog_end : fog_end;
    const godot::Color fog_color = fog_controller.get_fog_color(blend, godot::Color(sky_horizon_color.x, sky_horizon_color.y, sky_horizon_color.z), elevation, sun_color, sky_turbidity);
    const float fog_scatter = fog_controller.get_fog_scatter(blend, elevation);
    const int32_t fog_mode = static_cast<int32_t>(fog_controller.get_fog_mode());

    if (std::abs(fog_begin - cached_fog_begin) > PARAM_EPSILON) needs_update = true;
    if (std::abs(fog_end - cached_fog_end) > PARAM_EPSILON) needs_update = true;
    if (fog_color != cached_fog_color) needs_update = true;
    if (std::abs(fog_scatter - cached_fog_scatter) > PARAM_EPSILON) needs_update = true;
    if (fog_mode != cached_fog_mode) needs_update = true;
    if (std::abs(lod_grid_fog_begin - cached_lod_grid_fog_begin) > PARAM_EPSILON) needs_update = true;
    if (std::abs(lod_grid_fog_end - cached_lod_grid_fog_end) > PARAM_EPSILON) needs_update = true;
    // The gate compares what is pushed, so the world's own range is what is cached.
    if (std::abs(world_fog_begin - cached_fog_begin) > PARAM_EPSILON) needs_update = true;
    if (std::abs(world_fog_end - cached_fog_end) > PARAM_EPSILON) needs_update = true;

    if (!needs_update) return;

    // Update cached values
    cached_blend = blend;
    cached_sky_color = sky_color;
    cached_sky_warmth = sky_warmth;
    cached_sun_dir = sun_dir;
    cached_contrast = contrast;
    cached_saturation = saturation;
    cached_ao_color = ao_color;
    cached_ao_strength = ao_strength;
    cached_darkness_color = darkness_color;
    cached_mipmap_bias = mipmap_bias;
    cached_fog_begin = world_fog_begin;
    cached_fog_end = world_fog_end;
    cached_fog_color = fog_color;
    cached_fog_scatter = fog_scatter;
    cached_fog_mode = fog_mode;
    cached_lod_grid_fog_begin = lod_grid_fog_begin;
    cached_lod_grid_fog_end = lod_grid_fog_end;

    material_manager.update_shader_parameters(sky_intensity, sky_color, sun_dir, sky_warmth, sky_horizon_color, sky_zenith_color, sky_turbidity);
    material_manager.update_color_parameters(contrast, saturation, ao_color, ao_strength, darkness_color);
    material_manager.set_mipmap_bias(mipmaps_enabled ? mipmap_bias : 0.0f);

    material_manager.update_fog_parameters(world_fog_begin, world_fog_end, fog_color,
                                           fog_controller.get_shader_fog_density(), 0.012f, 200.0f, fog_color,
                                           0.35f, fog_scatter, sun_color,
                                           fog_mode);

    // The far mode is lit and faded by the same instant as everything else, and
    // pushed here so the two can never disagree about the time of day.
    apply_lod_grid_lighting(sky_intensity, sky_color, sky_warmth, fog_color);
}

} // namespace VoxelEngine
