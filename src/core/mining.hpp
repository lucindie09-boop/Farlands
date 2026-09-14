#ifndef FARLANDS_MINING_HPP
#define FARLANDS_MINING_HPP

#include "core/block_types.hpp"
#include "core/item_registry.hpp"

namespace VoxelEngine {

// Tool class that crushes blocks (data/items.json "tool"."class"). A block's
// own `crush_result` is the hammer's entire contract — what it is fast against
// and what it leaves behind — so no block has to name "hammer" anywhere.
inline constexpr const char* kHammerToolClass = "hammer";

[[nodiscard]] inline bool is_hammer(const ItemToolStats& tool) noexcept {
    return tool.tool_class == kHammerToolClass;
}

// A block is crushable when it names the block a hammer leaves behind instead.
// AIR (0) means "not crushable"; a crush_result of "air" is therefore not a way
// to describe a block that drops nothing.
[[nodiscard]] inline bool is_crushable(const BlockType& block) noexcept {
    return block.crush_result != BlockIDs::AIR;
}

// Pure tool-vs-block decision, split out so it can be unit-tested without the
// registries (which load from JSON on the Godot side).
//
// Returns the break-speed multiplier a tool grants against a block:
//   1.0  — the item is not a tool, its class does not match the block, or it is
//          below the block's min_tier.
//   tool.speed — the tool's class matches and its tier meets min_tier.
//
// A block prefers one tool class by name, but a hammer additionally matches
// every block it can crush: crushing is the hammer's job (the block names what
// the crush leaves behind), so stone can prefer the pickaxe and still be mined
// faster by a hammer. A block may also name "hammer" outright if it wants the
// speed bonus without being crushable.
[[nodiscard]] inline float tool_mining_speed(const ItemToolStats& tool,
                                            const BlockType& block) noexcept {
    if (!tool.is_tool()) {
        return 1.0f;
    }
    const bool class_matches = tool.tool_class == block.preferred_tool ||
                               (is_hammer(tool) && is_crushable(block));
    if (!class_matches || tool.tier < block.min_tier) {
        return 1.0f;
    }
    return tool.speed;
}

// Registry-backed version used by the hold-to-break path. `held` is the
// selected hotbar id: a block id, an item id (>= ItemRegistry::FIRST_ITEM_ID),
// or AIR for an empty hand. Break progress accumulates at
// (multiplier / hardness) per second, so a higher multiplier breaks sooner.
[[nodiscard]] inline float mining_speed_multiplier(BlockID block, BlockID held) noexcept {
    if (block == BlockIDs::AIR || held == BlockIDs::AIR) {
        return 1.0f;
    }
    const BlockType& bt = BlockRegistry::get_instance().get_block(block);
    // Nothing amplifies either way: no preferred tool and nothing to crush.
    if (bt.preferred_tool.empty() && !is_crushable(bt)) {
        return 1.0f;
    }
    const ItemToolStats* tool = ItemRegistry::get_instance().get_item_tool(held);
    if (tool == nullptr) {
        return 1.0f;
    }
    return tool_mining_speed(*tool, bt);
}

// The block a held tool turns `block` into, or AIR when it does not crush it.
// Pure, so the crush table can be tested without loading items.json.
[[nodiscard]] inline BlockID crushed_block(const BlockType& block,
                                           const ItemToolStats* tool) noexcept {
    if (tool == nullptr || !is_hammer(*tool)) {
        return BlockIDs::AIR;
    }
    return block.crush_result;
}

// The inventory stack a broken block yields.
struct BlockDrop {
    BlockID id = BlockIDs::AIR;  // AIR when the break yields nothing
    int32_t count = 0;
};

// Resolves what breaking a block yields given the tool stats of what is held
// (nullptr = bare hand or a non-tool item), in the order the break path must
// apply it:
//   1. variants collapse to the id they are stored as in an inventory slot
//      (slab halves drop the bottom variant, a merged full slab drops both,
//      stairs and walls drop their base block),
//   2. a hammer crushing a block that names a crush_result yields that block
//      instead (cobblestone -> gravel),
//   3. otherwise the block's own `drops` stands, if it names one
//      (stone -> cobblestone).
// Takes tool stats rather than a held id so the whole rule is reachable from
// tests, which run without items.json (items only load under the game runtime).
[[nodiscard]] inline BlockDrop block_drop(BlockID block, const ItemToolStats* tool) noexcept {
    BlockDrop drop;
    if (block == BlockIDs::AIR) {
        return drop;
    }

    const BlockRegistry& reg = BlockRegistry::get_instance();
    drop.id = block;
    drop.count = 1;

    if (const auto* slab_fam = reg.get_slab_family(block)) {
        if (block == slab_fam->full) {
            drop.count = 2;
        }
        drop.id = slab_fam->bottom;
    } else if (const auto* stair_fam = reg.get_stair_family(block)) {
        drop.id = stair_fam->base;
    } else if (const auto* wall_fam = reg.get_wall_family(block)) {
        drop.id = wall_fam->base;
    }

    // The block's own rules are keyed on the block that was actually broken,
    // not on the item a variant collapses to: a slab of a crushable block is
    // still a slab, and both fields are declared on the full block. The
    // hammer's tier does not gate the crush — tier only scales the break speed —
    // so a hammer always transforms what it is fast against.
    const BlockType& bt = reg.get_block(block);
    const BlockID crushed = crushed_block(bt, tool);
    if (crushed != BlockIDs::AIR) {
        // A crush overrides the ordinary drop: cobblestone yields cobblestone
        // by hand and gravel to a hammer.
        drop.id = crushed;
        drop.count = 1;  // one block in, one block out
        return drop;
    }
    if (bt.drops != BlockIDs::AIR) {
        drop.id = bt.drops;
        drop.count = 1;
    }
    return drop;
}

// Registry-backed version for the break paths: looks up the tool stats of the
// selected hotbar id (a block id, an item id, or AIR for an empty hand).
// Both break_block() (the collect) and the hold-to-break inventory gate call
// this, so the thing the gate checks for space is exactly the thing the break
// then hands over.
[[nodiscard]] inline BlockDrop resolve_block_drop(BlockID block, BlockID held) noexcept {
    return block_drop(block, ItemRegistry::get_instance().get_item_tool(held));
}

} // namespace VoxelEngine

#endif // FARLANDS_MINING_HPP
