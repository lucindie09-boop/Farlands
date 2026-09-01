#include "render/material_manager.hpp"

#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include "render/texture_array_generator.hpp"

using namespace godot;
using namespace VoxelEngine;

namespace {

const char* filter_hint_for_mode(int mode) {
    switch (mode) {
        case static_cast<int>(TextureFilterMode::Nearest): return "filter_nearest";
        case static_cast<int>(TextureFilterMode::Linear): return "filter_linear";
        case static_cast<int>(TextureFilterMode::LinearMipmap): return "filter_linear_mipmap";
        case static_cast<int>(TextureFilterMode::LinearMipmapAnisotropic): return "filter_linear_mipmap_anisotropic";
        case static_cast<int>(TextureFilterMode::NearestMipmap):
        default: return "filter_nearest_mipmap";
    }
}

void apply_filter_hint_to_material(const Ref<ShaderMaterial>& material, const String& hint, const String& old_hint) {
    if (!material.is_valid()) return;
    Ref<Shader> shader = material->get_shader();
    if (!shader.is_valid()) return;
    String code = shader->get_code();
    String new_code = code.replace(": filter_" + old_hint, ": filter_" + hint);
    if (new_code != code) {
        shader->set_code(new_code);
    }
}

} // namespace

void MaterialManager::update_shader_parameters(float sky_intensity, const Color& sky_color, const Vector3& sun_direction, const Color& sky_light_warmth, const Vector3& horizon_color, const Vector3& zenith_color, float sky_turbidity) {
    Ref<ShaderMaterial> material = get_material();
    if (material.is_valid()) {
        material->set_shader_parameter("sky_light_intensity", sky_intensity);
        material->set_shader_parameter("sky_light_color", sky_color);
        material->set_shader_parameter("sun_direction", sun_direction);
        material->set_shader_parameter("sky_light_warmth", sky_light_warmth);
        material->set_shader_parameter("horizon_color", horizon_color);
        material->set_shader_parameter("zenith_color", zenith_color);
        material->set_shader_parameter("sky_turbidity", sky_turbidity);
    }
    Ref<ShaderMaterial> water_mat = get_water_material();
    if (water_mat.is_valid()) {
        water_mat->set_shader_parameter("sky_light_intensity", sky_intensity);
        water_mat->set_shader_parameter("sky_light_color", sky_color);
        water_mat->set_shader_parameter("sun_direction", sun_direction);
        water_mat->set_shader_parameter("sky_light_warmth", sky_light_warmth);
        water_mat->set_shader_parameter("horizon_color", horizon_color);
        water_mat->set_shader_parameter("zenith_color", zenith_color);
        water_mat->set_shader_parameter("sky_turbidity", sky_turbidity);
    }
}

void MaterialManager::update_fog_parameters(float fog_begin, float fog_end, const Color& fog_color,
                                            float fog_density, float height_fog_density, float sea_level,
                                            const Color& aerial_color, float aerial_strength, float fog_scatter, const Color& fog_scatter_color,
                                            int fog_mode) {
    Ref<ShaderMaterial> material = get_material();
    if (material.is_valid()) {
        material->set_shader_parameter("fog_begin", fog_begin);
        material->set_shader_parameter("fog_end", fog_end);
        material->set_shader_parameter("fog_density", fog_density);
        material->set_shader_parameter("height_fog_density", height_fog_density);
        material->set_shader_parameter("sea_level", sea_level);
        material->set_shader_parameter("fog_scatter", fog_scatter);
        material->set_shader_parameter("fog_scatter_color", fog_scatter_color);
        material->set_shader_parameter("fog_mode", fog_mode);
        material->set_shader_parameter("fog_color", Vector3(fog_color.r, fog_color.g, fog_color.b));
        material->set_shader_parameter("aerial_color", Vector3(aerial_color.r, aerial_color.g, aerial_color.b));
        material->set_shader_parameter("aerial_strength", aerial_strength);
    }
    Ref<ShaderMaterial> water_mat = get_water_material();
    if (water_mat.is_valid()) {
        water_mat->set_shader_parameter("fog_begin", fog_begin);
        water_mat->set_shader_parameter("fog_end", fog_end);
        water_mat->set_shader_parameter("fog_density", fog_density);
        water_mat->set_shader_parameter("height_fog_density", height_fog_density);
        water_mat->set_shader_parameter("sea_level", sea_level);
        water_mat->set_shader_parameter("fog_scatter", fog_scatter);
        water_mat->set_shader_parameter("fog_scatter_color", fog_scatter_color);
        water_mat->set_shader_parameter("fog_mode", fog_mode);
        water_mat->set_shader_parameter("fog_color", Vector3(fog_color.r, fog_color.g, fog_color.b));
        water_mat->set_shader_parameter("aerial_color", Vector3(aerial_color.r, aerial_color.g, aerial_color.b));
        water_mat->set_shader_parameter("aerial_strength", aerial_strength);
    }
}

void MaterialManager::update_color_parameters(float contrast, float saturation, const Color& ao_color, float ao_strength, const Color& darkness_color) {
    Ref<ShaderMaterial> material = get_material();
    if (material.is_valid()) {
        material->set_shader_parameter("contrast", contrast);
        material->set_shader_parameter("saturation", saturation);
        material->set_shader_parameter("ao_color", Vector3(ao_color.r, ao_color.g, ao_color.b));
        material->set_shader_parameter("ao_strength", ao_strength);
        material->set_shader_parameter("darkness_color", Vector3(darkness_color.r, darkness_color.g, darkness_color.b));
    }
    Ref<ShaderMaterial> water_mat = get_water_material();
    if (water_mat.is_valid()) {
        water_mat->set_shader_parameter("contrast", contrast);
        water_mat->set_shader_parameter("saturation", saturation);
        water_mat->set_shader_parameter("ao_color", Vector3(ao_color.r, ao_color.g, ao_color.b));
        water_mat->set_shader_parameter("ao_strength", ao_strength);
        water_mat->set_shader_parameter("darkness_color", Vector3(darkness_color.r, darkness_color.g, darkness_color.b));
    }
}

void MaterialManager::update_player_light(const Vector3& position, float radius, float intensity, const Color& color) {
Ref<ShaderMaterial> material = get_material();
if (material.is_valid()) {
material->set_shader_parameter("player_light_position", position);
material->set_shader_parameter("player_light_radius", radius);
material->set_shader_parameter("player_light_intensity", intensity);
material->set_shader_parameter("player_light_color", color);
}
}

void MaterialManager::set_mipmap_bias(float bias) {
    mipmap_bias_ = bias;
    apply_mipmap_bias();
}

void MaterialManager::set_texture_filter_mode(int mode) {
    if (mode < static_cast<int>(TextureFilterMode::Nearest) || mode > static_cast<int>(TextureFilterMode::LinearMipmapAnisotropic)) {
        mode = static_cast<int>(TextureFilterMode::NearestMipmap);
    }
    if (texture_filter_mode_ == mode) return;
    texture_filter_mode_ = mode;
    TextureArrayGenerator::get_instance().set_mipmaps_enabled(mode_uses_mipmaps(mode));
    reload_textures();
    apply_filter_hint_if_needed();
    apply_mipmap_bias();
}

void MaterialManager::apply_mipmap_bias() {
    const float bias = TextureArrayGenerator::is_mipmaps_enabled() ? mipmap_bias_ : 0.0f;
    Ref<ShaderMaterial> material = get_material();
    if (material.is_valid()) {
        material->set_shader_parameter("mipmap_bias", bias);
    }
    Ref<ShaderMaterial> water_mat = get_water_material();
    if (water_mat.is_valid()) {
        water_mat->set_shader_parameter("mipmap_bias", bias);
    }
}

void MaterialManager::apply_filter_hint_if_needed() {
    if (filter_hint_applied_for_mode_ == texture_filter_mode_) return;
    const String hint = filter_hint_for_mode(texture_filter_mode_);
    const String old_hint = filter_hint_for_mode(filter_hint_applied_for_mode_);
    apply_filter_hint_to_material(get_material(), hint, old_hint);
    apply_filter_hint_to_material(get_water_material(), hint, old_hint);
    filter_hint_applied_for_mode_ = texture_filter_mode_;
}

void MaterialManager::set_textures_enabled(bool enabled) {
    TextureArrayGenerator::get_instance().set_textures_enabled(enabled);
    reload_textures();
}

void MaterialManager::set_compression_enabled(bool enabled) {
    TextureArrayGenerator::get_instance().set_compression_enabled(enabled);
    reload_textures();
}

bool MaterialManager::get_compression_enabled() const {
    return TextureArrayGenerator::is_compression_enabled();
}

void MaterialManager::reload_textures() {
    Ref<Texture2DArray> texture_array = TextureArrayGenerator::get_instance().get_texture_array();
    Ref<Texture2DArray> emissive_array = TextureArrayGenerator::get_instance().get_emissive_texture_array();
    if (cached_material.is_valid()) {
        if (texture_array.is_valid()) {
            cached_material->set_shader_parameter("texture_array", texture_array);
        }
        if (emissive_array.is_valid()) {
            cached_material->set_shader_parameter("emissive_array", emissive_array);
        }
    }
    if (cached_water_material.is_valid()) {
        if (texture_array.is_valid()) {
            cached_water_material->set_shader_parameter("texture_array", texture_array);
        }
        if (emissive_array.is_valid()) {
            cached_water_material->set_shader_parameter("emissive_array", emissive_array);
        }
    }
}

Ref<ShaderMaterial> MaterialManager::get_material() {
    if (!cached_material.is_valid()) {
        ResourceLoader* loader = ResourceLoader::get_singleton();
        cached_material = loader->load("res://materials/voxel_material.tres");
        if (!cached_material.is_valid()) {
            ERR_PRINT("Failed to load voxel_material.tres");
            return cached_material;
        }
        Ref<Texture2DArray> texture_array = TextureArrayGenerator::get_instance().get_texture_array();
        if (texture_array.is_valid()) {
            cached_material->set_shader_parameter("texture_array", texture_array);
        }
        Ref<Texture2DArray> emissive_array = TextureArrayGenerator::get_instance().get_emissive_texture_array();
        if (emissive_array.is_valid()) {
            cached_material->set_shader_parameter("emissive_array", emissive_array);
        }
    }
    return cached_material;
}

Ref<ShaderMaterial> MaterialManager::get_water_material() {
    if (!cached_water_material.is_valid()) {
        ResourceLoader* loader = ResourceLoader::get_singleton();
        cached_water_material = loader->load("res://materials/voxel_material_water.tres");
        if (!cached_water_material.is_valid()) {
            ERR_PRINT("Failed to load voxel_material_water.tres");
            return cached_water_material;
        }
        Ref<Texture2DArray> texture_array = TextureArrayGenerator::get_instance().get_texture_array();
        if (texture_array.is_valid()) {
            cached_water_material->set_shader_parameter("texture_array", texture_array);
        }
        Ref<Texture2DArray> emissive_array = TextureArrayGenerator::get_instance().get_emissive_texture_array();
        if (emissive_array.is_valid()) {
            cached_water_material->set_shader_parameter("emissive_array", emissive_array);
        }
    }
    return cached_water_material;
}
