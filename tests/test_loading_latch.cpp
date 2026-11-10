// The initial-loading budget latch: it starts only on a quiet chunk load with
// mesh pending, ends on 1 s without mesh work or on the cap, and the cap
// LATCHES -- it must not restart on the next quiet frame, and it releases only
// after the backlog has drained. These are the transitions the 36 s flight A/B
// depends on, pinned without a chunk map or a frame.
#include "doctest.h"
#include "world/loading_latch.hpp"

using VoxelEngine::InitialMeshHold;
using VoxelEngine::LoadingLatch;

TEST_CASE("the initial mesh hold ends at the first settle and never re-arms") {
    InitialMeshHold hold;
    CHECK(hold.active(true, false));   // the initial fill holds
    CHECK_FALSE(hold.active(true, true));  // first chunk-load settle releases it
    CHECK_FALSE(hold.active(true, false)); // a later flight must not pause meshes again
    CHECK_FALSE(hold.active(true, false));

    hold.reset();
    CHECK(hold.active(true, false));   // a new world gets its own one-shot hold
    CHECK_FALSE(hold.active(false, false));  // the configured switch still wins
}

TEST_CASE("the latch starts only on a quiet chunk load with mesh pending") {
    LoadingLatch latch;
    CHECK_FALSE(latch.tick(0.1, false, true));   // load busy
    CHECK_FALSE(latch.tick(0.1, true, false));   // nothing to mesh
    CHECK_FALSE(latch.active());
    CHECK(latch.tick(0.1, true, true));          // both conditions
    CHECK(latch.active());
}

TEST_CASE("an active episode survives the chunk load going busy") {
    LoadingLatch latch;
    CHECK(latch.tick(0.1, true, true));
    CHECK(latch.tick(5.0, false, true));  // load busy, mesh still pending
    CHECK(latch.active());
}

TEST_CASE("the quiet exit ends the episode without capping it") {
    LoadingLatch latch;
    CHECK(latch.tick(0.5, true, true));
    // The tick that drains the window reports the state AFTER the transition.
    CHECK_FALSE(latch.tick(1.0, true, false));  // 1 s without pending mesh
    CHECK_FALSE(latch.active());
    CHECK_FALSE(latch.capped());
    // A later tail can use the loading budgets again.
    CHECK(latch.tick(0.1, true, true));
    CHECK(latch.active());
}

TEST_CASE("the quiet exit wins when it and the cap land on the same frame") {
    LoadingLatch latch(1.0, 0.5);
    CHECK(latch.tick(0.6, true, true));           // elapsed 0.6
    CHECK_FALSE(latch.tick(0.6, true, false));    // quiet 0.6 >= 0.5 and cap 1.0
    CHECK_FALSE(latch.active());
    CHECK_FALSE(latch.capped());
}

TEST_CASE("the cap ends an episode and does not re-arm on the next quiet frame") {
    LoadingLatch latch(2.0, 1.0);
    CHECK(latch.tick(1.0, true, true));
    CHECK_FALSE(latch.tick(1.0, true, true));  // elapsed 2.0 -> cap
    CHECK_FALSE(latch.active());
    CHECK(latch.capped());
    // The regression: the load is still quiet and mesh still pending, so the
    // entry test would re-enter and reset the window. It must not.
    CHECK_FALSE(latch.tick(1.0 / 60.0, true, true));
    CHECK(latch.capped());
    // Releasing on a busy load is what let the next quiet frame restart the cap
    // in the first implementation, so a busy frame must not release it either.
    CHECK_FALSE(latch.tick(0.5, false, true));
    CHECK(latch.capped());
}

TEST_CASE("the cap releases after a quiet window, then a new tail can re-arm") {
    LoadingLatch latch(1.0, 1.0);
    CHECK_FALSE(latch.tick(1.0, true, true));  // start + cap in one tick
    CHECK(latch.capped());
    CHECK_FALSE(latch.tick(0.9, true, false));
    CHECK(latch.capped());                     // 0.9 s is not a window
    CHECK_FALSE(latch.tick(0.5, true, true));  // pending again resets it
    CHECK(latch.capped());
    CHECK_FALSE(latch.tick(0.9, true, false));
    CHECK(latch.capped());
    CHECK_FALSE(latch.tick(0.1, true, false));  // 1.0 s -> release
    CHECK_FALSE(latch.capped());
    CHECK(latch.tick(0.5, true, true));  // the later quiet-load tail boosts
    CHECK(latch.active());
    CHECK_FALSE(latch.capped());
}

TEST_CASE("reset clears both the active episode and the cap") {
    LoadingLatch latch(1.0, 1.0);
    CHECK_FALSE(latch.tick(1.0, true, true));
    CHECK(latch.capped());
    latch.reset();
    CHECK_FALSE(latch.active());
    CHECK_FALSE(latch.capped());
    CHECK(latch.tick(0.1, true, true));
    CHECK(latch.active());
}

TEST_CASE("the shipping defaults are a 30 s cap and a 1 s quiet exit") {
    LoadingLatch capped;
    bool held = true;
    for (int i = 0; i < 59; ++i) {
        held = held && capped.tick(0.5, true, true);  // 29.5 s
    }
    CHECK(held);
    CHECK(capped.active());
    CHECK_FALSE(capped.tick(0.5, true, true));  // 30.0 s
    CHECK(capped.capped());

    LoadingLatch drained;
    CHECK(drained.tick(0.5, true, true));
    CHECK_FALSE(drained.tick(1.0, true, false));  // 1.0 s without pending mesh
    CHECK_FALSE(drained.active());
    CHECK_FALSE(drained.capped());
}
