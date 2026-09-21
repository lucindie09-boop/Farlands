#ifndef FARLANDS_SWEEP_BAND_HPP
#define FARLANDS_SWEEP_BAND_HPP

#include "core/chunk_coords.hpp"
#include <cstdint>

namespace VoxelEngine {
namespace sweep {

// Which chunk slices of ONE column can pass the generation sweep's band filter.
//
// The filter rejects a chunk whose bottom is more than 32 blocks above the
// column's top content, and — outside the near-player full-column fill — one
// whose top is more than 32 blocks below the column's lowest surface. Both
// predicates are monotone in chunk y (bottom and top both rise with y), so the
// accepted set is contiguous and a (lo, hi) pair describes it EXACTLY.
//
// This exists because the sweep used to enumerate every slice of the world
// height for every column — 65 per column, 208,585 entries at render distance 32
// — and then let the filter throw most of them away while walking them. Measured
// with /genstats: 0.43–0.70% of the candidates it examined ever generated, 84%
// of the walk was vertically impossible entries, and even inside the frustum 75%
// of the candidates were band rejects. The band is computed once per column now,
// and the calls below are the single definition of it, so a chunk the filter
// would accept cannot be missing from the sweep list.
struct ChunkBand {
    int32_t lo = 1;  // inclusive, absolute chunk y
    int32_t hi = 0;  // inclusive; lo > hi means the column has no candidates
};

[[nodiscard]] inline bool empty(const ChunkBand& band) { return band.lo > band.hi; }

[[nodiscard]] inline int32_t count(const ChunkBand& band) {
    return empty(band) ? 0 : band.hi - band.lo + 1;
}

// The filter itself, in one place so the list and the walk cannot disagree.
[[nodiscard]] inline bool chunk_in_band(int32_t cy, float land_h, float top_h, bool fill_column) {
    const float chunk_bottom = static_cast<float>(cy * CHUNK_HEIGHT);
    const float chunk_top    = static_cast<float>((cy + 1) * CHUNK_HEIGHT);
    if (chunk_bottom > top_h + 32.0f) return false;
    if (!fill_column && chunk_top < land_h - 32.0f) return false;
    return true;
}

// The accepted slices of one column, found by TESTING each slice with the filter
// above rather than by inverting its arithmetic. Inverting it is where an
// off-by-one would silently drop a chunk that genuinely contains terrain, which
// is the bug this window exists to avoid (a chunk whose column had a mountain
// wall crossing it got skipped and left an invisible-solid hole); 32 comparisons
// per column is nothing next to the walk it replaces.
[[nodiscard]] inline ChunkBand band_for_column(float land_h, float top_h, bool fill_column,
                                              int32_t chunk_slices) {
    ChunkBand band;
    for (int32_t cy = 0; cy < chunk_slices; ++cy) {
        if (!chunk_in_band(cy, land_h, top_h, fill_column)) continue;
        if (empty(band)) band.lo = cy;
        band.hi = cy;
    }
    return band;
}

// The i-th slice of a band in the order the sweep offers them: from the
// player's own slice outward, alternating below and above. "Same level first" is
// deliberate. The player's own rows are what is being looked at, and for the
// near-player full-column fills the band reaches the world floor, where plain
// ascending order would offer hundreds of blocks of rock under the player before
// the surface beside them. Returns false once the band is exhausted.
[[nodiscard]] inline bool slice_cy(const ChunkBand& band, int32_t centre, uint32_t index, int32_t& out_cy) {
    if (empty(band)) return false;
    const int32_t c = centre < band.lo ? band.lo : (centre > band.hi ? band.hi : centre);
    const uint32_t limit = static_cast<uint32_t>(2 * count(band) + 2);
    uint32_t seen = 0;
    for (uint32_t k = 0; k <= limit; ++k) {
        const int32_t d = (k % 2 == 0) ? static_cast<int32_t>(k / 2)
                                       : -static_cast<int32_t>((k + 1) / 2);
        const int32_t cy = c + d;
        if (cy < band.lo || cy > band.hi) continue;
        if (seen == index) {
            out_cy = cy;
            return true;
        }
        ++seen;
    }
    return false;
}

} // namespace sweep
} // namespace VoxelEngine

#endif // FARLANDS_SWEEP_BAND_HPP
