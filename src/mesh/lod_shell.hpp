#ifndef FARLANDS_LOD_SHELL_HPP
#define FARLANDS_LOD_SHELL_HPP

#include <algorithm>
#include <cstdint>
#include <cstdlib>

namespace VoxelEngine {

// Enumerate the cells of a Chebyshev shell (a hollow cube band) around the
// origin: every (dx, dy, dz) whose max(|dx|, |dy|, |dz|) lies in
// [shell_min, shell_max], with |dy| additionally limited to vert_range.
//
// The LOD rescan needs the chunks sitting in a tier's transition band, which is
// this shell. Written as a direct walk of the band rather than a filter over the
// enclosing box: testing `max(...) in [min, max]` cell by cell visited ~1.7M
// cells per tier per frame to reach ~100k band cells, and that pure loop
// overhead was most of what the dirty-mesh-queue phase spent its milliseconds
// on. `tests/test_lod_shell.cpp` pins the visited set against the box filter it
// replaces, so the two cannot disagree.
//
// `fn(dx, dy, dz)` returns false to stop the walk early (the caller's cap).
template <typename Fn>
void for_each_shell_cell(int32_t shell_min, int32_t shell_max, int32_t vert_range, Fn&& fn) {
    if (shell_max < 0 || vert_range < 0) return;
    shell_min = std::max(0, shell_min);
    const int32_t dy_outer = std::min(vert_range, shell_max);
    const auto visit_column = [&](int32_t dx, int32_t dz, int32_t dy_lo, int32_t dy_hi) -> bool {
        for (int32_t dy = dy_lo; dy <= dy_hi; ++dy) {
            if (!fn(dx, dy, dz)) return false;
        }
        return true;
    };

    for (int32_t dx = -shell_max; dx <= shell_max; ++dx) {
        const int32_t abs_dx = std::abs(dx);
        for (int32_t dz = -shell_max; dz <= shell_max; ++dz) {
            const int32_t column_dist = std::max(abs_dx, std::abs(dz));
            if (column_dist >= shell_min) {
                // The column alone is already in the band, so any |dy| <= dy_outer
                // keeps the cell inside it.
                if (!visit_column(dx, dz, -dy_outer, dy_outer)) return;
            } else {
                // The column is inside the inner cube: |dy| itself must be in the
                // band (and inside vert_range).
                if (shell_min > dy_outer) continue;
                if (!visit_column(dx, dz, shell_min, dy_outer)) return;
                if (!visit_column(dx, dz, -dy_outer, -shell_min)) return;
            }
        }
    }
}

} // namespace VoxelEngine

#endif // FARLANDS_LOD_SHELL_HPP
