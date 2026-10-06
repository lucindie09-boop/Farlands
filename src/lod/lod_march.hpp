#ifndef FARLANDS_LOD_MARCH_HPP
#define FARLANDS_LOD_MARCH_HPP
#include <cstdint>

namespace VoxelEngine {
namespace lod {

// Finding a column's surface without generating anything is the whole point of
// the seed-grid far mode (see docs/lod-modes.md), so the search lives here: apart
// from the generator, apart from Godot, and pinned by its own test.
//
// Both entry points take a predicate instead of a generator — `solid(y)` answers
// "is world y inside the terrain" for one already-chosen column. That is what
// keeps the search independent of HOW the column was obtained (a fresh sample
// here, a cached per-column context there) and lets the test drive it with an
// analytic height instead of a world.
//
// Cost is the reason this is a march and not a scan. The generator already has a
// rigorous surface search that walks the whole surface band one block at a time,
// which is right for a single column and wrong for a grid: a 2048-block square at
// a 4-block spacing is 262,144 columns. The march strides down in `step`-block
// increments to the first solid sample and only then bisects, so its cost is
// about (band / step) + log2(step) evaluations instead of (band) with a factor of
// two. See tools/lod_sample_bench.cpp for what the two actually cost.

// Where a march found the surface, and what it cost to find it.
struct MarchResult {
    int32_t y = 0;
    // False when the whole scanned band was air — a column whose terrain is below
    // the band, or none at all. The caller decides what stands in for it; the
    // generator's own rigorous search falls back to the macro height.
    bool found = false;
    // Predicate calls. Kept because the cost model is the reason this function
    // exists, and a test can hold it to the bound rather than to a wall clock.
    int32_t evaluations = 0;
};

// The topmost solid y in [bottom_y, top_y]: stride down to the first solid
// sample, then walk back UP to the first air sample above it. `solid` must be
// false at `top_y` for the result to be the topmost transition rather than a
// ceiling, which is why callers start above the column's surface band.
//
// The second half is a walk and not a bisection, and that is the whole point: a
// bisection between a known-air sample and a known-solid one finds A transition,
// not the highest one, so on a band that holds more than one — which this field
// does, the 3D shape deformation is what makes overhangs — it converges to the
// wrong side of a shelf. Measured on 64 columns over real terrain before the
// change: 34 mismatches at a 2-block stride and 56 at 16, with the error scaling
// with the stride (worst case 15 blocks), which is a mesh hole and not a rounding
// error. Walking up from the first solid sample is exact for every non-empty
// column and costs at most `step` more evaluations.
//
// `step` is clamped to at least 1, and 1 degenerates to the per-block scan, so a
// caller can A/B the two by changing one argument.
template <typename SolidFn>
inline MarchResult march_surface(int32_t top_y, int32_t bottom_y, int32_t step, SolidFn&& solid) {
    MarchResult result;
    if (step < 1) step = 1;
    if (top_y < bottom_y) return result;

    int32_t ceiling = top_y + 1;  // nothing sampled above the first solid sample
    int32_t y = top_y;
    for (; y >= bottom_y; y -= step) {
        ++result.evaluations;
        if (solid(y)) break;
        ceiling = y;
    }
    if (y < bottom_y) return result;  // the whole band was air

    // `ceiling` is air (or above the band) and `y` is solid, so the topmost
    // transition is the highest solid y in [y, ceiling).
    result.found = true;
    while (y + 1 < ceiling) {
        ++result.evaluations;
        if (!solid(y + 1)) break;
        ++y;
    }
    result.y = y;
    return result;
}

} // namespace lod
} // namespace VoxelEngine

#endif // FARLANDS_LOD_MARCH_HPP
