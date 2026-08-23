#ifndef FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP
#define FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP

#include "core/chunk_data.hpp"
#include "mesh/mesh_types.hpp"

namespace VoxelEngine {

inline bool has_boundary_blocks_on_face(const ChunkData& chunk, FaceDirection dir) {
    if (chunk.is_all_air()) return false;
    switch (dir) {
        case FaceDirection::Bottom: { // -Y
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    if (chunk.get_block_unsafe(x, 0, z) != BlockIDs::AIR) return true;
                }
            }
            break;
        }
        case FaceDirection::Top: { // +Y
            for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    if (chunk.get_block_unsafe(x, CHUNK_HEIGHT - 1, z) != BlockIDs::AIR) return true;
                }
            }
            break;
        }
        case FaceDirection::Back: { // -Z
            for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    if (chunk.get_block_unsafe(x, y, 0) != BlockIDs::AIR) return true;
                }
            }
            break;
        }
        case FaceDirection::Front: { // +Z
            for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
                for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                    if (chunk.get_block_unsafe(x, y, CHUNK_DEPTH - 1) != BlockIDs::AIR) return true;
                }
            }
            break;
        }
        default:
            break;
    }
    return false;
}

inline bool should_dirty_neighbor(const ChunkData* center_chunk, FaceDirection dir, const ChunkData* installed_chunk) {
    if (!center_chunk || !installed_chunk) return false;
    if (dir == FaceDirection::Left || dir == FaceDirection::Right) {
        return true; // shared X face
    }
    if (!center_chunk->is_all_air()) return true;
    return has_boundary_blocks_on_face(*installed_chunk, dir);
}

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_BOUNDARY_DIRTY_HPP
