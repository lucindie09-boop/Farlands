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

// Optional held-item light (items.json "light" object). While an item with one
// of these is in the selected hotbar slot, it drives the player's dynamic light
// (see PlayerController::update_held_light): level and colour come from here and
// the light is forced on. `level` is the usual 0-15 light scale; the colour is a
// plain linear RGB triple rather than a godot::Color so the registry stays
// usable in fuzz builds, which have no godot dependency.
struct ItemLight {
    int32_t level = 0;   // 0-15, clamped on read by the light itself
    float r = 1.0f;
    float g = 1.0f;
    float b = 1.0f;
};

// Optional in-world use action on an item (items.json "use" object). An item
// with no "use" entry does nothing when right-clicked; see
// PlayerController::use_item(). `kind` selects the behaviour:
//   "pour" — write the fluid source named by `block` into the cell the crosshair
//            is against.
//   "fill" — pick up the fluid SOURCE the crosshair is on, emptying the cell.
// `block` is what a pour writes, resolved from `block_name` at load time; a fill
// has no block and leaves it AIR.
//
// Resolution happens here rather than at use because blocks are loaded before
// items (VoxelEngineController), and it is deliberately NOT resolved lazily: a
// name that does not resolve is a data mistake and has to be visible at startup
// rather than silently doing nothing the first time someone right-clicks.
struct ItemUseAction {
    std::string kind;        // "pour"/"fill"/"wand", or empty for an item with no in-world use
    std::string block_name;  // the target as written in items.json
    BlockID block = 0;       // resolved block id; AIR = the name did not resolve

    [[nodiscard]] bool has_use() const noexcept { return !kind.empty(); }
    [[nodiscard]] bool is_pour() const noexcept { return kind == "pour"; }
    [[nodiscard]] bool is_fill() const noexcept { return kind == "fill"; }
    // A wand does not touch the world itself: it is a tool whose clicks are
    // answered by the game's own layer (see PlayerController's wand signals),
    // because what it does is choose a thing and then place it where you look.
    [[nodiscard]] bool is_wand() const noexcept { return kind == "wand"; }
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
    // nullptr when the id is not an item. The returned action reports
    // has_use() == false for an item with no "use" entry.
    [[nodiscard]] const ItemUseAction* get_item_use(BlockID id) const noexcept;
    // nullptr when the id is not an item, or when the item lights nothing. The
    // returned light reports level 0 for an item with no "light" entry.
    [[nodiscard]] const ItemLight* get_item_light(BlockID id) const noexcept;
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
        ItemUseAction use;
        ItemLight light;
        bool has_light = false;
    };
    std::deque<ItemDef> items_;  // deque: name pointers stay valid on growth
};

} // namespace VoxelEngine
#endif
