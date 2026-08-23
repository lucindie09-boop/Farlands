#ifndef FARLANDS_CHUNK_NEIGHBOR_ACCESSOR_HPP
#define FARLANDS_CHUNK_NEIGHBOR_ACCESSOR_HPP
#include "core/chunk_data.hpp"

namespace VoxelEngine {

class ChunkNeighborAccessor {
public:
    const ChunkData* center = nullptr;

    // 6 orthogonal neighbors
    const ChunkData* neg_x = nullptr;
    const ChunkData* pos_x = nullptr;
    const ChunkData* neg_y = nullptr;
    const ChunkData* pos_y = nullptr;
    const ChunkData* neg_z = nullptr;
    const ChunkData* pos_z = nullptr;

    // 4 X-Z plane diagonals (same y-level)
    const ChunkData* neg_x_neg_z = nullptr;
    const ChunkData* neg_x_pos_z = nullptr;
    const ChunkData* pos_x_neg_z = nullptr;
    const ChunkData* pos_x_pos_z = nullptr;

    // 4 Y-X plane diagonals
    const ChunkData* neg_x_neg_y = nullptr;
    const ChunkData* pos_x_neg_y = nullptr;
    const ChunkData* neg_x_pos_y = nullptr;
    const ChunkData* pos_x_pos_y = nullptr;

    // 4 Y-Z plane diagonals
    const ChunkData* neg_y_neg_z = nullptr;
    const ChunkData* neg_y_pos_z = nullptr;
    const ChunkData* pos_y_neg_z = nullptr;
    const ChunkData* pos_y_pos_z = nullptr;

    // 8 triple-corner diagonals
    const ChunkData* neg_x_neg_y_neg_z = nullptr;
    const ChunkData* pos_x_neg_y_neg_z = nullptr;
    const ChunkData* neg_x_pos_y_neg_z = nullptr;
    const ChunkData* pos_x_pos_y_neg_z = nullptr;
    const ChunkData* neg_x_neg_y_pos_z = nullptr;
    const ChunkData* pos_x_neg_y_pos_z = nullptr;
    const ChunkData* neg_x_pos_y_pos_z = nullptr;
    const ChunkData* pos_x_pos_y_pos_z = nullptr;

    BlockID get_block(int32_t x, int32_t y, int32_t z) const;
    uint16_t get_light_packed(int32_t x, int32_t y, int32_t z) const;
    uint8_t get_sky_light(int32_t x, int32_t y, int32_t z) const;
    bool is_occluder(int32_t x, int32_t y, int32_t z) const;

private:
    // Resolve a diagonal/corner neighbor for multi-axis out-of-bounds access.
    // dx, dy, dz are in {-1, 0, 1} with at least two non-zero.
    const ChunkData* lookup_diagonal(int dx, int dy, int dz) const {
        if (dx != 0 && dy != 0 && dz == 0) {
            if (dx == -1 && dy == -1) return neg_x_neg_y;
            if (dx ==  1 && dy == -1) return pos_x_neg_y;
            if (dx == -1 && dy ==  1) return neg_x_pos_y;
            if (dx ==  1 && dy ==  1) return pos_x_pos_y;
        }
        if (dx == 0 && dy != 0 && dz != 0) {
            if (dy == -1 && dz == -1) return neg_y_neg_z;
            if (dy == -1 && dz ==  1) return neg_y_pos_z;
            if (dy ==  1 && dz == -1) return pos_y_neg_z;
            if (dy ==  1 && dz ==  1) return pos_y_pos_z;
        }
        if (dx != 0 && dy != 0 && dz != 0) {
            if (dx == -1 && dy == -1 && dz == -1) return neg_x_neg_y_neg_z;
            if (dx ==  1 && dy == -1 && dz == -1) return pos_x_neg_y_neg_z;
            if (dx == -1 && dy ==  1 && dz == -1) return neg_x_pos_y_neg_z;
            if (dx ==  1 && dy ==  1 && dz == -1) return pos_x_pos_y_neg_z;
            if (dx == -1 && dy == -1 && dz ==  1) return neg_x_neg_y_pos_z;
            if (dx ==  1 && dy == -1 && dz ==  1) return pos_x_neg_y_pos_z;
            if (dx == -1 && dy ==  1 && dz ==  1) return neg_x_pos_y_pos_z;
            if (dx ==  1 && dy ==  1 && dz ==  1) return pos_x_pos_y_pos_z;
        }
        return nullptr;
    }
};

} // namespace VoxelEngine
#endif
