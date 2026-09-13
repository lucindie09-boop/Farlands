#ifndef FARLANDS_NAV_PATH_SMOOTHER_HPP
#define FARLANDS_NAV_PATH_SMOOTHER_HPP

// -----------------------------------------------------------------------------
// String-pull smoothing.
//
// A grid path zigzags: on open ground A* may still hand back a staircase of
// cells. Smoothing keeps the endpoints and drops every intermediate node it can
// see past, using an exact walkability test on the straight 8-connected line
// between two nodes — so a leg is only collapsed when an agent could really
// walk it, including step limits and body clearance.
// -----------------------------------------------------------------------------

#include "pathfinding/move_generator.hpp"
#include "pathfinding/nav_view.hpp"

#include <vector>

namespace VoxelEngine {
namespace nav {

class PathSmoother {
public:
    PathSmoother(const NavView& view, NavCosts costs = NavCosts())
        : view_(view), costs_(costs), generator_(view, costs_) {}

    // Collapses a grid path into waypoints. The first and last nodes are always
    // kept; every waypoint is one of the input nodes, in order.
    [[nodiscard]] std::vector<NavNode> smooth(const std::vector<NavNode>& path) const;

    // True when an agent could walk the straight 8-connected line from `a` to
    // `b`, ending exactly on `b`.
    [[nodiscard]] bool leg_walkable(const NavNode& a, const NavNode& b) const;

private:
    const NavView& view_;
    NavCosts costs_;
    MoveGenerator generator_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_PATH_SMOOTHER_HPP
