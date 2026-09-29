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

TEST_CASE("Player step-up disabled while airborne or jumping") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        // Synthetic obstruction: block at y=0, empty at y=1
        d->set_block(0, 0, 0, BlockIDs::STONE);
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    // Test 1: Airborne player should not step up
    PlayerSim pc_airborne;
    pc_airborne.reset(Vector3(0.5f, 2.0f, 0.5f));  // Start in air above the block
    PlayerInput move_toward;
    move_toward.wish_direction = Vector3(0.0f, 0.0f, -0.5f);  // Move toward block

    // Simulate falling into the block — should NOT step up
    for (int i = 0; i < 10; ++i) {
        pc_airborne.accumulate_and_tick(1.0 / 20.0, move_toward, cr, 0.6f);
    }
    // Player should land on top of the block (y=1.0) or below, but stepped_up
    // should be false because on_floor_ was false when the step-up check ran
    CHECK(pc_airborne.get_position().y <= 1.1f);

    // Test 2: Jumping player should not step up on the jump tick
    ChunkMap cm2;
    {
        auto d = std::make_unique<ChunkData>();
        // Create a step at y=1 (player starts at y=1.0 on floor at y=0)
        d->set_block(0, 0, 0, BlockIDs::STONE);
        d->set_block(1, 0, 0, BlockIDs::STONE);  // Step at x=1
        cm2.insert(cm2.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr2(&cm2);

    PlayerSim pc_jump;
    pc_jump.reset(Vector3(0.5f, 1.0f, 0.5f));  // On floor at y=0

    // Settle
    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc_jump.accumulate_and_tick(1.0 / 20.0, idle, cr2);
    }
    CHECK(pc_jump.is_on_floor());

    // Jump while moving toward a step — should NOT step up on the jump tick
    // because velocity_.y > 0 after jump impulse
    PlayerInput jump_and_move;
    jump_and_move.wish_direction = Vector3(0.5f, 0.0f, 0.0f);  // Move toward step at x=1
    jump_and_move.jump_pressed = true;

    pc_jump.accumulate_and_tick(1.0 / 20.0, jump_and_move, cr2, 0.6f);

    // On the jump tick, step-up should be disabled (velocity_.y = JUMP_VELOCITY > 0)
    // Player should be airborne, not stepped up
    CHECK(pc_jump.get_velocity().y > 0.0f);  // Jump impulse applied
    CHECK(!pc_jump.is_on_floor());  // Airborne after jump
}

TEST_CASE("Fall damage: vanilla jump lands safely") {
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

    // Vanilla jump apex is ~1.25 blocks — well under the 3-block safe fall
    PlayerInput jump_input;
    jump_input.jump_pressed = true;
    pc.accumulate_and_tick(1.0 / 20.0, jump_input, cr);
    for (int i = 0; i < 40 && !pc.is_on_floor(); ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());
    CHECK(pc.consume_pending_fall_damage() == 0);
}

TEST_CASE("Fall damage: 7.5-block drop deals 4 half-hearts") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    // Floor top surface sits at y=1, so starting at y=8.5 is a 7.5-block fall
    // -> floor(7.5 - 3) = 4 half-hearts. A non-integer height keeps the
    // assertion away from a damage boundary where fp noise could flip the
    // floor() (e.g. an exact 5-block fall accumulating as 4.9999996).
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 8.5f, 0.5f));

    PlayerInput idle;
    for (int i = 0; i < 60 && !pc.is_on_floor(); ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    CHECK(pc.get_fall_distance() == doctest::Approx(0.0f));
    CHECK(pc.consume_pending_fall_damage() == 4);
    CHECK(pc.consume_pending_fall_damage() == 0);  // consumed exactly once
}

TEST_CASE("Fall damage: teleport reset clears pending damage") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 8.5f, 0.5f));
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));  // mid-fall teleport

    PlayerInput idle;
    for (int i = 0; i < 10; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());
    CHECK(pc.consume_pending_fall_damage() == 0);
}
