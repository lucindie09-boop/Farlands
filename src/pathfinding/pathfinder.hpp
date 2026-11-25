#ifndef FARLANDS_NAV_PATHFINDER_HPP
#define FARLANDS_NAV_PATHFINDER_HPP

// -----------------------------------------------------------------------------
// A* over the ground movement graph.
//
// Nodes are feet cells; edges come from MoveGenerator. The open set is a binary
// heap ordered by f and then by node key, so an identical query always returns
// an identical route. The heuristic is the octile distance over columns plus
// the vertical climb/descend penalties, which stays admissible against the move
// costs and tight: a climb costs step_up per block wherever it is paid, and a
// descent costs fall per block in a hop exactly as in a drop, so one rate covers
// both and the estimate can never sit above the route it is estimating.
//
// The search is budgeted twice over: an expansion cap and an optional
// wall-clock cap (NavQuery::max_ms). When either runs out it returns the
// best-effort run toward the frontier node closest to the goal, flagged with
// NavPath::truncated, so a caller always gets a usable prefix of a route
// instead of nothing.
//
// A goal is a feet cell, and it is anchored onto the ground a body would stand
// on at that cell's level. When the aimed column has no ground there — a target
// at the lip of a shelf, whose own column's floor is the plain below them — the
// nearest neighbouring column that DOES hold that level is the goal instead.
// Without that, the goal is a cell the agent can walk to and stand on under the
// target, so the search reports a complete route that never arrives at it. A wet
// column is anchored the same way, for the other reason: no move enters liquid,
// so a target standing in a puddle would be a goal nothing walks to.
// -----------------------------------------------------------------------------

#include "pathfinding/move_generator.hpp"
#include "pathfinding/nav_view.hpp"

#include <cstdint>
#include <string>

namespace VoxelEngine {
namespace nav {

// NavQuery lives in nav_types.hpp so callers that only queue a plan (the async
// service, the Godot binding) do not need to pull in the search itself.
class Pathfinder {
public:
    Pathfinder(const NavView& view, NavCosts costs = NavCosts())
        : view_(view), costs_(costs), generator_(view, costs_) {}

    [[nodiscard]] NavPath search(const NavQuery& query) const;

    [[nodiscard]] float heuristic(const NavNode& a, const NavNode& b) const;

    // Why the last search came back without a complete route (empty on success).
    [[nodiscard]] const std::string& last_error() const noexcept { return last_error_; }

private:
    // A goal resolved onto ground: the column whose surface it is, and the cell that surface
    // belongs to. The cell is the query's own unless the aimed column holds no ground at the
    // level that was aimed at — see resolve_goal.
    struct GoalAnchor {
        const NavView::Column* column = nullptr;
        int32_t x = 0;
        int32_t z = 0;
    };

    // The column a goal is anchored on, and which cell it belongs to.
    [[nodiscard]] GoalAnchor resolve_goal(const NavQuery& query) const;

    const NavView& view_;
    NavCosts costs_;
    MoveGenerator generator_;
    mutable std::string last_error_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_PATHFINDER_HPP
