#ifndef FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP
#define FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP

#include "core/chunk_data.hpp"
#include "mesh/mesh_types.hpp"

namespace VoxelEngine {

// Whether the neighbour on one face of a freshly installed chunk has to be
// re-meshed. The only question left is whether that neighbour can draw at all:
// a chunk that is all air never can, whatever arrived beside it, so queueing it
// buys a pop that frees the RIDs it never made - a third of the fill's drain
// pops were exactly this (docs/streaming.md). Every non-air neighbour is worth
// a rebuild on any face, the X faces included: their boundary is shared by
// construction, and the old face-by-face test only ever fired for an all-air
// neighbour anyway.
inline bool should_dirty_neighbor(const ChunkData* center_chunk, FaceDirection, const ChunkData* installed_chunk) {
    if (!center_chunk || !installed_chunk) return false;
    return !center_chunk->is_all_air();
}

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP
