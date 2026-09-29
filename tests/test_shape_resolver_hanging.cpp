#include "doctest.h"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "shape_resolver_test_support.hpp"

using namespace VoxelEngine;
using namespace shape_resolver_test;

TEST_CASE("a hanging stair is the upright one mirrored about the floor") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const ShapeFace faces[4] = {ShapeFace::Back, ShapeFace::Front, ShapeFace::Right,
                                ShapeFace::Left};
    for (uint8_t f = 0; f < 4; ++f) {
        const BlockType up = make_stair("test_mirror_up", faces[f]);
        const BlockType down = make_hanging_stair("test_mirror_down", faces[f]);

        CHECK(down.stair_hanging);
        CHECK(!up.stair_hanging);
        // The step points the same way whichever way up the stair is: what changed is
        // which half of the cell is the tall one, and that is carried by the boxes.
        CHECK(down.stair_step_face == up.stair_step_face);

        NeighborTable none;
        const ShapeBoxes a = resolve_with(up, none, ShapeBoxKind::Selection);
        const ShapeBoxes b = resolve_with(down, none, ShapeBoxKind::Selection);
        CHECK(a.count() == b.count());
        for (uint8_t i = 0; i < a.count() && i < b.count(); ++i) {
            CHECK(same_box(mirror_y(a[i]), b[i]));
        }
    }
}

TEST_CASE("a hanging stair turns with another hanging one, in the lower half") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID north_up =
        reg.register_block(make_hanging_stair("test_up_corner_n", ShapeFace::Back));
    const BlockID east_up =
        reg.register_block(make_hanging_stair("test_up_corner_e", ShapeFace::Right));
    const BlockID west_up =
        reg.register_block(make_hanging_stair("test_up_corner_w", ShapeFace::Left));
    const BlockID upright = reg.register_block(make_stair("test_up_corner_plain", ShapeFace::Back));
    if (north_up == BlockIDs::AIR || east_up == BlockIDs::AIR || west_up == BlockIDs::AIR ||
        upright == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north_up);

    NeighborTable table;
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);

    // The stair it turns with stands BEHIND the step (+Z) stepping toward +X, so the
    // quarter fills the right half of the region behind the step � the mirrored
    // counterpart of the upright stair's, in the LOWER half because that is where a
    // hanging stair's step is.
    table.set(ShapeFace::Front, east_up);
    const ShapeBoxes corner = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(corner.count() == 3);
    CHECK(has_box(corner, 0.5f, 0.0f, 0.5f, 1.0f, 0.5f, 1.0f));
    CHECK(has_box(corner, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));   // the hanging step, whole
    CHECK(has_box(corner, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f));   // the raised slab
    // A corner quarter is part of the block: it is solid for collision too.
    CHECK(resolve_with(stair, table, ShapeBoxKind::Collision).count() == 3);

    table.set(ShapeFace::Front, west_up);
    const ShapeBoxes mirror = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(mirror.count() == 3);
    CHECK(has_box(mirror, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f));

    // An upright stair behind the step points at the same side but is the other way
    // up, so it is not a turn at all: the two are mirrored, and the quarter would sit
    // in a step-shaped gap that is not there.
    table.set(ShapeFace::Front, upright);
    const ShapeBoxes mixed = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(mixed.count() == 2);
    CHECK(has_box(mixed, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));
}

TEST_CASE("a hanging stair's step is cut back by another hanging stair across it") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    // The stair under test is `stair/n_up`: step hanging at -Z.
    const BlockID north_up = reg.register_block(make_hanging_stair("test_up_cut_n", ShapeFace::Back));
    const BlockID east_up =
        reg.register_block(make_hanging_stair("test_up_cut_e", ShapeFace::Right));
    const BlockID west_up = reg.register_block(make_hanging_stair("test_up_cut_w", ShapeFace::Left));
    // ...and an upright one TURNED ACROSS it, which would cut an upright step. It is
    // the wrong way up, so the hanging step stays whole.
    const BlockID upright = reg.register_block(make_stair("test_up_cut_plain", ShapeFace::Left));
    if (north_up == BlockIDs::AIR || east_up == BlockIDs::AIR || west_up == BlockIDs::AIR ||
        upright == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north_up);

    // In FRONT of the hanging step (-Z), stepping toward -X: the -X half of the step is
    // what survives, exactly as for an upright stair.
    NeighborTable table;
    table.set(ShapeFace::Back, west_up);
    const ShapeBoxes cut = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(cut.count() == 2);
    CHECK(has_box(cut, 0.0f, 0.0f, 0.0f, 0.5f, 0.5f, 0.5f));   // the remainder
    CHECK(!has_box(cut, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));  // the whole hanging step is gone

    table.set(ShapeFace::Back, east_up);
    const ShapeBoxes cut_other = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(cut_other.count() == 2);
    CHECK(has_box(cut_other, 0.5f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));

    table.set(ShapeFace::Back, upright);
    const ShapeBoxes mixed = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(mixed.count() == 2);
    CHECK(has_box(mixed, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 0.5f));
}

// What one exhaustive sweep of a stair's six neighbours found.
struct StairSweep {
    int full_cells = 0;      // neighbourhoods covering every quadrant of the step's half
    int bad_box_counts = 0;  // resolutions that are not slab + at most a step and a quarter
    int turned = 0;          // neighbourhoods where the rules drew something extra
};

// The sweep: every combination of `kind_count` neighbours on six faces, watching for
// the three things that must never happen. Parameterized by which half the step lives
// in, because a hanging stair is the SAME rules mirrored: its step and its quarters are
// in the lower half and its slab � which covers all four quadrants of its own half by
// itself � is in the upper one. So the slab is skipped in both cases and only boxes
// lying inside the step's half can count as covering it.
StairSweep sweep_stair(const BlockType& stair, const BlockID* kinds, int kind_count) {
    StairSweep out;
    const float band_lo = stair.stair_hanging ? 0.0f : 0.5f;

    int32_t combinations = 1;
    for (int i = 0; i < 6; ++i) combinations *= kind_count;

    for (int32_t combo = 0; combo < combinations; ++combo) {
        NeighborTable table;
        int32_t rest = combo;
        for (uint8_t f = 0; f < 6; ++f) {
            table.faces[f] = kinds[rest % kind_count];
            rest /= kind_count;
        }
        const ShapeBoxes boxes = resolve_with(stair, table, ShapeBoxKind::Selection);
        // A stair is always its slab plus at most a step and one quarter.
        if (boxes.count() < 2 || boxes.count() > 4) ++out.bad_box_counts;
        if (boxes.count() > 2) ++out.turned;

        bool quadrant_open = false;
        for (int qx = 0; qx < 2 && !quadrant_open; ++qx) {
            for (int qz = 0; qz < 2 && !quadrant_open; ++qz) {
                bool covered = false;
                for (uint8_t i = 0; i < boxes.count(); ++i) {
                    const BlockAABB& b = boxes[i];
                    if (b.min[1] < band_lo - 1e-4f || b.max[1] > band_lo + 0.5f + 1e-4f) {
                        continue;  // the slab, or a box in the other half entirely
                    }
                    if (b.min[0] <= qx * 0.5f + 1e-4f && b.max[0] >= (qx + 1) * 0.5f - 1e-4f &&
                        b.min[2] <= qz * 0.5f + 1e-4f && b.max[2] >= (qz + 1) * 0.5f - 1e-4f) {
                        covered = true;
                        break;
                    }
                }
                if (!covered) quadrant_open = true;
            }
        }
        if (!quadrant_open) ++out.full_cells;
    }
    return out;
}

// The regression this whole rule set exists to fix: with the corner decided by a
// SIDE neighbour, two corners on opposite sides plus the whole step covered all
// four upper quadrants, so the stair resolved into a full cube. The rules are now
// the ones the geometry forces: the step is whole only while nothing cuts it, the
// squares the corner and the remnant call "this side" are one side each, and the
// corner's partner is a single cell, so at most one corner can ever appear. That
// leaves one of the four quadrants of the step's half open in every neighbourhood,
// which is what the sweep proves � a sweep that also has to find neighbour-dependent
// geometry, or it would pass on a stair that never turns at all.
//
// Both ways up are swept, because the hanging stairs have the same rules: the same
// property has to hold for the mirrored family, and a neighbourhood that only mixes
// the two kinds proves the up-ness guard keeps them apart.
TEST_CASE("no neighbourhood can turn a stair into a full cell") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID stair_ids[4] = {
        reg.register_block(make_stair("test_sweep_n", ShapeFace::Back)),
        reg.register_block(make_stair("test_sweep_s", ShapeFace::Front)),
        reg.register_block(make_stair("test_sweep_e", ShapeFace::Right)),
        reg.register_block(make_stair("test_sweep_w", ShapeFace::Left)),
    };
    const BlockID hanging_ids[4] = {
        reg.register_block(make_hanging_stair("test_sweep_n_up", ShapeFace::Back)),
        reg.register_block(make_hanging_stair("test_sweep_s_up", ShapeFace::Front)),
        reg.register_block(make_hanging_stair("test_sweep_e_up", ShapeFace::Right)),
        reg.register_block(make_hanging_stair("test_sweep_w_up", ShapeFace::Left)),
    };
    for (uint8_t i = 0; i < 4; ++i) {
        if (stair_ids[i] == BlockIDs::AIR || hanging_ids[i] == BlockIDs::AIR) {
            CHECK(false);
            return;
        }
    }

    // Each sweep carries the other family as its seventh kind, so every neighbourhood
    // in it is a mix and the guard is under test along with the rules.
    const BlockID upright_kinds[7] = {BlockIDs::AIR, BlockIDs::STONE, stair_ids[0], stair_ids[1],
                                      stair_ids[2], stair_ids[3], hanging_ids[0]};
    const BlockID hanging_kinds[7] = {BlockIDs::AIR, BlockIDs::STONE, hanging_ids[0],
                                      hanging_ids[1], hanging_ids[2], hanging_ids[3], stair_ids[0]};

    for (uint8_t v = 0; v < 4; ++v) {
        const StairSweep upright = sweep_stair(reg.get_block(stair_ids[v]), upright_kinds, 7);
        CHECK(upright.full_cells == 0);
        CHECK(upright.bad_box_counts == 0);
        // ...and the sweep has to reach the neighbour-dependent geometry at all, or
        // it proves nothing about it.
        CHECK(upright.turned > 0);

        const StairSweep hanging = sweep_stair(reg.get_block(hanging_ids[v]), hanging_kinds, 7);
        CHECK(hanging.full_cells == 0);
        CHECK(hanging.bad_box_counts == 0);
        CHECK(hanging.turned > 0);
    }
}
