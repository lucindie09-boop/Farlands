#include "doctest.h"

#include "fluid_rules_test_support.hpp"

#include "fluid_test_world.hpp"
#include "fluids/fluid_rules.hpp"

#include <cstdio>
#include <string>

using fluid_test::FluidTestWorld;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidStep;
using namespace fluid_rules_test;

// -----------------------------------------------------------------------------
// The scenario table.
//
// Every expectation below is grounded in the rule it comes from (see the header
// for the numbered list), and every scenario is run through BOTH drivers — the
// slow "recompute everything until quiet" sweep and the queued one — because
// the interesting failure is the two disagreeing, which means the queue missed
// a wake-up.
// -----------------------------------------------------------------------------

using namespace VoxelEngine;

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
