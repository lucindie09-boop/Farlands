#include "doctest.h"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "shape_resolver_test_support.hpp"

using namespace VoxelEngine;
using namespace shape_resolver_test;

TEST_CASE("a stair's left and right follow the way its step points") {
    // Pinned because the rule names are written against this convention: from the
    // point of view of standing on the stair, a step pointing -Z has -X on the left.
    CHECK(shape_step_left_of(ShapeFace::Back) == ShapeFace::Left);
    CHECK(shape_step_right_of(ShapeFace::Back) == ShapeFace::Right);
    CHECK(shape_step_left_of(ShapeFace::Front) == ShapeFace::Right);
    CHECK(shape_step_right_of(ShapeFace::Front) == ShapeFace::Left);
    CHECK(shape_step_left_of(ShapeFace::Right) == ShapeFace::Back);
    CHECK(shape_step_right_of(ShapeFace::Right) == ShapeFace::Front);
    CHECK(shape_step_left_of(ShapeFace::Left) == ShapeFace::Front);
    CHECK(shape_step_right_of(ShapeFace::Left) == ShapeFace::Back);
    // Not a side at all: the helper must not pretend otherwise.
    CHECK(shape_step_left_of(ShapeFace::Top) == ShapeFace::Top);
    CHECK(shape_opposite_face(ShapeFace::Back) == ShapeFace::Front);
}

TEST_CASE("a lone stair is its slab and its whole step, whichever way it points") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const ShapeFace steps[4] = {ShapeFace::Back, ShapeFace::Front, ShapeFace::Right,
                                ShapeFace::Left};
    for (ShapeFace step : steps) {
        const BlockType stair = make_stair("test_stair_lone", step);
        NeighborTable none;
        const ShapeBoxes boxes = resolve_with(stair, none, ShapeBoxKind::Selection);
        CHECK(boxes.count() == 2);
        CHECK(has_box(boxes, 0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f));        // the slab
        const BlockAABB expected = edge_half_box(step);
        CHECK(has_box(boxes, expected.min[0], 0.5f, expected.min[2], expected.max[0], 1.0f,
                      expected.max[2]));
        // ...and that is what a worldless consumer (icon, hand model, outline) sees.
        CHECK(stair.selection_boxes.size() == 2);
    }
}

TEST_CASE("the stair rules read their claim from the step's direction, not from the boxes") {
    // The step face and the guard side are not where the corner boxes are, so these
    // four masks are what the loader takes from the rule instead of the geometry.
    const BlockType stair = make_stair("test_stair_masks", ShapeFace::Back);
    const uint8_t back = shape_face_bit(ShapeFace::Back);
    const uint8_t front = shape_face_bit(ShapeFace::Front);
    const uint8_t left = shape_face_bit(ShapeFace::Left);
    const uint8_t right = shape_face_bit(ShapeFace::Right);

    const ShapePart* step = nullptr;
    const ShapePart* corner_left = nullptr;
    const ShapePart* corner_right = nullptr;
    const ShapePart* cut_left = nullptr;
    const ShapePart* cut_right = nullptr;
    for (const ShapePart& part : stair.parts) {
        switch (part.rule) {
            case ShapeRule::StairStep:        step = &part; break;
            case ShapeRule::StairCornerLeft:  corner_left = &part; break;
            case ShapeRule::StairCornerRight: corner_right = &part; break;
            case ShapeRule::StairCutLeft:     cut_left = &part; break;
            case ShapeRule::StairCutRight:    cut_right = &part; break;
            default: break;
        }
    }
    CHECK(step != nullptr);
    CHECK(corner_left != nullptr);
    CHECK(corner_right != nullptr);
    CHECK(cut_left != nullptr);
    CHECK(cut_right != nullptr);
    if (step == nullptr || corner_left == nullptr || corner_right == nullptr ||
        cut_left == nullptr || cut_right == nullptr) {
        return;
    }
    // The step and the remnants it is cut back to are decided in front of the step,
    // which is the one face they claim. What suppresses a cut is a neighbour rather
    // than a face, so the side it stands on is asked inside the rule.
    CHECK(step->faces == back);
    CHECK(cut_left->faces == back);
    CHECK(cut_right->faces == back);
    // A corner claims three: the stair it turns with is BEHIND the step (+Z here),
    // the step face itself, and the side the quarter sits on � the side, not the one
    // across from it, because that is the cell whose stair already reaches into the
    // quarter's own region.
    CHECK(corner_left->faces == static_cast<uint8_t>(front | back | left));
    CHECK(corner_right->faces == static_cast<uint8_t>(front | back | right));
}

TEST_CASE("a stair behind the step pointing at a side fills that corner") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    // make_stair builds the `stair/n` variant: step against -Z.
    const BlockID north = reg.register_block(make_stair("test_stair_n", ShapeFace::Back));
    const BlockID west = reg.register_block(make_stair("test_stair_w", ShapeFace::Left));
    const BlockID east = reg.register_block(make_stair("test_stair_e", ShapeFace::Right));
    const BlockID south = reg.register_block(make_stair("test_stair_s", ShapeFace::Front));
    // A hanging stair stepping toward +X: a side, so without the up-ness guard this is
    // exactly the neighbour that would fill a corner.
    const BlockID hanging = reg.register_block(make_hanging_stair("test_stair_n_up", ShapeFace::Right));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    NeighborTable table;
    // Alone: the slab and its step, no corner.
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);

    // The stair it turns with stands BEHIND the step, on +Z, stepping toward +X:
    // that is the corner on this stair's right, so the quarter fills the right half
    // of the region behind the step.
    table.set(ShapeFace::Front, east);
    const ShapeBoxes corner = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(corner.count() == 3);
    CHECK(has_box(corner, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f));
    CHECK(!has_box(corner, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f));
    // A corner quarter is part of the block: it is solid for collision too.
    CHECK(resolve_with(stair, table, ShapeBoxKind::Collision).count() == 3);

    // The same stair pointing the other way fills the mirror quarter.
    table.set(ShapeFace::Front, west);
    const ShapeBoxes mirror = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(mirror.count() == 3);
    CHECK(has_box(mirror, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f));

    // Neighbours behind the step that do not point at a side: no corner. A stair
    // pointing the way this one does is the flight continuing, one pointing the other
    // way is climbing off, and a hanging stair is not an upright step at all.
    const BlockID not_a_turn[5] = {north, south, hanging, BlockIDs::STONE, BlockIDs::AIR};
    for (int i = 0; i < 5; ++i) {
        NeighborTable no_turn;
        no_turn.set(ShapeFace::Front, not_a_turn[i]);
        const ShapeBoxes boxes = resolve_with(stair, no_turn, ShapeBoxKind::Selection);
        CHECK(boxes.count() == 2);
        CHECK(has_box(boxes, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));
    }
}

TEST_CASE("a stair turned across the step cuts it back to one side") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID north = reg.register_block(make_stair("test_cut_n", ShapeFace::Back));
    const BlockID east  = reg.register_block(make_stair("test_cut_e", ShapeFace::Right));
    const BlockID west  = reg.register_block(make_stair("test_cut_w", ShapeFace::Left));
    // Turned ACROSS this stair's step rather than pointing along it, so the up-ness
    // guard is the only thing keeping the upright step whole here.
    const BlockID hanging = reg.register_block(make_hanging_stair("test_cut_up", ShapeFace::Right));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    // In FRONT of the step, stepping toward -X (our left): the -X half of the step is
    // what survives, because that is the half the two steps run into each other on.
    NeighborTable table;
    table.set(ShapeFace::Back, west);
    const ShapeBoxes cut = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(cut.count() == 2);
    CHECK(has_box(cut, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 0.5f));    // the remainder
    CHECK(!has_box(cut, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));   // the whole step is gone

    table.set(ShapeFace::Back, east);
    const ShapeBoxes cut_other = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(cut_other.count() == 2);
    CHECK(has_box(cut_other, 0.5f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));

    // Everything else leaves the step whole: air, stone, a stair pointing the way
    // this one does, one climbing away from it, and a hanging stair (which is not an
    // upright step, so it cuts nothing).
    const BlockID south = reg.register_block(make_stair("test_cut_s", ShapeFace::Front));
    const BlockID whole[5] = {BlockIDs::AIR, BlockIDs::STONE, north, south, hanging};
    for (int i = 0; i < 5; ++i) {
        NeighborTable leaves_alone;
        leaves_alone.set(ShapeFace::Back, whole[i]);
        const ShapeBoxes boxes = resolve_with(stair, leaves_alone, ShapeBoxKind::Selection);
        CHECK(boxes.count() == 2);
        CHECK(has_box(boxes, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));
    }
}

// A stair standing on this one's step on the side a quarter would fill is already
// in that region, so the quarter is not drawn: two flights turning into each other
// do not both grow the same corner.
TEST_CASE("a corner is not drawn where a stair on the same step already stands") {
    // The guard: the quarter's own side, because that is the cell whose stair is
    // already standing in the region the quarter would fill.
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID north = reg.register_block(make_stair("test_guard_n", ShapeFace::Back));
    const BlockID east  = reg.register_block(make_stair("test_guard_e", ShapeFace::Right));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    // The turn behind the step points at our right (corner_right's side), and a
    // stair on our right is standing on the very region that quarter would fill.
    NeighborTable guarded;
    guarded.set(ShapeFace::Front, east);
    guarded.set(ShapeFace::Right, north);
    CHECK(resolve_with(stair, guarded, ShapeBoxKind::Selection).count() == 2);

    // The same stair on the other side does not suppress it: the quarter lives on
    // one side, and the guard is that side.
    NeighborTable other_side;
    other_side.set(ShapeFace::Front, east);
    other_side.set(ShapeFace::Left, north);
    CHECK(resolve_with(stair, other_side, ShapeBoxKind::Selection).count() == 3);
}

// A stair beside the half that would be cut away keeps the step whole: two flights
// meet along that half, so there is no step there to cut back to. This is what keeps
// a wide flight from developing a notch wherever something turns across it - and it
// is where the cut and the corner rules meet, because the turn BEHIND the step still
// fills its quarter while the step itself stays whole.
TEST_CASE("a flight beside the half that would be cut away keeps the step whole") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID north = reg.register_block(make_stair("test_suppress_n", ShapeFace::Back));
    const BlockID west  = reg.register_block(make_stair("test_suppress_w", ShapeFace::Left));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    // On its own, a stair in front pointing at our left cuts the step back to its
    // left half: the half the two steps run into each other on.
    NeighborTable alone;
    alone.set(ShapeFace::Back, west);
    const ShapeBoxes cut = resolve_with(stair, alone, ShapeBoxKind::Selection);
    CHECK(cut.count() == 2);
    CHECK(has_box(cut, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 0.5f));
    CHECK(!has_box(cut, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));

    // With a stair on this one's own step on our right - the half that would be cut
    // away - the cut is off, and the cell is the plain stair again: the whole step,
    // no remnant.
    NeighborTable beside;
    beside.set(ShapeFace::Back, west);
    beside.set(ShapeFace::Right, north);
    const ShapeBoxes whole = resolve_with(stair, beside, ShapeBoxKind::Selection);
    CHECK(whole.count() == 2);
    CHECK(has_box(whole, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));
    CHECK(!has_box(whole, 0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 0.5f));

    // ...and a turn behind the step still fills its quarter in that same cell, on
    // the side the flight is not on.
    NeighborTable turned;
    turned.set(ShapeFace::Back, west);    // the stair turned across the step
    turned.set(ShapeFace::Right, north);  // the flight it runs into along that half
    turned.set(ShapeFace::Front, west);   // and the turn behind the step, pointing left
    const ShapeBoxes boxes = resolve_with(stair, turned, ShapeBoxKind::Selection);
    CHECK(boxes.count() == 3);
    CHECK(has_box(boxes, 0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f));    // the step, whole
    CHECK(has_box(boxes, 0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f));    // ...and the corner
    // Three of the four upper quadrants at most, never all four: a cell can never
    // resolve into a full block.
    CHECK(!has_box(boxes, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f));
}

TEST_CASE("the canonical stair is the plain step the inventory should draw") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const ShapeFace steps[4] = {ShapeFace::Back, ShapeFace::Front, ShapeFace::Right,
                                ShapeFace::Left};
    for (ShapeFace step : steps) {
        const BlockType stair = make_stair("test_stair_canonical", step);

        ShapeBoxes canonical;
        resolve_canonical_boxes(stair, ShapeBoxKind::Selection, canonical);

        // Slab and whole step, no corner and no cut: both of those mean "a stair is
        // beside me", which a worldless consumer standing in the inventory cannot know.
        CHECK(canonical.count() == 2);
        CHECK(stair.selection_boxes.size() == 2);
        const BlockAABB expected = edge_half_box(step);
        CHECK(has_box(canonical, expected.min[0], 0.5f, expected.min[2], expected.max[0], 1.0f,
                      expected.max[2]));
    }
}
