#include "doctest.h"
#include "nav_test_world.hpp"
#include "pathfinding/pathfinder.hpp"

#include <cstdlib>

using namespace VoxelEngine::nav;
namespace nt = navtest;

TEST_CASE("A target standing in shallow water is reached on the shore beside them") {
    // The aimed goal cell is a column whose own floor holds the right level -- the plain is under
    // the water -- so anchoring on it is the "goal at the aimed level" case. But that cell is
    // liquid, and a move may not enter liquid: the goal is then a cell nothing can walk to, the
    // search answers "no route", and a goblin chasing a player standing in a puddle gives up.
    // The ground they are on is a column over, which is exactly what the ring anchoring is for.
    nt::World w;
    w.default_ground = 10;
    w.set_cell(0, 10, 0, CellClass::Liquid);  // the feet cell, one deep, plain underneath

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{-6, 10, 0};
    q.goal = NavNode{0, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    const NavNode end = path.nodes.back();
    CHECK(std::abs(end.x) + std::abs(end.z) == 1);  // beside them, not on them
    CHECK(end.y == 10);                             // and at their own level
}

TEST_CASE("A search that ran out of graph at exactly its cap is not called truncated") {
    // A one-block pillar four blocks tall in the middle of a plain: its top is unreachable, and the
    // plain inside the window is a finite graph. With no cap worth the name the search explores all
    // of it and says so. Ask again with the cap set to exactly the number of expansions that took,
    // and the loop stops on the same count with the heap already empty -- which is an exhausted
    // graph, not a cut-off search. Reporting it as truncated tells a caller to spend a bigger
    // budget on a goal that is unreachable at any budget.
    nt::World w;
    w.default_ground = 10;
    w.fill(0, 10, 0, 1, 4, 1);  // the pillar: cells 10..13, so its surface is 14

    NavView view = nt::make_view(w, 12);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{-5, 10, 0};
    q.goal = NavNode{0, 14, 0};
    q.max_expansions = 20000;

    const NavPath first = finder.search(q);
    CHECK_FALSE(first.found);
    CHECK_FALSE(first.truncated);
    CHECK(first.stats.expansions > 0);

    q.max_expansions = static_cast<int32_t>(first.stats.expansions);
    const NavPath again = finder.search(q);
    CHECK_FALSE(again.found);
    CHECK_FALSE(again.truncated);
    CHECK(finder.last_error() == "no route to the goal column");
}

TEST_CASE("The heuristic never overestimates a route that ends in a hop down") {
    // A one-cell gap with the far side three blocks down, and nothing else to cross on: the only
    // route is a hop, which costs the jump plus the full fall rate (a descent costs `fall` a
    // block however it is paid). The estimate is 2.0 + 1.2 = 3.2 for a 3.2 route -- admissible,
    // and exact, so the search never closes the genuine route as though it were too expensive.
    // A hop that is cheaper than a drop would need this term cheaper too, and a looser estimate
    // is not free: see the expansion counts in docs/pathfinding.md.
    nt::World w;
    w.default_ground = 12;
    for (int32_t z = -6; z <= 6; ++z) {
        w.add_hole(1, z);            // the gap
        w.set_ground(2, z, 9);       // the far side, three blocks down
    }

    NavView view = nt::make_view(w, 16);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    const NavNode start{0, 12, 0};
    const NavNode goal{2, 9, 0};
    const nt::DijkstraResult ref = nt::dijkstra_cost(view, gen, start, goal);
    CHECK(ref.cost == doctest::Approx(3.2f));  // 2.0 jump + 3 x 0.4

    CHECK(finder.heuristic(start, goal) <= ref.cost + 1e-4f);

    NavQuery q;
    q.start = start;
    q.goal = goal;
    const NavPath path = finder.search(q);
    CHECK(path.found);
    CHECK(nt::path_cost(view, gen, path.nodes) == doctest::Approx(ref.cost));
}

TEST_CASE("A goal under a roof is anchored on the ground beside it") {
    // The aimed column holds the level that was asked for, but no body fits over it: a target
    // under a one-block ceiling, or -- the case that found this -- a run-away point aimed at a
    // goblin's own feet height on a hillside, whose ground is above that height with the hill
    // across the body's head. Anchoring there made the column unusable, and the search answered
    // "goal column has no navigable surface" on EVERY ask: a fleeing goblin stood still in a
    // hollow while the player killed it. The ring is asked now, and the column beside it is dry
    // ground at the same level.
    nt::World w;
    w.default_ground = 10;
    w.set_cell(0, 11, 0, CellClass::Solid);  // a roof over the aimed floor: clearance is false

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{-6, 10, 0};
    q.goal = NavNode{0, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(finder.last_error().empty());
    CHECK(path.found);
    if (!path.found) return;
    const NavNode end = path.nodes.back();
    CHECK(std::abs(end.x) + std::abs(end.z) == 1);  // beside it, not under the roof
    CHECK(end.y == 10);
}

TEST_CASE("A goal with no standable column in its ring is still refused") {
    // The ring is a best effort, not a promise: roof all nine columns and there is nowhere beside
    // the aim to stand either, which is a target the caller has to hear about rather than be
    // handed a route to somewhere near it.
    nt::World w;
    w.default_ground = 10;
    w.fill(-1, 11, -1, 3, 1, 3);  // a 3x3 roof over the whole ring

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{-6, 10, 0};
    q.goal = NavNode{0, 10, 0};
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK(finder.last_error() == "goal column has no navigable surface");
}
