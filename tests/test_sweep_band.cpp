// The generation sweep now enumerates per-column y RANGES instead of every slice
// of the world height, so the range has to be EXACTLY the set of slices the band
// filter accepts. If it is ever one slice short, the sweep silently stops
// generating chunks that contain terrain — the invisible-solid-hole class of bug
// (`tools/sched_window_check.cpp` exists because of one), and one a renderer
// cannot tell you about. The first test below therefore checks the range against
// the predicate exhaustively, over the awkward shapes: a flat column, an ocean
// column whose top is the water surface far above the bed, a column entirely
// above the world, one entirely below it, and the exact boundaries where the
// 32-block window lands on a slice edge.
#include "doctest.h"
#include "world/sweep_band.hpp"
#include <algorithm>
#include <vector>

using namespace VoxelEngine;

namespace {

constexpr int32_t kSlices = WORLD_HEIGHT_Y / CHUNK_HEIGHT;  // 32

// Every slice the filter accepts, as a list, so it can be compared against the
// range rather than taken on trust.
std::vector<int32_t> accepted(float land_h, float top_h, bool fill) {
    std::vector<int32_t> out;
    for (int32_t cy = 0; cy < kSlices; ++cy) {
        if (sweep::chunk_in_band(cy, land_h, top_h, fill)) out.push_back(cy);
    }
    return out;
}

void check_exact(float land_h, float top_h, bool fill) {
    const std::vector<int32_t> want = accepted(land_h, top_h, fill);
    const sweep::ChunkBand band = sweep::band_for_column(land_h, top_h, fill, kSlices);
    CHECK(sweep::count(band) == static_cast<int32_t>(want.size()));
    if (want.empty()) {
        CHECK(sweep::empty(band));
        return;
    }
    CHECK_FALSE(sweep::empty(band));
    // Contiguous, and equal to the accepted set slice by slice — not merely the
    // same size.
    CHECK(band.lo == want.front());
    CHECK(band.hi == want.back());
    for (int32_t cy = 0; cy < kSlices; ++cy) {
        const bool in_range = (cy >= band.lo && cy <= band.hi);
        CHECK(in_range == sweep::chunk_in_band(cy, land_h, top_h, fill));
    }
}

} // namespace

TEST_CASE("a column's band is exactly the slices the filter accepts") {
    check_exact(200.0f, 210.0f, false);   // flat land, no fill
    check_exact(200.0f, 210.0f, true);    // the same, as a near-player fill
    check_exact(60.0f, 400.0f, false);    // a deep ocean: bed far below the surface
    check_exact(60.0f, 400.0f, true);
    check_exact(-500.0f, -400.0f, false); // entirely below the world
    check_exact(-500.0f, -400.0f, true);
    check_exact(3000.0f, 3100.0f, false); // entirely above it
    check_exact(3000.0f, 3100.0f, true);
    check_exact(1023.0f, 1024.0f, false); // the top of the world
    check_exact(0.0f, 0.0f, false);       // the bottom of it
    check_exact(0.0f, 0.0f, true);
}

TEST_CASE("the band's edges land where the 32-block window says, including on a slice boundary") {
    // The upper test is `chunk_bottom > top_h + 32`, so the highest accepted
    // slice is the one whose bottom is at or below that bound. Placing top_h so
    // the bound falls exactly on a slice edge is the case an off-by-one breaks,
    // so it is checked on both sides of the edge.
    const float land = 100.0f;
    for (int32_t cy = 1; cy < kSlices; ++cy) {
        const float edge = static_cast<float>(cy * CHUNK_HEIGHT) - 32.0f;  // bottom == edge - 0
        check_exact(land, edge, false);            // bound exactly on the slice bottom
        check_exact(land, edge - 0.5f, false);     // just under it
        check_exact(land, edge + 0.5f, false);     // just over it
    }
    // ...and the same for the lower bound, `chunk_top < land_h - 32`.
    const float top = 900.0f;
    for (int32_t cy = 0; cy < kSlices; ++cy) {
        const float edge = static_cast<float>((cy + 1) * CHUNK_HEIGHT) + 32.0f;
        check_exact(edge, top, false);
        check_exact(edge - 0.5f, top, false);
        check_exact(edge + 0.5f, top, false);
    }
}

TEST_CASE("a fill column reaches the world floor and a band-only column does not") {
    // The two rules differ only in the lower bound: within the fill radius the
    // whole column is wanted (rock under the player must be solid, not void).
    const sweep::ChunkBand filled = sweep::band_for_column(200.0f, 210.0f, true, kSlices);
    const sweep::ChunkBand banded = sweep::band_for_column(200.0f, 210.0f, false, kSlices);
    CHECK(filled.lo == 0);
    CHECK(banded.lo > 0);
    CHECK(filled.hi == banded.hi);
}

TEST_CASE("the slice order visits every slice once, starting at the player's level") {
    const sweep::ChunkBand band = sweep::band_for_column(200.0f, 210.0f, true, kSlices);
    const int32_t total = sweep::count(band);
    for (int32_t centre : {0, 6, 21, 31, 100, -100}) {
        std::vector<int32_t> seen;
        for (uint32_t i = 0; i < static_cast<uint32_t>(total); ++i) {
            int32_t cy = 0;
            CHECK(sweep::slice_cy(band, centre, i, cy));
            seen.push_back(cy);
        }
        // No repeats, nothing missing.
        std::vector<int32_t> sorted = seen;
        std::sort(sorted.begin(), sorted.end());
        sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
        CHECK(static_cast<int32_t>(sorted.size()) == total);
        CHECK(sorted.front() == band.lo);
        CHECK(sorted.back() == band.hi);
        // The first one offered is the player's own slice, clamped into range.
        const int32_t expected_first = centre < band.lo ? band.lo : (centre > band.hi ? band.hi : centre);
        CHECK(seen.front() == expected_first);
        // ...and then moves outward: the distance from the player's slice never
        // decreases, which is the property that makes the near rows win when a
        // frame's budget runs out part way through a column.
        for (size_t i = 1; i < seen.size(); ++i) {
            CHECK(std::abs(seen[i] - expected_first) >= std::abs(seen[i - 1] - expected_first));
        }
        // Asking past the end is a refusal, not a silent repeat of the last one.
        int32_t past = -1;
        CHECK_FALSE(sweep::slice_cy(band, centre, static_cast<uint32_t>(total), past));
    }
}

TEST_CASE("an empty band offers nothing") {
    const sweep::ChunkBand band = sweep::band_for_column(3000.0f, 3100.0f, false, kSlices);
    CHECK(sweep::empty(band));
    CHECK(sweep::count(band) == 0);
    int32_t cy = 0;
    CHECK_FALSE(sweep::slice_cy(band, 5, 0, cy));
}
