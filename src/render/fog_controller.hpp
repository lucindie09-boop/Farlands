#ifndef FARLANDS_FOG_CONTROLLER_HPP
#define FARLANDS_FOG_CONTROLLER_HPP
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/classes/environment.hpp>
#include <algorithm>
#include <cstdint>

namespace VoxelEngine {

#ifndef FARLANDS_SMOOTHSTEP_DEFINED
#define FARLANDS_SMOOTHSTEP_DEFINED
inline float smoothstep(float edge0, float edge1, float x) {
    float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
#endif

class FogController {
public:
    enum class FogMode : std::uint8_t {
        Disabled,
        Edge,
        Linear,
        Exponential
    };
    void update(godot::Environment* env, float blend, const godot::Color& sky_color, const godot::Color& fog_color, float fog_scatter) {
        if (!env) return;
        
        // All fog handling is done in the shader, Godot fog is disabled
        env->set_fog_enabled(false);
    }

    [[nodiscard]] float get_fog_begin() const {
        return 0.0f;
    }

    [[nodiscard]] float get_fog_end() const {
        if (fog_density <= 0.0f) return 0.0f;
        static constexpr float chunk_size = 32.0f;
        // density 0.1 → full fog at rd_blocks - chunk_size  (one chunk before edge)
        // density 1.0 → full fog at chunk_size              (one chunk from camera)
        float t = std::max((fog_density - 0.1f) / 0.9f, 0.0f);
        float end = (render_distance_blocks - chunk_size) - t * (render_distance_blocks - chunk_size - chunk_size);
        return std::max(end, chunk_size);
    }

    [[nodiscard]] float get_fog_density() const {
        return fog_density;
    }

    [[nodiscard]] float get_shader_fog_density() const {
        if (fog_density <= 0.0f) return 0.0f;
        return 0.7f;
    }

    [[nodiscard]] godot::Color get_fog_color(float blend, const godot::Color& horizon_color, float sun_elevation, const godot::Color& sun_color = godot::Color(1.0f, 1.0f, 1.0f), float sky_turbidity = 0.35f) const {
        if (!enabled) return horizon_color;

        godot::Color base_color;
        if (sun_elevation > -0.08f) {
            base_color = horizon_color;
        } else {
            float t = std::clamp((sun_elevation + 0.25f) / 0.17f, 0.0f, 1.0f);
            base_color = fog_color_night.lerp(horizon_color, t);
        }

        // Apply turbidity haze effect to match sky shader
        float haze = std::clamp(sky_turbidity * 1.4f - 0.2f, 0.0f, 1.0f);
        haze *= smoothstep(0.0f, 0.35f, blend);
        godot::Color haze_color = godot::Color(0.95f, 0.92f, 0.88f).lerp(sun_color, 0.25f);
        godot::Color turb_horizon = base_color.lerp(haze_color, haze * 0.55f);

        return turb_horizon;
    }

    [[nodiscard]] float get_fog_scatter(float blend, float sun_elevation) const {
        if (!enabled || fog_density <= 0.0f) return 0.0f;
        float horizon_factor = 1.0f - smoothstep(0.0f, 0.35f, std::abs(sun_elevation));
        return horizon_factor * blend * fog_scatter_intensity * fog_density;
    }

    void set_depth_begin_day(float v) { depth_begin_day = v; }
    void set_depth_end_day(float v) { depth_end_day = v; }
    void set_depth_begin_night(float v) { depth_begin_night = v; }
    void set_depth_end_night(float v) { depth_end_night = v; }
    void set_fog_scatter_intensity(float v) { fog_scatter_intensity = v; }
    void set_fog_density(float v) { fog_density = std::clamp(v, 0.0f, 1.0f); }
    void set_render_distance_blocks(float v) { render_distance_blocks = std::max(v, 16.0f); }
    float get_render_distance_blocks() const { return render_distance_blocks; }

    void set_fog_color_day(const godot::Color& c) { fog_color_day = c; }
    void set_fog_color_night(const godot::Color& c) { fog_color_night = c; }
    void set_fog_color_sunset(const godot::Color& c) { fog_color_sunset = c; }
    void set_fog_color_dawn(const godot::Color& c) { fog_color_dawn = c; }
    void set_enabled(bool v) { enabled = v; }
    [[nodiscard]] bool get_enabled() const { return enabled; }
    void set_fog_mode(FogMode mode) { fog_mode = mode; }
    [[nodiscard]] FogMode get_fog_mode() const { return fog_mode; }

private:
    FogMode fog_mode = FogMode::Edge; // Default to edge (mode 1)
    bool enabled = true;
    float render_distance_blocks = 512.0f;
    float fog_density = 0.3f;
    float depth_begin_night = 96.0f;
    float depth_begin_day = 160.0f;
    float depth_end_night = 1024.0f;
    float depth_end_day = 1536.0f;
    float fog_scatter_intensity = 0.5f;
    godot::Color fog_color_day = godot::Color(0.30f, 0.52f, 0.85f, 1.0f);
    godot::Color fog_color_night = godot::Color(0.025f, 0.045f, 0.090f, 1.0f);
    godot::Color fog_color_sunset = godot::Color(0.95f, 0.70f, 0.40f, 1.0f);
    godot::Color fog_color_dawn = godot::Color(0.95f, 0.70f, 0.40f, 1.0f);
};

} // namespace VoxelEngine

#endif // FARLANDS_FOG_CONTROLLER_HPP
