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

TEST_CASE("fluid: a block change wakes the fluid around it") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.cell_at(8, 1, 12).depth == 4);
    CHECK_FALSE(fixture.cell_at(8, 0, 12).present());

    // Open a hole under that water, exactly as a player breaking the block would,
    // and tell the simulation about it. Without the wake-up the pool sits there
    // and never notices.
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 0, 12, BlockIDs::AIR);
    fixture.sim.notify_block_changed(8, 0, 12);
    CHECK(fixture.sim.run_to_settled() > 0);

    CHECK(fixture.cell_at(8, 0, 12).falling);  // the pool poured into the hole
    CHECK(fixture.cell_at(8, 1, 12).present());
    // And the rest of the pool is unharmed.
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    CHECK(fixture.cell_at(11, 1, 8).depth == 3);
}

// A chunk arriving with fluid in it applies its edit map on a generation WORKER,
// and that wake has to be posted rather than applied: applying it there mutates
// the pending set and the queue from a thread the main thread is ticking, which
// surfaced as heap corruption. The behaviour must be identical, one drain later.
TEST_CASE("fluid: a posted wake is applied on the next drain, not before") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);

    // The same hole as the notify test, but posted from another thread.
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 0, 12, BlockIDs::AIR);
    std::thread worker([&] { fixture.sim.post_block_changed(8, 0, 12); });
    worker.join();

    // Nothing has touched the queue yet — that is the whole point of the post.
    CHECK(fixture.sim.pending_count() == 0);

    // One driver call picks it up, and the pool behaves exactly as it did when
    // the same wake went in directly.
    CHECK(fixture.sim.run_ticks(0) == 0);
    CHECK(fixture.sim.pending_count() > 0);
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.cell_at(8, 0, 12).falling);
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    CHECK(fixture.cell_at(11, 1, 8).depth == 3);
}

TEST_CASE("fluid: advance drains posted wakes even when the queue is idle") {
    SimFixture fixture;
    fixture.build_three_by_three();
    // The case that matters: a chunk streams in with water in it while nothing is
    // flowing, so the queue is empty and advance() takes its idle early-return.
    // The drain has to happen before that return or the water stays asleep
    // forever, until some unrelated edit happens to wake it.
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.pending_count() == 0);
    CHECK_FALSE(fixture.sim.has_due_work());

    // Placed straight into the chunk, the way a generated chunk arrives: nothing
    // tells the simulation directly, only the worker-side post does.
    fixture.map.get_chunk_data(0, 0, 0)->set_block(8, 1, 8, fixture.water_source_block());
    fixture.sim.post_block_changed(8, 1, 8);
    fixture.sim.advance(1.0 / 60.0);

    CHECK(fixture.sim.pending_count() > 0);
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.cell_at(8, 1, 8).is_source());
    CHECK(fixture.cell_at(11, 1, 8).depth == 3);
}

TEST_CASE("fluid: a world reset drops wakes posted for the old world") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.post_block_changed(8, 1, 8);
    fixture.sim.clear();
    // A stale post would wake a cell in a world that no longer exists.
    fixture.sim.advance(1.0 / 60.0);
    CHECK(fixture.sim.pending_count() == 0);
}

// The regression itself: many workers posting while the main thread drains. Before
// the inbox existed this was a plain data race on the pending set, and its cost was
// heap corruption rather than a wrong answer — so the assertion is primarily "the
// drains account for every post, and nothing was lost or double-applied".
TEST_CASE("fluid: posting from many threads loses and duplicates nothing") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);

    constexpr int kThreads = 4;
    constexpr int kPerThread = 500;
    std::atomic<int> posted{ 0 };
    std::atomic<bool> stop{ false };
    std::thread drainer([&] {
        size_t applied = 0;
        while (!stop.load() || applied < kThreads * kPerThread) {
            applied += fixture.sim.drain_posted();
        }
        // One last drain for anything that landed between the check and the exit.
        applied += fixture.sim.drain_posted();
        posted.store(static_cast<int>(applied));
    });

    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                // In the flood's reach, so each post really is a wake.
                fixture.sim.post_block_changed(8 + (i % 3), 1, 8 + t);
            }
        });
    }
    for (std::thread& w : workers) w.join();
    stop.store(true);
    drainer.join();

    CHECK(posted.load() == kThreads * kPerThread);
}

TEST_CASE("fluid: an edit far from any fluid wakes nothing") {
    SimFixture fixture;
    fixture.build_three_by_three();
    fixture.put_source(8, 8);
    fixture.sim.configure(FluidSim::Config{ 20.0, 4096, 100, 1.0 });
    CHECK(fixture.sim.run_to_settled() > 0);
    CHECK(fixture.sim.pending_count() == 0);

    // Eleven blocks from the source is outside the flood, so nothing there can
    // change a fluid's answer and the notification must cost nothing.
    fixture.sim.notify_block_changed(8, 1, 20);
    CHECK(fixture.sim.pending_count() == 0);

    // One block further in, and it does wake the fluid: this is the boundary.
    fixture.sim.notify_block_changed(8, 1, 15);
    CHECK(fixture.sim.pending_count() > 0);
}
