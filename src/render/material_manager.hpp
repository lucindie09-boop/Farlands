#ifndef FUK_MINECRAFT_MATERIAL_MANAGER_HPP
#define FUK_MINECRAFT_MATERIAL_MANAGER_HPP
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace godot {
class ShaderMaterial;
}

namespace VoxelEngine {

class MaterialManager {
public:
    void update_shader_parameters(float sky_intensity, const godot::Color& sky_color, const godot::Vector3& sun_direction = godot::Vector3(0.0f, 1.0f, 0.0f), const godot::Color& sky_light_warmth = godot::Color(1.0f, 1.0f, 1.0f, 1.0f), const godot::Vector3& horizon_color = godot::Vector3(1.0f, 1.0f, 1.0f), const godot::Vector3& zenith_color = godot::Vector3(1.0f, 1.0f, 1.0f), float sky_turbidity = 0.35f);
    void update_fog_parameters(float fog_begin, float fog_end, const godot::Color& fog_color,
                               float fog_density = 0.003f, float height_fog_density = 0.015f,
                               float sea_level = 64.0f, const godot::Color& aerial_color = godot::Color(1.0f, 1.0f, 1.0f, 1.0f),
                               float fog_scatter = 0.0f, const godot::Color& fog_scatter_color = godot::Color(1.0f, 1.0f, 1.0f, 1.0f),
                               int fog_mode = 1);
    void update_color_parameters(float contrast, float saturation, const godot::Color& ao_color, float ao_strength, const godot::Color& darkness_color);
void update_player_light(const godot::Vector3& position, float radius, float intensity, const godot::Color& color);
    godot::Ref<godot::ShaderMaterial> get_material();
    godot::Ref<godot::ShaderMaterial> get_water_material();

private:
    godot::Ref<godot::ShaderMaterial> cached_material;
    godot::Ref<godot::ShaderMaterial> cached_water_material;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_MATERIAL_MANAGER_HPP
