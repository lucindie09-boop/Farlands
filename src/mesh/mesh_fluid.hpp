#ifndef FARLANDS_MESH_FLUID_HPP
#define FARLANDS_MESH_FLUID_HPP

// -----------------------------------------------------------------------------
// Liquid surface geometry — the rule, with no chunk, no mesh builder and no
// registry in it. `passive_fluid_mesh` in mesh_builder_fluid.cpp is the only
// caller; tests call it directly, against a map of cells, which is the point of
// keeping it here.
//
// A liquid cell is not a box with a lower lid: its surface is a QUAD WITH FOUR
// INDEPENDENT CORNER HEIGHTS. Each corner takes its height from the 2x2 cells
// that share it, so a stream slopes down to its edges, a pool's rim dips where
// the water ends, and a side face's top edge follows the same two corners the
// surface does. That is why liquids cannot go through the generic emitters: a
// greedy run carries ONE top height and the per-AABB path carries one height per
// cell, so both can only produce a flat, stepped surface — which is what this
// replaced (a pool looked like a shallow staircase at every edge).
//
// The rule, restated in this project's vocabulary (a cell belongs to a liquid
// FAMILY by substance, and its strength is a depth 0..7 from a source, where 0
// is full strength):
//
//   For each of the four cells sharing a corner, in the corner's plane:
//     * if the cell ABOVE it holds the same substance, the corner is pinned to
//       FULL height. Checked first, and it is what makes a falling column a
//       column instead of a stack of shallow tiles;
//     * otherwise a same-substance cell contributes its own surface height (and
//       ten times that when it is full strength, so one source outweighs the
//       cells around it and the join between two levels lands near the weaker
//       one rather than halfway between them);
//     * a cell that does not hold a body — air, another liquid, anything
//       passable — contributes one full block of gap;
//     * a cell that does hold a body — stone, sand, a slab — contributes nothing
//       at all, neither gap nor weight, so a corner buried in rock has no gap to
//       measure and sits at full height.
//     height = 1 - total_gap / total_weight
//
// Follow that for a lone source ringed by air and every corner lands at 0.70,
// while the interior of a pool sits flat at 0.89: the pool is level and its rim
// falls away, which is the whole visual point. Sides are drawn from the cell
// floor up to those corner heights, and a face is only drawn when the cell next
// to it neither holds the same substance nor hides it (see face_visible).
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "mesh/mesh_types.hpp"

#include <cstdint>

namespace VoxelEngine {
namespace mesh_fluid {

// The liquid family a block is drawn as, or FluidKind::None when it is not a
// liquid at all. Two cells of the same family are one surface: the face between
// them is culled at any level and their corner heights average together.
//
// A block that declares a fluid state has that kind. The one liquid that
// deliberately does not — generated ocean water (`surface_water`), kept out of
// the fluid state table so the sea never ticks — is nevertheless water to look
// at, so it shares water's family. Without that the sea and a poured bucket
// would seam against each other and the shoreline would have no slope.
[[nodiscard]] inline FluidKind family_of(BlockID id, const BlockType& type) noexcept {
    if (type.fluid_kind != FluidKind::None) return type.fluid_kind;
    if (id == BlockIDs::SURFACE_WATER) return FluidKind::Water;
    return FluidKind::None;
}

// The surface height of a cell whose strength is `level` steps from full
// strength: depth 0 is 1/9 of a block ... depth 7 is 8/9, i.e. the surface sits
// at 1 - this. A level of 8 or more means "falling" and is read as full
// strength; this project stores a falling cell as depth 0 with a flag, so 8
// never arrives — the clamp is here so the rule reads like the rule.
[[nodiscard]] constexpr float height_percent(int level) noexcept {
    if (level >= 8) level = 0;
    if (level < 0) level = 0;
    return static_cast<float>(level + 1) / 9.0f;
}

// What the surface maths needs to know about one cell.
struct CellInfo {
    FluidKind family = FluidKind::None;  // None => not a liquid
    int level = 0;                       // steps from full strength; 0 = a source
    bool holds_body = false;             // does the cell fill space (BlockType::blocks_fluid)
};

[[nodiscard]] inline CellInfo classify(BlockID id, const BlockType& type) noexcept {
    CellInfo info;
    info.family = family_of(id, type);
    info.level = type.is_fluid_state() ? static_cast<int>(type.fluid_depth) : 0;
    info.holds_body = type.blocks_fluid();
    return info;
}

// The surface height at the corner whose MAX cell is (x, y, z): the corner is
// shared by (x,y,z), (x-1,y,z), (x,y,z-1) and (x-1,y,z-1), all in one plane.
// `lookup` maps (x, y, z) to a CellInfo for any cell, in or out of the chunk.
template <typename Lookup>
[[nodiscard]] float corner_height(const Lookup& lookup, FluidKind family,
                                  int32_t x, int32_t y, int32_t z) noexcept {
    float gap = 0.0f;
    int weight = 0;
    for (int j = 0; j < 4; ++j) {
        const int32_t cx = x - (j & 1);
        const int32_t cz = z - ((j >> 1) & 1);
        if (lookup(cx, y + 1, cz).family == family) {
            return 1.0f;  // a column: the same substance stands on this cell
        }
        const CellInfo here = lookup(cx, y, cz);
        if (here.family == family) {
            const float p = height_percent(here.level);
            if (here.level == 0) {
                gap += p * 10.0f;
                weight += 10;
            }
            gap += p;
            weight += 1;
            continue;
        }
        if (!here.holds_body) {
            gap += 1.0f;
            weight += 1;
        }
    }
    if (weight == 0) return 1.0f;  // walled in on all four sides: no gap to measure
    return 1.0f - gap / static_cast<float>(weight);
}

// The four corner heights of one cell, indexed [cz][cx] with 0 on the cell's
// -X/-Z side. Face vertices run (x,y,z), (x+1,y,z), (x+1,y,z+1), (x,y,z+1), i.e.
// h[0][0], h[0][1], h[1][1], h[1][0].
struct Corners {
    float h[2][2] = {{1.0f, 1.0f}, {1.0f, 1.0f}};

    // All four the same: the cell's surface is a flat quad, which is the only
    // case where neighbouring cells may be merged back into one quad (an ocean
    // would otherwise be one quad per cell).
    [[nodiscard]] bool flat() const noexcept {
        return h[0][0] == h[0][1] && h[0][0] == h[1][0] && h[0][0] == h[1][1];
    }
    [[nodiscard]] float max_height() const noexcept {
        float m = h[0][0];
        if (h[0][1] > m) m = h[0][1];
        if (h[1][0] > m) m = h[1][0];
        if (h[1][1] > m) m = h[1][1];
        return m;
    }
};

template <typename Lookup>
[[nodiscard]] Corners corners_of(const Lookup& lookup, FluidKind family,
                                 int32_t x, int32_t y, int32_t z) noexcept {
    Corners c;
    for (int cz = 0; cz < 2; ++cz) {
        for (int cx = 0; cx < 2; ++cx) {
            c.h[cz][cx] = corner_height(lookup, family, x + cx, y, z + cz);
        }
    }
    return c;
}

// Whether a liquid face in `dir` is drawn at all.
//
//   * the same substance never draws a face against itself, in any direction and
//     whatever the two levels are — that single rule is what makes a waterfall
//     read as one column instead of a stack of tiles, and it is why a pool has no
//     internal surfaces;
//   * a full opaque cube hides the face only when it reaches as high as the
//     liquid surface. Stone does; mud, whose top is one pixel down, would leave a
//     sliver of a full-strength surface behind it, so it does not — the same
//     height comparison the solid path makes;
//   * everything else (air, a slab, another liquid) leaves the face visible.
[[nodiscard]] inline bool face_visible(FluidKind family, const CellInfo& neighbor,
                                       const BlockType& neighbor_type,
                                       float surface_height) noexcept {
    if (neighbor.family == family) return false;
    if (!neighbor_type.is_full_cube()) return true;
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) return true;
    return (1.0f - neighbor_type.top_face_offset) < surface_height;
}

}  // namespace mesh_fluid
}  // namespace VoxelEngine

#endif  // FARLANDS_MESH_FLUID_HPP
