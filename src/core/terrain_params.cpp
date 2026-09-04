#include "core/terrain_params.hpp"
#include "core/json_config.hpp"

#include <algorithm>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
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
    if (root.has("elevation_scale")) {
        elevation_scale = static_cast<float>(static_cast<double>(root["elevation_scale"]));
    }
    if (root.has("elevation_amplitude")) {
        elevation_amplitude = static_cast<float>(static_cast<double>(root["elevation_amplitude"]));
    }
    if (root.has("elevation_bias")) {
        elevation_bias = static_cast<float>(static_cast<double>(root["elevation_bias"]));
    }
    if (root.has("surface_jitter_scale")) {
        surface_jitter_scale = static_cast<float>(static_cast<double>(root["surface_jitter_scale"]));
    }
    if (root.has("surface_jitter_amplitude")) {
        surface_jitter_amplitude = static_cast<float>(static_cast<double>(root["surface_jitter_amplitude"]));
    }
    if (root.has("roughness_scale")) {
        roughness_scale = static_cast<float>(static_cast<double>(root["roughness_scale"]));
    }
    if (root.has("roughness_min")) {
        roughness_min = static_cast<float>(static_cast<double>(root["roughness_min"]));
    }
    if (root.has("roughness_max")) {
        roughness_max = static_cast<float>(static_cast<double>(root["roughness_max"]));
    }
    if (root.has("roughness_detail_min")) {
        roughness_detail_min = static_cast<float>(static_cast<double>(root["roughness_detail_min"]));
    }
    if (root.has("climate_temp_base_scale")) {
        climate_temp_base_scale = static_cast<float>(static_cast<double>(root["climate_temp_base_scale"]));
    }
    if (root.has("climate_humidity_base_scale")) {
        climate_humidity_base_scale = static_cast<float>(static_cast<double>(root["climate_humidity_base_scale"]));
    }

    if (root.has("height_centers")) {
        godot::Array centers = root["height_centers"];
        // Fixed-size contract: exactly 3 centers in a fixed, order-dependent
        // position (index 0/1/2 blend by distance in chunk_generator.cpp).
        // Extra JSON entries are ignored; warn so a 4th entry is not silently
        // dead config.
        if (centers.size() > static_cast<int64_t>(height_centers.size())) {
            WARN_PRINT("terrain_config.json height_centers has more than 3 entries; extras are ignored (exactly 3, order-fixed)");
        }
        const size_t count = std::min(static_cast<size_t>(centers.size()), height_centers.size());
        for (size_t i = 0; i < count; ++i) {
            godot::Dictionary c = centers[static_cast<int>(i)];
            if (c.has("temp"))     height_centers[i].temp     = static_cast<float>(static_cast<double>(c["temp"]));
            if (c.has("hum"))      height_centers[i].hum      = static_cast<float>(static_cast<double>(c["hum"]));
            if (c.has("base_off")) height_centers[i].base_off = static_cast<float>(static_cast<double>(c["base_off"]));
            if (c.has("scale_m"))  height_centers[i].scale_m  = static_cast<float>(static_cast<double>(c["scale_m"]));
        }
    }

    return true;
#else
    (void)json_path;
    return false;
#endif
}

} // namespace VoxelEngine
