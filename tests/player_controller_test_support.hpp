#ifndef FARLANDS_TESTS_PLAYER_CONTROLLER_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_PLAYER_CONTROLLER_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the player controller tests.
//
// The controller needs a world to stand in: `make_test_chunk` hand-builds the
// render-data wrapper a chunk map stores (there is no engine runtime here to make
// one), the two `fill_*` helpers lay down the floor every locomotion test walks on
// and the pool the water tests swim in, and `make_forward_input` is the one input
// shape the movement tests share.
//
// These were file-local to test_player_controller.cpp (plain `static` functions);
// the split moved them here as `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/player_controller.hpp"

#include <cstring>
#include <memory>

using namespace VoxelEngine;
using namespace godot;

namespace player_controller_test {

inline std::unique_ptr<ChunkRenderData> make_test_chunk(std::unique_ptr<ChunkData> data) {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::move(data));
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

inline void fill_flat_floor(ChunkMap& cm) {
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

// A pool: stone floor at y=0 and water from y=1 to `water_top` everywhere in the
// surrounding chunks. The water's surface height is water_top + 0.88.
inline void fill_water_pool(ChunkMap& cm, int water_top) {
    BlockRegistry::get_instance().initialize_default_blocks();
    for (int cx = -1; cx <= 1; ++cx) {
        for (int cz = -1; cz <= 1; ++cz) {
            auto d = std::make_unique<ChunkData>();
            for (int x = 0; x < 32; ++x)
                for (int z = 0; z < 32; ++z) {
                    d->set_block(x, 0, z, BlockIDs::STONE);
                    for (int y = 1; y <= water_top; ++y)
                        d->set_block(x, y, z, BlockIDs::WATER);
                }
            cm.insert(cm.get_chunk_key(cx, 0, cz), make_test_chunk(std::move(d)));
        }
    }
}

// The same pool with a bank: stone from x=5 up to y=4, so its walkable top is at
// y=5.0 while the water surface is only 4.88. That is the shape a body cannot get
// out of by swimming alone, which is what the swim-into-a-wall lift is for.
inline void fill_pool_with_bank(ChunkMap& cm) {
    BlockRegistry::get_instance().initialize_default_blocks();
    auto d = std::make_unique<ChunkData>();
    for (int x = 0; x < 32; ++x)
        for (int z = 0; z < 32; ++z) {
            d->set_block(x, 0, z, BlockIDs::STONE);
            for (int y = 1; y <= 4; ++y)
                d->set_block(x, y, z, x >= 5 ? BlockIDs::STONE : BlockIDs::WATER);
        }
    cm.insert(cm.get_chunk_key(0, 0, 0), make_test_chunk(std::move(d)));
}

inline PlayerInput make_forward_input(bool sprint = false, bool sneak = false) {
    PlayerInput input;
    input.wish_direction = Vector3(0.0f, 0.0f, -1.0f); // forward = -Z
    input.sprint_held = sprint;
    input.sneak_held = sneak;
    input.move_forward_held = true;
    return input;
}

} // namespace player_controller_test

#endif // FARLANDS_TESTS_PLAYER_CONTROLLER_TEST_SUPPORT_HPP
