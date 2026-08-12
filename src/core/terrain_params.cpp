#include "core/terrain_params.hpp"

#include <algorithm>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

bool TerrainParams::load_from_json(const godot::String& json_path) noexcept {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(json_path, godot::FileAccess::READ);
    if (!file.is_valid()) {
        return false;
    }
    godot::String text = file->get_as_text();
    file->close();

    godot::Variant parsed = godot::JSON::parse_string(text);
    if (parsed.get_type() != godot::Variant::DICTIONARY) {
        return false;
    }
    godot::Dictionary root = parsed;

    if (root.has("height_base_y")) {
        height_base_y = static_cast<float>(static_cast<double>(root["height_base_y"]));
    }
    if (root.has("climate_temp_base_scale")) {
        climate_temp_base_scale = static_cast<float>(static_cast<double>(root["climate_temp_base_scale"]));
    }
    if (root.has("climate_humidity_base_scale")) {
        climate_humidity_base_scale = static_cast<float>(static_cast<double>(root["climate_humidity_base_scale"]));
    }

    if (root.has("height_centers")) {
        godot::Array centers = root["height_centers"];
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
