#include "doctest.h"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "shape_resolver_test_support.hpp"

using namespace VoxelEngine;
using namespace shape_resolver_test;

TEST_CASE("an isolated fence resolves to its post alone") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockType fence = make_fence("test_fence_isolated");
    NeighborTable none;

    const ShapeBoxes boxes = resolve_with(fence, none, ShapeBoxKind::Selection);

    CHECK(boxes.count() == 1);
    CHECK(boxes[0].min[0] == doctest::Approx(0.375f));
    CHECK(boxes[0].max[1] == doctest::Approx(1.0f));
}

TEST_CASE("a fence arms toward a neighbouring fence and only on that side") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence_id = reg.register_block(make_fence("test_fence_armed"));
    if (fence_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& fence = reg.get_block(fence_id);

    NeighborTable east;
    east.set(ShapeFace::Right, fence_id);
    const ShapeBoxes boxes = resolve_with(fence, east, ShapeBoxKind::Selection);

    CHECK(boxes.count() == 3);                          // post + two rails eastward
    CHECK(boxes_reaching(boxes, ShapeFace::Right) == 2);
    CHECK(boxes_reaching(boxes, ShapeFace::Left) == 0);
    CHECK(boxes_reaching(boxes, ShapeFace::Front) == 0);
    CHECK(boxes_reaching(boxes, ShapeFace::Back) == 0);

    // Both ends of a run at once: the arms are per side, not one shared set.
    NeighborTable both;
    both.set(ShapeFace::Right, fence_id);
    both.set(ShapeFace::Back, fence_id);
    const ShapeBoxes corner = resolve_with(fence, both, ShapeBoxKind::Selection);
    CHECK(corner.count() == 5);
    CHECK(boxes_reaching(corner, ShapeFace::Right) == 2);
    CHECK(boxes_reaching(corner, ShapeFace::Back) == 2);
}

TEST_CASE("a fence connects to a body-stopping block but not to air, water or a window") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence_id = reg.register_block(make_fence("test_fence_connect"));
    const BlockID pane_id = make_window(reg);
    if (fence_id == BlockIDs::AIR || pane_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& fence = reg.get_block(fence_id);

    NeighborTable table;
    table.set(ShapeFace::Right, BlockIDs::STONE);
    CHECK(resolve_with(fence, table, ShapeBoxKind::Selection).count() == 3);

    table.set(ShapeFace::Right, BlockIDs::WATER);
    CHECK(resolve_with(fence, table, ShapeBoxKind::Selection).count() == 1);

    table.set(ShapeFace::Right, pane_id);
    CHECK(resolve_with(fence, table, ShapeBoxKind::Selection).count() == 1);

    table.set(ShapeFace::Right, BlockIDs::AIR);
    CHECK(resolve_with(fence, table, ShapeBoxKind::Selection).count() == 1);

    // Another wood is still a fence: the rule is one rule, not one per material.
    table.set(ShapeFace::Right, fence_id);
    CHECK(resolve_with(fence, table, ShapeBoxKind::Selection).count() == 3);
}

TEST_CASE("collision resolves per part: an isolated post is not a wall") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence_id = reg.register_block(make_fence("test_fence_collision"));
    if (fence_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& fence = reg.get_block(fence_id);

    NeighborTable none;
    const ShapeBoxes isolated = resolve_with(fence, none, ShapeBoxKind::Collision);
    CHECK(isolated.count() == 1);
    CHECK(isolated[0].max[1] == doctest::Approx(1.5f));
    CHECK(isolated[0].max[0] == doctest::Approx(0.625f));  // post width, not a run

    NeighborTable run;
    run.set(ShapeFace::Right, fence_id);
    const ShapeBoxes connected = resolve_with(fence, run, ShapeBoxKind::Collision);
    CHECK(connected.count() == 2);
    // The arm's collision reaches the cell boundary, which is what makes a run a
    // 1.5-high wall a body cannot walk through rather than a row of posts.
    bool reaches_edge = false;
    for (uint8_t i = 0; i < connected.count(); ++i) {
        if (connected[i].max[0] >= 1.0f && connected[i].max[1] >= 1.5f) reaches_edge = true;
    }
    CHECK(reaches_edge);
}

TEST_CASE("the canonical resolution is the fence run the inventory should draw") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockType fence = make_fence("test_fence_canonical");

    ShapeBoxes canonical;
    resolve_canonical_boxes(fence, ShapeBoxKind::Selection, canonical);

    // Post plus both X arms, and nothing along Z.
    CHECK(canonical.count() == 5);
    CHECK(boxes_reaching(canonical, ShapeFace::Right) == 2);
    CHECK(boxes_reaching(canonical, ShapeFace::Left) == 2);
    CHECK(boxes_reaching(canonical, ShapeFace::Front) == 0);
    CHECK(boxes_reaching(canonical, ShapeFace::Back) == 0);
    // ...and that is what the static list a worldless consumer sees holds.
    CHECK(fence.selection_boxes.size() == 5);
    CHECK(fence.collision_boxes.size() == 3);
}

TEST_CASE("a lone pane is the post alone, not a sheet") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_lone"));
    if (sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);

    NeighborTable none;
    const ShapeBoxes boxes = resolve_with(sheet, none, ShapeBoxKind::Selection);
    CHECK(boxes.count() == 1);
    CHECK(boxes[0].min[0] == doctest::Approx(0.4375f));
    CHECK(boxes[0].max[0] == doctest::Approx(0.5625f));
    CHECK(boxes[0].min[2] == doctest::Approx(0.4375f));
    CHECK(boxes[0].max[2] == doctest::Approx(0.5625f));
    CHECK(boxes[0].min[1] == doctest::Approx(0.0f));
    CHECK(boxes[0].max[1] == doctest::Approx(1.0f));
}

TEST_CASE("a pane reaches exactly the faces it can seal against") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_faces"));
    if (sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);

    NeighborTable table;
    table.set(ShapeFace::Right, BlockIDs::STONE);
    const ShapeBoxes one = resolve_with(sheet, table, ShapeBoxKind::Selection);
    CHECK(one.count() == 2);
    // Post then the single +X arm, in part order: the arm reaches the boundary it
    // points at and no other, which is what makes a run of them one sheet.
    CHECK(one[1].max[0] == doctest::Approx(1.0f));
    CHECK(one[1].min[0] == doctest::Approx(0.5625f));
    CHECK(one[1].min[2] == doctest::Approx(0.4375f));
    CHECK(one[1].max[2] == doctest::Approx(0.5625f));

    // A neighbour of its own kind reaches too, so a run and a corner are composed
    // rather than being variants of their own.
    table.set(ShapeFace::Left, sheet_id);
    CHECK(resolve_with(sheet, table, ShapeBoxKind::Selection).count() == 3);
}

TEST_CASE("a pane seals against a window where a fence refuses one") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID glazing = make_glazing(reg);
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_glazing"));
    if (glazing == BlockIDs::AIR || sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);
    BlockType fence = make_fence("test_fence_vs_glazing");

    NeighborTable through;
    through.set(ShapeFace::Right, glazing);

    // The sheet: a whole face to press against, so it reaches, and the transparence
    // that disqualifies the rail is the whole point of the block.
    CHECK(resolve_with(sheet, through, ShapeBoxKind::Selection).count() == 2);
    // The rail: a bar will not be run into a window.
    CHECK(resolve_with(fence, through, ShapeBoxKind::Selection).count() == 1);
}

TEST_CASE("a pane's claim is the face it was authored on, not the height it spans") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_claim"));
    if (sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockID post_id = reg.register_block(make_fence("test_fence_claim"));
    if (post_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);

    // An arm spans the whole cell height, so a claim read off its boxes would ask
    // about up and down as well. The block above and below being things it cannot
    // seal against must therefore not take the arm away: the neighbour it is
    // authored on is a pane, and that is the only question an arm asks.
    NeighborTable tall;
    tall.set(ShapeFace::Back, sheet_id);
    tall.set(ShapeFace::Top, post_id);
    tall.set(ShapeFace::Bottom, post_id);
    const ShapeBoxes with_caps = resolve_with(sheet, tall, ShapeBoxKind::Selection);
    CHECK(with_caps.count() == 2);
    CHECK(with_caps[1].min[2] == doctest::Approx(0.0f));

    // ...and the converse, which is the ordinary case: nothing above or below at
    // all still leaves the sheet whole.
    NeighborTable bare;
    bare.set(ShapeFace::Back, sheet_id);
    CHECK(resolve_with(sheet, bare, ShapeBoxKind::Selection).count() == 2);
}

TEST_CASE("a pane run reproduces the flat sheet it replaced") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_run"));
    if (sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);

    // This is the invariant that lets one block id stand in for a variant per axis:
    // post plus the two arms along X covers exactly the cell-crossing plate the old
    // single-plane pane was, and post plus the two arms along Z covers the other
    // one. So nothing built with the old block changed shape.
    NeighborTable across_x;
    across_x.set(ShapeFace::Right, sheet_id);
    across_x.set(ShapeFace::Left, sheet_id);
    const ShapeBoxes x_run = resolve_with(sheet, across_x, ShapeBoxKind::Selection);
    CHECK(x_run.count() == 3);
    CHECK(same_box(hull(x_run), box(0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f)));

    NeighborTable across_z;
    across_z.set(ShapeFace::Front, sheet_id);
    across_z.set(ShapeFace::Back, sheet_id);
    const ShapeBoxes z_run = resolve_with(sheet, across_z, ShapeBoxKind::Selection);
    CHECK(z_run.count() == 3);
    CHECK(same_box(hull(z_run), box(0.4375f, 0.0f, 0.0f, 0.5625f, 1.0f, 1.0f)));

    // A corner is both, and still adds up to no more than the two plates it is
    // made of: four boxes against the four an unshared sheet would need.
    NeighborTable corner = across_x;
    corner.set(ShapeFace::Front, sheet_id);
    CHECK(resolve_with(sheet, corner, ShapeBoxKind::Selection).count() == 4);
}

TEST_CASE("pane collision resolves per part: a column to walk around, a sheet to not") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID sheet_id = reg.register_block(make_sheet("test_sheet_collision"));
    if (sheet_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& sheet = reg.get_block(sheet_id);

    NeighborTable none;
    const ShapeBoxes lone = resolve_with(sheet, none, ShapeBoxKind::Collision);
    CHECK(lone.count() == 1);
    // A lone pane reaches no boundary, so a body walks straight past it.
    CHECK(lone[0].max[0] < 1.0f);
    CHECK(lone[0].max[2] < 1.0f);

    NeighborTable run;
    run.set(ShapeFace::Right, sheet_id);
    run.set(ShapeFace::Left, sheet_id);
    const ShapeBoxes spanning = resolve_with(sheet, run, ShapeBoxKind::Collision);
    CHECK(spanning.count() == 3);
    CHECK(same_box(hull(spanning), box(0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f)));

    // A pane has no collision override of its own, so the two lists are the same
    // walk of the same parts and cannot drift. Worth pinning because every other
    // part-based family so far HAS had one (the fence's raised rail, the stair's
    // step).
    CHECK(sheet.selection_boxes.size() == 3);
}

TEST_CASE("the canonical pane is the flat sheet the inventory should draw") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockType sheet = make_sheet("test_sheet_canonical");

    ShapeBoxes canonical;
    resolve_canonical_boxes(sheet, ShapeBoxKind::Selection, canonical);

    CHECK(canonical.count() == 3);
    CHECK(same_box(hull(canonical), box(0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f)));
    // ...and that is what the static list a worldless consumer sees holds, in the
    // order the loader derives it: post, then the two arms along X.
    CHECK(sheet.selection_boxes.size() == 3);
    CHECK(sheet.selection_boxes[0].min[0] == doctest::Approx(0.4375f));
    CHECK(sheet.selection_boxes[1].max[0] == doctest::Approx(1.0f));
    CHECK(sheet.selection_boxes[2].min[0] == doctest::Approx(0.0f));
}
