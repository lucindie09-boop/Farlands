#include "core/item_registry.hpp"

#include "core/json_config.hpp"

#include <cstring>

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4505) // json_config inline helper unused in fuzz builds
#endif

namespace VoxelEngine {

namespace {

// A light colour is a fraction of full strength per channel; a fat-fingered 4.0
// would blow out every surface it touches, so it is clamped rather than trusted.
inline float clamp01(float v) noexcept {
    if (!(v > 0.0f)) return 0.0f;  // also catches NaN
    return v > 1.0f ? 1.0f : v;
}

} // namespace

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

const ItemToolStats* ItemRegistry::get_item_tool(BlockID id) const noexcept {
    if (!is_item(id)) {
        return nullptr;
    }
    return &items_[static_cast<size_t>(id - FIRST_ITEM_ID)].tool;
}

const ItemUseAction* ItemRegistry::get_item_use(BlockID id) const noexcept {
    if (!is_item(id)) {
        return nullptr;
    }
    return &items_[static_cast<size_t>(id - FIRST_ITEM_ID)].use;
}

const ItemLight* ItemRegistry::get_item_light(BlockID id) const noexcept {
    if (!is_item(id)) {
        return nullptr;
    }
    const ItemDef& def = items_[static_cast<size_t>(id - FIRST_ITEM_ID)];
    return def.has_light ? &def.light : nullptr;
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

        // Optional "tool" stats: {"class": "pickaxe", "tier": 1, "speed": 2.0}.
        // Missing/empty class leaves the item a plain non-tool object.
        if (entry.has("tool")) {
            const godot::Dictionary tool = entry["tool"];
            const godot::String tool_class = tool.get("class", godot::String());
            if (!tool_class.is_empty()) {
                def.tool.tool_class = tool_class.utf8().get_data();
                def.tool.tier = static_cast<int32_t>(static_cast<int64_t>(tool.get("tier", 0)));
                float speed = static_cast<float>(static_cast<double>(tool.get("speed", 1.0)));
                if (!(speed >= 1.0f)) speed = 1.0f;  // also catches NaN
                def.tool.speed = speed;
            }
        }
        // Optional "use" action: {"kind": "pour", "block": "water"}. Missing
        // leaves the item inert in the world (it can still be held and given).
        if (entry.has("use")) {
            const godot::Dictionary use = entry["use"];
            const godot::String kind = use.get("kind", godot::String());
            if (kind.is_empty()) {
                WARN_PRINT("items.json entry " + name + ": \"use\" has no kind, ignored");
            } else if (kind == "pour") {
                def.use.kind = "pour";
                const godot::String target = use.get("block", godot::String());
                def.use.block_name = target.utf8().get_data();
                def.use.block = BlockRegistry::get_instance()
                                    .get_block_id_by_name(def.use.block_name.c_str());
                if (def.use.block == BlockIDs::AIR) {
                    ERR_PRINT("items.json entry " + name + ": use.block \"" + target
                              + "\" is not a known block, so right-clicking it does nothing");
                }
            } else if (kind == "fill") {
                // A fill has no target block: it takes what is already in the
                // world. A "block" alongside it would be ignored, so say so.
                def.use.kind = "fill";
                if (use.has("block")) {
                    WARN_PRINT("items.json entry " + name
                               + ": \"use\" fills the held container, so its \"block\" is unused");
                }
            } else if (kind == "wand") {
                // A wand names no block either: it is a tool, and what it places
                // is chosen in game rather than fixed in the data.
                def.use.kind = "wand";
                if (use.has("block")) {
                    WARN_PRINT("items.json entry " + name
                               + ": a wand's target is chosen in game, so its \"block\" is unused");
                }
            } else {
                ERR_PRINT("items.json entry " + name + ": unknown use kind \"" + kind
                          + "\", ignored");
            }
        }
        // Optional held-item light: {"level": 14, "color": [1.0, 0.83, 0.6]}.
        // A colour is optional and defaults to warm white, so a torch can be
        // added with a level alone. Level 0 means "inert": it would force the
        // dynamic light on at zero strength, i.e. a light that lights nothing, so
        // it is treated as no light at all rather than silently disabling it.
        if (entry.has("light")) {
            const godot::Dictionary light = entry["light"];
            const int64_t level = static_cast<int64_t>(light.get("level", 0));
            if (level <= 0) {
                WARN_PRINT("items.json entry " + name + ": \"light\" level "
                           + godot::String::num_int64(level) + " lights nothing, ignored");
            } else {
                const int64_t clamped = level > 15 ? 15 : level;
                if (clamped != level) {
                    WARN_PRINT("items.json entry " + name + ": \"light\" level "
                               + godot::String::num_int64(level) + " exceeds 15, clamped");
                }
                def.light.level = static_cast<int32_t>(clamped);
                if (light.has("color")) {
                    const godot::Array rgb = light["color"];
                    if (rgb.size() >= 3) {
                        def.light.r = clamp01(static_cast<float>(static_cast<double>(rgb[0])));
                        def.light.g = clamp01(static_cast<float>(static_cast<double>(rgb[1])));
                        def.light.b = clamp01(static_cast<float>(static_cast<double>(rgb[2])));
                    } else {
                        WARN_PRINT("items.json entry " + name
                                   + ": \"light\" color needs 3 components, using the default");
                    }
                }
                def.has_light = true;
            }
        }
        items_.push_back(std::move(def));
    }
    return true;
}

#endif // !FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION

} // namespace VoxelEngine

#ifdef _MSC_VER
#pragma warning(pop)
#endif
