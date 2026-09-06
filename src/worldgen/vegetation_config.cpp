#include "worldgen/vegetation_config.hpp"
#include "core/json_config.hpp"

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

namespace {

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
// Optional int read with a fallback (defaults already applied).
int32_t read_int(const godot::Dictionary& d, const char* key, int32_t fallback) {
    if (!d.has(godot::String(key))) return fallback;
    return static_cast<int32_t>(static_cast<int64_t>(d[godot::String(key)]));
}
#endif

} // namespace

bool VegetationConfig::load(const godot::String& json_path) {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    std::optional<godot::Dictionary> root_opt = read_json_dictionary(json_path);
    if (!root_opt.has_value()) {
        return false;
    }
    const godot::Dictionary& root = *root_opt;

    tree_trunk_height = read_int(root, "tree_trunk_height", tree_trunk_height);

    if (root.has("hills")) {
        godot::Dictionary h = root["hills"];
        hills.chunk_chance_pct = read_int(h, "chunk_chance_pct", hills.chunk_chance_pct);
        hills.spacing_radius   = read_int(h, "spacing_radius", hills.spacing_radius);
    }

    return true;
#else
    (void)json_path;
    return false;
#endif
}

} // namespace VoxelEngine
