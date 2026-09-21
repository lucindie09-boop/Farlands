// The LOD rescan's shell walk, pinned against the box filter it replaced.
//
// The rescan visits the chunks in a tier's transition band: the Chebyshev shell
// max(|dx|,|dy|,|dz|) in [shell_min, shell_max]. It used to test that condition
// cell by cell over the whole enclosing box, which walked ~1.7M cells per tier
// per frame to reach ~100k band cells. The direct walk is a different loop, so
// the visited SET has to be provably the same one — including the vert_range cap
// on |dy| and the case where vert_range is smaller than the shell itself.
#include "doctest.h"
#include "mesh/lod_shell.hpp"

#include <algorithm>
#include <cstdlib>
#include <tuple>
#include <vector>

using VoxelEngine::for_each_shell_cell;

namespace {

using Cell = std::tuple<int32_t, int32_t, int32_t>;

// Every cell the enclosing box visits whose Chebyshev distance is in the band —
// the filter the direct walk has to reproduce.
std::vector<Cell> box_filter(int32_t shell_min, int32_t shell_max, int32_t vert_range) {
    std::vector<Cell> cells;
    for (int32_t dx = -shell_max; dx <= shell_max; ++dx) {
        for (int32_t dz = -shell_max; dz <= shell_max; ++dz) {
            for (int32_t dy = -vert_range; dy <= vert_range; ++dy) {
                const int32_t dist = std::max({std::abs(dx), std::abs(dy), std::abs(dz)});
                if (dist < shell_min || dist > shell_max) continue;
                cells.emplace_back(dx, dy, dz);
            }
        }
    }
    std::sort(cells.begin(), cells.end());
    return cells;
}

std::vector<Cell> walked(int32_t shell_min, int32_t shell_max, int32_t vert_range) {
    std::vector<Cell> cells;
    for_each_shell_cell(shell_min, shell_max, vert_range, [&](int32_t dx, int32_t dy, int32_t dz) {
        cells.emplace_back(dx, dy, dz);
        return true;
    });
    std::sort(cells.begin(), cells.end());
    return cells;
}

} // namespace

TEST_CASE("the shell walk visits the same cells as the box filter") {
    const int32_t cases[][3] = {
        {2, 4, 9},   // vert_range comfortably wider than the shell
        {1, 2, 2},   // vert_range exactly the shell's outer radius
        {5, 6, 3},   // vert_range SHORTER than the shell: only side columns qualify
        {3, 5, 4},   // vert_range between the shell's inner and outer radius
        {0, 3, 4},   // degenerate inner radius (the whole cube)
        {4, 4, 12},  // a single layer
        {1, 1, 1},   // the smallest real case
    };
    for (const auto& c : cases) {
        CHECK(walked(c[0], c[1], c[2]) == box_filter(c[0], c[1], c[2]));
    }
}

TEST_CASE("the shell walk visits each cell exactly once") {
    const std::vector<Cell> cells = walked(4, 6, 12);
    CHECK_FALSE(cells.empty());
    // Sorted above, so duplicates would be adjacent.
    CHECK(std::adjacent_find(cells.begin(), cells.end()) == cells.end());
    CHECK(cells.front() != cells.back());
}

TEST_CASE("the shell walk stops when the callback says so") {
    // The rescan caps how many chunks it queues per frame and relies on this to
    // end the walk early, so a false return must stop it immediately.
    int32_t visited = 0;
    for_each_shell_cell(2, 4, 9, [&](int32_t, int32_t, int32_t) {
        ++visited;
        return visited < 7;
    });
    CHECK(visited == 7);
}

TEST_CASE("an empty or inverted shell visits nothing") {
    CHECK(walked(5, 4, 9).empty());
    CHECK(walked(2, 4, -1).empty());
    // A negative inner radius means "no inner hole", i.e. the same set as 0,
    // rather than an empty band.
    CHECK(walked(-1, 4, 9) == walked(0, 4, 9));
    CHECK_FALSE(walked(-1, 4, 9).empty());
}
