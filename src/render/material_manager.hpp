#ifndef FARLANDS_MATERIAL_MANAGER_HPP
#define FARLANDS_MATERIAL_MANAGER_HPP
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>

namespace godot {
class ShaderMaterial;
}

namespace VoxelEngine {

// Filter/mipmap mode for the block texture arrays. Modes without mipmaps skip
// Image::generate_mipmaps() (single-level arrays) and sample with bias 0.
enum class TextureFilterMode : std::uint8_t {
    Nearest = 0,
    NearestMipmap = 1,            // default
    Linear = 2,
    LinearMipmap = 3,
    LinearMipmapAnisotropic = 4,
};

class MaterialManager {
public:
    void update_shader_parameters(float sky_intensity, const godot::Color& sky_color, const godot::Vector3& sun_direction = godot::Vector3(0.0f, 1.0f, 0.0f), const godot::Color& sky_light_warmth = godot::Color(1.0f, 1.0f, 1.0f, 1.0f), const godot::Vector3& horizon_color = godot::Vector3(1.0f, 1.0f, 1.0f), const godot::Vector3& zenith_color = godot::Vector3(1.0f, 1.0f, 1.0f), float sky_turbidity = 0.35f);
    void update_fog_parameters(float fog_begin, float fog_end, const godot::Color& fog_color,
                               float fog_density = 0.003f, float height_fog_density = 0.015f,
                               float sea_level = 64.0f, const godot::Color& aerial_color = godot::Color(1.0f, 1.0f, 1.0f, 1.0f),
                               float aerial_strength = 0.35f, float fog_scatter = 0.0f, const godot::Color& fog_scatter_color = godot::Color(1.0f, 1.0f, 1.0f, 1.0f),
                               int fog_mode = 1);
    void update_color_parameters(float contrast, float saturation, const godot::Color& ao_color, float ao_strength, const godot::Color& darkness_color);
void update_player_light(const godot::Vector3& position, float radius, float intensity, const godot::Color& color);
    // Texture filter/mipmap mode (see TextureFilterMode). Changing mode
    // regenerates the texture arrays if needed, re-assigns them to the
    // materials, and swaps the sampler filter hint in both shaders.
    void set_texture_filter_mode(int mode);
    int get_texture_filter_mode() const { return texture_filter_mode_; }
    void set_mipmap_bias(float bias);
    float get_mipmap_bias() const { return mipmap_bias_; }
    // Toggles between real block textures and the magenta/black checker
    // placeholder. Regenerates the arrays and re-assigns them to the materials.
    void set_textures_enabled(bool enabled);
    // Toggles GPU compression for texture arrays. Regenerates the arrays
    // and re-assigns them to the materials.
    void set_compression_enabled(bool enabled);
    bool get_compression_enabled() const;
    // Re-assigns the (possibly regenerated) texture arrays to both cached
    // materials. Must be called after TextureArrayGenerator regenerates them.
    void reload_textures();
    godot::Ref<godot::ShaderMaterial> get_material();
    godot::Ref<godot::ShaderMaterial> get_water_material();

private:
    godot::Ref<godot::ShaderMaterial> cached_material;
    godot::Ref<godot::ShaderMaterial> cached_water_material;

    int texture_filter_mode_ = static_cast<int>(TextureFilterMode::NearestMipmap);
    int filter_hint_applied_for_mode_ = static_cast<int>(TextureFilterMode::NearestMipmap);
    float mipmap_bias_ = 0.1f;

    static bool mode_uses_mipmaps(int mode) {
        return mode == static_cast<int>(TextureFilterMode::NearestMipmap) ||
               mode == static_cast<int>(TextureFilterMode::LinearMipmap) ||
               mode == static_cast<int>(TextureFilterMode::LinearMipmapAnisotropic);
    }
    void apply_mipmap_bias();
    void apply_filter_hint_if_needed();
};

} // namespace VoxelEngine

#endif // FARLANDS_MATERIAL_MANAGER_HPP
