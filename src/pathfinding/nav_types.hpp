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
    // A collision this tall within its cell counts as a surface the agent stands
    // on, rather than as an obstruction it has to walk around. It is a sixteenth
    // of a block because that is what the real sub-block details are: gravel_path
    // (shape `lowered/00625`) has a collision 0.0625 high, and at a half-block
    // threshold the column scan skipped it, found the ground beneath, and then the
    // body check saw the same block as an obstruction — making every path block in
    // the world impassable. Standing on top of it is both walkable and what the
    // player sees. A collision thinner than this is treated as decoration.
    float min_stand_top = 0.0625f;
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
    // The expansion cap was reached (the wall-clock budget, when set, was not).
    bool budget_exhausted = false;
    // The wall-clock budget ran out first.
    bool time_exhausted = false;
};

// A planning request: where the agent is and where it wants to end up, both as
// feet cells. The search re-anchors both ends onto their column's surface, so a
// caller may pass an airborne position (a falling entity, a target above
// ground).
struct NavQuery {
    NavNode start{};
    NavNode goal{};

    // Expansion cap. Reaching it returns a truncated (partial) route.
    int32_t max_expansions = 20000;
    // Wall-clock cap in milliseconds, checked every 64 expansions; 0 disables it.
    // Reaching it returns the same best-effort partial route as the expansion
    // cap, but bounded in time instead of in work, so a caller can promise a
    // latency on a machine (or a contended chunk map) slower than expected.
    double max_ms = 0.0;
    // Heuristic weight. 1.0 is optimal; above 1.0 trades optimality for speed.
    float weight = 1.0f;
    // How far the arriving node may sit vertically from the goal's own anchoring.
    // 0 (the default) means it must be standing on the surface the goal resolved
    // to, which is what an arriving agent always is. Do NOT loosen this to mean
    // "anywhere in the goal's column": a column can hold several standable
    // surfaces, so a column-only test is satisfied by the ground underneath a
    // floating staircase — a route that stops twenty blocks below its target.
    int32_t goal_y_slack = 0;
};

struct NavPath {
    bool found = false;
    // A budget ran out (expansions or time); the path is the best-effort run to
    // the frontier node closest to the goal (not a complete route).
    bool truncated = false;
    std::vector<NavNode> nodes;
    NavStats stats;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_TYPES_HPP
