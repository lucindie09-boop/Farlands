#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VoxelEngine {
namespace density_math {

// -------------------------------------------------------------------------
// Math helpers shared by the chunk generator's density field. Moved out of
// chunk_generator.hpp (which keeps class-scope using-declarations for them) so
// that header stays under the 500-line cap; the bodies are unchanged.
// -------------------------------------------------------------------------
inline float clamp01(float v) {
    return std::max(0.0f, std::min(1.0f, v));
}

inline float smoothstep(float edge0, float edge1, float x) {
    float t = clamp01((x - edge0) / (edge1 - edge0));
    return t * t * (3.0f - 2.0f * t);
}

inline float lerp(float a, float b, float t) {
    return a + (b - a) * t;
}

// Largest lattice node coordinate <= v (floor division, negative-safe).
inline constexpr int32_t lattice_base(int32_t v, int32_t spacing) {
    const int32_t q = v / spacing;
    const int32_t r = v % spacing;
    return (r < 0 ? q - 1 : q) * spacing;
}

// Trilinear interpolation over the 8 corners of a lattice cell. Corner
// order and lerp order are fixed so every consumer (chunk lattice, single
// point queries) computes bit-identical values.
inline float trilinear_interp(
    float v000, float v100, float v010, float v110,
    float v001, float v101, float v011, float v111,
    float fx, float fy, float fz) {
    const float x00 = lerp(v000, v100, fx);
    const float x01 = lerp(v010, v110, fx);
    const float x10 = lerp(v001, v101, fx);
    const float x11 = lerp(v011, v111, fx);
    const float y0 = lerp(x00, x01, fy);
    const float y1 = lerp(x10, x11, fy);
    return lerp(y0, y1, fz);
}

// Density from its components (macro delta + 3D shape displacement).
// band_inner/band_outer are the caller's per-column envelope (see
// shape_envelope): the displacement fades to zero by band_outer, so the
// surface can never leave that band no matter how large the strength is.
inline float density_from_shape(float delta, float shape_strength, float shape,
                                float band_inner, float band_outer) {
    const float surface_distance = std::abs(delta);
    float surface_band;
    if (band_outer <= band_inner) {
        // Degenerate envelope (weirdness_size 0): a hard cutoff at
        // band_outer, since smoothstep would divide by zero here.
        surface_band = (surface_distance < band_outer) ? 1.0f : 0.0f;
    } else {
        surface_band = 1.0f - smoothstep(band_inner, band_outer, surface_distance);
    }
    return delta + shape * shape_strength * surface_band;
}

} // namespace density_math
} // namespace VoxelEngine
