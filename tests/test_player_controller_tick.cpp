#include "doctest.h"

#include "player_controller_test_support.hpp"

#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/player_controller.hpp"

#include <cstring>

using namespace VoxelEngine;
using namespace godot;
using namespace player_controller_test;

TEST_CASE("Player gravity application order") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 5.0f, 0.5f));

    PlayerInput idle;
    // Drop from height with no input
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    // After 1 tick from rest: gravity subtracts 0.08, then drag multiplies by 0.98
    // velocity.y = (0.0 - 0.08) * 0.98 = -0.0784
    CHECK(pc.get_velocity().y == doctest::Approx(-0.0784f).epsilon(0.001f));
}

TEST_CASE("Player accumulator correctness") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    PlayerInput idle;

    // Feed exactly 0.05s (one tick at 20Hz)
    pc.accumulate_and_tick(0.05, idle, cr);
    CHECK(pc.get_accumulator_fraction() < 1e-5f);

    // Feed 0.12s (should run 2 ticks, leave 0.02 residue)
    pc.accumulate_and_tick(0.12, idle, cr);
    CHECK(pc.get_accumulator_fraction() > 0.0f);
    CHECK(pc.get_accumulator_fraction() < 1.0f);
}

TEST_CASE("Player accumulator does not re-fire edge-triggered inputs") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    // Settle on floor
    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Jump with 0.12s frame (2 ticks). Jump should only fire on first tick.
    PlayerInput jump_input;
    jump_input.jump_pressed = true;
    pc.accumulate_and_tick(0.12, jump_input, cr);

    // After 2 ticks: player should be airborne (jump consumed on tick 1, tick 2 is in air)
    // Position should have moved upward from 1.0
    CHECK(pc.get_position().y > 1.0f);
}

TEST_CASE("Player reset teleport") {
    PlayerSim pc;
    pc.reset(Vector3(100.0f, 200.0f, 300.0f));

    // render_position at partial=0 should be exactly the reset position
    // (prev_position_ == position_ after reset, no glide from default Vector3(0,0,0))
    Vector3 render_pos = pc.get_render_position(0.0f);
    CHECK(render_pos.x == doctest::Approx(100.0f));
    CHECK(render_pos.y == doctest::Approx(200.0f));
    CHECK(render_pos.z == doctest::Approx(300.0f));
}

TEST_CASE("Player jump latches across non-tick frames at 60fps") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    // Settle on floor
    PlayerInput idle;
    for (int i = 0; i < 10; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Reset to a known accumulator state
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    for (int i = 0; i < 10; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Simulate a 60fps frame where the jump arrives on a frame that does NOT
    // cross a tick boundary. At 60fps each frame is ~16.67ms = 0.01667s.
    // If the accumulator starts near 0, the first frame (0.01667) doesn't
    // reach the 0.05 tick threshold — no tick fires.
    //
    // With the old code, is_action_just_pressed returns true for only that
    // frame, so the tick that eventually fires never sees it. With the latch,
    // queue_jump() persists until a tick consumes it.

    // Feed a no-tick frame (accumulator starts near 0, 0.01667 < 0.05)
    float leftover = pc.get_accumulator_fraction() * (1.0f / 20.0f);
    // leftover should be small (< 0.05), so this frame won't tick
    CHECK(leftover < 0.05f);

    // Simulate the "jump frame" — queue jump but frame doesn't tick
    PlayerInput jump_frame;
    jump_frame.jump_pressed = true;
    pc.queue_jump();  // latch it
    pc.accumulate_and_tick(1.0 / 60.0, jump_frame, cr);

    // Jump was queued but possibly not consumed yet — feed more frames until tick fires
    PlayerInput no_input;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 60.0, no_input, cr);
    }

    // Player should now be airborne — the jump was consumed by a tick
    CHECK(pc.get_position().y > 1.0f);
}
