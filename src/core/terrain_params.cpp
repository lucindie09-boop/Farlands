#include "core/terrain_params.hpp"
#include "core/json_config.hpp"

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#endif

namespace VoxelEngine {

bool TerrainParams::load_from_json(const godot::String& json_path) noexcept {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    std::optional<godot::Dictionary> root_opt = read_json_dictionary(json_path);
    if (!root_opt.has_value()) {
        return false;
    }
    const godot::Dictionary& root = *root_opt;

    if (root.has("height_base_y")) {
        height_base_y = static_cast<float>(static_cast<double>(root["height_base_y"]));
    }
    if (root.has("macro_warp_amp_x1")) {
        macro_warp_amp_x1 = static_cast<float>(static_cast<double>(root["macro_warp_amp_x1"]));
    }
    if (root.has("macro_warp_amp_z1")) {
        macro_warp_amp_z1 = static_cast<float>(static_cast<double>(root["macro_warp_amp_z1"]));
    }
    if (root.has("macro_warp_amp_x2")) {
        macro_warp_amp_x2 = static_cast<float>(static_cast<double>(root["macro_warp_amp_x2"]));
    }
    if (root.has("macro_warp_amp_z2")) {
        macro_warp_amp_z2 = static_cast<float>(static_cast<double>(root["macro_warp_amp_z2"]));
    }
    if (root.has("shape_warp_amp_x")) {
        shape_warp_amp_x = static_cast<float>(static_cast<double>(root["shape_warp_amp_x"]));
    }
    if (root.has("shape_warp_amp_z")) {
        shape_warp_amp_z = static_cast<float>(static_cast<double>(root["shape_warp_amp_z"]));
    }
    if (root.has("mid_lattice_spacing")) {
        mid_lattice_spacing = static_cast<float>(static_cast<double>(root["mid_lattice_spacing"]));
    }
    if (root.has("mid_frequency")) {
        mid_frequency = static_cast<float>(static_cast<double>(root["mid_frequency"]));
    }
    if (root.has("mid_amplitude")) {
        mid_amplitude = static_cast<float>(static_cast<double>(root["mid_amplitude"]));
    }
    if (root.has("small_lattice_spacing")) {
        small_lattice_spacing = static_cast<float>(static_cast<double>(root["small_lattice_spacing"]));
    }
    if (root.has("small_frequency")) {
        small_frequency = static_cast<float>(static_cast<double>(root["small_frequency"]));
    }
    if (root.has("small_amplitude")) {
        small_amplitude = static_cast<float>(static_cast<double>(root["small_amplitude"]));
    }
    if (root.has("shape_strength_min")) {
        shape_strength_min = static_cast<float>(static_cast<double>(root["shape_strength_min"]));
    }
    if (root.has("shape_strength_max")) {
        shape_strength_max = static_cast<float>(static_cast<double>(root["shape_strength_max"]));
    }
    if (root.has("weirdness_scale")) {
        weirdness_scale = static_cast<float>(static_cast<double>(root["weirdness_scale"]));
    }
    if (root.has("weirdness_low")) {
        weirdness_low = static_cast<float>(static_cast<double>(root["weirdness_low"]));
    }
    if (root.has("weirdness_high")) {
        weirdness_high = static_cast<float>(static_cast<double>(root["weirdness_high"]));
    }
    if (root.has("climate_temp_base_scale")) {
        climate_temp_base_scale = static_cast<float>(static_cast<double>(root["climate_temp_base_scale"]));
    }
    if (root.has("climate_humidity_base_scale")) {
        climate_humidity_base_scale = static_cast<float>(static_cast<double>(root["climate_humidity_base_scale"]));
    }

    return true;
#else
    (void)json_path;
    return false;
#endif
}

} // namespace VoxelEngine