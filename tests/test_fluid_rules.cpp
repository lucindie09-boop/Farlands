#include "doctest.h"
#include "fluid_test_world.hpp"
#include "fluids/fluid_rules.hpp"

#include <cstdio>
#include <string>

using fluid_test::FluidTestWorld;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidStep;

// -----------------------------------------------------------------------------
// The scenario table.
//
// Every expectation below is grounded in the rule it comes from (see the header
// for the numbered list), and every scenario is run through BOTH drivers — the
// slow "recompute everything until quiet" sweep and the queued one — because
// the interesting failure is the two disagreeing, which means the queue missed
// a wake-up.
// -----------------------------------------------------------------------------

namespace {

// This build compiles doctest with exceptions off, which turns INFO/CAPTURE and
// the *_MESSAGE assertion forms into no-ops — so a failing structural assertion
// prints nothing but the expression. This dumps the layer the expectation is
// about instead, right before the failing CHECK line.
bool expect(bool ok, const char* what, const FluidTestWorld& w, int y) {
    if (!ok) {
        std::printf("\nexpected %s here, but the layer is:%s", what, w.layer(y).c_str());
    }
    return ok;
}    // Run the scene through both drivers and report the first disagreement, or an

// empty string when they agree everywhere.
std::string drivers_agree(const FluidTestWorld& scene) {
    FluidTestWorld by_sweep = scene;
    FluidTestWorld by_queue = scene;
    if (by_sweep.settle_sweep() < 0) return "the sweep driver never settled";
    if (by_queue.settle_queue() < 0) return "the queue driver never drained";
    const std::string diff = by_sweep.first_difference(by_queue);
    if (!diff.empty()) return "the drivers disagree at " + diff;
    return std::string();
}

// The fixture's sweep, visiting cells from the far corner inward instead. Only
// here to show the fixpoint is not an artefact of one traversal order.
int settle_sweep_reverse(FluidTestWorld& w) {
    for (int round = 0; round < 400; ++round) {
        bool changed = false;
        for (int y = w.size_y() - 1; y >= 0; --y) {
            for (int z = w.size_z() - 1; z >= 0; --z) {
                for (int x = w.size_x() - 1; x >= 0; --x) {
                    if (!w.at(x, y, z).present()) continue;
                    const FluidStep step = VoxelEngine::fluids::tick(w, x, y, z);
                    if (w.apply_step(step, x, y, z)) changed = true;
                }
            }
        }
        if (!changed) return round + 1;
    }
    return -1;
}

// One source on a flat floor, nothing else: the plainest possible scene.
FluidTestWorld plane_with_source() {
    FluidTestWorld w(20, 5, 20);
    w.set_floor(0);
    w.set_water(10, 1, 10);
    return w;
}

// A three-high plateau with a source on top, so the water has to run to the
// edge, fall, and land. Solid occupies x 0..5 at y 1..3.
FluidTestWorld waterfall() {
    FluidTestWorld w(14, 6, 14);
    w.set_floor(0);
    for (int z = 0; z < 14; ++z) {
        for (int x = 0; x <= 5; ++x) {
            w.set_solid(x, 1, z);
            w.set_solid(x, 2, z);
            w.set_solid(x, 3, z);
        }
    }
    w.set_water(4, 4, 6);
    return w;
}

// Two sources with exactly one cell between them.
FluidTestWorld two_sources() {
    FluidTestWorld w(15, 3, 15);
    w.set_floor(0);
    w.set_water(5, 1, 7);
    w.set_water(7, 1, 7);
    return w;
}

// A source four steps from a hole in the floor. Nothing else about the scene
// distinguishes the four directions, so anything that spreads away from the hole
// spread there for no reason.
FluidTestWorld hole_scene() {
    FluidTestWorld w(16, 4, 16);
    w.set_floor(0);
    w.set_open(9, 0, 10);
    w.set_water(5, 1, 10);
    return w;
}

// A source and a wall it has to stop against.
FluidTestWorld wall_scene() {
    FluidTestWorld w(14, 3, 14);
    w.set_floor(0);
    for (int z = 0; z < 14; ++z) {
        w.set_solid(6, 1, z);
        w.set_solid(6, 2, z);
    }
    w.set_water(3, 1, 7);
    return w;
}

} // namespace

TEST_CASE("fluid: the state encoding says what the rules read") {
    const FluidCell source{ FluidKind::Water, 0, false };
    const FluidCell falling{ FluidKind::Water, 0, true };
    const FluidCell runoff{ FluidKind::Water, 3, false };
    const FluidCell empty{};

    CHECK(source.is_source());
    CHECK_FALSE(falling.is_source());
    CHECK_FALSE(runoff.is_source());
    CHECK_FALSE(empty.present());

    // A source and a falling cell both supply a neighbour at full strength; a
    // runoff cell supplies its distance from a source.
    CHECK(source.feed_cost() == 0);
    CHECK(falling.feed_cost() == 0);
    CHECK(runoff.feed_cost() == 3);

    // Falling is one state, not eight: its depth is discarded, not compared.
    CHECK(FluidCell{ FluidKind::Water, 0, true } == FluidCell{ FluidKind::Water, 0, true });
    CHECK(FluidCell{ FluidKind::Water, 0, true } != FluidCell{ FluidKind::Water, 0, false });
    CHECK(FluidCell{ FluidKind::Water, 2, false } != FluidCell{ FluidKind::Water, 3, false });

    // Every real kind carries its own numbers; a kind with no traits is inert,
    // and ticking one must not invent behaviour for it.
    const VoxelEngine::fluids::FluidTraits* water = VoxelEngine::fluids::traits_for(FluidKind::Water);
    const VoxelEngine::fluids::FluidTraits* lava = VoxelEngine::fluids::traits_for(FluidKind::Lava);
    const VoxelEngine::fluids::FluidTraits* acid = VoxelEngine::fluids::traits_for(FluidKind::Acid);
    CHECK(water != nullptr);
    CHECK(lava != nullptr);
    CHECK(acid != nullptr);
    CHECK(VoxelEngine::fluids::traits_for(FluidKind::None) == nullptr);

    // Lava is water's slow, shallow cousin: less reach, and it creeps (a second
    // between one cell and the next, against water's quarter second).
    CHECK(lava->max_depth < water->max_depth);
    CHECK(lava->tick_delay > water->tick_delay);
    CHECK(lava->search_distance < water->search_distance);
    CHECK(lava->max_depth == 3);
    CHECK(lava->tick_delay == 20);

    // Acid keeps water's depth and moves sooner, and it hunts a drop one step
    // further out than water does — a substance that runs for the drain.
    CHECK(acid->max_depth == water->max_depth);
    CHECK(acid->tick_delay < water->tick_delay);
    CHECK(acid->search_distance > water->search_distance);

    // ...and it is the one substance that does not pool from a pair of side
    // sources, so a splash spends itself instead of becoming a spring.
    CHECK(water->sources_pair_into_source);
    CHECK(lava->sources_pair_into_source);
    CHECK_FALSE(acid->sources_pair_into_source);
}

// A substance's own numbers have to reach the world, not just the struct: one
// source on flat ground, settled, is the whole picture.
TEST_CASE("fluid: lava stops three cells out and acid reaches as far as water") {
    FluidTestWorld lava_world(20, 5, 20);
    lava_world.set_floor(0);
    lava_world.set_cell_fluid(10, 1, 10, FluidCell{ FluidKind::Lava, 0, false });
    CHECK(lava_world.settle_sweep() > 0);

    CHECK(lava_world.at(10, 1, 10).is_source());
    for (int d = 1; d <= 3; ++d) {
        CHECK(lava_world.at(10 + d, 1, 10).depth == d);
        CHECK(lava_world.at(10, 1, 10 + d).depth == d);
    }
    // Nothing at distance four, and nothing on the diagonal past where its depth
    // budget runs out: a diamond of radius three.
    CHECK_FALSE(lava_world.at(14, 1, 10).present());
    CHECK_FALSE(lava_world.at(13, 1, 11).present());
    CHECK(lava_world.count_fluid() == 25);
    CHECK(drivers_agree(lava_world) == std::string());

    FluidTestWorld acid_world(20, 5, 20);
    acid_world.set_floor(0);
    acid_world.set_cell_fluid(10, 1, 10, FluidCell{ FluidKind::Acid, 0, false });
    CHECK(acid_world.settle_sweep() > 0);

    // Seven, the same as water — only the clock differs.
    CHECK(acid_world.at(17, 1, 10).depth == 7);
    CHECK_FALSE(acid_world.at(18, 1, 10).present());
    CHECK(acid_world.count_fluid() == 113);
    CHECK(drivers_agree(acid_world) == std::string());
}

// The two halves of the pooling rule, side by side: the same scene, one
// substance apart.
TEST_CASE("fluid: acid does not pool from a pair of sources where water and lava do") {
    auto two_sources_of = [](FluidKind kind) {
        FluidTestWorld w(15, 3, 15);
        w.set_floor(0);
        w.set_cell_fluid(5, 1, 7, FluidCell{ kind, 0, false });
        w.set_cell_fluid(7, 1, 7, FluidCell{ kind, 0, false });
        return w;
    };

    FluidTestWorld water = two_sources_of(FluidKind::Water);
    CHECK(water.settle_sweep() > 0);
    CHECK(water.at(6, 1, 7).is_source());

    FluidTestWorld lava = two_sources_of(FluidKind::Lava);
    CHECK(lava.settle_sweep() > 0);
    CHECK(lava.at(6, 1, 7).is_source());

    // Acid's gap cell is fed at depth 1 from both sides and stays that: runoff,
    // not a spring. Two buckets of acid do not make a third.
    FluidTestWorld acid = two_sources_of(FluidKind::Acid);
    CHECK(acid.settle_sweep() > 0);
    CHECK_FALSE(acid.at(6, 1, 7).is_source());
    CHECK(acid.at(6, 1, 7).depth == 1);
    CHECK(drivers_agree(acid) == std::string());
}

TEST_CASE("fluid: one source spreads rings and stops at its maximum depth") {
    FluidTestWorld w = plane_with_source();
    CHECK(w.settle_sweep() > 0);

    // The source stands.
    CHECK(w.at(10, 1, 10).is_source());

    // Depth is the distance travelled, along each axis and around the corner
    // (a diagonal is two steps, so it is two deep at one out).
    for (int d = 1; d <= 7; ++d) {
        CHECK(w.at(10 + d, 1, 10).depth == d);
        CHECK(w.at(10, 1, 10 + d).depth == d);
    }
    CHECK(w.at(11, 1, 11).depth == 2);

    // Nothing exists at distance 8: a cell at the maximum depth cannot spread
    // further, so the fluid has an edge instead of growing forever.
    CHECK_FALSE(w.at(10, 1, 18).present());
    CHECK_FALSE(w.at(18, 1, 10).present());

    // Which is exactly a diamond of radius 7: 1 + 4*(1+2+...+7).
    CHECK(w.count_fluid() == 113);

    // Same answer through the queued driver (the comparison prints the first
    // disagreement, because a message macro would be compiled out here).
    CHECK(drivers_agree(plane_with_source()) == std::string());
}

TEST_CASE("fluid: a falling column stays full strength and lands at full strength") {
    FluidTestWorld w = waterfall();
    CHECK(w.settle_sweep() > 0);

    CHECK(w.at(4, 4, 6).is_source());
    // Two steps out along the plateau top the water is still runnning.
    CHECK(w.at(5, 4, 6).depth == 1);
    CHECK(w.at(6, 4, 6).depth == 2);

    // Rule 4: the column below is FALLING at every level, not thinning.
    CHECK(w.at(6, 3, 6).falling);
    CHECK(w.at(6, 2, 6).falling);
    CHECK(w.at(6, 1, 6).falling);

    // Rule 7: where it lands it fans out at depth 1, not at depth 8 — a falling
    // cell supplies its neighbours at full strength.
    CHECK(w.at(7, 1, 6).depth == 1);
    CHECK(w.at(6, 1, 5).depth == 1);
    CHECK(w.at(6, 1, 7).depth == 1);

    // ...but not into the plateau, which is where the column came off.
    CHECK(w.is_solid(5, 1, 6));
    CHECK_FALSE(w.at(5, 1, 6).present());

    // And nothing anywhere got inside a solid.
    for (int y = 0; y < w.size_y(); ++y) {
        for (int z = 0; z < w.size_z(); ++z) {
            for (int x = 0; x < w.size_x(); ++x) {
                if (w.is_solid(x, y, z)) CHECK_FALSE(w.at(x, y, z).present());
            }
        }
    }

    CHECK(drivers_agree(waterfall()) == std::string());
}

TEST_CASE("fluid: fluid hanging under a supply stays full strength instead of drying up") {
    // The narrowest case of the ordering inside a tick: a cell whose ONLY supply
    // is the fluid above it. If the dry-up verdict were final, a column would
    // delete itself on the way down — and worse, the source would immediately
    // write it back, so the scene would never settle at all.
    FluidTestWorld w(3, 4, 3);
    w.set_floor(0);
    w.set_water(1, 3, 1);

    const int rounds = w.settle_sweep();
    CHECK(rounds > 0);  // -1 = it oscillated instead of settling

    CHECK(w.at(1, 3, 1).is_source());
    CHECK(w.at(1, 2, 1).falling);
    CHECK(w.at(1, 1, 1).falling);
    CHECK(w.count_fluid() > 3);
}

TEST_CASE("fluid: two sources one cell apart turn that cell into a source") {
    FluidTestWorld w = two_sources();
    CHECK(w.settle_sweep() > 0);

    CHECK(w.at(5, 1, 7).is_source());
    CHECK(w.at(7, 1, 7).is_source());
    // Rule 5, the one that makes a pool self-sustaining.
    CHECK(expect(w.at(6, 1, 7).is_source(), "the gap cell to have become a source", w, 1));

    CHECK(drivers_agree(two_sources()) == std::string());
}

TEST_CASE("fluid: a source never dries up, even with nothing to sit on") {
    FluidTestWorld w(7, 5, 7);
    w.set_water(3, 3, 3);  // no floor under anything

    CHECK(w.settle_sweep() > 0);
    CHECK(w.at(3, 3, 3).is_source());
    // It still pours, and the pour is a falling column all the way down to the
    // bottom of the world (there is nothing to land on), so every cell in it
    // keeps full strength.
    CHECK(w.at(3, 2, 3).falling);
    CHECK(w.at(3, 1, 3).falling);
    CHECK(w.at(3, 0, 3).falling);
}

TEST_CASE("fluid: removing the source drains the pool without leaving strays") {
    FluidTestWorld w = plane_with_source();
    CHECK(w.settle_sweep() > 0);
    CHECK(w.count_fluid() == 113);

    w.set_open(10, 1, 10);
    const int rounds = w.settle_sweep(3000);
    CHECK(rounds > 0);  // -1 = the drain oscillated instead of settling
    CHECK(expect(w.count_fluid() == 0, "the pool to have drained completely", w, 1));

    // The queue driver has to drain it too, and to the same empty world: the
    // retreat happens from the outside in, so a missed wake-up shows up here as
    // water that never notices it has been cut off.
    FluidTestWorld by_queue = plane_with_source();
    CHECK(by_queue.settle_queue() > 0);
    by_queue.set_open(10, 1, 10);
    CHECK(by_queue.settle_queue() > 0);
    CHECK(expect(by_queue.count_fluid() == 0, "the queue driver to have drained it too",
                 by_queue, 1));
}

TEST_CASE("fluid: pouring onto a pool makes the cells above and below re-evaluate") {
    // The awkward case for a scheduler rather than for the rules: fluid created
    // on TOP of fluid changes two cells nobody wrote to. The cell below it
    // becomes full strength (rule 4 comes from above), and the cell above it
    // stops pouring down and fans out instead. Both are invisible to a driver
    // that only wakes the cell that changed.
    FluidTestWorld w(12, 6, 12);
    w.set_floor(0);
    w.set_water(3, 1, 5);
    CHECK(w.settle_sweep() > 0);
    CHECK(w.at(5, 1, 5).depth == 2);  // runoff, with clear air above it

    w.set_water(5, 3, 5);  // a second source, two cells above that cell
    CHECK(w.settle_sweep() > 0);

    CHECK(w.at(5, 2, 5).falling);
    // The cell under the pour notices, and is full strength rather than the
    // runoff cell it was.
    CHECK(w.at(5, 1, 5).falling);
    CHECK_FALSE(w.at(5, 1, 5).is_source());
    // The fan that rains back down onto the pool does the same to what it lands
    // on: this cell was depth 3 and is now full strength instead.
    CHECK(w.at(6, 1, 5).falling);
    // And the pourer notices its exit is now occupied, so it fans out where it
    // previously only poured.
    CHECK(w.at(4, 3, 5).present());
    CHECK(w.at(6, 3, 5).present());

    // The same two-phase scene through the queued driver, and it has to land on
    // the same world — this is the assertion a missed wake-up breaks.
    FluidTestWorld by_queue(12, 6, 12);
    by_queue.set_floor(0);
    by_queue.set_water(3, 1, 5);
    CHECK(by_queue.settle_queue() > 0);
    by_queue.set_water(5, 3, 5);
    CHECK(by_queue.settle_queue() > 0);
    CHECK(w.first_difference(by_queue) == std::string());
}

TEST_CASE("fluid: a stream runs for a hole instead of spreading around it") {
    FluidTestWorld w = hole_scene();
    CHECK(w.settle_sweep() > 0);

    // A one-cell channel toward the hole, one step deeper per cell.
    CHECK(w.at(6, 1, 10).depth == 1);
    CHECK(w.at(7, 1, 10).depth == 2);
    CHECK(w.at(8, 1, 10).depth == 3);
    CHECK(w.at(9, 1, 10).depth == 4);
    // ...which empties into the hole.
    CHECK(w.at(9, 0, 10).falling);

    // Rule 8: the three directions that cannot see the hole lost to the one that
    // can, so none of them got wet — even though the ground there is identical.
    CHECK_FALSE(w.at(4, 1, 10).present());
    CHECK_FALSE(w.at(5, 1, 9).present());
    CHECK_FALSE(w.at(5, 1, 11).present());
    // And it did not fan out sideways along the way either.
    CHECK_FALSE(w.at(6, 1, 9).present());
    CHECK_FALSE(w.at(8, 1, 9).present());
    // And that is the whole flood: the source, four cells of channel, and the
    // cell that fell into the hole. Nothing else in the scene is wet.
    CHECK(w.count_fluid() == 6);

    CHECK(drivers_agree(hole_scene()) == std::string());
}

TEST_CASE("fluid: a wall stops the flow and no solid ever holds fluid") {
    FluidTestWorld w = wall_scene();
    CHECK(w.settle_sweep() > 0);

    CHECK(w.at(5, 1, 7).depth == 2);
    CHECK(w.is_solid(6, 1, 7));
    CHECK_FALSE(w.at(6, 1, 7).present());
    // Nothing ever appeared on the far side of the wall.
    CHECK_FALSE(w.at(7, 1, 7).present());

    for (int y = 0; y < w.size_y(); ++y) {
        for (int z = 0; z < w.size_z(); ++z) {
            for (int x = 0; x < w.size_x(); ++x) {
                if (w.is_solid(x, y, z)) CHECK_FALSE(w.at(x, y, z).present());
            }
        }
    }

    CHECK(drivers_agree(wall_scene()) == std::string());
}

TEST_CASE("fluid: a settled cell is unchanged and pushes nothing") {
    FluidTestWorld w = plane_with_source();
    CHECK(w.settle_sweep() > 0);

    // The source: permanent, and every direction it could spread into is full.
    FluidStep step = VoxelEngine::fluids::tick(w, 10, 1, 10);
    CHECK_FALSE(step.remove);
    CHECK_FALSE(step.changed);
    CHECK(step.write_count == 0);

    // An interior runoff cell, supplied at the depth it already has.
    step = VoxelEngine::fluids::tick(w, 10, 1, 14);
    CHECK_FALSE(step.changed);
    CHECK(step.write_count == 0);

    // The outermost cell: at the maximum depth, and out of directions it could
    // thin into.
    step = VoxelEngine::fluids::tick(w, 10, 1, 17);
    CHECK_FALSE(step.changed);
    CHECK(step.write_count == 0);

    // An empty cell does nothing at all, so a driver may tick any position.
    step = VoxelEngine::fluids::tick(w, 1, 1, 1);
    CHECK_FALSE(step.changed);
    CHECK_FALSE(step.remove);
    CHECK(step.write_count == 0);
}

TEST_CASE("fluid: the drop search is bounded, and does not depend on world size") {
    // A bigger world is more places the search COULD look. It is the same scene
    // in every direction the search can reach, so it must cost exactly the same.
    FluidTestWorld small(16, 3, 16);
    small.set_floor(0);
    small.set_water(8, 1, 8);
    small.reset_query_count();
    (void)VoxelEngine::fluids::tick(small, 8, 1, 8);
    const long long small_queries = small.query_count();

    FluidTestWorld large(64, 3, 64);
    large.set_floor(0);
    large.set_water(32, 1, 32);
    large.reset_query_count();
    (void)VoxelEngine::fluids::tick(large, 32, 1, 32);
    const long long large_queries = large.query_count();

    CHECK(small_queries == large_queries);
    // And it is a constant rather than a scan: a lone source on flat ground with
    // no drop in range is the expensive case (the depth-4 search finds nothing),
    // and it measures ~1380 reads. An unbounded search would be two orders of
    // magnitude more AND would grow with the world, which is what the line above
    // is really watching for.
    CHECK(small_queries < 2000);
}

TEST_CASE("fluid: the result does not depend on the order cells are ticked in") {
    // The queue driver (tested above) is a real scheduling order; this is a
    // cruder one, the same sweep run backwards, to separate "the rules have one
    // answer" from "the rules have one answer under my traversal".
    FluidTestWorld forward = plane_with_source();
    FluidTestWorld backward = plane_with_source();
    CHECK(forward.settle_sweep() > 0);
    CHECK(settle_sweep_reverse(backward) > 0);
    CHECK(forward.first_difference(backward) == std::string());

    FluidTestWorld falls = waterfall();
    FluidTestWorld falls_back = waterfall();
    CHECK(falls.settle_sweep() > 0);
    CHECK(settle_sweep_reverse(falls_back) > 0);
    CHECK(falls.first_difference(falls_back) == std::string());
}
