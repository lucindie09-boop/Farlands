#pragma once

// Which far-field tiles the seed-grid mode wants, and which the loaded world
// already draws (docs/lod-modes.md).
//
// Pure arithmetic on tile indices, with no Godot and no generator, because this
// rule is what left four wedges of missing ground on the horizon the first time:
// the grid's inner hole was a SQUARE of whole tiles while the world loads a DISC,
// so the square's corners -- 1.41x the world's radius out -- were drawn by
// neither. The hole is now the world's own disc, applied as a shader clip against
// these same numbers, and every band of tiles the rule admits is pinned by
// tests/test_lod_tile_policy.cpp.

#include <algorithm>
#include <cstdint>

namespace VoxelEngine::lod {

// A tile is 256 blocks a side: the unit of build, drop and instance. At the four
// spacing levels a tile is 64, 16, 4 or 1 quad across, so the geometry per tile
// falls as fast as the tile count grows.
inline constexpr int32_t kTileBlocks = 256;

// Spacing levels: base, base*2, base*4, base*8.
inline constexpr int32_t kLevels = 4;

// The spacings that are geometry, and the request snapped onto them.
//
// A tile is a whole number of cells only when the spacing DIVIDES it, and the tiles
// of one level have to share their edge nodes or the surface cracks between them:
// `build_tile_mesh` refuses a spacing that does not divide the tile, which is the
// right refusal and also a silent one -- the tile comes back empty, the mode counts
// it as built, and the ground is simply not drawn. So the setting snaps to a power
// of two between 2 and the tile size, the nearest one, ties to the smaller: every
// one of them divides every tile and every level's spacing stays the same family.
// The settings slider snaps to the same set, so the number it shows is the number
// the geometry is built at.
inline int32_t lod_snap_spacing(int32_t blocks) {
    const int32_t wanted = std::clamp(blocks, 2, kTileBlocks);
    int32_t best = kTileBlocks;
    int32_t best_gap = kTileBlocks;
    for (int32_t spacing = kTileBlocks; spacing >= 2; spacing /= 2) {
        const int32_t gap = std::abs(wanted - spacing);
        if (gap < best_gap || (gap == best_gap && spacing < best)) {
            best = spacing;
            best_gap = gap;
        }
    }
    return best;
}

// How many whole tiles fit between the centre and the loaded world's edge.
inline int32_t lod_inner_tiles(int32_t inner_radius_blocks) {
    if (inner_radius_blocks <= 0) return 0;
    return (inner_radius_blocks + kTileBlocks - 1) / kTileBlocks;
}

// The spacing of a level's tiles, in blocks.
//
// The ladder doubles from the setting's base, and every level is CAPPED at the
// tile size -- a spacing that does not divide the tile places its nodes off the
// global lattice, and `build_tile_mesh` refuses such a tile outright (silently: the
// tile comes back empty and the ground is simply not drawn). A base of 64 wanted
// levels of 64 / 128 / 256 / 512, so its outer two levels used to be empty ground
// at every radius: the reach slider did nothing above a base of 32, and coarser
// settings saw LESS distance than finer ones rather than cheaper terrain.
//
// The OUTERMOST level is therefore pinned to the tile size, whatever the base: that
// level is not terrain, it is the start of the horizon -- beyond it lie the reach's
// own rings, and the whole band the setting offers is 46,000 tiles of ONE quad each
// at 256-block spacing against 64 quads each at 32. Tying that to the near detail
// would make the reach a quality setting: it would cost 64 times as much at the
// finest base and never finish. Pinned, "how far can I see" is the same distance at
// every spacing, which is what the settings row promises; the base spacing still
// shapes every band nearer than the horizon.
inline int32_t lod_spacing_for_level(int32_t base_blocks, int32_t level) {
    const int32_t base = std::clamp(base_blocks, 1, kTileBlocks);
    if (level >= kLevels - 1) return kTileBlocks;
    const int32_t spacing = base << std::max(0, level);
    return std::min(spacing, kTileBlocks);
}

// The spacing level a ring of tiles belongs to, or -1 for "no tile here".
//
// Tiles inside the inner square are WANTED at level 0 rather than skipped: the
// world covers a disc and the tiles are a square lattice, so the tiles straddling
// the disc's edge are the ones that fill the corners the disc cannot reach. What
// removes the part of them the world already draws is the clip in
// shaders/lod_grid.gdshader -- a clip rather than a hole, because a tile that
// straddles the edge has to be built once and stay valid while the player moves
// across it.
//
// `outer_rings` is the reach knob, and 0 means "the ladder as it was". The ladder
// itself -- `rings_per_level` rings per level, the spacing doubling with each -- is
// untouched by it: the knob says how many rings the OUTERMOST level covers, so
// everything inside keeps the distance it always had and only the coarsest band
// grows or shrinks.
//
// Reaching further belongs there and nowhere else: an outermost tile is one quad
// (its spacing is the tile size, see lod_spacing_for_level), so extra horizon costs
// one quad per tile, while the same 256 blocks at the innermost spacing is a tile
// of 4,096 quads. That is what makes "how far does it go" a slider rather than a rebuild --
// and why growing the reach leaves every built tile exactly where it is.
inline int32_t lod_level_for_distance(int32_t tile_distance, int32_t inner_tiles,
                                      int32_t rings_per_level, int32_t outer_rings = 0) {
    if (tile_distance < 0) return -1;
    const int32_t rings = std::max(1, rings_per_level);
    const int32_t outer = outer_rings > 0 ? outer_rings : rings;
    const int32_t out = tile_distance - std::max(0, inner_tiles);
    if (out <= 0) return 0;
    const int32_t ladder = rings * kLevels;
    if (out < ladder) return out / rings;
    // Past the ladder the outermost level keeps going, one ring at a time. Every
    // distance here is still a level-3 tile: a horizon that wants to be further
    // away than this is a horizon the spacing knob would have to pay for.
    if (out < ladder + (outer - rings)) return kLevels - 1;
    return -1;
}

// True when the loaded world draws the whole tile anyway, so building it would
// spend worker time on geometry the clip then throws away.
//
// Deliberately conservative: the world's disc is centred on the PLAYER, who can
// stand anywhere inside their own tile, so the test adds the worst case (half a
// tile diagonal) on top of the tile's own farthest corner. Erring the other way
// would leave a gap at the world's edge, which is the bug this file exists for.
inline bool lod_tile_hidden_by_world(int32_t tile_distance, int32_t inner_radius_blocks) {
    if (inner_radius_blocks <= 0) return false;
    constexpr double kTileDiagonal = 1.4142135623730951;
    constexpr double kHalfTileDiagonal = 0.7071067811865476;
    const double reach = static_cast<double>(tile_distance + 1) *
                         static_cast<double>(kTileBlocks) * kTileDiagonal;
    const double slack = static_cast<double>(kTileBlocks) * kHalfTileDiagonal;
    return reach + slack <= static_cast<double>(inner_radius_blocks);
}

// The tile count of the last wanted ring, counted from the centre: the bound a
// tile scan has to walk to. The ring indices it admits run from 0 (the tiles that
// straddle the world's disc) to this minus one.
inline int32_t lod_outer_tiles(int32_t inner_radius_blocks, int32_t rings_per_level,
                               int32_t outer_rings = 0) {
    const int32_t rings = std::max(1, rings_per_level);
    const int32_t outer = outer_rings > 0 ? outer_rings : rings;
    return lod_inner_tiles(inner_radius_blocks) + rings * (kLevels - 1) + outer;
}

// The radius the outermost ring reaches, in blocks: what the mode calls its
// horizon.
inline int32_t lod_outer_radius_blocks(int32_t inner_radius_blocks, int32_t rings_per_level,
                                       int32_t outer_rings = 0) {
    const int32_t tiles = lod_outer_tiles(inner_radius_blocks, rings_per_level, outer_rings);
    return (tiles - 1) * kTileBlocks;
}

} // namespace VoxelEngine::lod
