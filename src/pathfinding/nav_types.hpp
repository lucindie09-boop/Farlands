#ifndef FARLANDS_NAV_TYPES_HPP
#define FARLANDS_NAV_TYPES_HPP

// -----------------------------------------------------------------------------
// Voxel navigation — shared types for the ground-movement planner.
//
// The planner works on BLOCK CELLS. A node is the cell that contains an
// agent's feet (y = floor of the standing height), and vertical movement is
// expressed as an edge kind (step up / drop / hop) instead of as extra
// dimensions in the node key. That keeps the search 2.5D — an agent walking
// terrain only ever transitions between columns at their surfaces — which is
// what makes A* over a voxel world tractable at all.
//
// Nothing here touches Godot, so the planner core links into the standalone
// test binary.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <vector>

namespace VoxelEngine {
namespace nav {

// How one cell participates in navigation.
enum class CellClass : uint8_t {
    Unknown = 0,  // block data not resident — never traversable
    Air,          // no collision geometry
    Liquid,       // the body passes through; the mover pays a cost penalty
    Partial,      // collision fills part of the cell (slab, stair, snow layer)
    Solid         // collision fills the whole cell
};

// One sampled cell. The collision span is the union of the block's collision
// boxes reduced to [lo, hi] along y, in cell-local units.
struct Cell {
    CellClass cls = CellClass::Air;
    float lo = 0.0f;
    float hi = 0.0f;

    [[nodiscard]] bool blocks_body() const noexcept {
        return cls == CellClass::Solid || cls == CellClass::Partial;
    }
};

// Cost model. A flat walk to an orthogonal neighbour is exactly 1.0; every
// other term is a penalty relative to that. The numbers are deliberately
// conservative — drops and liquid are discouraged rather than forbidden — so
// the resulting route reads like something an agent would actually walk.
struct NavCosts {
    float walk = 1.0f;             // orthogonal step over level ground
    float diagonal = 1.41421356f;  // 8-way diagonal step
    float step_up = 0.5f;          // extra per block climbed
    float fall = 0.4f;             // extra per block descended
    float jump = 2.0f;             // flat cost of a hop across a one-cell gap
    float liquid = 0.6f;           // extra when either end of a move is in liquid
    float max_rise = 1.0f;         // a single move may climb at most this much
    float max_drop = 3.0f;         // a single move may descend at most this much
    float min_stand_top = 0.5f;    // a surface must reach this high in its cell to stand on
    float body_height = 1.8f;      // agent height used for clearance checks
    float epsilon = 1e-4f;
};

// The window of world cells the planner is allowed to look at. Outside it,
// cells above the top read as sky and everything else reads as unknown.
struct NavBox {
    int32_t min_x = 0, min_y = 0, min_z = 0;
    int32_t max_x = 0, max_y = 0, max_z = 0;
};

// A cell containing the agent's feet.
struct NavNode {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;

    [[nodiscard]] bool operator==(const NavNode& o) const noexcept {
        return x == o.x && y == o.y && z == o.z;
    }
    [[nodiscard]] bool operator!=(const NavNode& o) const noexcept { return !(*this == o); }
};

// 21 bits per axis with a bias, matching the chunk-key layout used elsewhere in
// the engine so node keys hash and compare as plain integers.
inline constexpr uint64_t kNodeAxisBits = 21;
inline constexpr uint32_t kNodeAxisOffset = 1u << 20;
inline constexpr uint32_t kNodeAxisMask = 0x1FFFFF;

[[nodiscard]] inline uint64_t node_key(int32_t x, int32_t y, int32_t z) noexcept {
    const uint64_t ux = (static_cast<uint32_t>(x) + kNodeAxisOffset) & kNodeAxisMask;
    const uint64_t uy = (static_cast<uint32_t>(y) + kNodeAxisOffset) & kNodeAxisMask;
    const uint64_t uz = (static_cast<uint32_t>(z) + kNodeAxisOffset) & kNodeAxisMask;
    return (ux << 42) | (uy << 21) | uz;
}

[[nodiscard]] inline NavNode node_from_key(uint64_t key) noexcept {
    NavNode n;
    n.x = static_cast<int32_t>(((key >> 42) & kNodeAxisMask) - kNodeAxisOffset);
    n.y = static_cast<int32_t>(((key >> 21) & kNodeAxisMask) - kNodeAxisOffset);
    n.z = static_cast<int32_t>((key & kNodeAxisMask) - kNodeAxisOffset);
    return n;
}

// Packs (x, z) — used to key column and cache maps.
[[nodiscard]] inline uint64_t column_key(int32_t x, int32_t z) noexcept {
    const uint64_t ux = (static_cast<uint32_t>(x) + kNodeAxisOffset) & kNodeAxisMask;
    const uint64_t uz = (static_cast<uint32_t>(z) + kNodeAxisOffset) & kNodeAxisMask;
    return (ux << 32) | uz;
}

enum class MoveKind : uint8_t {
    Walk,    // level step
    StepUp,  // step up onto a higher surface
    Drop,    // step down onto a lower surface
    Hop      // jump across a one-cell gap
};

// Search counters, reported so callers can budget and tune.
struct NavStats {
    size_t expansions = 0;
    size_t columns_resolved = 0;
    bool budget_exhausted = false;
};

struct NavPath {
    bool found = false;
    // The expansion budget ran out; the path is the best-effort run to the
    // frontier node closest to the goal (not a complete route).
    bool truncated = false;
    std::vector<NavNode> nodes;
    NavStats stats;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_TYPES_HPP
