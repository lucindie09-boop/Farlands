#include "doctest.h"

#include "world/cross_boundary_queue.hpp"

using namespace VoxelEngine;

// A world reset must not leave the previous fill's boundary remeshes behind.
// The next fill would drain them and dirty-mesh chunks that no longer exist;
// this is the list ChunkWorld::clear() empties.
TEST_CASE("cross-boundary remesh queue clear() empties the list") {
    CrossBoundaryRemeshQueue queue;
    queue.push({1, 2, 3});
    queue.push({-4, 5, -6});
    CHECK(queue.size() == 2);

    queue.clear();

    CHECK(queue.size() == 0);
    CHECK(queue.take_all().empty());
}

TEST_CASE("take_all() hands over the list and leaves the queue empty") {
    CrossBoundaryRemeshQueue queue;
    queue.push({7, 0, -9});

    const auto taken = queue.take_all();

    CHECK(taken.size() == 1);
    CHECK(taken[0].x == 7);
    CHECK(taken[0].z == -9);
    CHECK(queue.size() == 0);
}