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

#include <cmath>
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
    bool falling = false;                // a falling column: full height whatever the depth says
};

[[nodiscard]] inline CellInfo classify(BlockID id, const BlockType& type) noexcept {
    CellInfo info;
    info.family = family_of(id, type);
    info.level = type.is_fluid_state() ? static_cast<int>(type.fluid_depth) : 0;
    info.holds_body = type.blocks_fluid();
    info.falling = type.is_fluid_state() && type.fluid_falling;
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
//   * a DIFFERENT substance side by side is decided per corner, not by one
//     nominal height — see different_liquid_side_visible below (the caller
//     handles it, because it needs the shared edge's corner heights);
//   * everything else (air, a slab) leaves the face visible.
//
// `neighbor_is_above` (the neighbour in the cell above this one) changes the
// geometry: any block above sits on this cell's TOP PLANE, at height 1.0 — its
// floor, not its surface. A liquid surface below that plane is not covered by
// the block (the open band between surface and cell top is exactly why the face
// must draw), so the top face is visible unless the surface itself reaches 1.0 —
// a full-height column — where it would z-fight the block's floor. This is the
// fix for a block placed on runoff culling the surface beneath it.
[[nodiscard]] inline bool face_visible(FluidKind family, const CellInfo& neighbor,
                                       const BlockType& neighbor_type,
                                       float surface_height,
                                       bool neighbor_is_above = false) noexcept {
    if (neighbor.family == family) return false;
    if (neighbor_is_above) return surface_height < 1.0f;
    if (!neighbor_type.is_full_cube()) return true;
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) return true;
    return (1.0f - neighbor_type.top_face_offset) < surface_height;
}

// A side face against a DIFFERENT substance, decided on the shared edge's
// actual corner heights. Two liquid-liquid side faces sit on the same plane
// with opposite winding, so back-face culling means they are never visible
// from the same side — there is no double-blend. A face is redundant only
// when the neighbour's face covers it WHOLLY: its profile (floor to two
// corner heights, linear between) at-or-below the neighbour's at both shared
// corners. Opposing flows interleave — each is taller at one corner — so
// both draw; a uniformly shorter face is culled.
// `my_*` / `other_*` are the two shared-edge corner heights of each face,
// in the same corner order.
[[nodiscard]] inline bool different_liquid_side_visible(
    float my_c0, float my_c1, float other_c0, float other_c1) noexcept {
    return !(my_c0 <= other_c0 && my_c1 <= other_c1);
}

// ---------------------------------------------------------------------------
// Texture flow direction, packed into the liquid vertices' AO byte.
//
// A liquid is never occluded — the mesher packs full-bright AO into every
// fluid vertex and the surface shader never darkens by it — so that byte is
// free to carry something the water actually shows: which way its texture
// should drift. The surface's own corner heights already encode the slope,
// and water flows downhill, so the DIRECTION is derived from the same data
// that draws the surface; nothing in the simulation has to record it.
//
// Layout: high nibble = direction, low nibble = strength (0..15).
//   0        no flow (a level surface — a lone source, a pool's interior)
//   1..8     compass across the TOP face, clockwise from north (-Z):
//            1 N, 2 NE, 3 E (+X), 4 SE, 5 S (+Z), 6 SW, 7 W (-X), 8 NW
//   9        straight DOWN a SIDE face (the waterfall reading)
[[nodiscard]] inline uint8_t pack_flow(int direction, float strength) noexcept {
    if (direction <= 0 || strength <= 0.0f) return 0;
    if (direction > 9) direction = 9;
    if (strength > 1.0f) strength = 1.0f;
    const int s = static_cast<int>(strength * 15.0f + 0.5f);
    return static_cast<uint8_t>((direction << 4) | s);
}

// The flow byte for a TOP face from its four corner heights: the downhill
// gradient. A level surface has zero gradient and packs 0 (no flow); the
// strength grows with the steepness so deeper, faster water visibly drifts
// quicker. `corners` is indexed [cz][cx] with cx=1 on +X and cz=1 on +Z.
[[nodiscard]] inline uint8_t top_face_flow(const Corners& c) noexcept {
    const float west  = (c.h[0][0] + c.h[1][0]) * 0.5f;
    const float east  = (c.h[0][1] + c.h[1][1]) * 0.5f;
    const float north = (c.h[0][0] + c.h[0][1]) * 0.5f;
    const float south = (c.h[1][0] + c.h[1][1]) * 0.5f;
    // Downhill: toward the lower side. +x when the east side is lower, +z
    // when the south side is lower.
    const float fx = west - east;
    const float fz = north - south;
    const float mag = std::sqrt(fx * fx + fz * fz);
    // Below ~half a pixel of drop the surface reads as level (this is also
    // the merged-run case, where all four corners are identical).
    if (mag < 0.004f) return 0;
    // Compass bucket: 0 radians points north (-Z), a quarter turn east.
    const float angle = std::atan2(fx, -fz);
    int bucket = static_cast<int>(std::lround(angle * (4.0f / 3.14159265f))) & 7;
    const float strength = mag * 6.0f < 0.2f ? 0.2f : mag * 6.0f;
    return pack_flow(bucket + 1, strength);
}

// The flow byte for a SIDE face: a moving liquid reads as sliding down its
// own wall (the waterfall effect); a source's sides hold still.
[[nodiscard]] inline uint8_t side_face_flow(bool is_fluid_state, bool falling,
                                            int depth) noexcept {
    if (!is_fluid_state) return 0;
    if (falling) return pack_flow(9, 1.0f);
    if (depth > 0) return pack_flow(9, 0.45f);
    return 0;
}

}  // namespace mesh_fluid
}  // namespace VoxelEngine

#endif  // FARLANDS_MESH_FLUID_HPP
