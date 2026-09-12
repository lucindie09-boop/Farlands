#ifndef FARLANDS_ITEM_REGISTRY_HPP
#define FARLANDS_ITEM_REGISTRY_HPP

#include "core/block_types.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/variant/string.hpp>
#endif

namespace VoxelEngine {

// Optional tool stats on an item (items.json "tool" object). A plain item
// leaves tool_class empty, which reports is_tool() == false. `speed` is the
// break-speed multiplier applied when the tool's class matches a block's
// preferred tool; `tier` is the harvest level that block's min_tier gates on.
struct ItemToolStats {
    std::string tool_class;  // "pickaxe", "axe", "shovel", ... ("" = not a tool)
    int32_t tier = 0;        // harvest tier; 0 = hand level
    float speed = 1.0f;      // break multiplier vs. bare hand (>= 1.0)

    [[nodiscard]] bool is_tool() const noexcept { return !tool_class.empty(); }
};

// Non-placeable inventory objects (sticks, tools, ...) living in their own ID
// space above the block registry: item ids start at FIRST_ITEM_ID, so a single
// Inventory slot id can address either without changing any storage or stack
// logic. Loaded once at startup from res://data/items.json (entry order = id).
class ItemRegistry {
public:
    static constexpr uint16_t FIRST_ITEM_ID = 1024;

    static ItemRegistry& get_instance();

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    // Parses res://data/items.json. Missing/unparseable file returns false and
    // leaves current items untouched; entries without a name are skipped.
    [[nodiscard]] bool load_from_json(const godot::String& json_path) noexcept;
#endif

    // AIR (0) when unknown.
    [[nodiscard]] BlockID get_item_id_by_name(const char* name) const noexcept;
    // nullptr when the id is not an item.
    [[nodiscard]] const char* get_item_name(BlockID id) const noexcept;
    // nullptr when the id is not an item. Returns the texture filename, which
    // defaults to the item name when items.json omits it.
    [[nodiscard]] const char* get_item_texture(BlockID id) const noexcept;
    // nullptr when the id is not an item. The returned stats report
    // is_tool() == false for plain items with no "tool" entry.
    [[nodiscard]] const ItemToolStats* get_item_tool(BlockID id) const noexcept;
    [[nodiscard]] size_t get_item_count() const noexcept {
        return items_.size();
    }
    [[nodiscard]] bool is_item(BlockID id) const noexcept {
        return id >= FIRST_ITEM_ID && static_cast<size_t>(id - FIRST_ITEM_ID) < items_.size();
    }

private:
    struct ItemDef {
        std::string name;
        std::string texture;
        ItemToolStats tool;
    };
    std::deque<ItemDef> items_;  // deque: name pointers stay valid on growth
};

} // namespace VoxelEngine
#endif
