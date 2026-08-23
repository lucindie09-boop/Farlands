#ifndef FARLANDS_JSON_CONFIG_HPP
#define FARLANDS_JSON_CONFIG_HPP

#include <optional>

namespace godot {
class Dictionary;
class String;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

// Opens and parses a JSON config file, requiring the root to be a dictionary.
// Returns std::nullopt when the file is missing/unreadable, unparseable, or
// the root is not an object. Shared by the worldgen config loaders
// (terrain_params, biome_config, vegetation_config) so each loader only
// implements its own field walking.
inline std::optional<godot::Dictionary> read_json_dictionary(const godot::String& json_path) noexcept {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(json_path, godot::FileAccess::READ);
    if (!file.is_valid()) {
        return std::nullopt;
    }
    godot::String text = file->get_as_text();
    file->close();

    godot::Variant parsed = godot::JSON::parse_string(text);
    if (parsed.get_type() != godot::Variant::DICTIONARY) {
        return std::nullopt;
    }
    return static_cast<godot::Dictionary>(parsed);
#else
    (void)json_path;
    return std::nullopt;
#endif
}

} // namespace VoxelEngine

#endif // FARLANDS_JSON_CONFIG_HPP
