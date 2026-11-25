#ifndef FARLANDS_NAV_BLOCK_CLASS_HPP
#define FARLANDS_NAV_BLOCK_CLASS_HPP

// -----------------------------------------------------------------------------
// Maps a block to the planner's cell classification.
//
// Deliberately does NOT consult BlockType::is_full_cube(): the built-in default
// block set registers every block — air included — as a full cube, because that
// flag is only recomputed from real shapes when data/block_definitions.json is
// loaded under the game runtime. Collision boxes and the Solid property are
// accurate in both paths, so the shape is taken from those.
//
// This is the single place block -> nav cell conversion lives, so the in-game
// source and the standalone probe tools cannot drift apart.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "pathfinding/nav_types.hpp"

#include <algorithm>
#include <cstdint>

namespace VoxelEngine {
namespace nav {

[[nodiscard]] inline Cell classify_block(BlockID id) {
    if (id == BlockIDs::AIR) return Cell{CellClass::Air, 0.0f, 0.0f};

    const BlockRegistry& registry = BlockRegistry::get_instance();
    const BlockType& block = registry.get_block_fast(id);
    if (HasProperty(block.properties, BlockProperty::Liquid)) {
        // Water is a liquid a body can be in (it floats, and the walker swims), so a column of it
        // is somewhere a route may go. Lava and acid are the same navigation class and must never
        // be entered, and the fluid kind is the only thing that separates them -- so the flag is
        // set here, where that kind is still known.
        return Cell{CellClass::Liquid, 0.0f, 0.0f, block.fluid_kind == FluidKind::Water};
    }

    const auto& boxes = block.get_collision_boxes();
    if (boxes.empty()) {
        return HasProperty(block.properties, BlockProperty::Solid)
                   ? Cell{CellClass::Solid, 0.0f, 1.0f}
                   : Cell{CellClass::Air, 0.0f, 0.0f};
    }

    // Partial shapes (slabs, stairs, snow layers, fences) report the union of
    // their collision boxes along y within the cell.
    float lo = 1.0f;
    float hi = 0.0f;
    for (const auto& box : boxes) {
        lo = std::min(lo, box.min[1]);
        hi = std::max(hi, box.max[1]);
    }
    return Cell{CellClass::Partial, lo, hi};
}

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_BLOCK_CLASS_HPP
