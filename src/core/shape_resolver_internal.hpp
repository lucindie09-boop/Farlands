#pragma once

// The two neighbour questions the connector rules ask beyond the public surface:
// whether the cell above spans the strip a wall's reach occupies, and whether a
// wall reaches the cell beside it. Their bodies (and the cross-section constants
// they read) stay in core/shape_resolver.cpp; this exists only so
// core/shape_resolver_rules.cpp can call them, which is why they lose their
// file-local linkage and nothing else.

#include "core/shape_resolver.hpp"

namespace VoxelEngine {

// Whether the cell above spans the strip a reach on `face` occupies.
[[nodiscard]] bool above_covers_reach(const ShapeNeighborFn& neighbors,
                                     const BlockRegistry& registry, ShapeFace face) noexcept;

// Whether a wall reaches this neighbour at all, whatever height the reach is
// finally drawn at.
[[nodiscard]] bool wall_reaches(BlockID neighbor, const BlockType& type) noexcept;

} // namespace VoxelEngine
