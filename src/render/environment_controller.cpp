#include "render/environment_controller.hpp"
#include "world/chunk_world.hpp"
#include "lighting/light_propagator.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagation.hpp"
#include <algorithm>

namespace VoxelEngine {

void EnvironmentController::update(double delta, double runtime_elapsed, const godot::Vector3& player_pos,
                                   ChunkWorld& cw, LightPropagator& lp, MeshManager& mm,
                                   double initial_loading_duration) {
    day_night.update(delta);
    update_shader_parameters();

    material_manager.update_player_light(
        player_pos,
        8.0f,
        player_light.get_enabled() ? player_light.get_level() / 15.0f : 0.0f,
        godot::Color(1.0f, 0.9f, 0.7f)
    );
}

void EnvironmentController::update_environment(godot::Node* parent) {
    if (!parent) return;
    if (parent != cached_parent || !cached_world_env) {
        cached_parent = parent;
        cached_world_env = godot::Object::cast_to<godot::WorldEnvironment>(
            parent->get_node_or_null(godot::NodePath("WorldEnvironment"))
        );
        cached_sun_light = godot::Object::cast_to<godot::DirectionalLight3D>(
            parent->get_node_or_null(godot::NodePath("SunLight"))
        );
    }
    if (!cached_world_env) return;
    godot::Ref<godot::Environment> env = cached_world_env->get_environment();
    if (!env.is_valid()) return;

    const float blend = day_night.get_blend();
    const float elevation = day_night.get_sun_elevation();
    const godot::Color horizon_color = day_night.get_horizon_color();
    const godot::Color sun_color = day_night.get_sun_color();
    const godot::Vector3 sun_dir = day_night.get_sun_direction();

    sky_controller.update(env.ptr(), blend, static_cast<float>(day_night.get_raw_time()),
                          sun_color, sun_dir,
                          day_night.get_moon_phase(), 1.0f, day_night.get_sky_turbidity(), 1.0f,
                          fog_controller.get_fog_scatter(blend, elevation));
    fog_controller.update(env.ptr(), blend, horizon_color, fog_controller.get_fog_color(blend, horizon_color, elevation, sun_color, day_night.get_sky_turbidity()), fog_controller.get_fog_scatter(blend, elevation));

    env->set_ambient_source(godot::Environment::AMBIENT_SOURCE_SKY);
    env->set_ambient_light_color(day_night.get_ambient_color());
    env->set_ambient_light_energy(day_night.get_ambient_intensity());

    if (cached_sun_light) {
        godot::Vector3 light_pos = cached_sun_light->get_global_position();
        cached_sun_light->look_at(light_pos - sun_dir, godot::Vector3(0, 0, 1));

        float sun_visible = std::clamp((elevation + 0.08f) / 0.16f, 0.0f, 1.0f);
        float moon_visible = (1.0f - sun_visible) * (1.0f - blend);

        if (sun_visible > 0.0f) {
            cached_sun_light->set_color(sun_color);
            cached_sun_light->set_param(godot::Light3D::PARAM_ENERGY, 3.0f * sun_visible * day_night.get_day_intensity());
            cached_sun_light->set_shadow(false);
            cached_sun_light->set_sky_mode(godot::DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else if (moon_visible > 0.0f) {
            cached_sun_light->set_color(godot::Color(1.0f, 1.0f, 1.0f));
            cached_sun_light->set_param(godot::Light3D::PARAM_ENERGY, 0.25f * moon_visible * day_night.get_night_intensity());
            cached_sun_light->set_shadow(false);
            cached_sun_light->set_sky_mode(godot::DirectionalLight3D::SKY_MODE_LIGHT_ONLY);
        } else {
            cached_sun_light->set_param(godot::Light3D::PARAM_ENERGY, 0.0f);
            cached_sun_light->set_shadow(false);
        }
    }
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

void EnvironmentController::update_shader_parameters() {
    const float blend = day_night.get_blend();
    const float sky_intensity = day_night.get_sky_intensity();
    const godot::Color sky_color = day_night.get_sky_color();
    const godot::Vector3 sun_dir = day_night.get_sun_direction();

    const float elevation = day_night.get_sun_elevation();
    const godot::Color sun_color = day_night.get_sun_color();
    const godot::Color sky_warmth = sun_color;
    const float sky_turbidity = day_night.get_sky_turbidity();

    const godot::Vector3 sky_horizon_color = sky_controller.get_horizon_color(blend, elevation, sun_color, sky_turbidity);
    const godot::Vector3 sky_zenith_color = sky_controller.get_zenith_color(blend);

    // Check if any values changed meaningfully (epsilon-gated dirty check)
    bool needs_update = false;
    
    if (std::abs(blend - cached_blend) > PARAM_EPSILON) needs_update = true;
    if (sky_color != cached_sky_color) needs_update = true;
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
    const godot::Color fog_color = fog_controller.get_fog_color(blend, godot::Color(sky_horizon_color.x, sky_horizon_color.y, sky_horizon_color.z), elevation, sun_color, sky_turbidity);
    const float fog_scatter = fog_controller.get_fog_scatter(blend, elevation);
    const int32_t fog_mode = static_cast<int32_t>(fog_controller.get_fog_mode());

    if (std::abs(fog_begin - cached_fog_begin) > PARAM_EPSILON) needs_update = true;
    if (std::abs(fog_end - cached_fog_end) > PARAM_EPSILON) needs_update = true;
    if (fog_color != cached_fog_color) needs_update = true;
    if (std::abs(fog_scatter - cached_fog_scatter) > PARAM_EPSILON) needs_update = true;
    if (fog_mode != cached_fog_mode) needs_update = true;

    if (!needs_update) return;

    // Update cached values
    cached_blend = blend;
    cached_sky_color = sky_color;
    cached_sun_dir = sun_dir;
    cached_contrast = contrast;
    cached_saturation = saturation;
    cached_ao_color = ao_color;
    cached_ao_strength = ao_strength;
    cached_darkness_color = darkness_color;
    cached_mipmap_bias = mipmap_bias;
    cached_fog_begin = fog_begin;
    cached_fog_end = fog_end;
    cached_fog_color = fog_color;
    cached_fog_scatter = fog_scatter;
    cached_fog_mode = fog_mode;

    material_manager.update_shader_parameters(sky_intensity, sky_color, sun_dir, sky_warmth, sky_horizon_color, sky_zenith_color, sky_turbidity);
    material_manager.update_color_parameters(contrast, saturation, ao_color, ao_strength, darkness_color);
    material_manager.set_mipmap_bias(mipmaps_enabled ? mipmap_bias : 0.0f);

    material_manager.update_fog_parameters(fog_begin, fog_end, fog_color,
                                           fog_controller.get_shader_fog_density(), 0.012f, 200.0f, fog_color,
                                           fog_scatter, sun_color,
                                           fog_mode);
}

void EnvironmentController::update_player_light(const godot::Vector3& player_pos, double runtime_elapsed,
                                                 ChunkWorld& cw, LightPropagator& lp, MeshManager& mm,
                                                 double initial_loading_duration) {
    player_light.update(
        player_pos,
        runtime_elapsed,
        initial_loading_duration,
        cw.get_chunk_map(),
        [&lp](int32_t cx, int32_t cy, int32_t cz, std::vector<LightNode>& remove, std::vector<LightNode>& add) {
            lp.light_propagate_remove_locked(cx, cy, cz, remove, add);
        },
        [&lp](int32_t cx, int32_t cy, int32_t cz, std::vector<LightNode>& add) {
            lp.light_propagate_add_locked(cx, cy, cz, add);
        },
        [&mm](int32_t cx, int32_t cy, int32_t cz) { mm.mark_chunks_dirty_for_light(cx, cy, cz); }
    );
}

} // namespace VoxelEngine
