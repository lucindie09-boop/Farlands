#ifndef FUK_MINECRAFT_CHUNK_COORDS_HPP
#define FUK_MINECRAFT_CHUNK_COORDS_HPP
#include <cstdint>

namespace VoxelEngine {

// -----------------------------------------------------------------------------
// Chunk Dimensions
// -----------------------------------------------------------------------------
constexpr int32_t CHUNK_WIDTH  = 32;
constexpr int32_t CHUNK_HEIGHT = 32;
constexpr int32_t CHUNK_DEPTH  = 32;
constexpr int32_t CHUNK_VOLUME = CHUNK_WIDTH * CHUNK_HEIGHT * CHUNK_DEPTH;

constexpr int32_t WORLD_HEIGHT_Y = 1024;
constexpr int32_t SECTION_HEIGHT = 16;
constexpr int32_t CHUNK_SECTIONS = CHUNK_HEIGHT / SECTION_HEIGHT; // 32 / 16 = 2

constexpr int32_t SUBCHUNK_SIZE = 16;
constexpr int32_t SUBCHUNK_DIM = CHUNK_WIDTH / SUBCHUNK_SIZE; // 2
constexpr int32_t NUM_SUBCHUNKS = SUBCHUNK_DIM * SUBCHUNK_DIM * SUBCHUNK_DIM; // 8

// -----------------------------------------------------------------------------
// Position Types
// -----------------------------------------------------------------------------
struct ChunkPos {
    int32_t x;
    int32_t y;
    int32_t z;

    [[nodiscard]] constexpr bool operator==(const ChunkPos& other) const noexcept {
        return x == other.x && y == other.y && z == other.z;
    }
};

struct BlockPos {
    int32_t x;
    int32_t y;
    int32_t z;

    [[nodiscard]] constexpr int32_t to_index() const noexcept {
        return x + y * CHUNK_WIDTH + z * CHUNK_WIDTH * CHUNK_HEIGHT;
    }

    [[nodiscard]] constexpr bool is_valid() const noexcept {
        return x >= 0 && x < CHUNK_WIDTH &&
               y >= 0 && y < CHUNK_HEIGHT &&
               z >= 0 && z < CHUNK_DEPTH;
    }
};

// -----------------------------------------------------------------------------
// Sub-chunk Helpers
// -----------------------------------------------------------------------------
[[nodiscard]] inline constexpr int32_t subchunk_index(int32_t lx, int32_t ly, int32_t lz) noexcept {
    return (lx / SUBCHUNK_SIZE) + (ly / SUBCHUNK_SIZE) * SUBCHUNK_DIM + (lz / SUBCHUNK_SIZE) * SUBCHUNK_DIM * SUBCHUNK_DIM;
}

// -----------------------------------------------------------------------------
// World-to-Chunk Coordinate Conversion
// -----------------------------------------------------------------------------
inline constexpr void world_to_chunk_local(int32_t world_x, int32_t world_y, int32_t world_z,
                                           int32_t& chunk_x, int32_t& chunk_y, int32_t& chunk_z,
                                           int32_t& local_x, int32_t& local_y, int32_t& local_z) noexcept {
    if (world_x >= 0) {
        chunk_x = world_x >> 5;
        local_x = world_x & 31;
    } else {
        chunk_x = -((-world_x + 31) >> 5);
        local_x = world_x - chunk_x * CHUNK_WIDTH;
    }
    if (world_y >= 0) {
        chunk_y = world_y >> 5;
        local_y = world_y & 31;
    } else {
        chunk_y = -((-world_y + 31) >> 5);
        local_y = world_y - chunk_y * CHUNK_HEIGHT;
    }
    if (world_z >= 0) {
        chunk_z = world_z >> 5;
        local_z = world_z & 31;
    } else {
        chunk_z = -((-world_z + 31) >> 5);
        local_z = world_z - chunk_z * CHUNK_DEPTH;
    }
}

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_COORDS_HPP