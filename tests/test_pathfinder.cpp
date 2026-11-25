#include "doctest.h"
#include "nav_test_world.hpp"
#include "pathfinding/pathfinder.hpp"

#include <algorithm>

using namespace VoxelEngine::nav;
namespace nt = navtest;

TEST_CASE("Pathfinder walks a straight line across level ground") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 32);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{20, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    CHECK_FALSE(path.truncated);
    CHECK(path.nodes.size() == 21);
    CHECK(path.nodes.front() == q.start);
    CHECK(path.nodes.back() == q.goal);
    CHECK(nt::path_cost(view, gen, path.nodes) == doctest::Approx(20.0f));
    for (const NavNode& n : path.nodes) CHECK(n.z == 0);
    // An exact heuristic on open ground keeps the expansion count tiny.
    CHECK(path.stats.expansions <= 60);
}

TEST_CASE("Pathfinder routes through a gap instead of climbing the wall") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -20; z <= 20; ++z) w.set_ground(10, z, 12);
    w.set_ground(10, 0, 10);  // the doorway

    NavView view = nt::make_view(w, 32);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{20, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    const bool through_door = std::any_of(path.nodes.begin(), path.nodes.end(),
        [](const NavNode& n) { return n.x == 10 && n.z == 0; });
    CHECK(through_door);
}

TEST_CASE("Pathfinder descends a trench and climbs back out one block at a time") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -8; z <= 8; ++z) {
        w.set_ground(5, z, 9);
        w.set_ground(6, z, 8);
        w.set_ground(7, z, 9);
    }
    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{12, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    bool crossed_trench = false;
    int32_t deepest = 99;
    for (const NavNode& n : path.nodes) {
        if (n.x >= 5 && n.x <= 7) crossed_trench = true;
        deepest = std::min(deepest, n.y);
    }
    CHECK(crossed_trench);
    CHECK(deepest == 8);  // it walks the bottom of the trench
    // Every step in and out is a legal single drop / step up.
    CHECK(nt::path_cost(view, gen, path.nodes) > 0.0f);
}

TEST_CASE("Pathfinder reports an unreachable goal as no route") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t x = 18; x <= 22; ++x)
        for (int32_t z = -2; z <= 2; ++z)
            w.set_ground(x, z, 12);
    w.set_ground(20, 0, 10);  // a two-deep well, unreachable from level ground

    NavView view = nt::make_view(w, 32);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{20, 10, 0};
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK_FALSE(path.truncated);
    CHECK(path.stats.budget_exhausted == false);
    CHECK(finder.last_error() == "no route to the goal column");
    CHECK_FALSE(path.nodes.empty());
    CHECK(path.nodes.front() == q.start);
}

TEST_CASE("Pathfinder truncates to a best-effort run when the budget runs out") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 64);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{60, 10, 0};
    q.max_expansions = 1;
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK(path.truncated);
    CHECK(path.stats.budget_exhausted);
    CHECK_FALSE(path.stats.time_exhausted);
    CHECK_FALSE(path.nodes.empty());
    CHECK(path.nodes.front() == q.start);
    CHECK(finder.last_error() == "expansion budget exhausted");
}

TEST_CASE("A route cut short by the budget is still a legal chain of moves") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -8; z <= 4; ++z) w.set_ground(5, z, 12);  // wall, gap above z=4
    for (int32_t z = -4; z <= 8; ++z) w.set_ground(10, z, 12);

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    // A handful of expansions into a maze: the run is cut off, but what it
    // returns has to be walkable from where the agent is standing.
    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{15, 10, 0};
    q.max_expansions = 4;
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK(path.truncated);
    CHECK(path.nodes.size() > 1);
    CHECK(path.nodes.front() == q.start);
    CHECK(nt::path_cost(view, gen, path.nodes) > 0.0f);
}

TEST_CASE("Pathfinder climbs to a goal above a walkable space instead of stopping under it") {
    // A staircase of floating one-block steps over open ground. The goal column
    // holds two standable surfaces: the step, and the ground under it. A goal
    // test that only compares columns declares victory on the ground, twenty
    // blocks below the target, which is what "it refuses to climb the stairs"
    // looks like from in game.
    nt::World w;
    w.default_ground = 1;
    constexpr int32_t kSteps = 20;
    for (int32_t k = 1; k <= kSteps; ++k) {
        w.set_cell(k, k - 1, 0, CellClass::Solid, 0.0f, 1.0f);
    }

    NavView view = nt::make_view(w, 32);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 1, 0};  // at the foot of the flight
    q.goal = NavNode{kSteps, kSteps, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    CHECK(path.nodes.front() == q.start);
    CHECK(path.nodes.back() == q.goal);
    int32_t peak = 0;
    for (const NavNode& n : path.nodes) peak = std::max(peak, n.y);
    CHECK(peak == kSteps);
    CHECK(nt::path_cost(view, gen, path.nodes) > 0.0f);

    // Now start beside the middle of the flight, on the ground. Walking to the
    // goal's column at ground level is now far cheaper than going back down to
    // the bottom and climbing — so a goal test that only compares columns is
    // satisfied nineteen blocks below the target.
    NavQuery beside = q;
    beside.start = NavNode{kSteps / 2, 1, 2};

    NavView beside_view = nt::make_view(w, 32);
    Pathfinder beside_finder(beside_view, NavCosts{});
    const NavPath beside_path = beside_finder.search(beside);

    CHECK(beside_path.found);
    CHECK(beside_path.nodes.back() == beside.goal);

    // The brute-force reference has to agree, on the same graph with the same
    // costs. This is the case that pins ITS goal rule: a Dijkstra that accepted
    // the goal's column alone stops on the ground under the flight, nineteen
    // blocks below the target, and reports a far cheaper "optimum" than the only
    // real route — which would make the optimality comparisons above and below
    // meaningless, and would fail a correct planner.
    MoveGenerator beside_gen(beside_view, NavCosts{});
    const float beside_cost = nt::path_cost(beside_view, beside_gen, beside_path.nodes);
    CHECK(beside_cost > 0.0f);
    const nt::DijkstraResult beside_best =
        nt::dijkstra_cost(beside_view, beside_gen, beside.start, beside.goal);
    CHECK(beside_best.cost > 0.0f);
    CHECK(beside_cost == doctest::Approx(beside_best.cost).epsilon(1e-3f));
}

TEST_CASE("Pathfinder returns a usable partial route when the clock runs out") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 64);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{60, 10, 0};
    // 100 nanoseconds: the budget is already spent by the time the search has
    // resolved its two endpoint columns, so it stops before its first expansion.
    q.max_ms = 1e-4;
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK(path.truncated);
    CHECK(path.stats.time_exhausted);
    CHECK_FALSE(path.stats.budget_exhausted);
    // Still something the caller can use: it starts where the agent stands.
    CHECK_FALSE(path.nodes.empty());
    CHECK(path.nodes.front() == q.start);
    CHECK(finder.last_error() == "time budget exhausted");
}

TEST_CASE("A budget the search never reaches leaves the route untouched") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -12; z <= 12; ++z) w.set_ground(6, z, 12);
    w.set_ground(6, 0, 10);

    NavQuery unbudgeted;
    unbudgeted.start = NavNode{0, 10, 0};
    unbudgeted.goal = NavNode{14, 10, 0};

    NavQuery budgeted = unbudgeted;
    budgeted.max_ms = 5000.0;

    NavView view_a = nt::make_view(w, 24);
    Pathfinder finder_a(view_a, NavCosts{});
    const NavPath path_a = finder_a.search(unbudgeted);

    NavView view_b = nt::make_view(w, 24);
    Pathfinder finder_b(view_b, NavCosts{});
    const NavPath path_b = finder_b.search(budgeted);

    CHECK(path_a.found);
    CHECK(path_b.found);
    CHECK_FALSE(path_b.truncated);
    CHECK_FALSE(path_b.stats.time_exhausted);
    CHECK(path_a.nodes.size() == path_b.nodes.size());
    if (path_a.nodes.size() == path_b.nodes.size()) {
        for (size_t i = 0; i < path_a.nodes.size(); ++i) {
            CHECK(path_a.nodes[i] == path_b.nodes[i]);
        }
    }
}

TEST_CASE("A target standing past the lip of a shelf is reached beside it, not under it") {
    // The chasing player stands at the very edge of a three-block shelf, so their centre is in
    // the column past the lip: that column's ground is the plain three blocks below them. A goal
    // anchored there is a cell an agent can walk to and stand on, so the search calls the route
    // `found`, the goblin walks to it — and parks three blocks under the player it is chasing,
    // with the only way up (a staircase off the direct line) never asked for. The ground the
    // target is actually standing on is in the neighbouring column, and that is the goal.
    nt::World w;
    w.default_ground = 10;
    for (int32_t x = -6; x <= 0; ++x)
        for (int32_t z = -6; z <= 4; ++z) w.set_ground(x, z, 13);  // the shelf
    w.set_ground(-1, -7, 11);  // the way up: three one-block steps, off the direct line
    w.set_ground(-2, -7, 12);
    w.set_ground(-3, -7, 13);
    // A column beside the target one level HIGHER than their feet: a neighbour is only the
    // answer when its ground puts a body's feet in the cell the caller aimed at.
    w.set_ground(1, -1, 14);

    NavView view = nt::make_view(w, 32);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{3, 10, -8};  // on the plain, below and beyond the lip
    q.goal = NavNode{1, 13, 0};    // the player's own feet cell, past the lip
    const NavPath path = finder.search(q);

    CHECK(path.found);
    // The arrival is on the shelf, at the target's own level and a step from their cell — not
    // the plain under it.
    CHECK(path.nodes.back().y == 13);
    CHECK(std::abs(path.nodes.back().x - q.goal.x) <= 1);
    CHECK(std::abs(path.nodes.back().z - q.goal.z) <= 1);
    // ...and it got there the long way, over the steps off the direct line.
    const bool climbed = std::any_of(path.nodes.begin(), path.nodes.end(),
        [](const NavNode& n) { return n.z <= -7 && n.y >= 11; });
    CHECK(climbed);
    CHECK(nt::path_cost(view, gen, path.nodes) > 0.0f);
}

TEST_CASE("A route is measured from the floor the agent stands on, not the roof over its head") {
    // A sealed two-block-high corridor: free at y = 10 and 11, ceiling cells at y = 12, walls at
    // both ends. The target is on the corridor's own roof, which nothing inside it can reach.
    //
    // An expansion used to resolve a node's column with `column`, which looks up to `max_rise`
    // ABOVE the feet: the ceiling at y = 12 then answered as that column's floor (top 13), so the
    // search measured its steps from the roof, stepped onto it through the ceiling and called the
    // target reachable. In a live session the same fault showed as routes carrying a three-block
    // rise in a single move, and a body that walked into walls and hopped at one-block gaps it
    // cannot fit through.
    nt::World w;
    w.default_ground = 10;
    for (int32_t x = -3; x <= 4; ++x)
        for (int32_t z = -2; z <= 2; ++z) w.set_cell(x, 12, z, CellClass::Solid);  // the ceiling
    for (int32_t z = -2; z <= 2; ++z) {
        w.fill(-4, 10, z, 1, 3, 1);  // the end walls, up to and including the ceiling's level
        w.fill(5, 10, z, 1, 3, 1);
    }
    for (int32_t x = -4; x <= 5; ++x) {
        w.fill(x, 10, -3, 1, 3, 1);  // the side walls
        w.fill(x, 10, 3, 1, 3, 1);
    }

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};  // standing in the corridor
    q.goal = NavNode{0, 13, 0};   // on its roof
    const NavPath path = finder.search(q);

    CHECK_FALSE(path.found);
    CHECK(finder.last_error() == "no route to the goal column");
    const bool stayed_on_the_floor = std::all_of(path.nodes.begin(), path.nodes.end(),
        [](const NavNode& n) { return n.y == 10; });
    CHECK(stayed_on_the_floor);
}

TEST_CASE("Pathfinder refuses to start or finish on a broken column") {
    nt::World w;
    w.default_ground = 10;
    w.add_unresident(4, 0);
    NavView view = nt::make_view(w, 16);
    Pathfinder finder(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{4, 10, 0};
    const NavPath path = finder.search(q);
    CHECK_FALSE(path.found);
    CHECK(finder.last_error() == "goal column has no navigable surface");

    NavQuery q2;
    q2.start = NavNode{4, 10, 0};
    q2.goal = NavNode{0, 10, 0};
    const NavPath path2 = finder.search(q2);
    CHECK_FALSE(path2.found);
    CHECK(finder.last_error() == "start column has no navigable surface");
}

TEST_CASE("Pathfinder returns an identical route for identical queries") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -12; z <= 12; ++z) w.set_ground(6, z, 12);
    w.set_ground(6, 0, 10);

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{14, 10, 0};

    NavView view_a = nt::make_view(w, 24);
    Pathfinder finder_a(view_a, NavCosts{});
    const NavPath path_a = finder_a.search(q);

    NavView view_b = nt::make_view(w, 24);
    Pathfinder finder_b(view_b, NavCosts{});
    const NavPath path_b = finder_b.search(q);

    CHECK(path_a.found);
    CHECK(path_b.found);
    CHECK(path_a.nodes.size() == path_b.nodes.size());
    if (path_a.nodes.size() == path_b.nodes.size()) {
        for (size_t i = 0; i < path_a.nodes.size(); ++i) {
            CHECK(path_a.nodes[i] == path_b.nodes[i]);
        }
    }
}

TEST_CASE("Pathfinder matches brute-force Dijkstra over a winding maze") {
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -8; z <= 4; ++z) w.set_ground(5, z, 12);    // wall, gap above z=4
    for (int32_t z = -4; z <= 8; ++z) w.set_ground(10, z, 12);   // wall, gap below z=-4

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{15, 10, 0};
    q.max_expansions = 200000;
    const NavPath path = finder.search(q);

    CHECK(path.found);
    const float astar_cost = nt::path_cost(view, gen, path.nodes);
    CHECK(astar_cost > 0.0f);

    const nt::DijkstraResult best = nt::dijkstra_cost(view, gen, q.start, q.goal);
    CHECK(best.cost > 0.0f);
    CHECK(astar_cost == doctest::Approx(best.cost).epsilon(1e-3f));
}

TEST_CASE("Pathfinder matches brute-force Dijkstra under a rescaled cost table") {
    // The search's estimate of the remaining distance has to be expressed in the
    // cost table's own units. With the orthogonal unit hardcoded to 1.0, a table
    // whose walk term is smaller makes every estimate a multiple of the true
    // distance — an overestimate, which costs A* its optimality while still
    // returning a legal route. Comparing against Dijkstra on the same graph and
    // the same costs is what catches that.
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -8; z <= 4; ++z) w.set_ground(5, z, 12);    // wall, gap above z=4
    for (int32_t z = -4; z <= 8; ++z) w.set_ground(10, z, 12);   // wall, gap below z=-4

    NavCosts costs;
    costs.walk = 0.25f;
    costs.diagonal = 0.35f;

    NavView view = nt::make_view(w, 24);
    Pathfinder finder(view, costs);
    MoveGenerator gen(view, costs);

    NavQuery q;
    q.start = NavNode{0, 10, 0};
    q.goal = NavNode{15, 10, 0};
    q.max_expansions = 200000;
    const NavPath path = finder.search(q);

    CHECK(path.found);
    const float astar_cost = nt::path_cost(view, gen, path.nodes);
    CHECK(astar_cost > 0.0f);

    const nt::DijkstraResult best = nt::dijkstra_cost(view, gen, q.start, q.goal);
    CHECK(best.cost > 0.0f);
    CHECK(astar_cost == doctest::Approx(best.cost).epsilon(1e-3f));
}

TEST_CASE("Pathfinder snaps both ends onto their column surfaces") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 16);
    Pathfinder finder(view, NavCosts{});

    // A goal handed in mid-air (a falling entity, a target above ground) still
    // resolves to the column's surface.
    NavQuery q;
    q.start = NavNode{0, 25, 0};
    q.goal = NavNode{6, 18, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    CHECK(path.nodes.front().y == 10);
    CHECK(path.nodes.back().y == 10);
    CHECK(path.nodes.back().x == 6);
}
