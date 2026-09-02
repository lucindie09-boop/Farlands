#include "core/item_registry.hpp"

#include "core/json_config.hpp"

#include <cstring>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505) // json_config inline helper unused in fuzz builds
#endif

namespace VoxelEngine {

ItemRegistry& ItemRegistry::get_instance() {
    static ItemRegistry instance;
    return instance;
}

BlockID ItemRegistry::get_item_id_by_name(const char* name) const noexcept {
    if (name == nullptr) {
        return BlockIDs::AIR;
    }
    for (size_t i = 0; i < items_.size(); ++i) {
        if (std::strcmp(items_[i].name.c_str(), name) == 0) {
            return static_cast<BlockID>(FIRST_ITEM_ID + i);
        }
    }
    return BlockIDs::AIR;
}

const char* ItemRegistry::get_item_name(BlockID id) const noexcept {
    if (!is_item(id)) {
        return nullptr;
    }
    return items_[static_cast<size_t>(id - FIRST_ITEM_ID)].name.c_str();
}

const char* ItemRegistry::get_item_texture(BlockID id) const noexcept {
    if (!is_item(id)) {
        return nullptr;
    }
    return items_[static_cast<size_t>(id - FIRST_ITEM_ID)].texture.c_str();
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

bool ItemRegistry::load_from_json(const godot::String& json_path) noexcept {
    std::optional<godot::Dictionary> root_opt = read_json_dictionary(json_path);
    if (!root_opt.has_value()) {
        return false;
    }
    const godot::Dictionary& root = *root_opt;

    if (!root.has("items")) {
        return true;
    }

    const godot::Array entries = root["items"];
    for (int64_t i = 0; i < entries.size(); ++i) {
        const godot::Dictionary entry = entries[static_cast<int>(i)];
        const godot::String name = entry.get("name", godot::String());
        if (name.is_empty()) {
            WARN_PRINT("items.json entry " + godot::String::num_int64(i) + ": missing name, skipped");
            continue;
        }
        ItemDef def;
        def.name = name.utf8().get_data();
        def.texture = godot::String(entry.get("texture", name)).utf8().get_data();
        items_.push_back(std::move(def));
    }
    return true;
}

#endif // !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

} // namespace VoxelEngine

#ifdef _MSC_VER
#pragma warning(pop)
#endif
