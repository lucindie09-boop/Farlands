#ifndef FARLANDS_TERRAIN_SQUISH_HPP
#define FARLANDS_TERRAIN_SQUISH_HPP
#include "core/chunk_coords.hpp"
#include "core/terrain_params.hpp"
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// The squish: a TEST-ONLY toggle that compresses the world's whole vertical
// relief into a KEPT REGION of chunk slices -- one slice by default, so a
// column has a single terrain chunk instead of a band of them. It exists to
// measure what the vertical axis costs generation and streaming, by removing
// it -- not to be a world type.
//
// Three properties matter, and all are deliberate:
//
//   * The height map is MONOTONE and SATURATING, not a linear scale. A linear
//     scale sized for the worst-case relief bound (the ±500 macro base plus
//     every relief field, so ~±700 blocks) flattens typical terrain -- a few
//     hundred blocks of relief -- to under a block. The tanh sends typical
//     relief to most of the region and compresses only the extremes, so the
//     surface keeps its horizontal shape. That shape is the point of the test:
//     a superflat world would be cheap for a different reason (nothing to mesh),
//     and would measure the wrong thing.
//
//   * The 3D shape envelope shrinks to the region's reach (see
//     ChunkGenerator::shape_envelope). Left at ±28, the surface band would push
//     terrain out of a one-slice region no matter how flat the macro height was,
//     and the slices above and below would be generated to hold nothing.
//
// The span makes the region a SCALING knob rather than a second world: the map
// is the one-slice one scaled by the span about the region's centre, so relief
// amplitude -- and with it the number of chunks a column generates -- grows
// linearly while the terrain keeps its shape. Measured (docs/terrain-notes.md),
// fill time and per-column CPU both track slices-per-column almost exactly, so a
// span sweep is the cleanest reading of what height costs.
//
// Sea level squishes with the terrain, so the ocean/land split and water depth
// survive: a column that was below sea level still is.
//
// The squished world is NOT a faithful Farlands -- it is a measurement rig. It
// is deliberately not persisted in world.meta (see ChunkWorld::save_world_
// metadata), so it can never be mistaken for the world's real terrain.
namespace VoxelEngine {
namespace squish {

// Half the macro relief each KEPT SLICE holds, in blocks. The rest of the slice
// is left for the 3D shape band and rounding: the surface must land strictly
// inside [slice*32, slice*32+32) or the band filter will pull in the slice
// above. Scaled by squish::span to get the region's own half range.
inline constexpr float kHalfRange = 10.0f;

// The 3D shape reach per kept slice when squished, and the inner/outer ratio
// (9:28) the fade keeps from the unsquished envelope so displacement still ramps
// in rather than cutting off at a hard shell. Scaled by squish::span too.
inline constexpr float kBandOuter = 3.0f;
inline constexpr float kBandInner = kBandOuter * (9.0f / 28.0f);

// The kept region: `squish_slice` is its BOTTOM slice and `squish_span` its
// height in slices. 1 is the one-chunk squish; 8 is a 256-block world, the shape
// a shorter world height limit would have; 32 is the whole world, at which point
// the toggle is the OFF state's equal in extent though not in shape. Clamped so
// the region always fits inside the world: a span set near the top shortens
// rather than running past the sky.
[[nodiscard]] inline int32_t span(const TerrainParams& p) {
    constexpr int32_t kWorldSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    const int32_t bottom = std::clamp(p.squish_slice, 0, kWorldSlices - 1);
    const int32_t room = kWorldSlices - bottom;
    return std::max(1, std::min(p.squish_span, room));
}

// The region's own bounds -- the interval no squished surface may leave -- and
// its middle, which is where sea level and the map's centre sit.
[[nodiscard]] inline float slice_bottom(const TerrainParams& p) {
    return static_cast<float>(p.squish_slice * CHUNK_HEIGHT);
}

[[nodiscard]] inline float slice_top(const TerrainParams& p) {
    return slice_bottom(p) + static_cast<float>(span(p) * CHUNK_HEIGHT);
}

[[nodiscard]] inline float slice_center(const TerrainParams& p) {
    return slice_bottom(p) + static_cast<float>(span(p) * CHUNK_HEIGHT) * 0.5f;
}

// Macro relief the whole kept region holds, in blocks. The map is the one-slice
// one scaled by the span, so the terrain keeps its shape at every size and the
// relief AMPLITUDE -- the thing that decides how many chunks a column generates
// -- grows linearly with the knob. That is what makes the span a scaling
// experiment rather than a second, differently-shaped world.
[[nodiscard]] inline float half_range(const TerrainParams& p) {
    return kHalfRange * static_cast<float>(span(p));
}

[[nodiscard]] inline float band_outer(const TerrainParams& p) {
    return kBandOuter * static_cast<float>(span(p));
}

[[nodiscard]] inline float band_inner(const TerrainParams& p) {
    return kBandInner * static_cast<float>(span(p));
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
// relief using most of the kept region while the extremes saturate instead of
// escaping it; the amplitude is half_range (span-scaled) so every span is the
// same world, stretched. Exactly the identity when squished is off, so every
// caller can route through it unconditionally.
[[nodiscard]] inline float height(const TerrainParams& p, float h) {
    if (!p.squish_enabled) return h;
    const float softness = std::max(p.squish_softness, 1.0f);
    const float t = (h - p.sea_level) / softness;
    return slice_center(p) + half_range(p) * std::tanh(t);
}

// How far a column's real surface can sit from its squished macro height. This
// is what every squished height range is padded by: the span-scaled 3D shape
// reach plus the generator's DENSITY_MARGIN_SLACK (2 blocks, a rounding and
// density constant, not a geometric one, so it does not scale), exactly as
// density_margin() does for the unsquished envelope. The padded range then stays
// inside the kept region -- half_range + margin = 13*span + 2 < 16*span for
// every span >= 1 -- which is what keeps the band to the span's own slices.
[[nodiscard]] inline float margin(const TerrainParams& p) {
    return band_outer(p) + 2.0f;
}

} // namespace squish
} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_SQUISH_HPP
