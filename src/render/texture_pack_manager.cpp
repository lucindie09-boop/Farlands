#include "render/texture_pack_manager.hpp"

#include "core/json_config.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

namespace {

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
bool default_path_exists(const std::string& path) {
    return godot::FileAccess::file_exists(godot::String(path.c_str()));
}

// pack.json dict -> TexturePack, filling defaults for missing fields. The
// schema range is validated by the caller via schema_supported().
std::optional<TexturePack> parse_pack(const godot::String& root_dir,
                                      const godot::Dictionary& json) {
    try {
        TexturePack pack;
        pack.root_dir = std::string(root_dir.utf8().get_data()) + "/";
        
        if (json.has("name")) {
            godot::Variant name_var = json["name"];
            if (name_var.get_type() == godot::Variant::STRING) {
                pack.name = std::string(static_cast<godot::String>(name_var).utf8().get_data());
            }
        }
        if (pack.name.empty()) {
            pack.name = std::string(root_dir.get_file().utf8().get_data());
        }
        
        if (json.has("schema")) {
            godot::Variant schema_var = json["schema"];
            if (schema_var.get_type() == godot::Variant::INT) {
                pack.schema = static_cast<int>(static_cast<int64_t>(schema_var));
            }
        }
        if (json.has("min_supported")) {
            godot::Variant min_var = json["min_supported"];
            if (min_var.get_type() == godot::Variant::INT) {
                pack.min_supported = static_cast<int>(static_cast<int64_t>(min_var));
            }
        }
        if (json.has("max_supported")) {
            godot::Variant max_var = json["max_supported"];
            if (max_var.get_type() == godot::Variant::INT) {
                pack.max_supported = static_cast<int>(static_cast<int64_t>(max_var));
            }
        }
        if (json.has("base_resolution")) {
            godot::Variant res_var = json["base_resolution"];
            if (res_var.get_type() == godot::Variant::INT) {
                pack.base_resolution = static_cast<int>(static_cast<int64_t>(res_var));
            }
        }
        if (json.has("author")) {
            godot::Variant author_var = json["author"];
            if (author_var.get_type() == godot::Variant::STRING) {
                pack.author = std::string(static_cast<godot::String>(author_var).utf8().get_data());
            }
        }
        if (pack.min_supported > pack.max_supported) {
            std::swap(pack.min_supported, pack.max_supported);
        }
        return pack;
    } catch (...) {
        // Catch any exceptions during JSON parsing
        return std::nullopt;
    }
}
#endif // !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

} // namespace

TexturePackManager::TexturePackManager() {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    path_exists_ = default_path_exists;
#endif
}

TexturePackManager& TexturePackManager::get_instance() {
    static TexturePackManager instance;
    return instance;
}

void TexturePackManager::set_packs(std::vector<TexturePack> packs) {
    packs_ = std::move(packs);
    active_.clear();
}

void TexturePackManager::load_packs(const godot::String& root_dir) {
#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    godot::Ref<godot::DirAccess> dir = godot::DirAccess::open(root_dir);
    if (!dir.is_valid()) {
        // No packs directory is a valid state — run with built-in textures.
        set_packs({});
        return;
    }

    std::vector<TexturePack> loaded;

    const auto add_pack_dir = [&loaded](const godot::String& pack_dir) {
        const godot::String pack_json_path = pack_dir.path_join("pack.json");
        std::optional<godot::Dictionary> root = read_json_dictionary(pack_json_path);
        if (!root.has_value()) {
            WARN_PRINT("Texture pack skipped (missing/unparseable pack.json): " + pack_dir);
            return;
        }
        std::optional<TexturePack> parsed = parse_pack(pack_dir, *root);
        if (!parsed.has_value()) {
            WARN_PRINT("Texture pack skipped (failed to parse pack.json): " + pack_dir);
            return;
        }
        if (!schema_supported(*parsed)) {
            WARN_PRINT("Texture pack skipped (unsupported schema): " + pack_dir);
            return;
        }
        loaded.push_back(std::move(*parsed));
    };

    dir->list_dir_begin();
    godot::String entry = dir->get_next();
    while (!entry.is_empty()) {
        if (entry != "." && entry != "..") {
            const godot::String full = root_dir.path_join(entry);
            if (dir->current_is_dir()) {
                if (!entry.begins_with(".")) {
                    // Regular pack folder. Dot-dirs (the .cache of converted
                    // zips) are handled through the zip entries below.
                    add_pack_dir(full);
                }
            }
            // Zip files are not supported - use Python converter: python tools/pack_converter.py <zip> --out user://packs/
        }
        entry = dir->get_next();
    }
    dir->list_dir_end();

    set_packs(std::move(loaded));
#else
    (void)root_dir;
    set_packs({});
#endif
}

bool TexturePackManager::set_active_pack(const godot::String& name) {
    const godot::String lowered = name.to_lower();
    for (size_t i = 0; i < packs_.size(); ++i) {
        if (godot::String(packs_[i].name.c_str()).to_lower() == lowered) {
            active_.clear();
            active_.push_back(i);
            return true;
        }
    }
    return false;
}

void TexturePackManager::clear_active_pack() {
    active_.clear();
}

bool TexturePackManager::has_active_pack() const {
    return !active_.empty();
}

godot::String TexturePackManager::resolve(const godot::String& texture_name) const {
    const std::string resolved = resolve_texture(
        packs_, active_, std::string(texture_name.utf8().get_data()), path_exists_);
    return godot::String(resolved.c_str());
}

std::optional<godot::String> TexturePackManager::resolve_optional(const godot::String& texture_name) const {
    std::optional<std::string> resolved = resolve_texture_optional(
        packs_, active_, std::string(texture_name.utf8().get_data()), path_exists_);
    if (!resolved.has_value()) {
        return std::nullopt;
    }
    return godot::String(resolved->c_str());
}

int TexturePackManager::get_base_resolution() const {
    return base_resolution_for(packs_, active_);
}

} // namespace VoxelEngine
