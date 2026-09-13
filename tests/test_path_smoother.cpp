#include "doctest.h"
#include "nav_test_world.hpp"
#include "pathfinding/path_smoother.hpp"
#include "pathfinding/pathfinder.hpp"

#include <algorithm>

using namespace VoxelEngine::nav;
namespace nt = navtest;

namespace {

bool contains_in_order(const std::vector<NavNode>& haystack, const std::vector<NavNode>& needles) {
    size_t at = 0;
    for (const NavNode& n : needles) {
        bool hit = false;
        while (at < haystack.size()) {
            if (haystack[at] == n) {
                hit = true;
                ++at;
                break;
            }
            ++at;
        }
        if (!hit) return false;
    }
    return true;
}

} // namespace

TEST_CASE("PathSmoother collapses a straight grid path to its endpoints") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 16);
    PathSmoother smoother(view, NavCosts{});

    std::vector<NavNode> grid;
    for (int32_t x = 0; x <= 10; ++x) grid.push_back(NavNode{x, 10, 0});

    const std::vector<NavNode> waypoints = smoother.smooth(grid);
    CHECK(waypoints.size() == 2);
    CHECK(waypoints.front() == grid.front());
    CHECK(waypoints.back() == grid.back());
}

TEST_CASE("PathSmoother keeps a waypoint where the route is forced") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -12; z <= 12; ++z) w.set_ground(6, z, 12);
    w.set_ground(6, 4, 10);  // a doorway the route must detour through

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});
    PathSmoother smoother(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{12, 10, 0};
    const NavPath path = finder.search(q);
    CHECK(path.found);
    if (!path.found) return;

    const std::vector<NavNode> waypoints = smoother.smooth(path.nodes);
    CHECK(waypoints.size() >= 3);              // the detour cannot be cut out
    CHECK(waypoints.size() < path.nodes.size());
    CHECK(waypoints.front() == path.nodes.front());
    CHECK(waypoints.back() == path.nodes.back());
    // Smoothing never invents nodes: every waypoint is one of the input nodes,
    // in order.
    CHECK(contains_in_order(path.nodes, waypoints));
    // Straight legs between waypoints are genuinely walkable.
    for (size_t i = 1; i < waypoints.size(); ++i) {
        CHECK(smoother.leg_walkable(waypoints[i - 1], waypoints[i]));
    }
}

TEST_CASE("PathSmoother declines a leg that would have to climb two blocks") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 12);  // a two-block step in the middle of the leg
    NavView view = nt::make_view(w, 16);
    PathSmoother smoother(view, NavCosts{});

    CHECK_FALSE(smoother.leg_walkable(NavNode{0, 10, 0}, NavNode{2, 10, 0}));
    // A single-block step is walkable along the way.
    nt::World w2;
    w2.default_ground = 10;
    w2.set_ground(1, 0, 11);
    w2.set_ground(2, 0, 11);
    w2.set_ground(3, 0, 11);
    NavView view2 = nt::make_view(w2, 16);
    PathSmoother smoother2(view2, NavCosts{});
    CHECK(smoother2.leg_walkable(NavNode{0, 10, 0}, NavNode{3, 11, 0}));
}

TEST_CASE("PathSmoother passes short paths through unchanged") {
    nt::World w;
    NavView view = nt::make_view(w, 8);
    PathSmoother smoother(view, NavCosts{});

    std::vector<NavNode> single{NavNode{0, 10, 0}};
    CHECK(smoother.smooth(single).size() == 1);

    std::vector<NavNode> pair{NavNode{0, 10, 0}, NavNode{1, 10, 0}};
    CHECK(smoother.smooth(pair).size() == 2);
}
