#include "doctest.h"
#include "engine/player_controller.hpp"
#include "engine/collision_resolver.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;
using namespace godot;

static std::unique_ptr<ChunkRenderData> make_test_chunk(std::unique_ptr<ChunkData> data) {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::move(data));
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

static void fill_flat_floor(ChunkMap& cm) {
    BlockRegistry::get_instance().initialize_default_blocks();
    for (int cx = -1; cx <= 1; ++cx) {
        for (int cz = -1; cz <= 1; ++cz) {
            auto d = std::make_unique<ChunkData>();
            for (int x = 0; x < 32; ++x)
                for (int z = 0; z < 32; ++z)
                    d->set_block(x, 0, z, BlockIDs::STONE);
            cm.insert(cm.get_chunk_key(cx, 0, cz), make_test_chunk(std::move(d)));
        }
    }
}

static PlayerInput make_forward_input(bool sprint = false, bool sneak = false) {
    PlayerInput input;
    input.wish_direction = Vector3(0.0f, 0.0f, -1.0f); // forward = -Z
    input.sprint_held = sprint;
    input.sneak_held = sneak;
    return input;
}

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
    // Walk steady-state: 0.098 / 0.454 ≈ 0.2159 blocks/tick (4.317 blocks/s)
    CHECK(speed > 0.213f);
    CHECK(speed < 0.219f);
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
    // Sprint steady-state: 0.098 * 1.3 / 0.454 ≈ 0.2806 blocks/tick (5.612 blocks/s)
    CHECK(speed > 0.278f);
    CHECK(speed < 0.284f);
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
    // Sneak steady-state: 0.098 * 0.3 / 0.454 ≈ 0.0648 blocks/tick (1.295 blocks/s)
    CHECK(speed > 0.062f);
    CHECK(speed < 0.068f);
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
    CHECK(pc.get_position().x < 32.0f);
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
    sprint_jump.jump_pressed = true;
    sprint_jump.yaw = 0.0f; // facing -Z

    float pre_vx = pc.get_velocity().x;
    pc.accumulate_and_tick(1.0 / 20.0, sprint_jump, cr);

    // Should have horizontal boost (velocity.z should be more negative due to +0.2 boost in -Z)
    CHECK(pc.get_velocity().z < -0.3f);
}

TEST_CASE("Player gravity application order") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 5.0f, 0.5f));

    PlayerInput idle;
    // Drop from height with no input
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    // After 1 tick from rest: drag on zero = 0, then gravity subtracts 0.08
    // velocity.y = (0.0 * 0.98) - 0.08 = -0.08
    CHECK(pc.get_velocity().y == doctest::Approx(-0.08f).epsilon(0.001f));
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
