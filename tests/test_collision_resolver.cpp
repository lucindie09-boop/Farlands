#include "doctest.h"
#include "chunk_map_fixture.hpp"
#include "engine/collision_resolver.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;
using namespace godot;

using chunktest::make_test_chunk;

TEST_CASE("CollisionResolver empty world") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    CollisionResolver cr(&cm);

    Vector3 pos(0.5f, 100.0f, 0.5f);
    Vector3 motion(0.0f, -1.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size);
    CHECK(result.position.y == doctest::Approx(99.0f).epsilon(0.01f));
    CHECK(result.on_floor == false);
}

TEST_CASE("CollisionResolver is_block_solid basic") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    cm.insert(cm.get_chunk_key(0, 0, 0),
              make_test_chunk([] {
                  auto d = std::make_unique<ChunkData>();
                  d->set_block(0, 0, 0, BlockIDs::STONE);
                  return d;
              }()));
    CHECK(cm.size() == 1);
    CHECK(cm.is_block_solid(0, 0, 0) == true);
    CHECK(cm.is_block_solid(1, 0, 0) == false);
    CHECK(cm.is_block_solid(0, 1, 0) == false);
}

TEST_CASE("CollisionResolver is_aabb_solid basic") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    cm.insert(cm.get_chunk_key(0, 0, 0),
              make_test_chunk([] {
                  auto d = std::make_unique<ChunkData>();
                  d->set_block(0, 0, 0, BlockIDs::STONE);
                  return d;
              }()));
    CollisionResolver cr(&cm);

    AABB solid_aabb(Vector3(0.0f, 0.0f, 0.0f), Vector3(0.5f, 0.5f, 0.5f));
    CHECK(cr.is_aabb_solid(solid_aabb) == true);

    AABB air_aabb(Vector3(2.0f, 2.0f, 2.0f), Vector3(0.5f, 0.5f, 0.5f));
    CHECK(cr.is_aabb_solid(air_aabb) == false);
}

TEST_CASE("CollisionResolver flat floor stop") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        for (int x = 0; x < 32; ++x)
            for (int z = 0; z < 32; ++z)
                d->set_block(x, 0, z, BlockIDs::STONE);
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    Vector3 pos(0.5f, 1.0f, 0.5f);
    Vector3 motion(0.0f, -1.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size);
    CHECK(result.on_floor == true);
    CHECK(result.collided_y == true);
    CHECK(result.position.y >= 0.99f);
}

TEST_CASE("CollisionResolver wall stop X") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        for (int y = 0; y < 3; ++y)
            for (int z = 0; z < 32; ++z)
                d->set_block(5, y, z, BlockIDs::STONE);
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    Vector3 pos(3.0f, 1.0f, 0.5f);
    Vector3 motion(5.0f, 0.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size);
    CHECK(result.collided_x == true);
    CHECK(result.position.x + size.x / 2.0f <= 5.01f);
}

// The crucible's model, in block-local 16ths: four 1x1 corner legs 2px tall, a
// 1px floor plate across the whole footprint, then four 1px walls open at the
// top. Cavity: x/z from 1/16 to 15/16, floor surface at 3/16 = 0.1875.
static std::vector<BlockAABB> hollow_pot_model() {
    struct Box { float mn[3]; float mx[3]; };
    static const Box kBoxes[] = {
        {{0.0f, 0.0f, 0.0f}, {0.0625f, 0.125f, 0.0625f}},
        {{0.9375f, 0.0f, 0.0f}, {1.0f, 0.125f, 0.0625f}},
        {{0.0f, 0.0f, 0.9375f}, {0.0625f, 0.125f, 1.0f}},
        {{0.9375f, 0.0f, 0.9375f}, {1.0f, 0.125f, 1.0f}},
        {{0.0f, 0.125f, 0.0f}, {1.0f, 0.1875f, 1.0f}},
        {{0.0f, 0.1875f, 0.0f}, {0.0625f, 1.0f, 1.0f}},
        {{0.9375f, 0.1875f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0625f, 0.1875f, 0.0f}, {0.9375f, 1.0f, 0.0625f}},
        {{0.0625f, 0.1875f, 0.9375f}, {0.9375f, 1.0f, 1.0f}},
    };
    std::vector<BlockAABB> boxes;
    for (const Box& b : kBoxes) {
        BlockAABB a;
        for (int i = 0; i < 3; ++i) {
            a.min[i] = b.mn[i];
            a.max[i] = b.mx[i];
        }
        boxes.push_back(a);
    }
    return boxes;
}

// A hollow model that declares NO collision override, so collision falls back to
// the visible boxes (what data/block_shapes.json's "crucible" entry does).
static BlockID register_hollow_pot() {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    BlockType pot{};
    pot.name = "test_hollow_pot";
    pot.properties = BlockProperty::Solid | BlockProperty::Opaque;
    pot.visible_faces = {true, true, true, true, true, true};
    pot.selection_boxes = hollow_pot_model();
    pot.full_cube_ = false;
    pot.greedy_mergeable = false;
    return reg.register_block(pot);
}

TEST_CASE("collision falls back to the visible model when no override is declared") {
    const BlockID pot = register_hollow_pot();
    if (pot == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& bt = BlockRegistry::get_instance().get_block(pot);

    CHECK(bt.is_full_cube() == false);
    CHECK(bt.collision_boxes.empty());
    CHECK(bt.get_collision_boxes().size() == 9);
    // ...and the fallback really is the model, not a stand-in full cube.
    CHECK(bt.get_collision_boxes().size() == bt.selection_boxes.size());
    CHECK(bt.get_collision_boxes()[4].max[1] == doctest::Approx(0.1875f).epsilon(0.001f));

    // An explicit override still wins over the model (the pole's 1.5-high box).
    BlockType overridden = bt;
    overridden.name = "test_hollow_pot_override";
    BlockAABB cube;
    for (int i = 0; i < 3; ++i) {
        cube.min[i] = 0.0f;
        cube.max[i] = 1.0f;
    }
    overridden.collision_boxes = {cube};
    CHECK(overridden.get_collision_boxes().size() == 1);
    CHECK(overridden.get_collision_boxes()[0].max[1] == doctest::Approx(1.0f).epsilon(0.001f));
}

// Flat ground with the pot resting on it at cell (3,1,3): legs on the ground
// (top y=1.0), inner floor surface at y=1.1875, rim at y=2.0.
static void make_hollow_pot_fixture(ChunkMap& cm, BlockID pot) {
    auto d = std::make_unique<ChunkData>();
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 8; ++z)
            d->set_block(x, 0, z, BlockIDs::STONE);  // floor, top at y=1.0
    d->set_block(3, 1, 3, pot);
    cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
}

TEST_CASE("a body stands INSIDE a hollow block, on its inner floor") {
    const BlockID pot = register_hollow_pot();
    if (pot == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    ChunkMap cm;
    make_hollow_pot_fixture(cm, pot);
    CollisionResolver cr(&cm);

    // Dropped straight down the open mouth, centred on the cell so the body
    // (0.6 wide) fits the 14/16-wide cavity without touching a wall.
    Vector3 pos(3.5f, 5.0f, 3.5f);
    Vector3 motion(0.0f, -4.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size);
    CHECK(result.collided_y == true);
    // Feet on the inner floor plate (1 + 0.1875), NOT on the rim (2.0) — the
    // whole point of dropping the full-cube collision override.
    CHECK(result.position.y == doctest::Approx(1.1875f).epsilon(0.01f));
    CHECK(result.on_floor == true);

    // It really is inside the vessel: the body's own span stays within the
    // cavity walls, with the rim above the feet.
    CHECK(result.position.x - size.x * 0.5f > 3.0625f);
    CHECK(result.position.x + size.x * 0.5f < 3.9375f);
    CHECK(result.position.z - size.z * 0.5f > 3.0625f);
    CHECK(result.position.z + size.z * 0.5f < 3.9375f);

    // Standing there is stable: a second tick with no motion neither sinks nor
    // gets ejected, and the floor probe still reports ground under the feet.
    auto again = cr.resolve(result.position, Vector3(0.0f, 0.0f, 0.0f), size);
    CHECK(again.position.y == doctest::Approx(result.position.y).epsilon(0.01f));
    CHECK(again.on_floor == true);

    // ...and a jump out is possible: the 13/16 rim is below the 1.1-block
    // vanilla jump apex, so the body clears the wall from a standing start.
    CHECK(2.0f - result.position.y < 1.1f);
}

TEST_CASE("a body cannot walk into a hollow block from the side") {
    const BlockID pot = register_hollow_pot();
    if (pot == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    ChunkMap cm;
    make_hollow_pot_fixture(cm, pot);
    CollisionResolver cr(&cm);

    // Standing on the ground outside the pot, walking in +X at it. The legs sit
    // in the cells' outer 1/16 columns only, but the full-footprint floor plate
    // crosses the body's whole height span, so the approach is blocked.
    Vector3 pos(1.5f, 1.0f, 3.5f);
    Vector3 motion(6.0f, 0.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size, 0.6f);
    CHECK(result.collided_x == true);
    CHECK(result.position.x + size.x * 0.5f <= 3.01f);  // stopped at the pot's face
    CHECK(result.stepped_up == false);                   // 3/16 + wall is too tall to step

    // The same walk in -Z is blocked too: no gap between the legs at body height.
    auto sideways = cr.resolve(Vector3(3.5f, 1.0f, 1.5f), Vector3(0.0f, 0.0f, 6.0f), size, 0.6f);
    CHECK(sideways.collided_z == true);
    CHECK(sideways.position.z + size.z * 0.5f <= 3.01f);
}

// A liquid's shape is a surface height, not a wall. Note that the two registries
// describe water differently — the built-in defaults make it a full cube while
// data/block_shapes.json gives it a lowered shape — so the answer has to hold for
// both. This fixture uses the built-in defaults, the stricter of the two.
TEST_CASE("liquids do not stop bodies") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        for (int x = 0; x < 8; ++x)
            for (int z = 0; z < 8; ++z)
                d->set_block(x, 0, z, BlockIDs::STONE);   // floor, top at y=1.0
        for (int y = 1; y <= 4; ++y)
            d->set_block(2, y, 2, BlockIDs::WATER);      // a 4-deep water column
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    CHECK(cr.is_liquid_at(2, 3, 2) == true);
    CHECK(cr.is_solid_at(2, 3, 2) == false);   // swum through, not stood on
    CHECK(cr.is_solid_at(2, 0, 2) == true);    // the floor under it still stops a body
    CHECK(cr.is_liquid_at(2, 0, 2) == false);
    // The raw non-air query is deliberately unchanged: "is a block here" and
    // "does it stop a body" are different questions, and callers that want the
    // first (worldgen, terrain queries) keep asking it.
    CHECK(cm.is_block_solid(2, 3, 2) == true);

    // Same for the AABB path, which is what the swept collision actually uses.
    CHECK(cr.is_aabb_solid(AABB(Vector3(2.2f, 2.2f, 2.2f), Vector3(0.6f, 0.6f, 0.6f))) == false);
    CHECK(cr.is_aabb_solid(AABB(Vector3(2.2f, 0.2f, 2.2f), Vector3(0.6f, 0.6f, 0.6f))) == true);

    // A body dropped in falls through the water and lands on the floor beneath.
    auto result = cr.resolve(Vector3(2.5f, 8.0f, 2.5f), Vector3(0.0f, -10.0f, 0.0f),
                             Vector3(0.6f, 1.8f, 0.6f));
    CHECK(result.collided_y == true);
    CHECK(result.position.y == doctest::Approx(1.0f).epsilon(0.01f));
    CHECK(result.on_floor == true);
}

TEST_CASE("CollisionResolver no collision in open air") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    CollisionResolver cr(&cm);

    Vector3 pos(10.0f, 100.0f, 10.0f);
    Vector3 motion(1.0f, -2.0f, 1.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size);
    CHECK(result.position.x == doctest::Approx(pos.x + motion.x).epsilon(0.01f));
    CHECK(result.position.y == doctest::Approx(pos.y + motion.y).epsilon(0.01f));
    CHECK(result.position.z == doctest::Approx(pos.z + motion.z).epsilon(0.01f));
    CHECK(result.on_floor == false);
}

// Shared fixture: a flat floor (y=0 across the chunk) plus a one-block step wall
// at x=2 (occupying y=1, top at y=2.0). The player starts standing on the floor
// with feet at y=1.0 (body y in [1.0, 2.8)) and walks in +X toward the wall.
static void make_step_fixture(ChunkMap& cm) {
    auto d = std::make_unique<ChunkData>();
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 4; ++z) {
            d->set_block(x, 0, z, BlockIDs::STONE);           // floor, top at y=1.0
            d->set_block(x, 1, z, BlockIDs::STONE);           // fill wall column
        }
    // Hollow out the floor at y=1 everywhere except the step wall at x=2.
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 4; ++z)
            if (x != 2) d->set_block(x, 1, z, BlockIDs::AIR);
    cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
}

TEST_CASE("CollisionResolver step too tall") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    make_step_fixture(cm);
    CollisionResolver cr(&cm);

    // 1-block step with step_height=0.6 (vanilla max): the raised body still
    // overlaps the wall, so stepping must be rejected and the player blocked.
    Vector3 pos(0.5f, 1.0f, 0.5f);
    Vector3 motion(2.0f, 0.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size, 0.6f);
    CHECK(result.stepped_up == false);
    CHECK(result.collided_x == true);
    CHECK(result.on_floor == true);
    CHECK(result.position.x + size.x / 2.0f <= 2.01f);  // stopped at the wall face
}

TEST_CASE("CollisionResolver step within height") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    make_step_fixture(cm);
    CollisionResolver cr(&cm);

    // Same 1-block step but step_height=1.0 (synthetic: no block in the game is
    // <1 tall, so proving the mechanism needs a step_height >= the full cube).
    // The raised body clears the wall, horizontal travel succeeds, and the
    // player settles on top of the step (feet at y=2.0).
    Vector3 pos(0.5f, 1.0f, 0.5f);
    Vector3 motion(2.0f, 0.0f, 0.0f);
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size, 1.0f);
    CHECK(result.stepped_up == true);
    CHECK(result.on_floor == true);
    CHECK(result.position.x > 2.0f);          // cleared the wall
    CHECK(result.position.y >= 1.99f);        // settled on the step top (y=2.0)
}
