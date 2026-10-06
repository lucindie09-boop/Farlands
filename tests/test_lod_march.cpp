// The seed-grid surface march (src/lod/lod_march.hpp).
//
// The march exists to make a far column's surface cheap: stride down to the first
// solid sample instead of walking every block. Its contract is therefore two
// things at once — the SAME y as the per-block scan (a stride that misses a shelf
// is a mesh hole, not a rounding error), and fewer evaluations than that scan.
//
// The first version of it bisected between the last known-air sample and the
// first known-solid one, which finds A transition rather than the highest one: on
// real terrain (which has overhangs) it mismatched the scan on 34 of 64 columns at
// a 2-block stride and 56 at 16, worst case 15 blocks out. These cases pin the
// property that fix restored, including the multi-shelf profiles that broke it.
#include "doctest.h"
#include "lod/lod_march.hpp"

#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using VoxelEngine::lod::MarchResult;
using VoxelEngine::lod::march_surface;

namespace {

// A column described by explicit solid/air bands, top down: the topmost solid y
// with air above it is the answer a per-block scan would produce, computable
// directly from the bands.
int32_t expected_top(const std::vector<std::pair<int32_t, int32_t>>& bands, int32_t top_y) {
    // bands are (bottom, top) inclusive, in no particular order.
    for (int32_t y = top_y; y > top_y - 4096; --y) {
        bool here = false;
        bool above = false;
        for (const auto& band : bands) {
            if (y >= band.first && y <= band.second) here = true;
            if (y + 1 >= band.first && y + 1 <= band.second) above = true;
        }
        if (here && !above) return y;
    }
    return top_y;
}

} // namespace

TEST_CASE("the march agrees with a per-block scan on a plain slope") {
    // A monotone ramp: the answer is exact for every stride, and the stride only
    // changes the cost.
    for (int32_t step : {1, 2, 3, 4, 8, 16}) {
        for (int32_t surface : {0, 7, 64, 311, 1000}) {
            const int32_t top = surface + 40;
            const int32_t bottom = surface - 40;
            const MarchResult r = march_surface(top, bottom, step,
                                                [&](int32_t y) { return y <= surface; });
            CHECK(r.found);
            CHECK(r.y == surface);
        }
    }
}

TEST_CASE("the march finds the TOPMOST transition, not just any one") {
    // Two shelves with a gap between them: a bisection between the last air
    // sample and the first solid sample lands on the wrong side of the lower
    // shelf whenever the stride jumps the upper one.
    const std::vector<std::pair<int32_t, int32_t>> bands = {{200, 210}, {150, 160}, {0, 120}};
    for (int32_t step : {1, 2, 3, 4, 8, 16, 32}) {
        const int32_t top = 300;
        const int32_t bottom = 0;
        const MarchResult r = march_surface(top, bottom, step, [&](int32_t y) {
            for (const auto& band : bands) {
                if (y >= band.first && y <= band.second) return true;
            }
            return false;
        });
        CHECK(r.found);
        CHECK(r.y == 210);
        CHECK(r.y == expected_top(bands, top));
    }
}

// A column's solid bands, from a list of (bottom, top) pairs.
int32_t top_transition(const std::vector<std::pair<int32_t, int32_t>>& bands, int32_t top_y) {
    return expected_top(bands, top_y);
}

bool solid_at(const std::vector<std::pair<int32_t, int32_t>>& bands, int32_t y) {
    for (const auto& band : bands) {
        if (y >= band.first && y <= band.second) return true;
    }
    return false;
}

TEST_CASE("the march is exact wherever nothing is thinner than the stride") {
    // The contract the mode needs. A lattice of spacing `step` always has a point
    // inside any interval of length >= step, so while every body and every gap is
    // at least as thick as the stride, the march cannot skip a transition -- which
    // is why it reproduced the rigorous search exactly on 64 real columns at
    // strides 2 through 16 (tools/lod_sample_bench.cpp).
    std::mt19937 rng(20261006);
    std::uniform_int_distribution<int32_t> surface_dist(200, 700);
    std::uniform_int_distribution<int32_t> gap_dist(50, 90);
    std::uniform_int_distribution<int32_t> thickness_dist(50, 80);
    std::uniform_int_distribution<int32_t> band_count(0, 3);

    for (int32_t trial = 0; trial < 200; ++trial) {
        const int32_t surface = surface_dist(rng);
        std::vector<std::pair<int32_t, int32_t>> bands = {{0, surface}};
        int32_t above = surface;
        const int32_t count = band_count(rng);
        for (int32_t b = 0; b < count; ++b) {
            const int32_t thickness = thickness_dist(rng);
            above += gap_dist(rng) + thickness;
            bands.emplace_back(above - thickness, above);
        }
        const int32_t top = above + 100;
        auto solid = [&](int32_t y) { return solid_at(bands, y); };
        const int32_t want = top_transition(bands, top);
        for (int32_t step : {2, 4, 8, 16, 32}) {
            const MarchResult r = march_surface(top, 0, step, solid);
            CHECK(r.found);
            CHECK(r.y == want);
        }
    }
}

TEST_CASE("a feature thinner than the stride is missed, and that is stated") {
    // The resolution limit, pinned rather than pretended away: a one-block shelf
    // between two samples is invisible to the march, which reports the top of the
    // body below it. That is the same trade an 8-block grid makes about any
    // feature 8 blocks wide -- the far field samples a surface, it does not survey
    // one -- but the returned y must still be a REAL transition (solid below, air
    // above) and must never be above the true surface.
    const std::vector<std::pair<int32_t, int32_t>> bands = {{0, 300}, {327, 327}};
    auto solid = [&](int32_t y) { return solid_at(bands, y); };
    const int32_t top = 400;
    CHECK(top_transition(bands, top) == 327);

    const MarchResult r = march_surface(top, 0, 16, solid);
    CHECK(r.found);
    CHECK(r.y == 300);        // the thin shelf is above the first solid sample
    CHECK(r.y < top_transition(bands, top));
    CHECK(solid(r.y));
    CHECK_FALSE(solid(r.y + 1));
    // ... and a stride that lands on the shelf does find it.
    const MarchResult hit = march_surface(top, 0, 1, solid);
    CHECK(hit.y == 327);
}

TEST_CASE("the searched point is always a real transition") {
    // Whatever is skipped, the answer is never a point inside rock or inside air:
    // a mesh built from it has a surface at every node, not a spike.
    std::mt19937 rng(4);
    std::uniform_int_distribution<int32_t> surface_dist(100, 500);
    std::uniform_int_distribution<int32_t> offset_dist(1, 40);
    std::uniform_int_distribution<int32_t> thickness_dist(1, 8);
    for (int32_t trial = 0; trial < 400; ++trial) {
        const int32_t surface = surface_dist(rng);
        std::vector<std::pair<int32_t, int32_t>> bands = {{0, surface}};
        const int32_t count = offset_dist(rng) % 4;
        int32_t above = surface;
        for (int32_t b = 0; b < count; ++b) {
            const int32_t thickness = thickness_dist(rng);
            above += offset_dist(rng) + thickness;
            bands.emplace_back(above - thickness, above);
        }
        const int32_t top = above + 60;
        auto solid = [&](int32_t y) { return solid_at(bands, y); };
        for (int32_t step : {2, 3, 5, 8, 16}) {
            const MarchResult r = march_surface(top, 0, step, solid);
            CHECK(r.found);
            CHECK(solid(r.y));
            // Air immediately above it, unless the walk was capped by the sample
            // that ended the search (which is air by construction).
            CHECK_FALSE(solid(r.y + 1));
            CHECK(r.y <= top_transition(bands, top));
        }
    }
}

TEST_CASE("a stride of one is the per-block scan") {
    const int32_t surface = 123;
    int32_t calls = 0;
    const MarchResult r = march_surface(200, 0, 1, [&](int32_t y) {
        ++calls;
        return y <= surface;
    });
    CHECK(r.found);
    CHECK(r.y == surface);
    // One call per block from 200 down to 123, and the walk upward is already
    // satisfied by the sample above.
    CHECK(calls == 200 - surface + 1);
    CHECK(r.evaluations == calls);
}

TEST_CASE("a coarse stride costs less than the scan") {
    const int32_t surface = 311;   // band top .. bottom spans 80 blocks
    const int32_t top = surface + 40;
    const int32_t bottom = surface - 40;
    int32_t scan_calls = 0;
    const MarchResult scan = march_surface(top, bottom, 1, [&](int32_t y) {
        ++scan_calls;
        return y <= surface;
    });
    CHECK(scan.y == surface);
    for (int32_t step : {4, 8, 16}) {
        int32_t calls = 0;
        const MarchResult m = march_surface(top, bottom, step, [&](int32_t y) {
            ++calls;
            return y <= surface;
        });
        CHECK(m.y == surface);
        CHECK(m.evaluations == calls);
        // The march is (band/step) + (the walk up to the exact transition), so it
        // must beat the scan for every stride that divides the band evenly.
        CHECK(calls < scan_calls);
    }
}

TEST_CASE("an all-air band reports nothing found") {
    int32_t calls = 0;
    const MarchResult r = march_surface(100, 0, 8, [&](int32_t) {
        ++calls;
        return false;
    });
    CHECK_FALSE(r.found);
    CHECK(r.evaluations == calls);
    CHECK(calls == 13);  // 100, 92, ... 4 — the last sample above the floor
}

TEST_CASE("a solid column reports its ceiling") {
    // Solid from the very first sample down: the walk up has nothing to climb,
    // so the answer is the top of the band, which is what the caller started at.
    const MarchResult r = march_surface(100, 0, 8, [&](int32_t) { return true; });
    CHECK(r.found);
    CHECK(r.y == 100);
}

TEST_CASE("degenerate inputs are refused rather than guessed") {
    CHECK_FALSE(march_surface(0, 100, 4, [](int32_t) { return true; }).found);
    // A step below 1 is clamped to 1, not treated as a division.
    const MarchResult r = march_surface(10, 0, 0, [](int32_t y) { return y <= 5; });
    CHECK(r.found);
    CHECK(r.y == 5);
}

TEST_CASE("a blocked band still costs at most the stride plus the scan") {
    // The bound the cost model depends on: band/step samples down, then at most
    // `step` samples back up.
    const int32_t surface = 500;
    for (int32_t step : {2, 4, 8, 16, 32}) {
        const int32_t top = surface + 64;
        const int32_t bottom = surface - 64;
        const MarchResult r = march_surface(top, bottom, step,
                                            [&](int32_t y) { return y <= surface; });
        CHECK(r.y == surface);
        const int32_t down = (top - surface + step - 1) / step;
        CHECK(r.evaluations <= down + step + 1);
    }
}
