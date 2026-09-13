#include "doctest.h"
#include "nav_test_world.hpp"

#include <cmath>

using namespace VoxelEngine::nav;
namespace nt = navtest;

namespace {

// Finds the move from `from` that lands on (x, z), or nullptr.
const NavMove* find_move(const NavMoveList& moves, int32_t x, int32_t z) {
    for (int i = 0; i < moves.count; ++i) {
        if (moves.items[i].to.x == x && moves.items[i].to.z == z) return &moves.items[i];
    }
    return nullptr;
}

} // namespace

TEST_CASE("MoveGenerator walks level ground in eight directions") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(moves.count == 8);  // four orthogonal + four diagonal, no hops on flat ground
    for (int i = 0; i < moves.count; ++i) {
        CHECK(moves.items[i].kind == MoveKind::Walk);
        CHECK(moves.items[i].to.y == 10);
        const bool diagonal = moves.items[i].to.x != 0 && moves.items[i].to.z != 0;
        CHECK(moves.items[i].cost == doctest::Approx(diagonal ? 1.41421356f : 1.0f));
    }
}

TEST_CASE("MoveGenerator climbs a single block but refuses a two-block step") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 11);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* up = find_move(moves, 1, 0);
    CHECK(up != nullptr);
    if (up != nullptr) {
        CHECK(up->kind == MoveKind::StepUp);
        CHECK(up->cost == doctest::Approx(1.5f));  // walk + one step_up
        CHECK(up->to.y == 11);
    }

    // Two blocks of rise exceeds max_rise, so the column must not be enterable
    // at all (no step, and no hop from this side).
    nt::World w2;
    w2.default_ground = 10;
    w2.set_ground(1, 0, 12);
    NavView view2 = nt::make_view(w2, 8);
    MoveGenerator gen2(view2, NavCosts{});
    const NavView::Column* col2 = view2.column(from.x, from.z, from.y);
    CHECK(col2 != nullptr);
    if (col2 == nullptr) return;
    NavMoveList moves2;
    gen2.generate(from, *col2, moves2);
    CHECK(find_move(moves2, 1, 0) == nullptr);
    CHECK(find_move(moves2, 2, 0) == nullptr);  // cannot hop a wall either
}

TEST_CASE("MoveGenerator descends up to three blocks and refuses more") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 7);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* down = find_move(moves, 1, 0);
    CHECK(down != nullptr);
    if (down != nullptr) {
        CHECK(down->kind == MoveKind::Drop);
        CHECK(down->cost == doctest::Approx(1.0f + 3.0f * 0.4f));
        CHECK(down->to.y == 7);
    }

    // Four blocks is past max_drop: no step into it...
    nt::World w2;
    w2.default_ground = 10;
    w2.set_ground(1, 0, 6);
    NavView view2 = nt::make_view(w2, 8);
    MoveGenerator gen2(view2, NavCosts{});
    const NavView::Column* col2 = view2.column(from.x, from.z, from.y);
    CHECK(col2 != nullptr);
    if (col2 == nullptr) return;
    NavMoveList moves2;
    gen2.generate(from, *col2, moves2);
    CHECK(find_move(moves2, 1, 0) == nullptr);
    // ...but it can be hopped, since the pit is only one cell wide and the
    // landing column is level with the take-off.
    const NavMove* over = find_move(moves2, 2, 0);
    CHECK(over != nullptr);
    if (over != nullptr) {
        CHECK(over->kind == MoveKind::Hop);
        CHECK(over->cost == doctest::Approx(2.0f));
    }
}

TEST_CASE("MoveGenerator blocks a diagonal that would clip a corner") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 20);  // a wall directly east of the origin
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 1) == nullptr);   // clipped by the east column
    CHECK(find_move(moves, 1, -1) == nullptr);
    CHECK(find_move(moves, 0, 1) != nullptr);   // the open side still works
}

TEST_CASE("MoveGenerator hops a one-cell gap") {
    nt::World w;
    w.default_ground = 10;
    w.add_hole(1, 0);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 0) == nullptr);  // nothing to stand on in the gap
    const NavMove* hop = find_move(moves, 2, 0);
    CHECK(hop != nullptr);
    if (hop != nullptr) {
        CHECK(hop->kind == MoveKind::Hop);
        CHECK(hop->cost == doctest::Approx(2.0f));
        CHECK(hop->to.y == 10);
    }
}

TEST_CASE("MoveGenerator never enters or crosses unresident chunks") {
    nt::World w;
    w.default_ground = 10;
    w.add_unresident(1, 0);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 0) == nullptr);
    // The hop needs the body to pass over the unknown cell, so it is refused
    // rather than treated as empty space.
    CHECK(find_move(moves, 2, 0) == nullptr);
}

TEST_CASE("MoveGenerator charges extra for wading through liquid") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 9);
    w.set_cell(1, 9, 0, CellClass::Liquid);
    w.set_cell(1, 10, 0, CellClass::Liquid);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    const NavView::Column* wet = view.column(1, 0, 10);
    CHECK(wet != nullptr);
    if (wet == nullptr) return;
    CHECK(wet->liquid);

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* into_water = find_move(moves, 1, 0);
    CHECK(into_water != nullptr);
    if (into_water != nullptr) {
        CHECK(into_water->kind == MoveKind::Drop);
        CHECK(into_water->cost == doctest::Approx(1.0f + 0.4f + 0.6f));
    }
}
