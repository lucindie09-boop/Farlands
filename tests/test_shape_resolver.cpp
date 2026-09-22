#include "doctest.h"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "core/chunk_data.hpp"
#include "mesh/mesh_builder.hpp"

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

// A stair as data/block_shapes.json spells one out: the slab, the step, the two
// corners that can be filled, and the two remnants the step is cut back to. The
// loader reads the claim faces off the rule rather than the boxes (the step face
// and a guard side are not where a corner box is), and so does this.
BlockType make_stair(const char* name, ShapeFace step_face) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.stair_step_face = static_cast<uint8_t>(step_face);

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

// A hanging stair, as data/block_shapes.json has it: the same box model with no
// neighbour-dependent parts at all, so nothing turns a corner with it and it turns
// none of its own.
BlockType make_hanging_stair(const char* name, ShapeFace step_face) {
    BlockType bt = make_stair(name, step_face);
    bt.parts.clear();
    bt.stair_step_face = kNoStairFace;
    bt.selection_boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f), edge_half_box(step_face)};
    return bt;
}

// A window: body-stopping, but you see through it, so nothing arms into it.
BlockID make_pane(BlockRegistry& reg) {
    BlockType pane{};
    pane.name = "test_window_pane";
    pane.properties = BlockProperty::Solid | BlockProperty::Transparent;
    pane.selection_boxes = {box(0.0f, 0.0f, 0.4375f, 1.0f, 1.0f, 0.5625f)};
    pane.full_cube_ = false;
    return reg.register_block(pane);
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
    const BlockID pane_id = make_pane(reg);
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
    const BlockID hanging = reg.register_block(make_hanging_stair("test_stair_n_up", ShapeFace::Back));
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
    const BlockID hanging = reg.register_block(make_hanging_stair("test_cut_up", ShapeFace::Back));
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

// The regression this whole rule set exists to fix: with the corner decided by a
// SIDE neighbour, two corners on opposite sides plus the whole step covered all
// four upper quadrants, so the stair resolved into a full cube. The rules are now
// the ones the geometry forces: the step is whole only while nothing cuts it, the
// squares the corner and the remnant call "this side" are one side each, and the
// corner's partner is a single cell, so at most one corner can ever appear. That
// leaves one of the four upper quadrants open in every neighbourhood, which is what
// the sweep proves — a sweep that also has to find neighbour-dependent geometry, or
// it would pass on a stair that never turns at all.
TEST_CASE("no neighbourhood can turn a stair into a full cell") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID stair_ids[4] = {
        reg.register_block(make_stair("test_sweep_n", ShapeFace::Back)),
        reg.register_block(make_stair("test_sweep_s", ShapeFace::Front)),
        reg.register_block(make_stair("test_sweep_e", ShapeFace::Right)),
        reg.register_block(make_stair("test_sweep_w", ShapeFace::Left)),
    };
    const BlockID hanging = reg.register_block(make_hanging_stair("test_sweep_up", ShapeFace::Back));
    for (uint8_t i = 0; i < 4; ++i) {
        if (stair_ids[i] == BlockIDs::AIR) {
            CHECK(false);
            return;
        }
    }
    if (hanging == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const BlockID kinds[7] = {BlockIDs::AIR, BlockIDs::STONE, stair_ids[0], stair_ids[1],
                              stair_ids[2], stair_ids[3], hanging};
    const int32_t kind_count = 7;
    int32_t combinations = 1;
    for (int i = 0; i < 6; ++i) combinations *= kind_count;

    for (uint8_t v = 0; v < 4; ++v) {
        const BlockType& stair = reg.get_block(stair_ids[v]);
        int full_cells = 0;
        int bad_box_counts = 0;
        int turned = 0;
        for (int32_t combo = 0; combo < combinations; ++combo) {
            NeighborTable table;
            int32_t rest = combo;
            for (uint8_t f = 0; f < 6; ++f) {
                table.faces[f] = kinds[rest % kind_count];
                rest /= kind_count;
            }
            const ShapeBoxes boxes = resolve_with(stair, table, ShapeBoxKind::Selection);
            // A stair is always its slab plus at most a step and one quarter.
            if (boxes.count() < 2 || boxes.count() > 4) ++bad_box_counts;
            if (boxes.count() > 2) ++turned;

            // Is any of the four upper quadrants left open? The slab is y 0..0.5, so
            // it is skipped: only boxes raised in the upper half count as covering.
            bool quadrant_open = false;
            for (int qx = 0; qx < 2 && !quadrant_open; ++qx) {
                for (int qz = 0; qz < 2 && !quadrant_open; ++qz) {
                    bool covered = false;
                    for (uint8_t i = 0; i < boxes.count(); ++i) {
                        const BlockAABB& b = boxes[i];
                        if (b.min[1] < 0.5f - 1e-4f) continue;
                        if (b.min[0] <= qx * 0.5f + 1e-4f && b.max[0] >= (qx + 1) * 0.5f - 1e-4f &&
                            b.min[2] <= qz * 0.5f + 1e-4f && b.max[2] >= (qz + 1) * 0.5f - 1e-4f) {
                            covered = true;
                            break;
                        }
                    }
                    if (!covered) quadrant_open = true;
                }
            }
            if (!quadrant_open) ++full_cells;
        }
        CHECK(full_cells == 0);
        CHECK(bad_box_counts == 0);
        // ...and the sweep has to reach the neighbour-dependent geometry at all, or
        // it proves nothing about it.
        CHECK(turned > 0);
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

