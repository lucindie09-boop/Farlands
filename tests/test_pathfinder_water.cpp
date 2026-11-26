#include "doctest.h"
#include "nav_test_world.hpp"
#include "pathfinding/pathfinder.hpp"

using namespace VoxelEngine::nav;
namespace nt = navtest;

// Water on the ground planner: it is the one liquid a body may be in, so it is a legal step at a
// surcharge, and the column's surface is the liquid's own top (where a swimmer rides) rather than
// the floor under it. The lava half of the same picture is here because the two are one rule: the
// refusal is on the CELL, which is why a cost can be a surcharge again.
TEST_CASE("a route wades a two-cell channel instead of the long way round") {
    // The player's own report, as a picture: two banks at 10 with a two-cell channel of water
    // between them, running the full width of the window so there is no dry way across it. Water
    // is slow, not dangerous, so the route is the crossing -- a dozen cells round the end of the
    // channel is not better than two cells in it.
    nt::World w;
    w.default_ground = 10;
    for (int32_t z = -16; z <= 16; ++z) {
        for (int32_t x = 0; x <= 1; ++x) {
            w.set_ground(x, z, 9);
            w.set_water(x, 9, z);
            w.set_water(x, 10, z);
        }
    }

    NavView view = nt::make_view(w, 16);
    Pathfinder finder(view, NavCosts{});
    MoveGenerator gen(view, NavCosts{});

    NavQuery q;
    q.start = NavNode{-6, 10, 0};
    q.goal = NavNode{6, 10, 0};
    const NavPath path = finder.search(q);

    CHECK(path.found);
    CHECK(path.nodes.size() <= 15);
    CHECK(nt::path_cost(view, gen, path.nodes) > 0.0f);
    bool waded = false;
    for (const NavNode& n : path.nodes) {
        if (n.x >= 0 && n.x <= 1) waded = true;
    }
    CHECK(waded);

    // The same channel filled with lava -- a liquid no body can be in -- is still a wall: the
    // crossing does not exist, and nothing else in the window gets the route across either.
    nt::World lava = w;
    for (int32_t z = -16; z <= 16; ++z) {
        for (int32_t x = 0; x <= 1; ++x) {
            lava.set_cell(x, 9, z, CellClass::Liquid);
            lava.set_cell(x, 10, z, CellClass::Liquid);
        }
    }
    NavView hard = nt::make_view(lava, 16);
    Pathfinder blocked(hard, NavCosts{});
    CHECK_FALSE(blocked.search(q).found);
}

TEST_CASE("a route steps out of water onto a bank at the water line, not one above it") {
    // The live trace (2026-11-25 22:01): a goblin chasing a player onto a tree ringed by flowing
    // water kept planning the same staircase and could not take its first step. The step was a
    // full block above the water's own top, and a swimmer cannot take it: the lift that carries a
    // body over a rim only exists while it is still in the liquid (pinned by "A body in water
    // cannot climb a lip a full block above the surface" in test_player_controller_water.cpp).
    // So the planner refuses that direction, and a route that would have ended in the body
    // pressing at the wall is simply not found.
    nt::World w;
    w.default_ground = 10;
    // A pond two cells deep with its surface at 10, and a three-by-three island in the middle:
    // the only way onto the island is out of the water.
    for (int32_t z = -4; z <= 4; ++z) {
        for (int32_t x = -4; x <= 4; ++x) {
            if (x >= -1 && x <= 1 && z >= -1 && z <= 1) continue;  // the island
            w.set_ground(x, z, 8);
            w.set_water(x, 8, z);
            w.set_water(x, 9, z);
        }
    }
    NavCosts costs;
    NavQuery q;
    q.start = NavNode{-3, 10, 0};

    // The island's top is the water line itself (10): the body floats up to it and steps out.
    {
        NavView view = nt::make_view(w, 16);
        Pathfinder finder(view, costs);
        q.goal = NavNode{0, 10, 0};
        const NavPath path = finder.search(q);
        CHECK(path.found);
        CHECK(!path.nodes.empty());
        if (!path.nodes.empty()) CHECK(path.nodes.back() == q.goal);
    }

    // The same island one block higher (11): every route onto it would step up out of the
    // water, so there is no route at all rather than one the body can only press at.
    nt::World raised = w;
    for (int32_t z = -1; z <= 1; ++z)
        for (int32_t x = -1; x <= 1; ++x)
            raised.set_ground(x, z, 11);
    {
        NavView view = nt::make_view(raised, 16);
        Pathfinder finder(view, costs);
        q.goal = NavNode{0, 11, 0};
        CHECK_FALSE(finder.search(q).found);
    }
}
