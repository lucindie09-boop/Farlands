#include "doctest.h"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "core/chunk_data.hpp"
#include "mesh/mesh_builder.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace VoxelEngine;

namespace {

BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// The fence as data/block_shapes.json spells it: a post, and one claimed arm per
// side carrying two rails and a 1.5-high collision. Built here rather than loaded
// because a unit test has no engine to read the JSON with — the probe checks the
// real file, and this checks the machinery the file feeds.
std::vector<ShapePart> fence_parts() {
    const auto part = [](std::vector<BlockAABB> boxes, std::vector<BlockAABB> collision,
                         ShapeRule rule) {
        ShapePart p;
        p.boxes = std::move(boxes);
        p.collision_boxes = std::move(collision);
        p.rule = rule;
        p.faces = shape_box_faces(p.boxes);
        return p;
    };

    std::vector<ShapePart> parts;
    parts.push_back(part({box(0.375f, 0.0f, 0.375f, 0.625f, 1.0f, 0.625f)},
                         {box(0.375f, 0.0f, 0.375f, 0.625f, 1.5f, 0.625f)}, ShapeRule::None));
    parts.push_back(part({box(0.4375f, 0.75f, 0.0f, 0.5625f, 0.9375f, 0.5625f),
                          box(0.4375f, 0.375f, 0.0f, 0.5625f, 0.5625f, 0.5625f)},
                         {box(0.4375f, 0.0f, 0.0f, 0.5625f, 1.5f, 0.625f)}, ShapeRule::Fence));
    parts.push_back(part({box(0.4375f, 0.75f, 0.4375f, 0.5625f, 0.9375f, 1.0f),
                          box(0.4375f, 0.375f, 0.4375f, 0.5625f, 0.5625f, 1.0f)},
                         {box(0.4375f, 0.0f, 0.375f, 0.5625f, 1.5f, 1.0f)}, ShapeRule::Fence));
    parts.push_back(part({box(0.4375f, 0.75f, 0.4375f, 1.0f, 0.9375f, 0.5625f),
                          box(0.4375f, 0.375f, 0.4375f, 1.0f, 0.5625f, 0.5625f)},
                         {box(0.4375f, 0.0f, 0.375f, 1.0f, 1.5f, 0.5625f)}, ShapeRule::Fence));
    parts.push_back(part({box(0.0f, 0.75f, 0.4375f, 0.5625f, 0.9375f, 0.5625f),
                          box(0.0f, 0.375f, 0.4375f, 0.5625f, 0.5625f, 0.5625f)},
                         {box(0.0f, 0.0f, 0.375f, 0.5625f, 1.5f, 0.5625f)}, ShapeRule::Fence));
    return parts;
}

BlockType make_fence(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.parts = fence_parts();
    bt.connector = ShapeRule::Fence;

    // The same derivation the loader does: the static lists are the canonical
    // flattening of the parts, so a consumer without a world still draws a fence.
    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    bt.selection_boxes.assign(canonical.begin(), canonical.end());
    ShapeBoxes canonical_collision;
    resolve_canonical_boxes(bt, ShapeBoxKind::Collision, canonical_collision);
    bt.collision_boxes.assign(canonical_collision.begin(), canonical_collision.end());

    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
    return bt;
}

// Six neighbours, indexed by ShapeFace.
struct NeighborTable {
    BlockID faces[6] = {BlockIDs::AIR, BlockIDs::AIR, BlockIDs::AIR,
                        BlockIDs::AIR, BlockIDs::AIR, BlockIDs::AIR};

    void set(ShapeFace face, BlockID id) { faces[static_cast<uint8_t>(face)] = id; }
};

BlockID table_lookup(void* ctx, ShapeFace face) {
    return static_cast<NeighborTable*>(ctx)->faces[static_cast<uint8_t>(face)];
}

ShapeBoxes resolve_with(const BlockType& bt, NeighborTable& table, ShapeBoxKind kind) {
    ShapeBoxes boxes;
    resolve_shape_boxes(bt, BlockRegistry::get_instance(), ShapeNeighborFn{&table_lookup, &table},
                        kind, boxes);
    return boxes;
}

// The hull of a resolved set: what the boxes cover between them. Comparing a hull
// against a single box is how the pane tests below check that a run of sheets adds
// up to the one flat plate a pane used to be, rather than to something that merely
// has the right number of boxes.
BlockAABB hull(const ShapeBoxes& boxes) {
    BlockAABB b = boxes[0];
    for (uint8_t i = 1; i < boxes.count(); ++i) {
        for (int k = 0; k < 3; ++k) {
            if (boxes[i].min[k] < b.min[k]) b.min[k] = boxes[i].min[k];
            if (boxes[i].max[k] > b.max[k]) b.max[k] = boxes[i].max[k];
        }
    }
    return b;
}

bool same_box(const BlockAABB& a, const BlockAABB& b) {
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(a.min[k] - b.min[k]) > 1e-4f) return false;
        if (std::fabs(a.max[k] - b.max[k]) > 1e-4f) return false;
    }
    return true;
}

// How many resolved boxes reach a given cell face. The arms are told apart by
// geometry rather than by index, so this cannot pass on a shape that arms the
// wrong way.
int boxes_reaching(const ShapeBoxes& boxes, ShapeFace face) {
    int count = 0;
    for (const BlockAABB& b : boxes) {
        const uint8_t mask = shape_box_faces({b});
        if ((mask & shape_face_bit(face)) != 0 &&
            (mask & shape_face_bit(ShapeFace::Bottom)) == 0) {
            ++count;
        }
    }
    return count;
}

// How many boxes reach a side of the cell, at whatever height they sit. The helper
// above skips anything resting on the floor, because a fence's rails are raised and
// its post is not a rail of its own; a wall's arm stands on the floor and still
// reaches sideways, so it needs the plain count.
int boxes_reaching_side(const ShapeBoxes& boxes, ShapeFace face) {
    int count = 0;
    for (const BlockAABB& b : boxes) {
        if ((shape_box_faces({b}) & shape_face_bit(face)) != 0) ++count;
    }
    return count;
}

// A half-cell box: `face` names the side it hugs. A stair's raised half is half the
// cell high, so building the parts from the step direction this way means the test
// spells the same geometry the shape file does, variant by variant, instead of a
// second hand-written copy of sixteen numbers.
BlockAABB edge_half_box(ShapeFace face) {
    switch (face) {
        case ShapeFace::Back:  return box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f);
        case ShapeFace::Front: return box(0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f);
        case ShapeFace::Left:  return box(0.0f, 0.5f, 0.0f, 0.5f, 1.0f, 1.0f);
        case ShapeFace::Right: return box(0.5f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f);
        default:               break;
    }
    return box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 1.0f);
}

// The quarter where two half-cell boxes cross, which is what every
// neighbour-dependent stair part is: the step's own half crossed with a side half.
BlockAABB quarter_of(ShapeFace a, ShapeFace b) {
    const BlockAABB first = edge_half_box(a);
    const BlockAABB second = edge_half_box(b);
    BlockAABB out{};
    for (int i = 0; i < 3; ++i) {
        out.min[i] = first.min[i] > second.min[i] ? first.min[i] : second.min[i];
        out.max[i] = first.max[i] < second.max[i] ? first.max[i] : second.max[i];
    }
    return out;
}

bool has_box(const ShapeBoxes& boxes, float x0, float y0, float z0, float x1, float y1,
             float z1) {
    for (uint8_t i = 0; i < boxes.count(); ++i) {
        const BlockAABB& b = boxes[i];
        if (b.min[0] == doctest::Approx(x0) && b.min[1] == doctest::Approx(y0) &&
            b.min[2] == doctest::Approx(z0) && b.max[0] == doctest::Approx(x1) &&
            b.max[1] == doctest::Approx(y1) && b.max[2] == doctest::Approx(z1)) {
            return true;
        }
    }
    return false;
}

// The same box flipped about the cell floor, which is the whole difference between a
// stair that climbs and one that hangs: the reference draws the hanging variants from
// the same models mirrored, and the shape file carries them that way.
BlockAABB mirror_y(const BlockAABB& b) {
    return box(b.min[0], 1.0f - b.max[1], b.min[2], b.max[0], 1.0f - b.min[1], b.max[2]);
}

// A stair as data/block_shapes.json spells one out: the slab, the step, the two
// corners that can be filled, and the two remnants the step is cut back to. The
// loader reads the claim faces off the rule rather than the boxes (the step face
// and a guard side are not where a corner box is), and so does this.
//
// `hanging` mirrors every box about the floor and says so on the block, which is the
// whole of the difference between the two ways up: the rules are the same five.
BlockType make_stair(const char* name, ShapeFace step_face, bool hanging = false) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.stair_step_face = static_cast<uint8_t>(step_face);
    bt.stair_hanging = hanging;

    const ShapeFace behind = shape_opposite_face(step_face);
    const ShapeFace left = shape_step_left_of(step_face);
    const ShapeFace right = shape_step_right_of(step_face);

    ShapePart slab;
    slab.boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f)};

    ShapePart step;
    step.rule = ShapeRule::StairStep;
    step.boxes = {edge_half_box(step_face)};

    ShapePart corner_left;
    corner_left.rule = ShapeRule::StairCornerLeft;
    corner_left.boxes = {quarter_of(behind, left)};

    ShapePart corner_right;
    corner_right.rule = ShapeRule::StairCornerRight;
    corner_right.boxes = {quarter_of(behind, right)};

    ShapePart cut_left;
    cut_left.rule = ShapeRule::StairCutLeft;
    cut_left.boxes = {quarter_of(step_face, left)};

    ShapePart cut_right;
    cut_right.rule = ShapeRule::StairCutRight;
    cut_right.boxes = {quarter_of(step_face, right)};

    bt.parts = {slab, step, corner_left, corner_right, cut_left, cut_right};

    if (hanging) {
        for (ShapePart& part : bt.parts) {
            for (BlockAABB& b : part.boxes) b = mirror_y(b);
        }
    }

    // The same two steps the loader takes: claims come off the boxes unless the rule
    // supplies them, and the static lists are the canonical flattening of the parts.
    for (ShapePart& part : bt.parts) {
        part.faces = shape_box_faces(part.boxes);
        const uint8_t from_rule = shape_rule_faces_for(part.rule, bt);
        if (from_rule != 0) part.faces = from_rule;
    }

    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    bt.selection_boxes.assign(canonical.begin(), canonical.end());
    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
    return bt;
}

// A hanging stair, as data/block_shapes.json has it: the same five rules on the same
// box model mirrored about the cell floor, so the slab is the raised half and the step
// hangs. It carries a real step face and a real up-ness, which is what lets it turn
// with another hanging stair while a stair of the other kind — in either role — is
// inert to it.
BlockType make_hanging_stair(const char* name, ShapeFace step_face) {
    return make_stair(name, step_face, /*hanging=*/true);
}

// A window: body-stopping, but you see through it, so nothing arms into it. Named
// for what it tests rather than for its material, because the pane family below is
// the one block in the game called "pane".
BlockID make_window(BlockRegistry& reg) {
    BlockType pane{};
    pane.name = "test_window";
    pane.properties = BlockProperty::Solid | BlockProperty::Transparent;
    pane.selection_boxes = {box(0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f)};
    pane.full_cube_ = false;
    return reg.register_block(pane);
}

// A full cube you can see through: what the fence rule turns away and what the pane
// rule is for. No boxes set, so it is a full cube exactly like real glass.
// Half a cell tall across the whole footprint: something a wall can hold up without
// it resting on the post's own 2/16 column, which is the distinction the post's
// coverage test is about.
BlockID make_ledge(BlockRegistry& reg) {
    BlockType ledge{};
    ledge.name = "test_ledge";
    ledge.properties = BlockProperty::Solid | BlockProperty::Opaque;
    ledge.selection_boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f)};
    ledge.full_cube_ = false;
    return reg.register_block(ledge);
}

BlockID make_glazing(BlockRegistry& reg) {
    BlockType glass{};
    glass.name = "test_glazing";
    glass.properties = BlockProperty::Solid | BlockProperty::Transparent;
    return reg.register_block(glass);
}

// The pane as data/block_shapes.json spells it: a centre post, and one arm per
// direction it can seal against. Built the way make_fence is, with one difference
// that matters: each arm's claim is DECLARED as the single face it points at rather
// than read off its boxes. An arm spans the whole cell height, so the geometry on
// its own would claim up and down too, and a sheet is not something that needs a
// block above and below it to exist.
std::vector<ShapePart> sheet_parts() {
    const auto arm = [](const BlockAABB& b, ShapeFace face) {
        ShapePart p;
        p.boxes = {b};
        p.rule = ShapeRule::Pane;
        p.faces = shape_face_bit(face);
        p.faces_declared = true;
        return p;
    };

    std::vector<ShapePart> parts;
    ShapePart post;
    post.boxes = {box(0.4375f, 0.0f, 0.4375f, 0.5625f, 1.0f, 0.5625f)};
    post.faces = shape_box_faces(post.boxes);
    parts.push_back(std::move(post));
    parts.push_back(arm(box(0.4375f, 0.0f, 0.0f, 0.5625f, 1.0f, 0.4375f), ShapeFace::Back));
    parts.push_back(arm(box(0.4375f, 0.0f, 0.5625f, 0.5625f, 1.0f, 1.0f), ShapeFace::Front));
    parts.push_back(arm(box(0.5625f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f), ShapeFace::Right));
    parts.push_back(arm(box(0.0f, 0.0f, 0.4375f, 0.4375f, 1.0f, 0.5625f), ShapeFace::Left));
    return parts;
}

BlockType make_sheet(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties =
        BlockProperty::Solid | BlockProperty::Transparent | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.parts = sheet_parts();
    bt.connector = ShapeRule::Pane;

    // The same derivation the loader does, and note the collision list is left
    // empty on purpose, exactly as the JSON leaves it: a pane's collision IS its
    // sheets, so the two lists cannot drift apart in the first place.
    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    bt.selection_boxes.assign(canonical.begin(), canonical.end());

    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
    return bt;
}

// The wall as data/block_shapes.json spells it out: a post, a cap the post grows
// while something stands on it, an arm per side that reaches another wall, and a
// brace per side for a neighbour offering a whole face. Two hands, two rules,
// because they are two heights: an arm bridging to another thin wall stops short of
// the top, while a brace carries the column out to the face it leans on. Spelled
// from the direction the way the stair parts are, and every hand's claim is a
// DECLARED single face exactly as the file declares it — an arm also reaches the
// bottom of its cell, and a wall in the air does not stop reaching sideways.
std::vector<ShapePart> wall_parts() {
    const auto footprint = [](ShapeFace face) -> BlockAABB {
        switch (face) {
            case ShapeFace::Back:  return box(0.3125f, 0.0f, 0.0f, 0.6875f, 1.0f, 0.5f);
            case ShapeFace::Front: return box(0.3125f, 0.0f, 0.5f, 0.6875f, 1.0f, 1.0f);
            case ShapeFace::Left:  return box(0.0f, 0.0f, 0.3125f, 0.5f, 1.0f, 0.6875f);
            default:               return box(0.5f, 0.0f, 0.3125f, 1.0f, 1.0f, 0.6875f);
        }
    };

    const auto hand = [&](ShapeFace face, float top, ShapeRule rule) {
        ShapePart p;
        BlockAABB b = footprint(face);
        b.max[1] = top;
        p.boxes = {b};
        BlockAABB c = b;
        c.max[1] = 1.5f;  // every wall part is 1.5 high to walk into, however tall it draws
        p.collision_boxes = {c};
        p.rule = rule;
        p.faces = shape_face_bit(face);
        p.faces_declared = true;
        return p;
    };

    std::vector<ShapePart> parts;
    // The post is 8/16 wide and full height, and its box meets no cell boundary at
    // all — which is exactly why it is the one part in the file whose rule claims no
    // face and answers for the whole cell instead.
    ShapePart post;
    post.rule = ShapeRule::WallPost;
    post.boxes = {box(0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f)};
    post.collision_boxes = {box(0.25f, 0.0f, 0.25f, 0.75f, 1.5f, 0.75f)};
    post.faces = shape_box_faces(post.boxes);
    parts.push_back(std::move(post));

    parts.push_back(hand(ShapeFace::Back, 0.875f, ShapeRule::WallArm));
    parts.push_back(hand(ShapeFace::Front, 0.875f, ShapeRule::WallArm));
    parts.push_back(hand(ShapeFace::Right, 0.875f, ShapeRule::WallArm));
    parts.push_back(hand(ShapeFace::Left, 0.875f, ShapeRule::WallArm));
    parts.push_back(hand(ShapeFace::Back, 1.0f, ShapeRule::WallBrace));
    parts.push_back(hand(ShapeFace::Front, 1.0f, ShapeRule::WallBrace));
    parts.push_back(hand(ShapeFace::Right, 1.0f, ShapeRule::WallBrace));
    parts.push_back(hand(ShapeFace::Left, 1.0f, ShapeRule::WallBrace));
    return parts;
}

BlockType make_wall(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.parts = wall_parts();
    // The loader takes this from the first connector rule it meets in the parts,
    // which is what the other wall's arm rule asks about.
    bt.connector = ShapeRule::WallArm;

    // The same two steps the loader takes: rules that answer with faces of their own
    // override the declared ones, and the static lists are the canonical flattening.
    for (ShapePart& part : bt.parts) {
        const uint8_t from_rule = shape_rule_faces_for(part.rule, bt);
        if (from_rule != 0) part.faces = from_rule;
    }

    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    bt.selection_boxes.assign(canonical.begin(), canonical.end());
    ShapeBoxes canonical_collision;
    resolve_canonical_boxes(bt, ShapeBoxKind::Collision, canonical_collision);
    bt.collision_boxes.assign(canonical_collision.begin(), canonical_collision.end());

    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
    return bt;
}

} // namespace

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

    // A plank, a window, a fence's post: anything whose boxes span the post's own
    // 2/16 column and reach the top of the cell is being carried by it.
    CHECK(mid_run_count(BlockIDs::STONE) == 3);
    CHECK(mid_run_count(fence_id) == 3);

    // A ledge is half a cell tall, a liquid is not something a wall holds up, and a
    // wall above is inert: a wall two high stays the rail one high is.
    CHECK(mid_run_count(ledge_id) == 2);
    CHECK(mid_run_count(BlockIDs::WATER) == 2);
    CHECK(mid_run_count(wall_id) == 2);

    // ...and a run whose arms are full height on both sides of an axis is a column
    // already, so it takes no post however much is piled on it.
    NeighborTable braced;
    braced.set(ShapeFace::Front, BlockIDs::STONE);
    braced.set(ShapeFace::Back, BlockIDs::STONE);
    braced.set(ShapeFace::Top, BlockIDs::STONE);
    const ShapeBoxes solid = resolve_with(wall, braced, ShapeBoxKind::Selection);
    CHECK(solid.count() == 2);
    for (uint8_t i = 0; i < solid.count(); ++i) {
        CHECK(solid[i].max[1] == doctest::Approx(1.0f));  // two braces, no post
    }
}

TEST_CASE("a wall's post reads the layout and the cell above it, and nothing else") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_post_sweep"));
    if (wall_id == BlockIDs::AIR) {
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
        for (int above = 0; above < 2; ++above) {
            for (int below = 0; below < 2; ++below) {
                NeighborTable table;
                for (int i = 0; i < 4; ++i) {
                    if (connected[i]) table.set(sides[i], wall_id);
                }
                if (above == 1) table.set(ShapeFace::Top, BlockIDs::STONE);
                if (below == 1) table.set(ShapeFace::Bottom, BlockIDs::STONE);
                const ShapeBoxes boxes = resolve_with(wall, table, ShapeBoxKind::Selection);

                bool post_is_up = false;
                uint8_t arms = 0;
                for (uint8_t i = 0; i < boxes.count(); ++i) {
                    if (boxes[i].max[1] >= 1.0f - 1e-4f) post_is_up = true;
                    else ++arms;
                }
                // Whatever is above only ever adds the post to a plain run; the
                // layout decides the rest, and the cell underneath decides nothing.
                CHECK(post_is_up == (!through_run || above == 1));
                uint8_t expect_arms = static_cast<uint8_t>(connected[0] + connected[1] +
                                                          connected[2] + connected[3]);
                CHECK(arms == expect_arms);
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
    // A bridging arm stops short of the top, which is what tells it apart from a brace.
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

TEST_CASE("a wall braces against a whole face but not against anything thinner or a window") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall_id = reg.register_block(make_wall("test_wall_braced"));
    const BlockID pane_id = make_window(reg);
    const BlockID fence_id = reg.register_block(make_fence("test_wall_neighbour_fence"));
    const BlockID glazing_id = make_glazing(reg);
    if (wall_id == BlockIDs::AIR || pane_id == BlockIDs::AIR || fence_id == BlockIDs::AIR ||
        glazing_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& wall = reg.get_block(wall_id);

    NeighborTable table;
    table.set(ShapeFace::Right, BlockIDs::STONE);
    const ShapeBoxes stone = resolve_with(wall, table, ShapeBoxKind::Selection);
    CHECK(stone.count() == 2);
    bool brace_is_full = false;
    for (uint8_t i = 0; i < stone.count(); ++i) {
        if (stone[i].max[0] >= 1.0f && stone[i].max[1] == doctest::Approx(1.0f)) brace_is_full = true;
    }
    CHECK(brace_is_full);

    // A rail is 14/16 high and a window is not a whole face: there is nothing out at
    // that face for a buttress to lean on, so the post stands alone.
    table.set(ShapeFace::Right, fence_id);
    CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);
    table.set(ShapeFace::Right, pane_id);
    CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);

    // ...and glass is a whole face, but bracing against it would draw solid-looking
    // stone through a window, so a wall refuses what a pane happily seals against.
    table.set(ShapeFace::Right, glazing_id);
    CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);

    table.set(ShapeFace::Right, BlockIDs::WATER);
    CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);
    table.set(ShapeFace::Right, BlockIDs::AIR);
    CHECK(resolve_with(wall, table, ShapeBoxKind::Selection).count() == 1);
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
        if (part.rule != ShapeRule::WallArm && part.rule != ShapeRule::WallBrace) continue;
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

    // Post plus the two X arms — which is exactly a wall held in the hand: a column
    // with a run through it — and no braces: a brace means "something solid is beside
    // me", which a worldless consumer cannot know.
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

TEST_CASE("a shape with more boxes than the resolver can carry reports it") {
    BlockRegistry::get_instance().initialize_default_blocks();
    BlockType bt{};
    bt.name = "test_overflow";
    bt.properties = BlockProperty::NoOcclusion;
    bt.full_cube_ = false;

    ShapePart part;
    part.boxes.reserve(ShapeBoxes::kCapacity + 1);
    for (int i = 0; i <= ShapeBoxes::kCapacity; ++i) {
        part.boxes.push_back(box(0.0f, 0.0f, 0.0f, 1.0f, 0.0625f * (i % 16 + 1), 1.0f));
    }
    part.faces = shape_box_faces(part.boxes);
    bt.parts.push_back(std::move(part));

    NeighborTable none;
    const ShapeBoxes boxes = resolve_with(bt, none, ShapeBoxKind::Selection);
    CHECK(boxes.overflowed);
    CHECK(boxes.count() == ShapeBoxes::kCapacity);
}

// The property that replaces a neighbour-update pass: the geometry of a cell is a
// pure function of the cell and its six neighbours, so a chunk meshed while the
// neighbour was already there and one remeshed after the neighbour arrived have to
// be identical. The variant-per-state design this engine deliberately does not have
// would fail this the moment an edit landed beside an existing block, unless an
// update pass ran — which is exactly the machinery that does not exist here.
TEST_CASE("a chunk meshed after the neighbour arrives matches one meshed with it already there") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence_id = reg.register_block(make_fence("test_fence_mesh"));
    if (fence_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const int32_t y = 15;
    const int32_t z = 15;

    ChunkData incremental;
    incremental.fill_blocks(BlockIDs::AIR);
    incremental.set_block(15, y, z, fence_id);
    incremental.compute_section_flags();

    MeshBuilder first;
    first.set_greedy_enabled(false);
    first.build_mesh(incremental);
    const size_t lonely_vertices = first.get_vertices().size();

    // The neighbour arrives and the chunk is remeshed, exactly as an edit does.
    incremental.set_block(16, y, z, fence_id);
    incremental.compute_section_flags();
    MeshBuilder after_edit;
    after_edit.set_greedy_enabled(false);
    after_edit.build_mesh(incremental);

    // The same two cells, from a chunk that never saw them apart.
    ChunkData at_load;
    at_load.fill_blocks(BlockIDs::AIR);
    at_load.set_block(15, y, z, fence_id);
    at_load.set_block(16, y, z, fence_id);
    at_load.compute_section_flags();
    MeshBuilder loaded;
    loaded.set_greedy_enabled(false);
    loaded.build_mesh(at_load);

    CHECK(lonely_vertices > 0);
    // The arm is geometry: the pair draws more than the post alone.
    CHECK(after_edit.get_vertices().size() > lonely_vertices);

    CHECK(after_edit.get_vertices().size() == loaded.get_vertices().size());
    CHECK(after_edit.get_indices().size() == loaded.get_indices().size());
    if (after_edit.get_vertices().size() == loaded.get_vertices().size() &&
        after_edit.get_indices().size() == loaded.get_indices().size()) {
        CHECK(std::memcmp(after_edit.get_vertices().data(), loaded.get_vertices().data(),
                          after_edit.get_vertices().size() * sizeof(Vertex)) == 0);
        CHECK(std::memcmp(after_edit.get_indices().data(), loaded.get_indices().data(),
                          after_edit.get_indices().size() * sizeof(uint32_t)) == 0);
    }

    // And the reverse direction is the same world too: removing the neighbour takes
    // the arm away again rather than leaving it behind.
    incremental.set_block(16, y, z, BlockIDs::AIR);
    incremental.compute_section_flags();
    MeshBuilder after_removal;
    after_removal.set_greedy_enabled(false);
    after_removal.build_mesh(incremental);
    CHECK(after_removal.get_vertices().size() == lonely_vertices);
}

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
    // the step face itself, and the side the quarter sits on — the side, not the one
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
    // quarter fills the right half of the region behind the step — the mirrored
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
// in the lower half and its slab — which covers all four quadrants of its own half by
// itself — is in the upper one. So the slab is skipped in both cases and only boxes
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
// which is what the sweep proves — a sweep that also has to find neighbour-dependent
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

// Same property as the fence pair above, for the shapes whose claims are read on
// faces their own boxes do not reach: load-time, paste-time and post-edit meshes of
// the same world agree to the byte.
TEST_CASE("a chunk meshed after a stair is cut beside it matches one meshed with it there") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    // The stair under test steps toward +X, so the cell to its east stands in front
    // of the step: a stair turned across it there cuts the step back.
    const BlockID stepper = reg.register_block(make_stair("test_stair_mesh_e", ShapeFace::Right));
    const BlockID crossing = reg.register_block(make_stair("test_stair_mesh_n", ShapeFace::Back));
    if (stepper == BlockIDs::AIR || crossing == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const int32_t y = 15;
    const int32_t z = 15;

    ChunkData incremental;
    incremental.fill_blocks(BlockIDs::AIR);
    incremental.set_block(15, y, z, stepper);
    incremental.compute_section_flags();
    MeshBuilder whole;
    whole.set_greedy_enabled(false);
    whole.build_mesh(incremental);
    const std::vector<Vertex> whole_vertices = whole.get_vertices();

    incremental.set_block(16, y, z, crossing);
    incremental.compute_section_flags();
    MeshBuilder after_edit;
    after_edit.set_greedy_enabled(false);
    after_edit.build_mesh(incremental);
    const std::vector<Vertex>& cut_vertices = after_edit.get_vertices();
    CHECK(!whole_vertices.empty());
    // The cut is geometry: the step lost its other half, whatever the vertex count
    // does about face culling.
    const bool changed = cut_vertices.size() != whole_vertices.size() ||
                         (cut_vertices.size() == whole_vertices.size() &&
                          std::memcmp(cut_vertices.data(), whole_vertices.data(),
                                      cut_vertices.size() * sizeof(Vertex)) != 0);
    CHECK(changed);

    ChunkData at_load;
    at_load.fill_blocks(BlockIDs::AIR);
    at_load.set_block(15, y, z, stepper);
    at_load.set_block(16, y, z, crossing);
    at_load.compute_section_flags();
    MeshBuilder loaded;
    loaded.set_greedy_enabled(false);
    loaded.build_mesh(at_load);

    CHECK(after_edit.get_vertices().size() == loaded.get_vertices().size());
    CHECK(after_edit.get_indices().size() == loaded.get_indices().size());
    if (after_edit.get_vertices().size() == loaded.get_vertices().size() &&
        after_edit.get_indices().size() == loaded.get_indices().size()) {
        CHECK(std::memcmp(after_edit.get_vertices().data(), loaded.get_vertices().data(),
                          after_edit.get_vertices().size() * sizeof(Vertex)) == 0);
        CHECK(std::memcmp(after_edit.get_indices().data(), loaded.get_indices().data(),
                          after_edit.get_indices().size() * sizeof(uint32_t)) == 0);
    }

    // And taking the neighbour away puts the whole step back.
    incremental.set_block(16, y, z, BlockIDs::AIR);
    incremental.compute_section_flags();
    MeshBuilder after_removal;
    after_removal.set_greedy_enabled(false);
    after_removal.build_mesh(incremental);
    CHECK(after_removal.get_vertices().size() == whole_vertices.size());
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

