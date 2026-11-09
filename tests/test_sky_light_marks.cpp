// Whether a sky-light pass asks the mesh manager for anything.
//
// The fill's tail used to pay for a remesh of every chunk a light pass touched.
// An all-air band has no geometry for the light to move, so the pass writes
// nothing and must not reach the manager at all; the manager's own all-air skip
// (queue_light_dirty_chunk) is the second guard behind that. This test sits at the
// pass boundary and records what the propagator asks for, so an empty request is
// observable rather than inferred from a mask.
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "lighting/light_propagator.hpp"
#include "mesh/mesh_manager.hpp"
#include "mesh_manager_stub.hpp"

#include <cstring>
#include <memory>

using namespace VoxelEngine;

namespace {

// ChunkRenderData carries engine-typed members the test build never touches, so it
// is allocated raw and zeroed the way the other light tests do.
std::unique_ptr<ChunkRenderData> make_test_chunk() {
    void* buf = ::operator new(sizeof(ChunkRenderData));
    std::memset(buf, 0, sizeof(ChunkRenderData));
    auto* rd = reinterpret_cast<ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<ChunkData>(std::make_unique<ChunkData>());
    rd->is_mesh_dirty = false;
    return std::unique_ptr<ChunkRenderData>(rd);
}

} // namespace

TEST_CASE("a sky write over an all-air band asks for no remesh") {
    BlockRegistry::get_instance().initialize_default_blocks();

    ChunkMap map;
    for (int32_t cy = -1; cy <= 1; ++cy)
        for (int32_t cz = -1; cz <= 1; ++cz)
            for (int32_t cx = -1; cx <= 1; ++cx)
                map.insert(map.get_chunk_key(cx, cy, cz), make_test_chunk());

    // Every chunk runs its own column scan first, exactly as generation does; an
    // open chunk ends up at full sky and has nothing for the walk to raise.
    for (int32_t cy = -1; cy <= 1; ++cy) {
        for (int32_t cz = -1; cz <= 1; ++cz) {
            for (int32_t cx = -1; cx <= 1; ++cx) {
                auto lock = map.lock_keys_exclusive({map.get_chunk_key(cx, cy, cz)});
                ChunkData* chunk = map.get_chunk_data_fast(cx, cy, cz);
                if (chunk) chunk->propagate_sky_light(nullptr);
            }
        }
    }

    LightPropagator propagator;
    propagator.set_chunk_map(&map);
    MeshManager manager;
    propagator.set_mesh_manager(&manager);
    mesh_manager_stub::clear_light_marks();

    // Open sky everywhere: the walk has nothing to write, so the wrapper must not
    // call the manager even once.
    CHECK(propagator.scatter_sky_light_region(0, 0, 0) == 0);
    CHECK(mesh_manager_stub::light_marks().empty());

    // A roof under a live manager: the same pass now writes the dimple and must
    // reach the manager, so an empty list above is a real "nothing to mark".
    {
        auto lock = map.lock_keys_exclusive({map.get_chunk_key(0, 0, 0)});
        ChunkData* centre = map.get_chunk_data_fast(0, 0, 0);
        for (int32_t z = 10; z <= 12; ++z)
            for (int32_t x = 10; x <= 12; ++x)
                centre->set_block(x, 20, z, BlockIDs::STONE);
        centre->propagate_sky_light(nullptr);
    }
    mesh_manager_stub::clear_light_marks();
    CHECK(propagator.scatter_sky_light_region(0, 0, 0) != 0);
    CHECK_FALSE(mesh_manager_stub::light_marks().empty());
}
