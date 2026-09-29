#ifndef FARLANDS_TESTS_SHAPE_RESOLVER_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_SHAPE_RESOLVER_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixture builders shared by the shape resolver tests.
//
// A shape has no world: it resolves from the block table's own record of its
// parts plus, for the shapes that read their neighbours, a hand-built table.
// So each family here is a whole little fixture — the parts, the registry entry
// and the lookup table it resolves against — along with the resolution probes
// that compare boxes the way the tests think about them (which face does a box
// reach, what is the hull, is this the box I expect).
//
// These were file-local to test_shape_resolver.cpp; the split moved them here, so
// they are `inline` in a named namespace rather than internal-linkage statics.
// -----------------------------------------------------------------------------

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"

#include <cmath>
#include <cstring>
#include <vector>

using namespace VoxelEngine;

namespace shape_resolver_test {

inline BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// The fence as data/block_shapes.json spells it: a post, and one claimed arm per
// side carrying two rails and a 1.5-high collision. Built here rather than loaded
// because a unit test has no engine to read the JSON with — the probe checks the
// real file, and this checks the machinery the file feeds.
inline std::vector<ShapePart> fence_parts() {
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

inline BlockType make_fence(const char* name) {
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

inline BlockID table_lookup(void* ctx, ShapeFace face) {
    return static_cast<NeighborTable*>(ctx)->faces[static_cast<uint8_t>(face)];
}

inline ShapeBoxes resolve_with(const BlockType& bt, NeighborTable& table, ShapeBoxKind kind) {
    ShapeBoxes boxes;
    resolve_shape_boxes(bt, BlockRegistry::get_instance(), ShapeNeighborFn{&table_lookup, &table},
                        kind, boxes);
    return boxes;
}

// The hull of a resolved set: what the boxes cover between them. Comparing a hull
// against a single box is how the pane tests below check that a run of sheets adds
// up to the one flat plate a pane used to be, rather than to something that merely
// has the right number of boxes.
inline BlockAABB hull(const ShapeBoxes& boxes) {
    BlockAABB b = boxes[0];
    for (uint8_t i = 1; i < boxes.count(); ++i) {
        for (int k = 0; k < 3; ++k) {
            if (boxes[i].min[k] < b.min[k]) b.min[k] = boxes[i].min[k];
            if (boxes[i].max[k] > b.max[k]) b.max[k] = boxes[i].max[k];
        }
    }
    return b;
}

inline bool same_box(const BlockAABB& a, const BlockAABB& b) {
    for (int k = 0; k < 3; ++k) {
        if (std::fabs(a.min[k] - b.min[k]) > 1e-4f) return false;
        if (std::fabs(a.max[k] - b.max[k]) > 1e-4f) return false;
    }
    return true;
}

// How many resolved boxes reach a given cell face. The arms are told apart by
// geometry rather than by index, so this cannot pass on a shape that arms the
// wrong way.
inline int boxes_reaching(const ShapeBoxes& boxes, ShapeFace face) {
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
inline int boxes_reaching_side(const ShapeBoxes& boxes, ShapeFace face) {
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
inline BlockAABB edge_half_box(ShapeFace face) {
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
inline BlockAABB quarter_of(ShapeFace a, ShapeFace b) {
    const BlockAABB first = edge_half_box(a);
    const BlockAABB second = edge_half_box(b);
    BlockAABB out{};
    for (int i = 0; i < 3; ++i) {
        out.min[i] = first.min[i] > second.min[i] ? first.min[i] : second.min[i];
        out.max[i] = first.max[i] < second.max[i] ? first.max[i] : second.max[i];
    }
    return out;
}

inline bool has_box(const ShapeBoxes& boxes, float x0, float y0, float z0, float x1, float y1,
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
inline BlockAABB mirror_y(const BlockAABB& b) {
    return box(b.min[0], 1.0f - b.max[1], b.min[2], b.max[0], 1.0f - b.min[1], b.max[2]);
}

// A stair as data/block_shapes.json spells one out: the slab, the step, the two
// corners that can be filled, and the two remnants the step is cut back to. The
// loader reads the claim faces off the rule rather than the boxes (the step face
// and a guard side are not where a corner box is), and so does this.
//
// `hanging` mirrors every box about the floor and says so on the block, which is the
// whole of the difference between the two ways up: the rules are the same five.
inline BlockType make_stair(const char* name, ShapeFace step_face, bool hanging = false) {
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
inline BlockType make_hanging_stair(const char* name, ShapeFace step_face) {
    return make_stair(name, step_face, /*hanging=*/true);
}

// A window: body-stopping, but you see through it, so nothing arms into it. Named
// for what it tests rather than for its material, because the pane family below is
// the one block in the game called "pane".
inline BlockID make_window(BlockRegistry& reg) {
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
inline BlockID make_ledge(BlockRegistry& reg) {
    BlockType ledge{};
    ledge.name = "test_ledge";
    ledge.properties = BlockProperty::Solid | BlockProperty::Opaque;
    ledge.selection_boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f)};
    ledge.full_cube_ = false;
    return reg.register_block(ledge);
}

inline BlockID make_glazing(BlockRegistry& reg) {
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
inline std::vector<ShapePart> sheet_parts() {
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

inline BlockType make_sheet(const char* name) {
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

// The wall as data/block_shapes.json spells it out: a post, and a reach per side it
// meets. Two hands, two rules,
// because they are two heights: the reach stops short of the cell top while the cell
// above it is open, and the other rule draws the same reach at full height while the
// cell above spans it. Spelled from the direction the way the stair parts are, and
// every hand's claim is a DECLARED single face exactly as the file declares it — a
// reach also reaches the bottom of its cell, and a wall in the air does not stop
// reaching sideways.
inline std::vector<ShapePart> wall_parts() {
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
    parts.push_back(hand(ShapeFace::Back, 1.0f, ShapeRule::WallBearing));
    parts.push_back(hand(ShapeFace::Front, 1.0f, ShapeRule::WallBearing));
    parts.push_back(hand(ShapeFace::Right, 1.0f, ShapeRule::WallBearing));
    parts.push_back(hand(ShapeFace::Left, 1.0f, ShapeRule::WallBearing));
    return parts;
}

inline BlockType make_wall(const char* name) {
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

} // namespace shape_resolver_test

#endif // FARLANDS_TESTS_SHAPE_RESOLVER_TEST_SUPPORT_HPP
