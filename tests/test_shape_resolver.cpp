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

// A stair as data/block_shapes.json spells the `stair/n` variant: the slab plus a
// step against the -Z face, and the two inner corner quarters claimed on the X
// sides with an explicit "faces" list (a corner box reaches the far cell boundary
// as well as the neighbour's, and only the neighbour's side is the claim).
BlockType make_stair(const char* name, uint8_t step_face) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.stair_step_face = step_face;

    ShapePart straight;
    straight.boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f),
                      box(0.0f, 0.5f, 0.0f, 1.0f, 1.0f, 0.5f)};

    ShapePart east;
    east.rule = ShapeRule::StairInner;
    east.faces = shape_face_bit(ShapeFace::Right);
    east.boxes = {box(0.5f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f)};

    ShapePart west;
    west.rule = ShapeRule::StairInner;
    west.faces = shape_face_bit(ShapeFace::Left);
    west.boxes = {box(0.0f, 0.5f, 0.5f, 0.5f, 1.0f, 1.0f)};

    bt.parts = {straight, east, west};

    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    bt.selection_boxes.assign(canonical.begin(), canonical.end());
    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
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

TEST_CASE("a stair draws its inner corner only against a stair climbing toward it") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    // make_stair builds the `stair/n` variant: step against -Z.
    const BlockID north   = reg.register_block(make_stair("test_stair_n", static_cast<uint8_t>(ShapeFace::Back)));
    const BlockID west    = reg.register_block(make_stair("test_stair_w", static_cast<uint8_t>(ShapeFace::Left)));
    const BlockID east    = reg.register_block(make_stair("test_stair_e", static_cast<uint8_t>(ShapeFace::Right)));
    const BlockID south   = reg.register_block(make_stair("test_stair_s", static_cast<uint8_t>(ShapeFace::Front)));
    const BlockID hanging = reg.register_block(make_stair("test_stair_n_up", kNoStairFace));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    NeighborTable table;
    // Alone: the slab and its step, no corner.
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);

    // `stair/w` to the east climbs toward us, so its step meets the face we share.
    table.set(ShapeFace::Right, west);
    const ShapeBoxes corner = resolve_with(stair, table, ShapeBoxKind::Selection);
    CHECK(corner.count() == 3);
    bool fills_east_notch = false;
    for (uint8_t i = 0; i < corner.count(); ++i) {
        const BlockAABB& b = corner[i];
        if (b.min[0] == doctest::Approx(0.5f) && b.min[2] == doctest::Approx(0.5f) &&
            b.min[1] == doctest::Approx(0.5f) && b.max[0] == doctest::Approx(1.0f)) {
            fills_east_notch = true;
        }
    }
    CHECK(fills_east_notch);
    // A corner quarter is part of the block: it is solid for collision too.
    CHECK(resolve_with(stair, table, ShapeBoxKind::Collision).count() == 3);

    // Every other orientation against that face: right shape, wrong way round.
    table.set(ShapeFace::Right, north);   // same way round as us: two steps in a row
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);
    table.set(ShapeFace::Right, east);    // climbing away
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);
    table.set(ShapeFace::Right, south);   // climbing along our step
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);
    table.set(ShapeFace::Right, hanging); // a hanging `*_up` stair has no upright face
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);
    table.set(ShapeFace::Right, BlockIDs::STONE);  // a wall is not a stair
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);
    table.set(ShapeFace::Right, BlockIDs::AIR);
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 2);

    // Both sides at once is an L with corners on both: per-side parts, not one set.
    table.set(ShapeFace::Right, west);
    table.set(ShapeFace::Left, east);
    CHECK(resolve_with(stair, table, ShapeBoxKind::Selection).count() == 4);
}

// The corner box reaches the far cell boundary as well as the neighbour's, so a
// claim derived from the geometry would read Right|Back and corner against a stair
// BEHIND this one too — a corner on a side where nothing is attached. The explicit
// "faces" list on the part is what keeps the claim to the one side it belongs to.
TEST_CASE("a stair's corner claims the neighbour's face, not every face its box reaches") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID north = reg.register_block(make_stair("test_stair_claim_n", static_cast<uint8_t>(ShapeFace::Back)));
    const BlockID south = reg.register_block(make_stair("test_stair_claim_s", static_cast<uint8_t>(ShapeFace::Front)));
    if (north == BlockIDs::AIR) {
        CHECK(false);
        return;
    }
    const BlockType& stair = reg.get_block(north);

    // A `stair/s` behind us climbs away from us, which would satisfy a Back claim if
    // the box's reach decided the claim. It must not: the east quarter stays absent.
    NeighborTable behind;
    behind.set(ShapeFace::Back, south);
    CHECK(resolve_with(stair, behind, ShapeBoxKind::Selection).count() == 2);

    // ...and the declared face still works on its own, so the claim was narrowed
    // rather than lost.
    NeighborTable side;
    side.set(ShapeFace::Right, reg.register_block(make_stair("test_stair_claim_w",
                                                             static_cast<uint8_t>(ShapeFace::Left))));
    CHECK(side.faces[static_cast<uint8_t>(ShapeFace::Right)] != BlockIDs::AIR);
    CHECK(resolve_with(stair, side, ShapeBoxKind::Selection).count() == 3);
}

TEST_CASE("the canonical stair is the plain step the inventory should draw") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const BlockType stair = make_stair("test_stair_canonical", static_cast<uint8_t>(ShapeFace::Back));

    ShapeBoxes canonical;
    resolve_canonical_boxes(stair, ShapeBoxKind::Selection, canonical);

    // Slab and step, no corner: a corner means "a neighbour is there", which a
    // worldless consumer standing in the inventory cannot know.
    CHECK(canonical.count() == 2);
    CHECK(stair.selection_boxes.size() == 2);
    CHECK(stair.selection_boxes[1].max[2] == doctest::Approx(0.5f));
}
