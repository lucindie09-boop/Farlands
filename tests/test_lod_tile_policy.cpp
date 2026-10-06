// The far-field tile rule (src/lod/lod_tile_policy.hpp).
//
// These assertions exist because of a horizon full of holes: the mode skipped
// every tile inside the loaded world's radius, but "inside the radius" was
// rounded UP to whole tiles, so the inner hole was a square. The world loads a
// disc, so the square's four corners -- from the world's radius out to 1.41x it --
// were drawn by neither the world nor the grid. Every direction looked fine and
// four wedges were missing.
//
// So the cases below are mostly about the tiles that STRADDLE the world's edge:
// they must be wanted, at the finest spacing, even though most of each one is
// clipped away.

#include "doctest.h"
#include "lod/lod_tile_policy.hpp"

#include <cstdint>

namespace {

using namespace VoxelEngine::lod;

constexpr int32_t kRings = 2;

// The render distances the engine actually runs at, in blocks.
constexpr int32_t kRadiusSmall = 256;  // render_distance 8
constexpr int32_t kRadiusBig = 2048;   // render_distance 64

} // namespace

TEST_CASE("lod tile policy: the tile straddling the world's edge is wanted") {
    // The regression: with a 256-block world, the tile containing the player was
    // skipped (out < 0), and that tile is where the first wedge lives -- the
    // player stands at a corner of it, so 3/4 of it is past the disc.
    const int32_t inner = lod_inner_tiles(kRadiusSmall);
    CHECK(inner == 1);
    CHECK(lod_level_for_distance(0, inner, kRings) == 0);
    CHECK(lod_level_for_distance(1, inner, kRings) == 0);

    // The same at a large render distance, where several tiles are inside the
    // square: all of them are still wanted, because each one reaches past the
    // disc somewhere (or is cheap enough that building it is not worth a hole).
    const int32_t inner_big = lod_inner_tiles(kRadiusBig);
    CHECK(inner_big == 8);
    for (int32_t d = 0; d < inner_big; ++d) {
        CHECK(lod_level_for_distance(d, inner_big, kRings) == 0);
    }
}

TEST_CASE("lod tile policy: spacing still doubles out from the world's edge") {
    const int32_t inner = lod_inner_tiles(kRadiusSmall);
    CHECK(lod_level_for_distance(inner + 0, inner, kRings) == 0);
    CHECK(lod_level_for_distance(inner + 1, inner, kRings) == 0);
    CHECK(lod_level_for_distance(inner + 2, inner, kRings) == 1);
    CHECK(lod_level_for_distance(inner + 3, inner, kRings) == 1);
    CHECK(lod_level_for_distance(inner + 4, inner, kRings) == 2);
    CHECK(lod_level_for_distance(inner + 6, inner, kRings) == 3);
    // Four levels of two rings each: the fifth level is not a level.
    CHECK(lod_level_for_distance(inner + 8, inner, kRings) == -1);
    CHECK(lod_level_for_distance(inner + 40, inner, kRings) == -1);
}

TEST_CASE("lod tile policy: only a whole tile inside the disc is skipped") {
    // A tile whose farthest corner (plus the worst player offset within their own
    // tile) is within the world's radius cannot be seen at all: every fragment of
    // it is clipped.
    const int32_t radius = 4096;  // render_distance 128
    CHECK(lod_tile_hidden_by_world(0, radius));
    CHECK(lod_tile_hidden_by_world(8, radius));
    CHECK_FALSE(lod_tile_hidden_by_world(11, radius));

    // The tile the player is standing in is skipped exactly when the world reaches
    // across it -- and that is not a hole, because the world is drawing all of it.
    CHECK(lod_tile_hidden_by_world(0, kRadiusBig));    // 2048: well past the corner
    CHECK_FALSE(lod_tile_hidden_by_world(0, kRadiusSmall));  // 256: the corner is outside

    // So a small world keeps its straddling tiles: the case that was broken.
    CHECK_FALSE(lod_tile_hidden_by_world(1, kRadiusSmall));

    // And with no loaded world there is nothing to hide behind.
    CHECK_FALSE(lod_tile_hidden_by_world(0, 0));
}

TEST_CASE("lod tile policy: the outer radius grows with the loaded world") {
    // The horizon is the loaded radius plus eight rings of tiles, so a bigger
    // render distance pushes the far field out rather than eating into it.
    CHECK(lod_outer_radius_blocks(kRadiusSmall, kRings) == 2048);
    CHECK(lod_outer_radius_blocks(kRadiusBig, kRings) == 3840);
    CHECK(lod_outer_radius_blocks(kRadiusBig, kRings) >
          lod_outer_radius_blocks(kRadiusSmall, kRings));
    for (int32_t r = 32; r <= 64; r += 1) {
        CHECK(lod_outer_radius_blocks(r * 32, kRings) > r * 32);
    }
}

TEST_CASE("lod tile policy: the reach knob only grows the outermost level") {
    // 0 means "the ladder as it was", which is what every setting saved before the
    // knob existed carries: the levels must be identical, distance for distance.
    CHECK(lod_outer_tiles(kRadiusSmall, kRings, 0) == lod_outer_tiles(kRadiusSmall, kRings));
    CHECK(lod_outer_radius_blocks(kRadiusSmall, kRings, 0) ==
          lod_outer_radius_blocks(kRadiusSmall, kRings));
    for (int32_t d = 0; d <= 12; ++d) {
        CHECK(lod_level_for_distance(d, 0, kRings, 0) == lod_level_for_distance(d, 0, kRings));
    }

    // Everything inside the outermost level does not move when the reach does.
    // That is the whole reason the knob is on the OUTER level: nothing inside is
    // re-sampled at a finer spacing and no tile already built has to be thrown
    // away, so a slider drag is a few dozen quads rather than a rebuild.
    for (int32_t outer = 1; outer <= 12; ++outer) {
        for (int32_t d = 0; d < kRings * kLevels; ++d) {
            CHECK(lod_level_for_distance(d, 0, kRings, outer) ==
                  lod_level_for_distance(d, 0, kRings));
        }
    }

    // Each extra ring is one more ring of tiles at the last level, so the horizon
    // grows by exactly one tile's width (256 blocks).
    for (int32_t outer = 1; outer <= 12; ++outer) {
        CHECK(lod_outer_tiles(0, kRings, outer) == lod_outer_tiles(0, kRings) + outer - kRings);
        CHECK(lod_outer_radius_blocks(0, kRings, outer) ==
              lod_outer_radius_blocks(0, kRings) + (outer - kRings) * kTileBlocks);
    }

    // ...and every one of them is drawn at the coarsest level, where a tile is a
    // single quad: the difference between a free knob and a rebuild.
    const int32_t inner = lod_inner_tiles(kRadiusSmall);
    const int32_t ladder = lod_outer_tiles(kRadiusSmall, kRings);
    const int32_t first_extra = ladder;  // one ring past the ladder's last
    CHECK(lod_level_for_distance(first_extra, inner, kRings, kRings + 2) == kLevels - 1);
    CHECK(lod_level_for_distance(ladder + 1, inner, kRings, kRings + 2) == kLevels - 1);
    // The ring after the new last one is not wanted at all, so the scan that walks
    // the outer bound stops exactly where the geometry does.
    CHECK(lod_level_for_distance(ladder + 2, inner, kRings, kRings + 2) == -1);

    // A reach SHORTER than the ladder is a shorter horizon, not a smaller world:
    // the levels inside are the same and only the outer band shrinks.
    CHECK(lod_outer_tiles(0, kRings, 1) == lod_outer_tiles(0, kRings) - kRings + 1);
    CHECK(lod_level_for_distance(first_extra - 1, inner, kRings, 1) == kLevels - 1);
    CHECK(lod_level_for_distance(first_extra, inner, kRings, 1) == -1);
}

TEST_CASE("lod tile policy: the horizon's spacing does not follow the detail setting") {
    // The regression: a level's spacing was `base << level` with no cap, so a base of
    // 64 asked for 64 / 128 / 256 / 512 and the last two could not be built at all --
    // a spacing that does not divide the 256-block tile is refused by the builder,
    // which is silent and counted as built. Above a base of 32 the outer levels were
    // ground nobody drew, so the reach slider did nothing there and 32, the coarsest
    // setting that still built its last level, was the one that saw furthest.
    for (int32_t base : {2, 4, 8, 16, 32, 64, 128, 256}) {
        for (int32_t level = 0; level < kLevels; ++level) {
            const int32_t spacing = lod_spacing_for_level(base, level);
            // Whole cells per tile: exactly the condition build_tile_mesh refuses on.
            CHECK(spacing > 0);
            CHECK(spacing <= kTileBlocks);
            CHECK(kTileBlocks % spacing == 0);
        }
        // A ladder never steps back inwards, whatever the base.
        for (int32_t level = 1; level < kLevels; ++level) {
            CHECK(lod_spacing_for_level(base, level) >=
                  lod_spacing_for_level(base, level - 1));
        }
    }

    // The base still shapes every band nearer than the horizon.
    CHECK(lod_spacing_for_level(8, 0) == 8);
    CHECK(lod_spacing_for_level(8, 1) == 16);
    CHECK(lod_spacing_for_level(8, 2) == 32);
    CHECK(lod_spacing_for_level(32, 0) == 32);
    CHECK(lod_spacing_for_level(32, 1) == 64);
    CHECK(lod_spacing_for_level(32, 2) == 128);

    // ...and the OUTERMOST level is the horizon's own spacing at every base, so "how
    // far can I see" is one distance and one cost, whether the near field is built at
    // 8-block cells or at 128. That is what makes the reach a distance setting.
    for (int32_t base : {2, 4, 8, 16, 32, 64, 128, 256}) {
        CHECK(lod_spacing_for_level(base, kLevels - 1) == kTileBlocks);
    }

    // A base coarser than 32 is capped rather than refused: 256 doubles to 512 on
    // paper and is 256 on the ground, which is a tile one quad across.
    CHECK(lod_spacing_for_level(256, 1) == kTileBlocks);
    CHECK(lod_spacing_for_level(256, 2) == kTileBlocks);
    CHECK(lod_spacing_for_level(128, 2) == kTileBlocks);
}

TEST_CASE("lod tile policy: the spacing snaps onto the values that build") {
    // A tile is a whole number of cells only when the spacing divides it, and the
    // tiles of a level share their edge nodes through that division. A spacing that
    // does not divide the tile makes build_tile_mesh return an EMPTY mesh -- the tile
    // counts as built and the ground is not drawn -- so the setting snaps rather than
    // accepting a request it cannot build.
    for (int32_t spacing : {2, 4, 8, 16, 32, 64, 128, 256}) {
        CHECK(lod_snap_spacing(spacing) == spacing);
        CHECK(kTileBlocks % lod_snap_spacing(spacing) == 0);
    }
    // Every one of these is a settings slider's own value: 24 and 40 and 96 used to
    // wipe out a level.
    CHECK(lod_snap_spacing(24) == 16);   // a tie goes to the smaller
    CHECK(lod_snap_spacing(40) == 32);
    CHECK(lod_snap_spacing(96) == 64);   // ...and so does this one
    CHECK(lod_snap_spacing(100) == 128);
    CHECK(lod_snap_spacing(12) == 8);
    // Out of range is clamped to the family's ends, never to zero or to a fraction.
    CHECK(lod_snap_spacing(0) == 2);
    CHECK(lod_snap_spacing(-64) == 2);
    CHECK(lod_snap_spacing(1000) == 256);
    CHECK(lod_snap_spacing(kTileBlocks) == kTileBlocks);
}
