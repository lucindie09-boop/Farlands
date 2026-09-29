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

TEST_CASE("Player falls through water and sinks slowly") {
    ChunkMap cm;
    fill_water_pool(cm, 12);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 22.0f, 0.5f));

    // A liquid is passable, so the body drops in rather than stopping on it.
    PlayerInput idle;
    bool entered = false;
    int entry_tick = 0;
    float entry_speed = 0.0f;
    for (int i = 0; i < 200 && !entered; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
        if (pc.is_in_water()) {
            entered = true;
            entry_tick = i;
            entry_speed = pc.get_velocity().y;
        }
    }
    CHECK(entered);
    // It arrived at falling speed, i.e. the water did not stop it at the surface.
    CHECK(entry_speed < -0.5f);
    CHECK(pc.get_position().y < 13.0f);

    // The drag bleeds that speed off and settles into a slow sink (~0.08
    // blocks/tick = 1.6 blocks/s). Without the water constants the body would
    // keep compounding gravity and hit the floor in a handful of ticks.
    for (int i = 0; i < 12; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_velocity().y > -0.15f);
    CHECK(pc.get_velocity().y < 0.0f);
    CHECK(pc.is_in_water());
    CHECK(!pc.is_on_floor());

    // It does still reach the bottom, just slowly: from the surface at 12.88 to
    // the floor at 1.0 is ~12 blocks of sinking.
    int sunk_ticks = 0;
    while (sunk_ticks < 600 && !pc.is_on_floor()) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
        ++sunk_ticks;
    }
    CHECK(pc.is_on_floor());
    CHECK(pc.get_position().y == doctest::Approx(1.0f).epsilon(0.05f));
    CHECK(sunk_ticks > 30);
}

// A half-height block, standing in for the slabs/stairs data/block_shapes.json
// supplies at runtime: the point is a block whose own cell is NOT a liquid while a
// body standing on it is under water.
static BlockID register_test_half_block() {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    BlockType half{};
    half.name = "test_half_block";
    half.properties = BlockProperty::Solid | BlockProperty::Opaque;
    half.visible_faces = {true, true, true, true, true, true};
    BlockAABB box{};
    box.min[0] = 0.0f; box.min[1] = 0.0f; box.min[2] = 0.0f;
    box.max[0] = 1.0f; box.max[1] = 0.5f; box.max[2] = 1.0f;
    half.selection_boxes = {box};
    half.full_cube_ = false;
    half.greedy_mergeable = false;
    return reg.register_block(half);
}

TEST_CASE("Standing on a submerged half block still counts as being in water") {
    const BlockID half = register_test_half_block();
    if (half == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        for (int x = 0; x < 32; ++x)
            for (int z = 0; z < 32; ++z) {
                d->set_block(x, 0, z, BlockIDs::STONE);
                d->set_block(x, 1, z, half);              // walkable top at y=1.5
                for (int y = 2; y <= 5; ++y)
                    d->set_block(x, y, z, BlockIDs::WATER);
            }
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.5f, 0.5f));
    PlayerInput idle;
    for (int i = 0; i < 10; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());
    CHECK(pc.get_position().y == doctest::Approx(1.5f).epsilon(0.02f));

    // The cell the feet are in holds the block, not liquid...
    CHECK(cr.is_liquid_at(0, 1, 0) == false);
    CHECK(cr.is_solid_at(0, 1, 0) == true);
    // ...yet the body is under water. A feet-only sample would call this body dry:
    // standing on a submerged slab, unaffected by drag, breathing.
    CHECK(pc.is_in_water());
}

TEST_CASE("Player swims up to the surface while holding jump") {
    ChunkMap cm;
    fill_water_pool(cm, 12);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(0.5f, 3.0f, 0.5f));

    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_in_water());
    const float start_y = pc.get_position().y;
    // Sinking with no input, which is what the rise has to beat.
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_position().y < start_y);

    PlayerSim swimmer;
    swimmer.reset(Vector3(0.5f, 3.0f, 0.5f));
    PlayerInput swim;
    swim.jump_pressed = true;
    float max_y = start_y;
    for (int i = 0; i < 300; ++i) {
        swimmer.accumulate_and_tick(1.0 / 20.0, swim, cr);
        max_y = std::max(max_y, swimmer.get_position().y);
    }
    // The water surface is 12.88; rising above 12.5 puts the body's head (1.8
    // tall) well clear of it, i.e. it floated up rather than sinking.
    CHECK(max_y > 12.5f);
}

TEST_CASE("Water breaks a fall, stone does not") {
    PlayerInput idle;

    ChunkMap cm;
    fill_water_pool(cm, 12);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 25.0f, 0.5f));
    for (int i = 0; i < 800 && !pc.is_on_floor(); ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());
    // ~12 blocks of falling in air, then in through the water: no damage.
    CHECK(pc.consume_pending_fall_damage() == 0);

    // Control: the identical drop onto plain stone always hurts, so the check
    // above cannot pass just because fall damage stopped working.
    ChunkMap flat;
    fill_flat_floor(flat);
    CollisionResolver cr_flat(&flat);
    PlayerSim pc_flat;
    pc_flat.reset(Vector3(0.5f, 25.0f, 0.5f));
    for (int i = 0; i < 800 && !pc_flat.is_on_floor(); ++i) {
        pc_flat.accumulate_and_tick(1.0 / 20.0, idle, cr_flat);
    }
    CHECK(pc_flat.is_on_floor());
    CHECK(pc_flat.consume_pending_fall_damage() > 0);
}

TEST_CASE("Swimming into a bank lifts the body out of the water") {
    ChunkMap cm;
    fill_pool_with_bank(cm);
    CollisionResolver cr(&cm);

    PlayerSim pc;
    pc.reset(Vector3(3.0f, 3.0f, 0.5f));

    PlayerInput idle;
    PlayerInput swim_toward_bank;
    swim_toward_bank.wish_direction = Vector3(1.0f, 0.0f, 0.0f);
    swim_toward_bank.jump_pressed = true;

    for (int i = 0; i < 200; ++i) {
        // Swim until the body is standing on something — not until a fixed tick
        // count, or it keeps walking past the edge of this one-chunk fixture.
        const bool swimming = !pc.is_on_floor();
        pc.accumulate_and_tick(1.0 / 20.0, swimming ? swim_toward_bank : idle, cr);
    }

    // Swimming alone tops out just under the surface (the in-water test fails at
    // 4.9) and can never reach the bank top at 5.0, so the only way to end up
    // standing above 4.9 is the lift while pressed against the wall.
    CHECK(pc.is_on_floor());
    CHECK(pc.get_position().y > 4.9f);
    CHECK(pc.get_position().x > 4.6f);
}

TEST_CASE("Swimming against a wall gains height that open water does not") {
    ChunkMap cm;
    fill_pool_with_bank(cm);
    CollisionResolver cr(&cm);

    // The bank's walkable top is y=5.0 while the water surface is 4.88, i.e. the
    // shore sits fractionally above the water line: a body that can only reach
    // the surface can never get out of the pool.
    PlayerSim into_wall;
    into_wall.reset(Vector3(4.0f, 3.0f, 0.5f));
    PlayerSim open_water;
    open_water.reset(Vector3(4.0f, 3.0f, 0.5f));

    PlayerInput inward;
    inward.wish_direction = Vector3(1.0f, 0.0f, 0.0f);   // into the bank
    inward.jump_pressed = true;
    PlayerInput outward;
    outward.wish_direction = Vector3(-1.0f, 0.0f, 0.0f);  // away, open water
    outward.jump_pressed = true;

    float into_wall_peak = into_wall.get_position().y;
    float open_water_peak = open_water.get_position().y;
    for (int i = 0; i < 80; ++i) {
        into_wall.accumulate_and_tick(1.0 / 20.0, inward, cr);
        open_water.accumulate_and_tick(1.0 / 20.0, outward, cr);
        into_wall_peak = std::max(into_wall_peak, into_wall.get_position().y);
        open_water_peak = std::max(open_water_peak, open_water.get_position().y);
    }
    // Open water: the body floats up to the surface (4.88) and stops there.
    CHECK(open_water_peak > 4.8f);
    CHECK(open_water_peak < 5.2f);
    // At the wall: the lift keeps climbing well past the bank top at 5.0.
    // Measured 6.26 against 5.08 for the identical open-water swim.
    CHECK(into_wall_peak > 5.5f);
    CHECK(into_wall_peak > open_water_peak + 0.5f);
}
