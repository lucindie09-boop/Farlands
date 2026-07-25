#include "doctest.h"
#include "engine/collision_resolver.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;
using namespace godot;

// godot::RID default-constructs via GDExtension bindings that are only
// initialised inside the Godot runtime.  In a standalone test binary those
// pointers are null, so constructing ChunkRenderData (which embeds two RIDs)
// immediately SIGSEGVs.  Workaround: allocate the struct with ::operator new,
// zero the memory (RID opaque bytes = 0 is a valid "null" RID), then
// placement-new only the data member that we actually need.
static std::unique_ptr<ChunkRenderData> make_test_chunk(std::unique_ptr<ChunkData> data) {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::move(data));
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

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
    CHECK(result.position.x + size.x <= 5.01f);
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

TEST_CASE("CollisionResolver step too tall") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        // Create a 1-block step: solid at y=0, empty at y=1
        d->set_block(0, 0, 0, BlockIDs::STONE);
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    // Approach from below with step_height=0 (disabled)
    Vector3 pos(0.5f, 0.5f, 0.5f);
    Vector3 motion(0.0f, 0.6f, 0.0f);  // Try to step up 0.6 blocks
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size, 0.0f);
    // With step_height=0, player should stop at the block edge
    CHECK(result.on_floor == true);
    CHECK(result.stepped_up == false);
    CHECK(result.position.y <= 1.01f);  // Should not climb onto the block
}

TEST_CASE("CollisionResolver step within height") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkMap cm;
    {
        auto d = std::make_unique<ChunkData>();
        // Synthetic fixture: no block in the current game produces a 0.6-tall obstruction
        // (all blocks are full 1×1×1 cubes). Manually set a block at y=0, leave y=1 empty,
        // and approach from a position where the horizontal AABB clips the block edge.
        d->set_block(0, 0, 0, BlockIDs::STONE);
        cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
    }
    CollisionResolver cr(&cm);

    // Approach from a fractional Y that clips the block edge
    Vector3 pos(0.5f, 0.4f, 0.5f);  // Feet at y=0.4, so horizontal motion clips the block at y=0
    Vector3 motion(0.5f, 0.0f, 0.0f);  // Move horizontally into the block
    Vector3 size(0.6f, 1.8f, 0.6f);

    auto result = cr.resolve(pos, motion, size, 0.6f);
    // With step_height=0.6, player should step up onto the block
    CHECK(result.stepped_up == true);
    CHECK(result.position.y >= 0.99f);  // Should be raised to top of block (y=1.0)
}
