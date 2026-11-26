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

TEST_CASE("MoveGenerator refuses to hop a gap that is liquid") {
    // A liquid column has no body space over it, so it looked exactly like a one-cell gap and a
    // route was found across a channel of lava. A goblin took that route, jumped, and stood in
    // the lava -- and because a route existed it never asked for a crossing to be built.
    nt::World w;
    w.default_ground = 10;
    w.add_hole(1, 0);
    w.set_cell(1, 10, 0, CellClass::Liquid);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 0) == nullptr);  // not into it
    CHECK(find_move(moves, 2, 0) == nullptr);  // and not over it
}

TEST_CASE("MoveGenerator refuses a diagonal squeeze between two blocking columns") {
    // A corner no 0.6-wide body fits through: two columns that both stop it at its own height,
    // meeting diagonally. That move is refused, and the search goes round — while the same corner
    // with a wall on only one side is a corner a body cuts (below).
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 14);  // a wall directly east of the origin
    w.set_ground(0, 1, 14);  // ...and one north of it: the squeeze
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 1) == nullptr);   // between the two of them
    CHECK(find_move(moves, 1, -1) != nullptr);  // one wall only: the room on the other side
    CHECK(find_move(moves, 0, 1) == nullptr);   // and the wall itself is not a step
}

TEST_CASE("MoveGenerator passes a corner with a wall on one side and a hole on the other") {
    // The report, in cells: 1 is a wall of three blocks at the corner, 2 and 3 are ground, 4 is a
    // hole. The player crosses 2 -> 3 across it — his feet on the two columns he is crossing while
    // his body slants past the wall and over the hole, carried by the cells his box still overlaps —
    // and the goblin would not: the old rule required both beside-columns to be walkable COLUMNS,
    // and a hole never is. The beside columns are the ones a body slants THROUGH, so what they owe
    // it is space at the move's height, and the second half of this is the walker's: it slides along
    // the open side and refuses a foot it could not stand on.
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 14);  // 1: the wall
    w.add_hole(0, 1);        // 4: the hole
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};  // 2
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* across = find_move(moves, 1, 1);  // to 3
    CHECK(across != nullptr);
    if (across != nullptr) {
        CHECK(across->kind == MoveKind::Walk);
        CHECK(across->to.y == 10);
        CHECK(across->cost == doctest::Approx(1.41421356f));
    }
    // ...and the hole is still a hole: a straight step into it has nothing to stand on.
    CHECK(find_move(moves, 0, 1) == nullptr);
}

TEST_CASE("MoveGenerator refuses a diagonal over a lava corner, and allows the same corner in water") {
    // A wall beside a pit of lava is not the wall-beside-a-hole case. Liquids do not stop a body, so
    // the body's own space says the corner is clear and a route would slant out over the hazard;
    // the refusal is on the CELL, at the move's height and under it, which is the same refusal the
    // plain steps make. Water is the liquid a body may be in, so that corner stays a crossing.
    for (const bool lava : {true, false}) {
        nt::World w;
        w.default_ground = 10;
        w.set_ground(1, 0, 14);  // a wall at one side of the corner
        for (int32_t y = 9; y <= 10; ++y) {
            if (lava) w.set_cell(0, y, 1, CellClass::Liquid);
            else w.set_water(0, y, 1);
        }
        NavView view = nt::make_view(w, 8);
        MoveGenerator gen(view, NavCosts{});

        const NavNode from{0, 10, 0};
        const NavView::Column* col = view.column(from.x, from.z, from.y);
        CHECK(col != nullptr);
        if (col == nullptr) continue;

        NavMoveList moves;
        gen.generate(from, *col, moves);
        const NavMove* across = find_move(moves, 1, 1);
        if (lava) {
            CHECK(across == nullptr);
        } else {
            CHECK(across != nullptr);
        }
    }
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

TEST_CASE("MoveGenerator will not hop a gap that climbs") {
    // A hop is pressed off the lip and has to carry the body across while it is airborne, and
    // any rise spends that clearance instead: measured on the sim the goblin walks with, a hop
    // up one block lands against the far wall and drops into the gap at every launch distance at
    // its own pace. So a rising gap has no hop across it -- the search reaches that ground with
    // a step up beside it and goes round.
    nt::World w;
    w.default_ground = 10;
    w.add_hole(1, 0);
    w.set_ground(2, 0, 11);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 2, 0) == nullptr);
}

TEST_CASE("MoveGenerator hops a gap that drops, and charges the descent at the plain rate") {
    nt::World w;
    w.default_ground = 10;
    w.add_hole(1, 0);
    w.set_ground(2, 0, 9);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* over = find_move(moves, 2, 0);
    CHECK(over != nullptr);
    if (over != nullptr) {
        CHECK(over->kind == MoveKind::Hop);
        CHECK(over->cost == doctest::Approx(2.4f));  // 2.0 jump + one block of fall
        CHECK(over->to.y == 9);
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

TEST_CASE("MoveGenerator refuses to walk into liquid, and still charges for leaving it") {
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

    // A cost is not a wall. Charging extra for a wet cell was read by the search as "wade
    // anyway" whenever the dry way round was more than half a step longer, so a body with no
    // swim walked into lava and the sea; the cell is simply not a place to walk now.
    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 0) == nullptr);

    // Leaving liquid is still legal, and still carries the wading surcharge: a body that is
    // already in it has to be able to get out.
    const NavNode wet_feet{1, view.feet_cell(wet->surface_top), 0};
    const NavView::Column* wet_col = view.column(wet_feet.x, wet_feet.z, wet_feet.y);
    CHECK(wet_col != nullptr);
    if (wet_col == nullptr) return;
    NavMoveList out;
    gen.generate(wet_feet, *wet_col, out);
    CHECK(out.count > 0);  // the way out is not closed by the way in being shut
    const NavMove* onto_land = find_move(out, -1, 0);
    if (onto_land != nullptr) {
        CHECK(onto_land->cost >= NavCosts{}.walk + NavCosts{}.liquid - 1e-4f);
    }
}

TEST_CASE("MoveGenerator wades water, and only water") {
    nt::World w;
    w.default_ground = 10;
    w.set_ground(1, 0, 9);
    w.set_water(1, 9, 0);
    w.set_water(1, 10, 0);
    NavView view = nt::make_view(w, 8);
    MoveGenerator gen(view, NavCosts{});

    const NavNode from{0, 10, 0};
    const NavView::Column* col = view.column(from.x, from.z, from.y);
    CHECK(col != nullptr);
    if (col == nullptr) return;

    NavMoveList moves;
    gen.generate(from, *col, moves);
    const NavMove* wet = find_move(moves, 1, 0);
    CHECK(wet != nullptr);
    if (wet == nullptr) return;
    // The step lands on the pond's SURFACE (feet cell 11 over water cells at 9 and 10), which is
    // where a swimmer's feet are -- and it carries the wading surcharge on top of the climb.
    CHECK(wet->to.y == 11);
    CHECK(wet->kind == MoveKind::StepUp);
    CHECK(wet->cost >= NavCosts{}.walk + NavCosts{}.liquid - 1e-4f);
}

TEST_CASE("MoveGenerator still refuses a liquid with no body in it") {
    // The same pond, filled with lava: the cell is a wall, whatever the surcharge would say.
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
    NavMoveList moves;
    gen.generate(from, *col, moves);
    CHECK(find_move(moves, 1, 0) == nullptr);
}
