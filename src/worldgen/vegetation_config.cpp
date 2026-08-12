#include "worldgen/vegetation_config.hpp"

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
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

    tree_trunk_height = read_int(root, "tree_trunk_height", tree_trunk_height);

    if (root.has("forest")) {
        godot::Dictionary f = root["forest"];
        forest.chunk_chance_pct      = read_int(f, "chunk_chance_pct", forest.chunk_chance_pct);
        forest.min_trees             = read_int(f, "min_trees", forest.min_trees);
        forest.max_trees             = read_int(f, "max_trees", forest.max_trees);
        forest.column_chance_pct     = read_int(f, "column_chance_pct", forest.column_chance_pct);
        forest.spacing_radius        = read_int(f, "spacing_radius", forest.spacing_radius);
        forest.boulder_chance_per_10000 = read_int(f, "boulder_chance_per_10000", forest.boulder_chance_per_10000);
        forest.boulder_radius        = read_int(f, "boulder_radius", forest.boulder_radius);
    }

    if (root.has("plains")) {
        godot::Dictionary p = root["plains"];
        plains.chunk_chance_pct = read_int(p, "chunk_chance_pct", plains.chunk_chance_pct);
        plains.spacing_radius   = read_int(p, "spacing_radius", plains.spacing_radius);
    }

    if (root.has("desert")) {
        godot::Dictionary d = root["desert"];
        desert.cactus_chance_per_1000 = read_int(d, "cactus_chance_per_1000", desert.cactus_chance_per_1000);
        desert.min_height             = read_int(d, "min_height", desert.min_height);
        desert.max_height             = read_int(d, "max_height", desert.max_height);
    }

    return true;
#else
    (void)json_path;
    return false;
#endif
}

} // namespace VoxelEngine
