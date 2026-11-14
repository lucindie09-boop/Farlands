#ifndef FARLANDS_GENERATED_WATER_WAKE_HPP
#define FARLANDS_GENERATED_WATER_WAKE_HPP

// -----------------------------------------------------------------------------
// The bounded answer to "why doesn't the sea flow?".
//
// Worldgen emits `surface_water`, a static block that declares no fluid state, so
// the simulation never ticks it (see docs/fluids.md). Making every ocean cell live
// would put a source in every sea-level cell and let runoff creep along every
// shore; instead this scan is the LOCAL half: an edit next to generated water
// converts only the cells within a small radius into the live `water` source, and
// the rules take it from there.
//
// The scan itself is pure — it reads through a caller lambda and reports cells
// through another — so a test drives it over a flat grid and the editor drives it
// over the chunk map, while the radius and the cheap pre-check are the same code.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"

#include <cstdint>

namespace VoxelEngine {
namespace fluids {

// A generated-water cell the wake would convert, in WORLD coordinates.
struct WakeCell {
    int32_t x = 0;
    int32_t y = 0;
    int32_t z = 0;
};

// Calls `emit(WakeCell)` for every cell whose block is `generated`, in the cube of
// `radius` around (x, y, z), and returns how many there were.
//
// Two properties worth keeping: the seven-cell pre-check runs first, because most
// edits a player makes have no generated water within one block and the full cube
// is 125 reads; and the edited cell itself is never emitted (it is the one the
// caller just wrote, and `place_block` refuses to write into a non-air cell, so it
// cannot be generated water anyway).
template <typename At, typename Emit>
int find_generated_water(At&& at, Emit&& emit, BlockID generated,
                         int32_t x, int32_t y, int32_t z, int radius) {
    if (generated == BlockIDs::AIR || radius <= 0) {
        return 0;
    }
    const int32_t offsets[7][3] = {
        { 0, 0, 0 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 0, 1 },
        { 0, 0, -1 }, { 0, 1, 0 }, { 0, -1, 0 },
    };
    bool near = false;
    for (const auto& offset : offsets) {
        if (at(x + offset[0], y + offset[1], z + offset[2]) == generated) {
            near = true;
            break;
        }
    }
    if (!near) {
        return 0;
    }

    int found = 0;
    for (int32_t dz = -radius; dz <= radius; ++dz) {
        for (int32_t dy = -radius; dy <= radius; ++dy) {
            for (int32_t dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dy == 0 && dz == 0) {
                    continue;
                }
                if (at(x + dx, y + dy, z + dz) != generated) {
                    continue;
                }
                emit(WakeCell{ x + dx, y + dy, z + dz });
                ++found;
            }
        }
    }
    return found;
}

// How far a wake converts, in cells. Resolved from, in order: the probe override
// set through `set_water_wake_radius_override`, the FARLANDS_WATER_WAKE_RADIUS
// environment variable, then 2. Zero turns the wake off.
void set_water_wake_radius_override(int radius);
[[nodiscard]] int water_wake_radius();

} // namespace fluids
} // namespace VoxelEngine

#endif // FARLANDS_GENERATED_WATER_WAKE_HPP
