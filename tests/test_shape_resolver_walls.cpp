#include "doctest.h"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "shape_resolver_test_support.hpp"

using namespace VoxelEngine;
using namespace shape_resolver_test;

TEST_CASE("a wall's post is up everywhere a rail does not already read as the wall") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_post"));
    if (wall_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);
    auto post_is_up = [](const ShapeBoxes& boxes) {
        for (uint8_t i = 0; i < boxes.count(); ++i) {
            if (boxes[i].max[1] >= 1.0f - 1e-4f) return true;
        }
        return false;
    };

    // A wall on its own is a post: full height, 8/16 wide, nothing else.
    NeighborTable none;
    const ShapeBoxes alone = resolve_with(wall, none, ShapeBoxKind::Selection);
    CHECK(alone.count() == 1);
    CHECK(has_box(alone, 0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f));

    // A plain through-run is the layout with no post, along either axis: the arms
    // alone, 14/16 high, are what a run of wall reads as.
    NeighborTable run_z;
    run_z.set(ShapeFace::Back, wall_id);
    run_z.set(ShapeFace::Front, wall_id);
    const ShapeBoxes along_z = resolve_with(wall, run_z, ShapeBoxKind::Selection);
    CHECK(along_z.count() == 2);
    CHECK_FALSE(post_is_up(along_z));

    NeighborTable run_x;
    run_x.set(ShapeFace::Left, wall_id);
    run_x.set(ShapeFace::Right, wall_id);
    const ShapeBoxes along_x = resolve_with(wall, run_x, ShapeBoxKind::Selection);
    CHECK(along_x.count() == 2);
    CHECK_FALSE(post_is_up(along_x));

    // Every other layout carries one. The end of a run and a corner are the two that
    // matter in a build; a T junction and a cross are what the same rule does next.
    NeighborTable end_of_run;
    end_of_run.set(ShapeFace::Front, wall_id);
    const ShapeBoxes end = resolve_with(wall, end_of_run, ShapeBoxKind::Selection);
    CHECK(end.count() == 2);
    CHECK(post_is_up(end));

    NeighborTable corner;
    corner.set(ShapeFace::Back, wall_id);
    corner.set(ShapeFace::Right, wall_id);
    const ShapeBoxes turn = resolve_with(wall, corner, ShapeBoxKind::Selection);
    CHECK(turn.count() == 3);
    CHECK(post_is_up(turn));

    NeighborTable tee;
    tee.set(ShapeFace::Back, wall_id);
    tee.set(ShapeFace::Front, wall_id);
    tee.set(ShapeFace::Right, wall_id);
    const ShapeBoxes junction = resolve_with(wall, tee, ShapeBoxKind::Selection);
    CHECK(junction.count() == 4);
    CHECK(post_is_up(junction));

    // A cross is the other layout without a post: four arms already meet in the
    // middle, so there is no column for one to be.
    NeighborTable cross;
    cross.set(ShapeFace::Back, wall_id);
    cross.set(ShapeFace::Front, wall_id);
    cross.set(ShapeFace::Left, wall_id);
    cross.set(ShapeFace::Right, wall_id);
    const ShapeBoxes crossing = resolve_with(wall, cross, ShapeBoxKind::Selection);
    CHECK(crossing.count() == 4);
    CHECK_FALSE(post_is_up(crossing));
}

TEST_CASE("a run carries its post only while something rests on its footprint") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_post_cover"));
    const BlockID fence_id = reg.register_block(make_fence("test_wall_post_fence"));
    const BlockID ledge_id = make_ledge(reg);
    if (wall_id == BlockIDs::AIR || fence_id == BlockIDs::AIR || ledge_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);

    auto mid_run_count = [&](BlockID above) {
        NeighborTable table;
        table.set(ShapeFace::Front, wall_id);
        table.set(ShapeFace::Back, wall_id);
        table.set(ShapeFace::Top, above);
        return resolve_with(wall, table, ShapeBoxKind::Selection).count();
    };

    // A fence's post: its boxes span the post's own 2/16 column and reach the top of
    // the cell, so the run is holding it up and the post comes up to meet it.
    CHECK(mid_run_count(fence_id) == 3);

    // A whole block over a run is the other answer, and the opposite one. It spans the
    // reaches as well as the column, so both reaches run to the cell top and the run is
    // a column already: there is no post to put between them, and what is left is the
    // two reaches.
    CHECK(mid_run_count(BlockIDs::STONE) == 2);

    // A ledge is half a cell tall, a liquid is not something a wall holds up, and a
    // wall above is inert: a wall two high stays the rail one high is.
    CHECK(mid_run_count(ledge_id) == 2);
    CHECK(mid_run_count(BlockIDs::WATER) == 2);
    CHECK(mid_run_count(wall_id) == 2);

    // ...and the same holds for a run reached on both sides of its axis by whole faces
    // with a block resting over it: two full-height reaches, no post.
    NeighborTable carried;
    carried.set(ShapeFace::Front, BlockIDs::STONE);
    carried.set(ShapeFace::Back, BlockIDs::STONE);
    carried.set(ShapeFace::Top, BlockIDs::STONE);
    const ShapeBoxes solid = resolve_with(wall, carried, ShapeBoxKind::Selection);
    CHECK(solid.count() == 2);
    for (uint8_t i = 0; i < solid.count(); ++i) {
        CHECK(solid[i].max[1] == doctest::Approx(1.0f));  // reaches run to the top, no post
    }
}

TEST_CASE("a wall's post reads the layout and the cell above it, and nothing else") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_post_sweep"));
    const BlockID fence_id = reg.register_block(make_fence("test_wall_post_sweep_fence"));
    if (wall_id == BlockIDs::AIR || fence_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);
    const ShapeFace sides[4] = {ShapeFace::Back, ShapeFace::Right, ShapeFace::Front, ShapeFace::Left};

    for (int mask = 0; mask < 16; ++mask) {
        bool connected[4];
        bool any = false;
        for (int i = 0; i < 4; ++i) {
            connected[i] = (mask & (1 << i)) != 0;
            any = any || connected[i];
        }
        // A plain through-run and a cross are the two layouts without a post of
        // their own; everything else is a cell where a column is what holds the run up.
        const bool through_run = any && connected[0] == connected[2] && connected[1] == connected[3];
        for (int above = 0; above < 3; ++above) {
            for (int below = 0; below < 2; ++below) {
                NeighborTable table;
                for (int i = 0; i < 4; ++i) {
                    if (connected[i]) table.set(sides[i], wall_id);
                }
                if (above == 1) table.set(ShapeFace::Top, BlockIDs::STONE);
                if (above == 2) table.set(ShapeFace::Top, fence_id);
                if (below == 1) table.set(ShapeFace::Bottom, BlockIDs::STONE);
                const ShapeBoxes boxes = resolve_with(wall, table, ShapeBoxKind::Selection);

                // Every side that reaches carries exactly one reach, at the height the
                // cell above decides: a whole block spans the strip a reach stands on, a
                // fence's post does not.
                for (int i = 0; i < 4; ++i) {
                    CHECK(boxes_reaching_side(boxes, sides[i]) == (connected[i] ? 1 : 0));
                    if (!connected[i]) continue;
                    float top = 0.0f;
                    for (const BlockAABB& b : boxes) {
                        if ((shape_box_faces({b}) & shape_face_bit(sides[i])) == 0) continue;
                        if (b.max[1] > top) top = b.max[1];
                    }
                    CHECK(top == doctest::Approx(above == 1 ? 1.0f : 0.875f));
                }

                // The post is the layout's and the cell underneath decides nothing. On a
                // plain run the cell above does decide, and its two answers are
                // opposites: a fence's post covers the post's own footprint, so the run
                // carries one up to meet it, while a whole block spans the reaches
                // instead, so both of them run to the top and the column is already
                // there.
                const bool post_is_up = has_box(boxes, 0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f);
                if (through_run) CHECK(post_is_up == (above == 2));
                else CHECK(post_is_up);
            }
        }
    }
}

TEST_CASE("a wall arms toward another wall, whatever it is made of, and only that side") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_armed"));
    const BlockID other_id = reg.register_block(make_wall("test_wall_other_material"));
    if (wall_id == BlockIDs::AIR || other_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);
    CHECK(wall.connector == ShapeRule::WallArm);

    NeighborTable east;
    east.set(ShapeFace::Right, wall_id);
    const ShapeBoxes boxes = resolve_with(wall, east, ShapeBoxKind::Selection);
    CHECK(boxes.count() == 2);
    CHECK(boxes_reaching_side(boxes, ShapeFace::Right) == 1);
    CHECK(boxes_reaching_side(boxes, ShapeFace::Left) == 0);
    CHECK(boxes_reaching_side(boxes, ShapeFace::Front) == 0);
    CHECK(boxes_reaching_side(boxes, ShapeFace::Back) == 0);
    // A reach with open sky above it stops short of the top.
    bool arm_is_short = false;
    for (uint8_t i = 0; i < boxes.count(); ++i) {
        if (boxes[i].max[0] >= 1.0f && boxes[i].max[1] == doctest::Approx(0.875f)) arm_is_short = true;
    }
    CHECK(arm_is_short);

    // Another material is still a wall: the rule is one rule, not one per family.
    east.set(ShapeFace::Right, other_id);
    CHECK(resolve_with(wall, east, ShapeBoxKind::Selection).count() == 2);

    // Both ends of a run at once: the arms are per side, not one shared set.
    NeighborTable corner;
    corner.set(ShapeFace::Right, wall_id);
    corner.set(ShapeFace::Back, wall_id);
    const ShapeBoxes turn = resolve_with(wall, corner, ShapeBoxKind::Selection);
    CHECK(turn.count() == 3);
    CHECK(boxes_reaching_side(turn, ShapeFace::Right) == 1);
    CHECK(boxes_reaching_side(turn, ShapeFace::Back) == 1);
}

TEST_CASE("a wall reaches what it meets, at one height whatever it is") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_reach"));
    const BlockID sheet_id = reg.register_block(make_sheet("test_wall_neighbour_sheet"));
    const BlockID pane_id = make_window(reg);
    const BlockID fence_id = reg.register_block(make_fence("test_wall_neighbour_fence"));
    const BlockID glazing_id = make_glazing(reg);
    const BlockID ledge_id = make_ledge(reg);
    if (wall_id == BlockIDs::AIR || sheet_id == BlockIDs::AIR || pane_id == BlockIDs::AIR ||
        fence_id == BlockIDs::AIR || glazing_id == BlockIDs::AIR || ledge_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);

    // How tall the box on a side stands, read off the boxes that reach that side: -1
    // means nothing reaches it at all.
    const auto reach_top = [](const ShapeBoxes& boxes, ShapeFace side) {
        float top = -1.0f;
        for (uint8_t i = 0; i < boxes.count(); ++i) {
            const BlockAABB& b = boxes[i];
            const bool reaches = (side == ShapeFace::Right) ? b.max[0] >= 1.0f - 1e-4f
                                 : (side == ShapeFace::Left)  ? b.min[0] <= 1e-4f
                                 : (side == ShapeFace::Front) ? b.max[2] >= 1.0f - 1e-4f
                                                              : b.min[2] <= 1e-4f;
            if (reaches && b.max[1] > top) top = b.max[1];
        }
        return top;
    };

    // Another wall and a whole block draw the same reach at the SAME height, which is
    // the whole point of moving the height off the neighbour: a buttress against stone
    // and a run to the next wall are the same piece of wall.
    NeighborTable beside_wall;
    beside_wall.set(ShapeFace::Right, wall_id);
    const ShapeBoxes wall_side = resolve_with(wall, beside_wall, ShapeBoxKind::Selection);
    CHECK(wall_side.count() == 2);
    CHECK(reach_top(wall_side, ShapeFace::Right) == doctest::Approx(0.875f));

    NeighborTable beside_stone;
    beside_stone.set(ShapeFace::Right, BlockIDs::STONE);
    const ShapeBoxes stone_side = resolve_with(wall, beside_stone, ShapeBoxKind::Selection);
    CHECK(stone_side.count() == 2);
    CHECK(reach_top(stone_side, ShapeFace::Right) == doctest::Approx(0.875f));

    // What reaches: another wall, a sheet a wall can bite into, and anything offering a
    // whole face � glass included, because a whole face is a whole face whether or not
    // it is transparent, and a short reach through a window is not the buttress that
    // used to be drawn there.
    for (BlockID neighbor : {wall_id, sheet_id, glazing_id, BlockIDs::STONE}) {
        NeighborTable table;
        table.set(ShapeFace::Right, neighbor);
        CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 2);
    }
    // A bare window is NOT one of them, and that is the pane rule's own asymmetry read
    // from this side: what a wall reaches is the family's block, not a flat pane of
    // glass that happens to look identical.
    {
        NeighborTable table;
        table.set(ShapeFace::Right, pane_id);
        CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);
    }

    // What does not: anything a body can walk through, and anything half a cell tall.
    // There is nothing standing out at that face for a reach to meet, which is where
    // this parts company with the fence's reach into the side of a slab.
    for (BlockID neighbor : {fence_id, ledge_id, BlockIDs::WATER, BlockIDs::AIR}) {
        NeighborTable table;
        table.set(ShapeFace::Right, neighbor);
        CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);
    }

    // The cell above is what makes a reach full height. A whole block and a slab span
    // the strip the reach stands on; a fence's post misses it at the cell's own edge
    // and a sheet misses it across, so neither is something a wall is carrying.
    const auto top_under = [&](BlockID above) {
        NeighborTable table;
        table.set(ShapeFace::Right, BlockIDs::STONE);
        if (above != BlockIDs::AIR) table.set(ShapeFace::Top, above);
        return reach_top(resolve_with(wall, table, ShapeBoxKind::Selection), ShapeFace::Right);
    };
    CHECK(top_under(BlockIDs::STONE) == doctest::Approx(1.0f));
    CHECK(top_under(ledge_id) == doctest::Approx(1.0f));
    CHECK(top_under(fence_id) == doctest::Approx(0.875f));
    CHECK(top_under(pane_id) == doctest::Approx(0.875f));
    CHECK(top_under(BlockIDs::AIR) == doctest::Approx(0.875f));

    // Exactly one of the two heights per side that reaches, whichever it is: the two
    // rules are one reach split in two, so a side can never draw both or neither.
    const ShapeFace sides[4] = {ShapeFace::Back, ShapeFace::Right, ShapeFace::Front,
                                ShapeFace::Left};
    for (int mask = 0; mask < 16; ++mask) {
        NeighborTable table;
        for (int i = 0; i < 4; ++i) {
            if (mask & (1 << i)) table.set(sides[i], wall_id);
        }
        for (BlockID above : {BlockIDs::AIR, BlockIDs::STONE, fence_id}) {
            if (above != BlockIDs::AIR) table.set(ShapeFace::Top, above);
            const ShapeBoxes boxes = resolve_with(wall, table, ShapeBoxKind::Selection);
            for (int i = 0; i < 4; ++i) {
                CHECK(boxes_reaching_side(boxes, sides[i]) == ((mask & (1 << i)) ? 1 : 0));
            }
            table.set(ShapeFace::Top, BlockIDs::AIR);
        }
    }
}

TEST_CASE("a wall's arm is claimed by the side it points at, not by the foot it stands on") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_claim"));
    if (wall_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);

    // A run in the air, with nothing below and nothing above it, still arms. An arm's
    // boxes reach the bottom of the cell, so a claim read off them would demand a
    // block underneath and silently drop the arm on a wall built out over a drop.
    NeighborTable airborne;
    airborne.set(ShapeFace::Right, wall_id);
    CHECK(resolve_with(wall, airborne, ShapeBoxKind::Selection).count() == 2);

    NeighborTable grounded;
    grounded.set(ShapeFace::Bottom, BlockIDs::STONE);
    CHECK(resolve_with(wall, grounded, ShapeBoxKind::Selection).count() == 1);

    // Every hand declares the one side it points at, which is the only case the
    // loader takes on the author's word, and each one is a face its boxes reach.
    for (const ShapePart& part : wall.parts) {
        if (part.rule != ShapeRule::WallArm && part.rule != ShapeRule::WallBearing) continue;
        CHECK(part.faces_declared);
        CHECK((part.faces & ~shape_box_faces(part.boxes)) == 0);  // a face its boxes reach
        int set_bits = 0;
        for (uint8_t f = 0; f < 6; ++f) {
            if (part.faces & shape_face_bit(static_cast<ShapeFace>(f))) ++set_bits;
        }
        CHECK(set_bits == 1);
    }
}

TEST_CASE("wall collision resolves per part: a lone post is a post, a run is a barrier") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_collision"));
    if (wall_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);

    NeighborTable none;
    const ShapeBoxes isolated = resolve_with(wall, none, ShapeBoxKind::Collision);
    CHECK(isolated.count() == 1);
    CHECK(isolated[0].max[1] == doctest::Approx(1.5f));
    CHECK(isolated[0].max[0] == doctest::Approx(0.75f));  // post width, not a run

    NeighborTable run;
    run.set(ShapeFace::Right, wall_id);
    const ShapeBoxes connected = resolve_with(wall, run, ShapeBoxKind::Collision);
    CHECK(connected.count() == 2);
    bool reaches_edge = false;
    for (uint8_t i = 0; i < connected.count(); ++i) {
        if (connected[i].max[0] >= 1.0f && connected[i].max[1] >= 1.5f) reaches_edge = true;
    }
    CHECK(reaches_edge);
}

TEST_CASE("the canonical wall is the run the inventory should draw") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockType wall = make_wall("test_wall_canonical");

    ShapeBoxes canonical;
    resolve_canonical_boxes(wall, ShapeBoxKind::Selection, canonical);

    // Post plus the two short reaches along X � which is exactly a wall held in the
    // hand: a column with a run through it � and neither at full height, because a
    // worldless consumer cannot know whether anything is over the cell carrying them,
    // and no reach on the other axis at all.
    CHECK(canonical.count() == 3);
    CHECK(has_box(canonical, 0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f));
    CHECK(boxes_reaching_side(canonical, ShapeFace::Right) == 1);
    CHECK(boxes_reaching_side(canonical, ShapeFace::Left) == 1);
    CHECK(boxes_reaching_side(canonical, ShapeFace::Front) == 0);
    CHECK(boxes_reaching_side(canonical, ShapeFace::Back) == 0);
    // ...and that is what the static lists a worldless consumer sees hold.
    CHECK(wall.selection_boxes.size() == 3);
    CHECK(wall.collision_boxes.size() == 3);
}
