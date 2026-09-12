#ifndef FARLANDS_MINING_HPP
#define FARLANDS_MINING_HPP

#include "core/block_types.hpp"
#include "core/item_registry.hpp"

namespace VoxelEngine {

// Pure tool-vs-block decision, split out so it can be unit-tested without the
// registries (which load from JSON on the Godot side).
//
// Returns the break-speed multiplier a tool grants against a block:
//   1.0  — the item is not a tool, the block has no preferred tool, the tool's
//          class does not match, or the tool is below the block's min_tier.
//   tool.speed — the tool's class matches and its tier meets min_tier.
[[nodiscard]] inline float tool_mining_speed(const ItemToolStats& tool,
                                            const BlockType& block) noexcept {
    if (!tool.is_tool() || block.preferred_tool.empty()) {
        return 1.0f;
    }
    if (tool.tool_class != block.preferred_tool) {
        return 1.0f;
    }
    if (tool.tier < block.min_tier) {
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
    if (bt.preferred_tool.empty()) {
        return 1.0f;
    }
    const ItemToolStats* tool = ItemRegistry::get_instance().get_item_tool(held);
    if (tool == nullptr) {
        return 1.0f;
    }
    return tool_mining_speed(*tool, bt);
}

} // namespace VoxelEngine

#endif // FARLANDS_MINING_HPP
