#ifndef FARLANDS_LIGHT_PROPAGATION_HPP
#define FARLANDS_LIGHT_PROPAGATION_HPP
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <array>
#include <cstdint>
#include <vector>

namespace VoxelEngine {

class ChunkData;

struct LightNode {
    int32_t cx = 0;
    int32_t cy = 0;
    int32_t cz = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

inline constexpr int16_t kDiamondOffsets[6][3] = {
    {1, 0, 0}, {-1, 0, 0},
    {0, 1, 0}, {0, -1, 0},
    {0, 0, 1}, {0, 0, -1}
};

struct EmissiveSource {
    int8_t rdx = 0;
    int8_t rdy = 0;
    int8_t rdz = 0;
    int16_t x = 0;
    int16_t y = 0;
    int16_t z = 0;
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    LightEmissionPattern pattern = LightEmissionPattern::Diamond;
};

// For BlockLightRegion (relative chunk coordinates)
inline bool wrap_local_to_region(int16_t& nx, int16_t& ny, int16_t& nz,
                                  int8_t& rdx, int8_t& rdy, int8_t& rdz) {
    if (nx < 0) { nx += CHUNK_WIDTH; rdx--; }
    else if (nx >= CHUNK_WIDTH) { nx -= CHUNK_WIDTH; rdx++; }
    if (ny < 0) { ny += CHUNK_HEIGHT; rdy--; }
    else if (ny >= CHUNK_HEIGHT) { ny -= CHUNK_HEIGHT; rdy++; }
    if (nz < 0) { nz += CHUNK_DEPTH; rdz--; }
    else if (nz >= CHUNK_DEPTH) { nz -= CHUNK_DEPTH; rdz++; }
    return rdx >= -1 && rdx <= 1 && rdy >= -1 && rdy <= 1 && rdz >= -1 && rdz <= 1;
}

// For world-space propagation (absolute chunk coordinates)
inline void wrap_local_to_world(int16_t& nx, int16_t& ny, int16_t& nz,
                                 int32_t& cx, int32_t& cy, int32_t& cz) {
    if (nx < 0) { nx += CHUNK_WIDTH; cx--; }
    else if (nx >= CHUNK_WIDTH) { nx -= CHUNK_WIDTH; cx++; }
    if (ny < 0) { ny += CHUNK_HEIGHT; cy--; }
    else if (ny >= CHUNK_HEIGHT) { ny -= CHUNK_HEIGHT; cy++; }
    if (nz < 0) { nz += CHUNK_DEPTH; cz--; }
    else if (nz >= CHUNK_DEPTH) { nz -= CHUNK_DEPTH; cz++; }
}

void propagate_chunk_block_light_additive(ChunkData& chunk);

} // namespace VoxelEngine

#endif // FARLANDS_LIGHT_PROPAGATION_HPP