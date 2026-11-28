#ifndef FARLANDS_NAV_FIELD_HPP
#define FARLANDS_NAV_FIELD_HPP

// -----------------------------------------------------------------------------
// NavField — one backward Dijkstra from a single goal, answering every agent
// chasing it.
//
// A crowd is the case the per-agent search is wrong for. Twelve chasers on one
// target ask twelve nearly identical questions every few ticks, each one taking
// chunk-shard locks and a worker slot; one search run BACKWARDS from the target
// answers all of them, and answering a follower is a hash lookup and a walk along
// `next`.
//
// The field is built with `MoveGenerator::predecessors`, which is the same
// generator the search expands with, read backwards — so every edge in the field
// is an edge A* could have walked, at A*'s own cost. Nothing about legality is
// re-derived here.
//
// Bound, not exhaustive: a box around the goal, a node cap and a cost cap. A
// follower outside them is simply not in the field, `route` returns nothing, and
// the caller falls back to its own A*. That is the rule that keeps this additive:
// the field is an optimisation for the common case, never the only answer.
//
// Godot-free, like the rest of the planner: the test binary reads it directly.
// -----------------------------------------------------------------------------

#include "pathfinding/move_generator.hpp"
#include "pathfinding/nav_view.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace nav {

// One node of the field: a feet cell, the block a body stands on there (what a walker consumes),
// its cost TO the goal, and the node one legal move closer to it.
struct NavFieldCell {
    NavNode feet{};
    NavNode support{};      // the support block under the feet (PathService's own convention)
    float cost = 0.0f;      // cost from here to the goal
    uint64_t next = 0;      // node key of the next cell toward the goal (0 at the goal)
    bool has_next = false;
};

class NavField {
public:
    struct Limits {
        int32_t max_nodes = 12000;
        float max_cost = 128.0f;  // 0 disables the cost bound
    };

    // Builds the field over `box`, anchored by `resolve_goal` exactly as a search anchors its
    // goal. Returns false when the goal has no navigable surface (an unloaded column, an aim into
    // a hillside): there is then no field to answer with and the caller's A* reports it.
    bool build(const NavView& view, const NavCosts& costs, const NavNode& goal, const NavBox& box,
               const Limits& limits);

    [[nodiscard]] bool empty() const noexcept { return cells_.empty(); }
    [[nodiscard]] size_t size() const noexcept { return cells_.size(); }
    [[nodiscard]] const NavNode& goal() const noexcept { return goal_; }

    // Exact node lookup — never a nearby one. A follower the field does not hold is a follower
    // this field cannot answer, and the caller must ask its own search rather than be walked to a
    // guessed cell (a guess is a route to a place the body is not standing).
    [[nodiscard]] bool contains(const NavNode& from) const;

    // The node itself, for callers that need its cost or its `next` (the tests walk the chain this
    // way); nullptr when the field does not hold it.
    [[nodiscard]] const NavFieldCell* find(const NavNode& from) const;

    // Follows `next` from `from`, appending SUPPORT cells (the block under each node's feet), the
    // convention `PathService::to_support` uses. Returns how many were appended; zero when `from`
    // is not in the field.
    size_t route(const NavNode& from, size_t max_cells, std::vector<NavNode>& out) const;

private:
    NavNode goal_{};
    std::unordered_map<uint64_t, NavFieldCell> cells_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_FIELD_HPP
