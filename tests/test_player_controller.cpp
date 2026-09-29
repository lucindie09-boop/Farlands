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

TEST_CASE("Player walk steady state") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    auto input = make_forward_input(false, false);
    for (int i = 0; i < 100; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, input, cr);
    }

    float speed = std::sqrt(pc.get_velocity().x * pc.get_velocity().x
                          + pc.get_velocity().z * pc.get_velocity().z);
    // Walk steady-state: 0.1 / 0.454 ≈ 0.2203 blocks/tick
    CHECK(speed > 0.213f);
    CHECK(speed < 0.225f);
}

TEST_CASE("Player sprint steady state") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    auto input = make_forward_input(true, false);
    for (int i = 0; i < 100; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, input, cr);
    }

    float speed = std::sqrt(pc.get_velocity().x * pc.get_velocity().x
                          + pc.get_velocity().z * pc.get_velocity().z);
    // Sprint steady-state: 0.1 * 1.3 / 0.454 ≈ 0.2864 blocks/tick
    CHECK(speed > 0.278f);
    CHECK(speed < 0.294f);
}

TEST_CASE("Player sneak steady state") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    auto input = make_forward_input(false, true);
    for (int i = 0; i < 100; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, input, cr);
    }

    float speed = std::sqrt(pc.get_velocity().x * pc.get_velocity().x
                          + pc.get_velocity().z * pc.get_velocity().z);
    // Sneak steady-state: 0.1 * 0.3 / 0.454 ≈ 0.0661 blocks/tick
    CHECK(speed > 0.062f);
    CHECK(speed < 0.070f);
}

TEST_CASE("Player sneak edge-guard prevents falling off") {
    ChunkMap cm;
    BlockRegistry::get_instance().initialize_default_blocks();
    // Create floor only in chunks cx=-1 and cx=0 (NOT cx=1).
    // Chunk cx=0 covers world x=0..31. No chunk at cx=1 means cliff at x=32.
    for (int cx = -1; cx <= 0; ++cx) {
        for (int cz = -1; cz <= 1; ++cz) {
            auto d = std::make_unique<ChunkData>();
            for (int x = 0; x < 32; ++x)
                for (int z = 0; z < 32; ++z)
                    d->set_block(x, 0, z, BlockIDs::STONE);
            cm.insert(cm.get_chunk_key(cx, 0, cz), make_test_chunk(std::move(d)));
        }
    }
    CollisionResolver cr(&cm);

    // Place player near the cliff edge (chunk boundary at world x=32)
    PlayerSim pc;
    pc.reset(Vector3(30.5f, 1.0f, 0.5f));

    // Settle on floor
    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Sneak toward the +X cliff — edge-guard should stop us before x=32
    PlayerInput sneak_forward;
    sneak_forward.wish_direction = Vector3(1.0f, 0.0f, 0.0f);
    sneak_forward.sneak_held = true;

    for (int i = 0; i < 100; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, sneak_forward, cr);
    }

    // Player should NOT have walked off the cliff
    // Graduated clamp allows overhang up to hitbox half-width past the last block edge
    CHECK(pc.get_position().x < 32.3f);
    CHECK(pc.is_on_floor());
}

TEST_CASE("Player jump height") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    // Stand still, then jump
    PlayerInput idle;
    // First let the player settle on the floor
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Now jump
    PlayerInput jump_input;
    jump_input.jump_pressed = true;
    pc.accumulate_and_tick(1.0 / 20.0, jump_input, cr);

    float apex_y = pc.get_position().y;
    // Simulate until we reach apex (velocity.y transitions positive -> negative)
    PlayerInput air_input;
    bool was_rising = true;
    for (int i = 0; i < 30; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, air_input, cr);
        if (pc.get_velocity().y < 0.0f && was_rising) {
            apex_y = pc.get_position().y;
            was_rising = false;
        }
        if (pc.get_position().y > apex_y) {
            apex_y = pc.get_position().y;
        }
    }
    // Jump apex: ~1.2522 blocks above ground (starting at y=1.0)
    float jump_height = apex_y - 1.0f;
    CHECK(jump_height > 1.15f);
    CHECK(jump_height < 1.35f);
}

TEST_CASE("Player sprint-jump horizontal boost") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));

    // Settle
    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // Sprint forward + jump
    PlayerInput sprint_jump;
    sprint_jump.wish_direction = Vector3(0.0f, 0.0f, -1.0f);
    sprint_jump.sprint_held = true;
    sprint_jump.move_forward_held = true;
    sprint_jump.jump_pressed = true;
    sprint_jump.yaw = 0.0f; // facing -Z

    float pre_vx = pc.get_velocity().x;
    pc.accumulate_and_tick(1.0 / 20.0, sprint_jump, cr);

    // Boost (+0.2) is applied BEFORE friction (matching vanilla jump() → travel() order),
    // so friction partially consumes it. Post-jump velocity.z ≈ -(0.286+0.2)*0.546+0.13 ≈ -0.395
    // but on the next tick it settles. Just verify a meaningful boost occurred.
    CHECK(pc.get_velocity().z < -0.15f);
}
