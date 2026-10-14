#include "doctest.h"

#include "player_controller_test_support.hpp"

#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/player_controller.hpp"

#include <memory>

using namespace VoxelEngine;
using namespace godot;
using namespace player_controller_test;

// A flat stone floor across the surrounding chunks with one extra cell of `id`
// laid on top, so a body can be placed overlapping it.
static void fill_floor_with_block(ChunkMap& cm, BlockID id, int bx, int by, int bz) {
    BlockRegistry::get_instance().initialize_default_blocks();
    for (int chx = -1; chx <= 1; ++chx) {
        for (int chz = -1; chz <= 1; ++chz) {
            auto d = std::make_unique<ChunkData>();
            for (int x = 0; x < 32; ++x)
                for (int z = 0; z < 32; ++z)
                    d->set_block(x, 0, z, BlockIDs::STONE);
            if (chx == 0 && chz == 0 && id != BlockIDs::AIR) {
                d->set_block(bx, by, bz, id);
            }
            cm.insert(cm.get_chunk_key(chx, 0, chz), make_test_chunk(std::move(d)));
        }
    }
}

TEST_CASE("Suffocation needs a full solid cube") {
    ChunkMap cm;
    fill_flat_floor(cm);  // stone at y=0
    CollisionResolver cr(&cm);
    CHECK(cr.is_suffocating_at(0, 0, 0));       // the stone floor: a full cube
    CHECK_FALSE(cr.is_suffocating_at(0, 5, 0)); // air
}

TEST_CASE("A liquid does not suffocate") {
    ChunkMap cm;
    fill_water_pool(cm, 4);
    CollisionResolver cr(&cm);
    CHECK_FALSE(cr.is_suffocating_at(0, 2, 0)); // the water
    CHECK(cr.is_suffocating_at(0, 0, 0));       // the stone floor under it
}

TEST_CASE("contact_damage_for_aabb reports a touching cactus cell") {
    ChunkMap cm;
    fill_floor_with_block(cm, BlockIDs::CACTUS, 1, 1, 0);
    CollisionResolver cr(&cm);

    // A box inside the cactus cell reports its damage...
    CHECK(cr.contact_damage_for_aabb(AABB(Vector3(1.1f, 1.1f, 0.1f), Vector3(0.2f, 0.2f, 0.2f))) == 1);
    // ...and a box clear of it reports none.
    CHECK(cr.contact_damage_for_aabb(AABB(Vector3(5.0f, 1.1f, 5.0f), Vector3(0.2f, 0.2f, 0.2f))) == 0);
}

TEST_CASE("Player drowns after the breath runs out under water") {
    ChunkMap cm;
    fill_water_pool(cm, 4);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 2.0f, 0.5f));
    PlayerInput idle;

    // Submerged well short of the breath limit: no damage yet.
    for (int i = 0; i < 50; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_air_ticks() < PlayerSim::MAX_AIR);
    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(0.0f));

    // Run past the limit with the eye never clearing: the breath empties and the
    // drown source starts landing.
    for (int i = 0; i < PlayerSim::MAX_AIR + 60; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_air_ticks() == 0);
    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta > 0.0f);
    CHECK(e.hurt);
}

TEST_CASE("A body with its eye above the water does not drown") {
    ChunkMap cm;
    fill_water_pool(cm, 1);  // only the feet cell is water; the eye is in air
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    PlayerInput idle;

    for (int i = 0; i < 400; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_in_water());  // the feet are wet...
    CHECK(pc.get_air_ticks() == PlayerSim::MAX_AIR);  // ...but the eye breathes
    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(0.0f));
}

TEST_CASE("A body overlapping a cactus takes its contact damage") {
    ChunkMap cm;
    fill_floor_with_block(cm, BlockIDs::CACTUS, 0, 1, 0);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    PlayerInput idle;

    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(1.0f));
    CHECK(e.hurt);
}

TEST_CASE("A body clear of a cactus takes no contact damage") {
    ChunkMap cm;
    fill_floor_with_block(cm, BlockIDs::CACTUS, 3, 1, 0);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    PlayerInput idle;

    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(0.0f));
}

TEST_CASE("A head inside a full solid block suffocates") {
    ChunkMap cm;
    // Stone in the body's head cell (feet rest at y=1, the eye sits at ~2.62).
    fill_floor_with_block(cm, BlockIDs::STONE, 0, 2, 0);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    PlayerInput idle;

    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(1.0f));
    CHECK(e.hurt);
}
