#ifndef FUK_MINECRAFT_SKY_CONTROLLER_HPP
#define FUK_MINECRAFT_SKY_CONTROLLER_HPP
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <godot_cpp/classes/sky.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <algorithm>

namespace VoxelEngine {

static const char* SKY_SHADER_SRC = R"(
shader_type sky;

uniform float blend = 1.0;
uniform vec3 sun_direction = vec3(0.0, 1.0, 0.0);
uniform vec3 sun_color = vec3(1.0, 0.95, 0.7);
uniform float exposure = 1.0;
uniform vec3 horizon_color = vec3(0.30, 0.52, 0.85);
uniform vec3 zenith_color = vec3(0.06, 0.20, 0.85);
uniform float sky_turbidity = 0.35;
uniform float moon_phase = 0.5;
uniform sampler2D sun_texture;
uniform sampler2D star_texture;

vec3 aces_tonemap(vec3 x) {
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

float hash2(vec2 p) {
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float hash3(vec3 p) {
    return fract(sin(dot(p, vec3(12.9898, 78.233, 45.543))) * 43758.5453);
}

float value_noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    float a = hash2(i);
    float b = hash2(i + vec2(1.0, 0.0));
    float c = hash2(i + vec2(0.0, 1.0));
    float d = hash2(i + vec2(1.0, 1.0));
    vec2 u = f * f * (3.0 - 2.0 * f);
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

void sky() {
    vec3 dir = normalize(EYEDIR);
    float up = max(dir.y, 0.0);

    vec3 sun_fwd = normalize(sun_direction);
    float sun_elevation = sun_fwd.y;
    float sun_dot = max(dot(dir, sun_fwd), 0.0);

    float gradient = 1.0 - (1.0 - up) * sqrt(1.0 - up);

    // Turbidity: Mie haze washes horizon, desaturates zenith
    float haze = clamp(sky_turbidity * 1.4 - 0.2, 0.0, 1.0);
    haze *= smoothstep(0.0, 0.35, blend);
    vec3 haze_color = mix(vec3(0.95, 0.92, 0.88), sun_color, 0.25);
    vec3 turb_horizon = mix(horizon_color, haze_color, haze * 0.55);
    float z_luma = dot(zenith_color, vec3(0.299, 0.587, 0.114));
    vec3 pale_zenith = mix(zenith_color, vec3(z_luma) * vec3(0.75, 0.80, 0.92), haze * 0.60);

    vec3 base_sky = mix(turb_horizon, pale_zenith, gradient);

    // Sun glow widens with turbidity
    float sun_glow_w = mix(1.0, 2.2, haze);
    float sun_glow = (pow(sun_dot, 20.0) * 0.04 + pow(sun_dot, 60.0 / sun_glow_w) * 0.08);
    base_sky += sun_color * sun_glow * smoothstep(-0.08, 0.08, sun_elevation);
    // Sun using texture
    vec3 sun_right = normalize(cross(vec3(0.0, 1.0, 0.0), sun_fwd + vec3(0.001, 0.0, 0.0)));
    vec3 sun_up_vec = cross(sun_fwd, sun_right);
    float sun_vis = smoothstep(-0.08, 0.08, sun_elevation);
    if (dot(dir, sun_fwd) > 0.0 && sun_vis > 0.0) {
        vec2 suv = vec2(dot(dir, sun_right), dot(dir, sun_up_vec)) / dot(dir, sun_fwd);
        vec2 tex_uv = suv / 0.13 + 0.5;
        float edge = 1.0 - smoothstep(0.0, 0.04, min(min(tex_uv.x, 1.0 - tex_uv.x), min(tex_uv.y, 1.0 - tex_uv.y)));
        float in_range = float(tex_uv.x >= -0.01 && tex_uv.x <= 1.01 && tex_uv.y >= -0.01 && tex_uv.y <= 1.01);
        vec4 texel = texture(sun_texture, tex_uv);
        float alpha = texel.a * (1.0 - edge * 0.4) * in_range;
        vec3 sun_tex_col = texel.rgb * vec3(1.0, 0.5, 0.08) * 2.0;
        base_sky += (sun_tex_col * alpha + exp(-length(suv) * 5.0) * 0.04) * sun_vis;
    }

    // Moon using texture
    vec3 moon_dir = -sun_fwd;
    float moon_dot = max(dot(dir, moon_dir), 0.0);
    float moon_vis = smoothstep(-0.08, 0.08, moon_dir.y);
    float moon_dim = 1.0 - blend * 0.75;
    if (moon_dot > 0.0 && moon_vis > 0.0) {
        vec2 muv = vec2(-dot(dir, sun_right), dot(dir, sun_up_vec)) / dot(dir, moon_dir);
        vec2 tex_uv = muv / 0.13 + 0.5;
        float edge = 1.0 - smoothstep(0.0, 0.04, min(min(tex_uv.x, 1.0 - tex_uv.x), min(tex_uv.y, 1.0 - tex_uv.y)));
        float in_range = float(tex_uv.x >= -0.01 && tex_uv.x <= 1.01 && tex_uv.y >= -0.01 && tex_uv.y <= 1.01);
        vec4 texel = texture(sun_texture, tex_uv);
        float alpha = texel.a * (1.0 - edge * 0.4) * in_range;
        vec3 moon_tint = vec3(0.5, 0.65, 1.0);
        float halo = exp(-length(muv) * 5.0) * 0.02;
        base_sky += (texel.rgb * moon_tint * alpha * 1.5 + halo) * moon_vis * moon_dim;
    }

    // Stars
    float star_vis = 1.0 - smoothstep(0.3, 0.8, blend);
    if (star_vis > 0.001 && dir.y > -0.05) {
        vec3 cell = floor(dir * 100.0);
        float h = hash3(cell);
        if (h > 0.997) {
            vec3 sr = vec3(
                fract(sin(dot(cell + vec3(1.0, 0.0, 0.0), vec3(12.9898, 78.233, 45.543))) * 43758.5453),
                fract(sin(dot(cell + vec3(0.0, 1.0, 0.0), vec3(43.123, 21.789, 98.765))) * 43758.5453),
                fract(sin(dot(cell + vec3(0.0, 0.0, 1.0), vec3(65.432, 34.567, 12.345))) * 43758.5453)
            ) - 0.5;
            vec3 sw = normalize(cell + sr);
            float ss = 0.002 + h * 0.003;
            if (length(dir - sw) < ss) {
                vec3 up_ref = abs(sw.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
                vec3 sri = normalize(cross(sw, up_ref));
                vec3 sup = cross(sri, sw);
                vec2 tev = vec2(dot(dir, sri), dot(dir, sup)) / ss + 0.5;
                float ir = float(tev.x >= -0.01 && tev.x <= 1.01 && tev.y >= -0.01 && tev.y <= 1.01);
                vec4 tex = texture(star_texture, tev);
                float a = tex.a * ir;
                if (a > 0.01) {
                    float t = 0.85 + 0.15 * sin(TIME * 2.5 + h * 1000.0);
                    base_sky += tex.rgb * a * t * star_vis * 4.0;
                }
            }
        }
    }
    // North star
    if (star_vis > 0.001 && dir.y > -0.05) {
        vec3 ns = normalize(vec3(0.0, 0.6, -0.8));
        float sd = length(dir - ns);
        if (sd < 0.011) {
            vec3 up_ref = abs(ns.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
            vec3 sri = normalize(cross(ns, up_ref));
            vec3 sup = cross(sri, ns);
            vec2 tev = vec2(dot(dir, sri), dot(dir, sup)) / 0.011 + 0.5;
            float ir = float(tev.x >= -0.01 && tev.x <= 1.01 && tev.y >= -0.01 && tev.y <= 1.01);
            vec4 tex = texture(star_texture, tev);
            float a = tex.a * ir;
            if (a > 0.01) {
                base_sky += tex.rgb * a * star_vis * 8.0;
            }
        }
    }

    base_sky = aces_tonemap(base_sky * exposure);

    COLOR = max(base_sky, vec3(0.0));
}
)";

class SkyController {
private:
    godot::Vector3 compute_horizon_color(float blend, float sun_elevation, const godot::Color& sun_color, float sky_turbidity) const {
        godot::Vector3 day_horizon(0.30f, 0.52f, 0.85f);
        godot::Vector3 night_horizon(0.025f, 0.045f, 0.090f);

        float sunset_factor = std::clamp(std::abs(sun_elevation) / 0.35f, 0.0f, 1.0f);
        godot::Vector3 sunset_horizon(0.95f, 0.70f, 0.40f);

        godot::Vector3 base_color = night_horizon.lerp(day_horizon, blend);
        godot::Vector3 horizon_color = base_color.lerp(sunset_horizon, (1.0f - sunset_factor) * 0.5f * blend);

        // Apply turbidity haze effect to match sky shader
        float haze = std::clamp(sky_turbidity * 1.4f - 0.2f, 0.0f, 1.0f);
        haze *= smoothstep(0.0f, 0.35f, blend);
        godot::Vector3 haze_color = godot::Vector3(0.95f, 0.92f, 0.88f).lerp(godot::Vector3(sun_color.r, sun_color.g, sun_color.b), 0.25f);
        godot::Vector3 turb_horizon = horizon_color.lerp(haze_color, haze * 0.55f);

        return turb_horizon;
    }

    godot::Vector3 compute_zenith_color(float blend) const {
        godot::Vector3 day_zenith(0.06f, 0.20f, 0.85f);
        godot::Vector3 night_zenith(0.001f, 0.004f, 0.015f);
        return night_zenith.lerp(day_zenith, blend);
    }

public:
    [[nodiscard]] godot::Vector3 get_horizon_color(float blend, float sun_elevation = 0.0f, const godot::Color& sun_color = godot::Color(1.0f, 1.0f, 1.0f), float sky_turbidity = 0.35f) const {
        return compute_horizon_color(blend, sun_elevation, sun_color, sky_turbidity);
    }

    [[nodiscard]] godot::Vector3 get_zenith_color(float blend) const {
        return compute_zenith_color(blend);
    }

    void ensure_sky(godot::Environment* env) {
        godot::Ref<godot::Sky> sky_res = env->get_sky();
        if (!sky_res.is_valid()) {
            sky_res.instantiate();
            env->set_sky(sky_res);
        }
        if (!cached_mat.is_valid()) {
            godot::Ref<godot::Material> base_mat = sky_res->get_material();
            if (base_mat.is_valid()) {
                cached_mat = base_mat;
            }
        }
        if (!cached_mat.is_valid()) {
            cached_mat.instantiate();
            godot::Ref<godot::Shader> shader;
            shader.instantiate();
            shader->set_code(SKY_SHADER_SRC);
            cached_mat->set_shader(shader);
            sky_res->set_material(cached_mat);
        }
        if (!sun_texture_loaded) {
            godot::Ref<godot::Texture2D> tex = godot::ResourceLoader::get_singleton()->load("res://textures/atmosphere/sun.png");
            if (tex.is_valid()) {
                cached_mat->set_shader_parameter("sun_texture", tex);
            }
            sun_texture_loaded = true;
        }
        if (!star_texture_loaded) {
            godot::Ref<godot::Texture2D> tex = godot::ResourceLoader::get_singleton()->load("res://textures/atmosphere/north_star.png");
            if (tex.is_valid()) {
                cached_mat->set_shader_parameter("star_texture", tex);
            }
            star_texture_loaded = true;
        }
    }

    void update(godot::Environment* env, float blend, float time, const godot::Color& sun_color, const godot::Vector3& sun_dir, float moon_phase, float exposure, float sky_turbidity = 0.35f, float aurora_intensity = 1.0f, float fog_scatter = 0.0f) {
        if (!env) return;
        if (!sky_ready) {
            ensure_sky(env);
            env->set_background(godot::Environment::BG_SKY);
            sky_ready = true;
        }
        if (!cached_mat.is_valid()) return;

        cached_mat->set_shader_parameter(p_blend, blend);
        cached_mat->set_shader_parameter(p_sun_dir, sun_dir);
        cached_mat->set_shader_parameter(p_sun_color, godot::Vector3(sun_color.r, sun_color.g, sun_color.b));
        cached_mat->set_shader_parameter(p_exposure, exposure);
        cached_mat->set_shader_parameter(p_moon_phase, moon_phase);

        float sun_elevation = sun_dir.y;
        godot::Vector3 horizon_color = compute_horizon_color(blend, sun_elevation, sun_color, sky_turbidity);
        godot::Vector3 zenith_color = compute_zenith_color(blend);
        cached_mat->set_shader_parameter(p_horizon_color, horizon_color);
        cached_mat->set_shader_parameter(p_zenith_color, zenith_color);
        cached_mat->set_shader_parameter("sky_turbidity", sky_turbidity);
    }

private:
    bool sky_ready = false;
    bool sun_texture_loaded = false;
    bool star_texture_loaded = false;
    godot::Ref<godot::ShaderMaterial> cached_mat;
    godot::StringName p_blend = godot::StringName("blend");
    godot::StringName p_sun_dir = godot::StringName("sun_direction");
    godot::StringName p_sun_color = godot::StringName("sun_color");
    godot::StringName p_exposure = godot::StringName("exposure");
    godot::StringName p_moon_phase = godot::StringName("moon_phase");
    godot::StringName p_horizon_color = godot::StringName("horizon_color");
    godot::StringName p_zenith_color = godot::StringName("zenith_color");
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_SKY_CONTROLLER_HPP