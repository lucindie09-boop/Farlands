#ifndef FARLANDS_TERRAIN_SQUISH_HPP
#define FARLANDS_TERRAIN_SQUISH_HPP
#include "core/chunk_coords.hpp"
#include "core/terrain_params.hpp"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// The squish: a TEST-ONLY toggle that compresses the world's whole vertical
// relief into ONE chunk slice, so a column has a single terrain chunk instead
// of a band of them. It exists to measure what the vertical axis costs
// generation and streaming, by removing it -- not to be a world type.
//
// Two properties matter, and both are deliberate:
//
//   * The height map is MONOTONE and SATURATING, not a linear scale. A linear
//     scale sized for the worst-case relief bound (the ±500 macro base plus
//     every relief field, so ~±700 blocks) flattens typical terrain -- a few
//     hundred blocks of relief -- to under a block. The tanh sends typical
//     relief to most of the slice and compresses only the extremes, so the
//     surface keeps its horizontal shape. That shape is the point of the test:
//     a superflat world would be cheap for a different reason (nothing to mesh),
//     and would measure the wrong thing.
//
//   * The 3D shape envelope shrinks to kBandOuter (see
//     ChunkGenerator::shape_envelope). Left at ±28, the surface band would push
//     terrain out of the slice no matter how flat the macro height was, and the
//     slices above and below would be generated to hold nothing.
//
// Sea level squishes with the terrain, so the ocean/land split and water depth
// survive: a column that was below sea level still is.
//
// The squished world is NOT a faithful Farlands -- it is a measurement rig. It
// is deliberately not persisted in world.meta (see ChunkWorld::save_world_
// metadata), so it can never be mistaken for the world's real terrain.
namespace VoxelEngine {
namespace squish {

// Half the macro relief a slice holds, in blocks. The rest of the slice is left
// for the 3D shape band and rounding: the surface must land strictly inside
// [slice*32, slice*32+32) or the band filter will pull in the slice above.
inline constexpr float kHalfRange = 10.0f;

// The 3D shape reach when squished, and the inner/outer ratio (9:28) the fade
// keeps from the unsquished envelope so displacement still ramps in rather than
// cutting off at a hard shell.
inline constexpr float kBandOuter = 3.0f;
inline constexpr float kBandInner = kBandOuter * (9.0f / 28.0f);

// World y at the middle of the slice the terrain is compressed into, and the
// slice's own bounds -- the interval no squished surface may leave.
[[nodiscard]] inline float slice_center(const TerrainParams& p) {
    return static_cast<float>(p.squish_slice * CHUNK_HEIGHT) +
           static_cast<float>(CHUNK_HEIGHT) * 0.5f;
}

[[nodiscard]] inline float slice_bottom(const TerrainParams& p) {
    return static_cast<float>(p.squish_slice * CHUNK_HEIGHT);
}

[[nodiscard]] inline float slice_top(const TerrainParams& p) {
    return slice_bottom(p) + static_cast<float>(CHUNK_HEIGHT);
}

// The sea level terrain is generated against: the slice centre when squished,
// the configured level otherwise. Every comparison that decides land vs ocean,
// or where water tops out, must read this rather than params.sea_level, or a
// squished world turns into one global ocean (terrain at y≈48 under a sea
// level of 200).
[[nodiscard]] inline float sea_level(const TerrainParams& p) {
    return p.squish_enabled ? slice_center(p) : p.sea_level;
}

// Monotone squash of a macro height about sea level. tanh is what keeps typical
// relief using most of the slice while the extremes saturate instead of
// escaping it. Exactly the identity when squished is off, so every caller can
// route through it unconditionally.
[[nodiscard]] inline float height(const TerrainParams& p, float h) {
    if (!p.squish_enabled) return h;
    const float softness = std::max(p.squish_softness, 1.0f);
    const float t = (h - p.sea_level) / softness;
    return slice_center(p) + kHalfRange * std::tanh(t);
}

// How far a column's real surface can sit from its squished macro height. This
// is what every squished height range is padded by: kBandOuter for the 3D shape
// plus the generator's DENSITY_MARGIN_SLACK, exactly as density_margin() does
// for the unsquished envelope. The padded range then stays inside the slice
// (kHalfRange + margin = 15 < 16), which is what keeps the band to one slice.
[[nodiscard]] inline float margin(const TerrainParams& p) {
    return kBandOuter + 2.0f;
}

} // namespace squish
} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_SQUISH_HPP
