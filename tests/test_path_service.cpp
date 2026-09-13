// Tests for PathService: the half of the planner that owns a job's life cycle.
//
// The case these exist for is the pool. The engine's controller destroys its
// ThreadPool and builds a fresh one whenever the runtime state is reset
// (clear_editor_chunks -> reset_runtime_state(true)), and ChunkManager keeps one
// PathService for the life of the node. A service that held a ThreadPool& would
// therefore be queueing a task into freed memory on the next request after a
// reset — a use-after-free that only shows up in the editor, not in the game.
// submit() takes the pool per call instead, so the stale reference cannot be
// written down; the case below runs one service across two pools to pin that.

#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "core/block_types.hpp"
#include "core/thread_pool.hpp"
#include "pathfinding/path_service.hpp"

#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>
#include <vector>

using namespace VoxelEngine;
namespace nav = VoxelEngine::nav;
using chunktest::insert_floor_chunk;

namespace {

// Drains finished plans until `want` have arrived or the deadline passes, so a
// broken job fails the case instead of hanging the suite.
std::vector<nav::PathResult> wait_for_plans(nav::PathService& service, size_t want) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    std::vector<nav::PathResult> out;
    while (out.size() < want && std::chrono::steady_clock::now() < deadline) {
        for (nav::PathResult& r : service.poll()) out.push_back(std::move(r));
        if (out.size() < want) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return out;
}

} // namespace

TEST_CASE("PathService plans on whichever pool it is handed") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    for (int32_t cx = 0; cx < 3; ++cx) insert_floor_chunk(cm, cx, 0, 0);  // world x 0..95

    nav::PathService service(cm);
    CHECK(service.pending() == 0);

    // The first pool stands in for the one the engine builds at startup, and it
    // is destroyed before the second plan is submitted — exactly what
    // clear_editor_chunks does to the real pool.
    uint64_t first_id = 0;
    {
        ThreadPool first_pool(1);
        first_id = service.submit(first_pool, nav::NavNode{2, 5, 2}, nav::NavNode{60, 5, 2});
        CHECK(first_id != 0);
        const std::vector<nav::PathResult> first = wait_for_plans(service, 1);
        CHECK(first.size() == 1);
        if (first.size() != 1) return;
        CHECK(first[0].id == first_id);
        CHECK(first[0].found);
        CHECK_FALSE(first[0].truncated);
    }
    CHECK(service.pending() == 0);

    // A second pool, and the same service. Under a held pool reference this
    // submit would be queueing into a destroyed pool.
    ThreadPool second_pool(1);
    const uint64_t second_id = service.submit(second_pool, nav::NavNode{2, 5, 2},
                                              nav::NavNode{60, 5, 2});
    CHECK(second_id != first_id);
    const std::vector<nav::PathResult> second = wait_for_plans(service, 1);
    CHECK(second.size() == 1);
    if (second.size() != 1) return;
    CHECK(second[0].id == second_id);
    CHECK(second[0].found);
    CHECK_FALSE(second[0].truncated);
    CHECK(second[0].error.empty());

    // A route over the live chunk map. The entries are SUPPORT blocks — the
    // block each node stands on — so the floor chunk's top-of-y=1 floor is cell
    // y=0. The ranged reader is what makes the read cost scale with columns
    // rather than with cells.
    CHECK(second[0].nodes.size() >= 2);
    CHECK(second[0].nodes.front() == nav::NavNode{2, 0, 2});
    CHECK(second[0].nodes.back() == nav::NavNode{60, 0, 2});
    CHECK_FALSE(second[0].waypoints.empty());
    CHECK(second[0].waypoints.size() <= second[0].nodes.size());
    CHECK(second[0].cells_read > second[0].lock_acquisitions);
    CHECK(second[0].lock_acquisitions > 0);

    CHECK(service.finished_count() == 2);
}

TEST_CASE("PathService reports a failed plan instead of queueing nothing") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    insert_floor_chunk(cm, 0, 0, 0);  // world x 0..31 only

    nav::PathService service(cm);
    ThreadPool pool(1);

    // A goal in a chunk that was never generated reads as Unknown, so the goal
    // column has no navigable surface: the plan comes back as a failure with a
    // reason, not as a silent no-op.
    service.submit(pool, nav::NavNode{2, 5, 2}, nav::NavNode{200, 5, 2});
    const std::vector<nav::PathResult> results = wait_for_plans(service, 1);
    CHECK(results.size() == 1);
    if (results.size() != 1) return;
    CHECK_FALSE(results[0].found);
    CHECK_FALSE(results[0].truncated);
    CHECK_FALSE(results[0].error.empty());
    CHECK(results[0].id != 0);
}
