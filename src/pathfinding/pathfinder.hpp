#ifndef FARLANDS_NAV_PATHFINDER_HPP
#define FARLANDS_NAV_PATHFINDER_HPP

// -----------------------------------------------------------------------------
// A* over the ground movement graph.
//
// Nodes are feet cells; edges come from MoveGenerator. The open set is a binary
// heap ordered by f and then by node key, so an identical query always returns
// an identical route. The heuristic is the octile distance over columns plus
// the vertical climb/descend penalties, which stays admissible against the move
// costs (a climb costs step_up per block, a drop costs fall per block, and both
// are additive on top of the horizontal leg).
//
// The search is budgeted twice over: an expansion cap and an optional
// wall-clock cap (NavQuery::max_ms). When either runs out it returns the
// best-effort run toward the frontier node closest to the goal, flagged with
// NavPath::truncated, so a caller always gets a usable prefix of a route
// instead of nothing.
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
    const NavView& view_;
    NavCosts costs_;
    MoveGenerator generator_;
    mutable std::string last_error_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_PATHFINDER_HPP
