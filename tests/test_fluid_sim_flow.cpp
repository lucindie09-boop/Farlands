#include "doctest.h"

#include "fluid_sim_test_support.hpp"

#include "chunk_map_fixture.hpp"
#include "fluids/chunk_fluid.hpp"
#include "fluids/fluid_rules.hpp"
#include "fluids/fluid_sim.hpp"
#include "fluids/fluid_state_table.hpp"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace VoxelEngine;
namespace fluids = VoxelEngine::fluids;
using VoxelEngine::FluidKind;
using VoxelEngine::fluids::FluidCell;
using VoxelEngine::fluids::FluidSim;
using VoxelEngine::fluids::FluidStateTable;
using VoxelEngine::fluids::FluidWriteRecord;
using namespace fluid_sim_test;

TEST_CASE("fluid: lava flows on its own clock and stops at its own depth") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });

    const BlockID source = fixture.table.block_for(FluidCell{ FluidKind::Lava, 0, false });
    CHECK(source != BlockIDs::AIR);
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 1, 8, source);
    fixture.sim.notify_block_changed(8, 1, 8);

    // Five ticks is a quarter of a second, which is exactly when WATER would
    // move; to lava it is nothing. Ticks rather than seconds here, because the
    // subject is the per-kind delay and not the wall clock's rounding.
    fixture.sim.run_ticks(5);
    CHECK_FALSE(fixture.cell_at(9, 1, 8).present());

    // Twenty ticks, one second: the first ring, as lava at depth one.
    fixture.sim.run_ticks(15);
    CHECK(fixture.cell_at(9, 1, 8).present());
    CHECK(fixture.cell_at(9, 1, 8).kind == FluidKind::Lava);
    CHECK(fixture.cell_at(9, 1, 8).depth == 1);

    // And it stops where lava stops: three cells out. There is no block for a
    // fourth depth and no rule that would ask for one.
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    CHECK(fixture.cell_at(8, 1, 8).kind == FluidKind::Lava);
    CHECK(fixture.cell_at(11, 1, 8).depth == 3);
    CHECK(fixture.cell_at(11, 1, 8).kind == FluidKind::Lava);
    CHECK_FALSE(fixture.cell_at(12, 1, 8).present());
    CHECK_FALSE(fixture.cell_at(8, 1, 12).present());
    CHECK(fixture.count_fluid() == 25);  // a diamond of radius 3
}

TEST_CASE("fluid: a splash of acid drains where the same water would have pooled") {
    // The rule acid is missing, end to end: two sources with one cell between
    // them. Water turns that cell into a spring and the pool lives forever; acid
    // leaves it as runoff, so when the two sources go the whole thing dries up.
    auto pour_pair = [](FluidKind kind, SimFixture& fixture) {
        fixture.build_three_by_three();
        fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 200, 1.0 });
        const BlockID source = fixture.table.block_for(FluidCell{ kind, 0, false });
        fixture.map.get_chunk_data(0, 0, 0)->set_block(5, 1, 7, source);
        fixture.map.get_chunk_data(0, 0, 0)->set_block(7, 1, 7, source);
        fixture.sim.notify_block_changed(5, 1, 7);
        fixture.sim.notify_block_changed(7, 1, 7);
        CHECK(fixture.sim.run_to_settled() > 0);
    };

    SimFixture water;
    pour_pair(FluidKind::Water, water);
    CHECK(water.cell_at(6, 1, 7).is_source());

    SimFixture acid;
    pour_pair(FluidKind::Acid, acid);
    CHECK_FALSE(acid.cell_at(6, 1, 7).is_source());
    CHECK(acid.cell_at(6, 1, 7).present());

    // Take the two sources away: the water pool stands (the middle spring now
    // feeds it), while the acid has nothing left holding it up.
    water.map.get_chunk_data(0, 0, 0)->set_block(5, 1, 7, BlockIDs::AIR);
    water.map.get_chunk_data(0, 0, 0)->set_block(7, 1, 7, BlockIDs::AIR);
    water.sim.notify_block_changed(5, 1, 7);
    water.sim.notify_block_changed(7, 1, 7);
    CHECK(water.sim.run_to_settled() > 0);
    CHECK(water.cell_at(6, 1, 7).is_source());
    CHECK(water.count_fluid() > 1);

    acid.map.get_chunk_data(0, 0, 0)->set_block(5, 1, 7, BlockIDs::AIR);
    acid.map.get_chunk_data(0, 0, 0)->set_block(7, 1, 7, BlockIDs::AIR);
    acid.sim.notify_block_changed(5, 1, 7);
    acid.sim.notify_block_changed(7, 1, 7);
    CHECK(acid.sim.run_to_settled() > 0);
    CHECK(acid.count_fluid() == 0);
}

TEST_CASE("fluid: the same scene floods the same way twice") {
    // Ordering is (due tick, wake order, position), all of which are the
    // simulation's own, so a replay cannot wander. This is what makes a bug
    // report reproducible.
    auto run_once = [](std::string& out) {
        SimFixture fixture;
        fixture.build_three_by_three();
        fixture.put_source(8, 8);
        fixture.sim.configure(FluidSim::Config{ 20.0, 64, 100, 1.0 });
        fixture.sim.run_to_settled();
        for (int32_t z = 0; z < 16; ++z) {
            for (int32_t x = 0; x < 16; ++x) {
                const FluidCell cell = fixture.cell_at(x, 1, z);
                if (!cell.present()) {
                    out += ".";
                    continue;
                }
                out += cell.is_source() ? "S" : std::to_string(static_cast<int>(cell.depth));
                out += cell.falling ? "F" : "";
            }
        }
    };
    std::string first;
    std::string second;
    run_once(first);
    run_once(second);
    CHECK(first == second);
    CHECK(first.find('S') != std::string::npos);
}

TEST_CASE("fluid: a flood front advances at the same rate in every direction") {
    SimFixture fixture;
    fixture.build_three_by_three();
    const int32_t sx = 8;
    const int32_t sz = 8;
    fixture.put_source(sx, sz);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });

    // How far the flood has reached along a direction: the last step out that
    // holds any fluid at all.
    const auto reach = [&](int32_t dx, int32_t dz) {
        int last = 0;
        for (int step = 1; step <= 8; ++step) {
            if (fixture.cell_at(sx + dx * step, 1, sz + dz * step).present()) last = step;
        }
        return last;
    };

    // Water moves one cell per 5 ticks, so after 5*step ticks the front must sit
    // exactly `step` cells out in ALL FOUR directions, with the depth equal to
    // the distance. Checking one axis (as the test above does) is not enough:
    // a spreading cell wakes +x, -x, +z, -z in that order, so the cells that
    // come last in a tick are the ones a same-tick write lands on — and when
    // that write replaced their pending tick instead of adding one, only the
    // north/south fronts fell a whole delay behind, for good. A flood has to
    // spread as a diamond, and it has to grow at one rate.
    for (int step = 1; step <= 5; ++step) {
        fixture.sim.run_ticks(5);
        CHECK(reach(1, 0) == step);
        CHECK(reach(-1, 0) == step);
        CHECK(reach(0, 1) == step);
        CHECK(reach(0, -1) == step);
        CHECK(fixture.cell_at(sx + step, 1, sz).depth == step);
        CHECK(fixture.cell_at(sx - step, 1, sz).depth == step);
        CHECK(fixture.cell_at(sx, 1, sz + step).depth == step);
        CHECK(fixture.cell_at(sx, 1, sz - step).depth == step);
    }

    // And it still settles: the front stops at seven, the pool is the same
    // diamond whatever order it grew in, and the queue drains.
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.sim.pending_count() == 0);
    CHECK(reach(1, 0) == 7);
    CHECK(reach(-1, 0) == 7);
    CHECK(reach(0, 1) == 7);
    CHECK(reach(0, -1) == 7);
    CHECK(fixture.count_fluid() == 113);
}
