#ifndef FARLANDS_TESTS_FLUID_RULES_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_FLUID_RULES_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the fluid rules scenario table.
//
// Each scene builder lays out one situation the rules must get right (a plane with a
// source, a waterfall, two sources, a hole, a pit at the edge of reach, a pit past
// it, a wall). `drivers_agree` runs a scene through BOTH drivers -- the slow
// "recompute everything until quiet" sweep and the queued one -- and returns the
// first disagreement, because the interesting failure is the two disagreeing, which
// means the queue missed a wake-up.
//
// These were file-local to test_fluid_rules.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "fluid_test_world.hpp"
#include "fluids/fluid_rules.hpp"

#include <cstdio>
#include <string>

using namespace VoxelEngine;
using fluid_test::FluidTestWorld;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidStep;

namespace fluid_rules_test {

// This build compiles doctest with exceptions off, which turns INFO/CAPTURE and
// the *_MESSAGE assertion forms into no-ops — so a failing structural assertion
// prints nothing but the expression. This dumps the layer the expectation is
// about instead, right before the failing CHECK line.
inline bool expect(bool ok, const char* what, const FluidTestWorld& w, int y) {
    if (!ok) {
        std::printf("\nexpected %s here, but the layer is:%s", what, w.layer(y).c_str());
    }
    return ok;
}    // Run the scene through both drivers and report the first disagreement, or an

// empty string when they agree everywhere.
inline std::string drivers_agree(const FluidTestWorld& scene) {
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
inline int settle_sweep_reverse(FluidTestWorld& w) {
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
inline FluidTestWorld plane_with_source() {
    FluidTestWorld w(20, 5, 20);
    w.set_floor(0);
    w.set_water(10, 1, 10);
    return w;
}

// A three-high plateau with a source on top, so the water has to run to the
// edge, fall, and land. Solid occupies x 0..5 at y 1..3.
inline FluidTestWorld waterfall() {
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
inline FluidTestWorld two_sources() {
    FluidTestWorld w(15, 3, 15);
    w.set_floor(0);
    w.set_water(5, 1, 7);
    w.set_water(7, 1, 7);
    return w;
}

// A source four steps from a hole in the floor. Nothing else about the scene
// distinguishes the four directions, so anything that spreads away from the hole
// spread there for no reason.
inline FluidTestWorld hole_scene() {
    FluidTestWorld w(16, 4, 16);
    w.set_floor(0);
    w.set_open(9, 0, 10);
    w.set_water(5, 1, 10);
    return w;
}

// A lava source with a hole in the floor under the LAST cell it can reach — the
// third one north. A hole there is a route: the channel walks out and pours in.
inline FluidTestWorld pit_at_reach() {
    FluidTestWorld w(15, 4, 15);
    w.set_floor(0);
    w.set_open(7, 0, 4);
    w.set_cell_fluid(7, 1, 7, FluidCell{ FluidKind::Lava, 0, false });
    return w;
}

// The same scene with the hole one cell further north: now it sits under the
// cell just past the end of the reach, where no lava can ever stand. Nothing
// else about the floor differs.
inline FluidTestWorld pit_past_reach() {
    FluidTestWorld w(15, 4, 15);
    w.set_floor(0);
    w.set_open(7, 0, 3);
    w.set_cell_fluid(7, 1, 7, FluidCell{ FluidKind::Lava, 0, false });
    return w;
}

// A source and a wall it has to stop against.
inline FluidTestWorld wall_scene() {
    FluidTestWorld w(14, 3, 14);
    w.set_floor(0);
    for (int z = 0; z < 14; ++z) {
        w.set_solid(6, 1, z);
        w.set_solid(6, 2, z);
    }
    w.set_water(3, 1, 7);
    return w;
}

} // namespace fluid_rules_test

#endif // FARLANDS_TESTS_FLUID_RULES_TEST_SUPPORT_HPP
