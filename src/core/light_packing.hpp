#ifndef FARLANDS_LIGHT_PACKING_HPP
#define FARLANDS_LIGHT_PACKING_HPP
#include <cstdint>

namespace VoxelEngine {

// -----------------------------------------------------------------------------
// Light packing: 4 bits per channel (sky, r, g, b) packed into a uint16_t.
// This gives 16 discrete levels per channel and consumes 2 bytes per voxel.
// For a 32x32x32 chunk (32,768 voxels) that's ~64 KB of light data per chunk.
// -----------------------------------------------------------------------------

[[nodiscard]] inline constexpr uint16_t pack_light(uint8_t sky, uint8_t r, uint8_t g, uint8_t b) noexcept {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(sky & 0x0F)) |
        (static_cast<uint16_t>(r & 0x0F) << 4) |
        (static_cast<uint16_t>(g & 0x0F) << 8) |
        (static_cast<uint16_t>(b & 0x0F) << 12)
    );
}

[[nodiscard]] inline constexpr uint8_t unpack_sky(uint16_t v) noexcept { return static_cast<uint8_t>(v & 0x0F); }
[[nodiscard]] inline constexpr uint8_t unpack_r(uint16_t v) noexcept   { return static_cast<uint8_t>((v >> 4) & 0x0F); }
[[nodiscard]] inline constexpr uint8_t unpack_g(uint16_t v) noexcept   { return static_cast<uint8_t>((v >> 8) & 0x0F); }
[[nodiscard]] inline constexpr uint8_t unpack_b(uint16_t v) noexcept   { return static_cast<uint8_t>((v >> 12) & 0x0F); }

static_assert(sizeof(uint16_t) == 2, "Light packing assumes 16-bit storage");
static_assert(pack_light(15, 15, 15, 15) == 0xFFFF, "pack_light must saturate all 16 bits");
static_assert(pack_light(0, 0, 0, 0) == 0x0000, "pack_light must zero all bits");
static_assert(unpack_sky(pack_light(7, 0, 0, 0)) == 7, "sky round-trip failed");
static_assert(unpack_r(pack_light(0, 7, 0, 0)) == 7,   "r round-trip failed");
static_assert(unpack_g(pack_light(0, 0, 7, 0)) == 7,   "g round-trip failed");
static_assert(unpack_b(pack_light(0, 0, 0, 7)) == 7,   "b round-trip failed");

} // namespace VoxelEngine

#endif // FARLANDS_LIGHT_PACKING_HPP