#ifndef FARLANDS_TESTS_CHUNK_MAP_FIXTURE_HPP
#define FARLANDS_TESTS_CHUNK_MAP_FIXTURE_HPP

// Shared fixture for tests that need a real ChunkMap.
//
// godot::RID default-constructs via GDExtension bindings that are only
// initialised inside the Godot runtime. In a standalone test binary those
// pointers are null, so constructing ChunkRenderData (which embeds two RIDs)
// immediately SIGSEGVs. Workaround: allocate the struct with ::operator new,
// zero the memory (RID opaque bytes = 0 is a valid "null" RID), then
// placement-new only the data member we actually need.

#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_types.hpp"
#include "mesh/chunk_render_data.hpp"

#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <utility>

namespace chunktest {

inline std::unique_ptr<VoxelEngine::ChunkRenderData> make_test_chunk(
    std::unique_ptr<VoxelEngine::ChunkData> data) {
    void* buf = ::operator new(sizeof(VoxelEngine::ChunkRenderData));
    std::memset(buf, 0, sizeof(VoxelEngine::ChunkRenderData));
    auto* rd = reinterpret_cast<VoxelEngine::ChunkRenderData*>(buf);
    new (&rd->data) std::unique_ptr<VoxelEngine::ChunkData>(std::move(data));
    rd->is_mesh_dirty = false;
    return std::unique_ptr<VoxelEngine::ChunkRenderData>(rd);
}

// Fills a fresh chunk with `fill` and inserts it under its chunk coordinates.
inline void insert_chunk(VoxelEngine::ChunkMap& map, int32_t cx, int32_t cy, int32_t cz,
                         const std::function<void(VoxelEngine::ChunkData&)>& fill) {
    auto data = std::make_unique<VoxelEngine::ChunkData>();
    fill(*data);
    map.insert(map.get_chunk_key(cx, cy, cz), make_test_chunk(std::move(data)));
}

// A chunk whose floor is a single layer of stone at local y=0, so every column
// in it has a standable surface with its top at world y = cy*32 + 1.
inline void insert_floor_chunk(VoxelEngine::ChunkMap& map, int32_t cx, int32_t cy, int32_t cz) {
    insert_chunk(map, cx, cy, cz, [](VoxelEngine::ChunkData& d) {
        for (int32_t x = 0; x < VoxelEngine::CHUNK_WIDTH; ++x)
            for (int32_t z = 0; z < VoxelEngine::CHUNK_DEPTH; ++z)
                d.set_block(x, 0, z, VoxelEngine::BlockIDs::STONE);
    });
}

} // namespace chunktest

#endif // FARLANDS_TESTS_CHUNK_MAP_FIXTURE_HPP
