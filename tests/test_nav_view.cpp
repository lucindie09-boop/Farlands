#include "doctest.h"
#include "nav_test_world.hpp"

using namespace VoxelEngine::nav;
namespace nt = navtest;

TEST_CASE("NavView resolves the standable surface of a flat column") {
    nt::World w;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(0, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->found);
    CHECK_FALSE(col->unknown);
    CHECK_FALSE(col->liquid);
    CHECK(col->clearance);
    CHECK(col->surface_cell == 9);
    CHECK(col->surface_top == doctest::Approx(10.0f));
    CHECK(view.feet_cell(col->surface_top) == 10);
}

TEST_CASE("NavView treats a slab as a standable surface") {
    nt::World w;
    w.default_ground = 10;
    w.set_cell(1, 10, 0, CellClass::Partial, 0.0f, 0.5f);  // bottom slab on the ground
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(1, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->found);
    CHECK(col->clearance);
    CHECK(col->surface_top == doctest::Approx(10.5f));
    // The body starts at 10.5, so the slab's own cell does not obstruct it.
    CHECK(view.feet_cell(col->surface_top) == 10);
}

TEST_CASE("NavView refuses columns whose chunk is not resident") {
    nt::World w;
    w.add_unresident(2, 0);
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(2, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->unknown);
    CHECK_FALSE(col->found);
    CHECK_FALSE(col->clearance);
}

TEST_CASE("NavView requires headroom for the body") {
    // A standable block above the ground is simply a higher surface — the
    // agent would stand on top of it. What actually blocks a column is a
    // collision the agent cannot stand on sitting in its body's span (a snow
    // layer under a ceiling, a low overhang).
    nt::World w;
    w.default_ground = 10;
    w.set_cell(3, 11, 0, CellClass::Partial, 0.0f, 0.25f);  // thin obstruction at chest height
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(3, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->found);
    CHECK(col->surface_top == doctest::Approx(10.0f));
    CHECK_FALSE(col->clearance);

    // A two-block-high pillar is a surface the agent stands on, not an
    // obstruction it stands beside.
    nt::World w2;
    w2.default_ground = 10;
    w2.fill(4, 10, 0, 1, 2, 1);
    NavView view2 = nt::make_view(w2, 8);
    const NavView::Column* pillar = view2.column(4, 0, 10);
    CHECK(pillar != nullptr);
    if (pillar == nullptr) return;
    CHECK(pillar->found);
    CHECK(pillar->surface_top == doctest::Approx(12.0f));
    CHECK(pillar->clearance);
}

TEST_CASE("NavView reports standing in liquid and keeps the floor below it") {
    nt::World w;
    w.default_ground = 8;
    w.set_ground(1, 0, 8);
    w.set_cell(1, 8, 0, CellClass::Liquid, 0.0f, 1.0f);
    w.set_cell(1, 9, 0, CellClass::Liquid, 0.0f, 1.0f);
    w.set_cell(1, 10, 0, CellClass::Liquid, 0.0f, 1.0f);
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(1, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->found);
    CHECK(col->liquid);          // the feet are submerged
    CHECK(col->surface_top == doctest::Approx(8.0f));  // the floor, not the water top
    CHECK(col->clearance);
}

TEST_CASE("NavView memoises per column and hint") {
    nt::World w;
    NavView view = nt::make_view(w, 8);

    (void)view.column(0, 0, 10);
    const size_t after_first = view.columns_resolved();
    CHECK(after_first == 1);

    (void)view.column(0, 0, 10);
    (void)view.column(0, 0, 10);
    CHECK(view.columns_resolved() == after_first);

    // A different hint is a different scan, so it resolves again — a scan that
    // started below a tall column cannot see its top.
    (void)view.column(0, 0, 20);
    CHECK(view.columns_resolved() == after_first + 1);
}

TEST_CASE("NavView clamps the window: sky above, unknown below") {
    nt::World w;
    w.min_y = 4;
    w.max_y = 16;
    w.default_ground = 10;
    NavView view = nt::make_view(w, 4);

    CHECK(view.sample(0, 16, 0).cls == CellClass::Air);       // at the top
    CHECK(view.sample(0, 20, 0).cls == CellClass::Air);       // above the window
    CHECK(view.sample(0, 3, 0).cls == CellClass::Unknown);    // below the window
    CHECK(view.sample(99, 10, 0).cls == CellClass::Unknown);  // outside horizontally
}

TEST_CASE("NavView only accepts a surface high enough in its cell") {
    nt::World w;
    w.default_ground = 10;
    // A layer thinner than the standable threshold must not be walkable; the
    // ground below it is.
    w.set_cell(4, 10, 0, CellClass::Partial, 0.0f, 0.25f);
    NavView view = nt::make_view(w, 8);

    const NavView::Column* col = view.column(4, 0, 10);
    CHECK(col != nullptr);
    if (col == nullptr) return;
    CHECK(col->found);
    CHECK(col->surface_top == doctest::Approx(10.0f));  // the ground, not the thin layer
}
