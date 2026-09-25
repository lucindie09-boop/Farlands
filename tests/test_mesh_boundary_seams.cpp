// The face two neighbouring cells share
// -----------------------------------------------------------------------------
// A connector shape's boxes are resolved per cell, so the static list beside a
// block is not what it draws: a wall's held model is a post and the two arms along
// X, while in the world it draws an arm on every side it meets. Both cases below
// are that gap. If the mesher culls against the static list, neither cell sees the
// other's arm, both draw the face between them, and the two coincident quads fight
// for the depth buffer all the way to the pixel — at every junction in the world,
// which is exactly where a player looks. So a shape resolved per cell is asked
// what it is drawing before the face is emitted, and the cell on the negative side
// of the boundary keeps the whole of its own face while the other gives up the
// part the neighbour covers: the plane is then drawn exactly once, and never
// twice, without the two meshes having to agree on anything but that rule.
//
// The shapes are built here rather than read from data/block_shapes.json because a
// unit test has no engine to load the JSON with: the probe checks the real files
// and this checks the machinery they feed.
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/shape_resolver.hpp"
#include "mesh/mesh_builder.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace VoxelEngine;

namespace {

BlockAABB box(float x0, float y0, float z0, float x1, float y1, float z1) {
    BlockAABB b{};
    b.min[0] = x0; b.min[1] = y0; b.min[2] = z0;
    b.max[0] = x1; b.max[1] = y1; b.max[2] = z1;
    return b;
}

// The wall as data/block_shapes.json spells it: a post, and a reach per side it
// meets, drawn short while the cell above is open and full height while the cell
// above spans the reach. Only the short ones matter here.
std::vector<ShapePart> wall_parts() {
    const auto footprint = [](ShapeFace face) -> BlockAABB {
        switch (face) {
            case ShapeFace::Back:  return box(0.3125f, 0.0f, 0.0f, 0.6875f, 1.0f, 0.5f);
            case ShapeFace::Front: return box(0.3125f, 0.0f, 0.5f, 0.6875f, 1.0f, 1.0f);
            case ShapeFace::Left:  return box(0.0f, 0.0f, 0.3125f, 0.5f, 1.0f, 0.6875f);
            default:               return box(0.5f, 0.0f, 0.3125f, 1.0f, 1.0f, 0.6875f);
        }
    };
    const auto hand = [&](ShapeFace face, float top) {
        ShapePart p;
        BlockAABB b = footprint(face);
        b.max[1] = top;
        p.boxes = {b};
        p.rule = ShapeRule::WallArm;
        p.faces = shape_face_bit(face);
        p.faces_declared = true;
        return p;
    };

    std::vector<ShapePart> parts;
    ShapePart post;
    post.rule = ShapeRule::WallPost;
    post.boxes = {box(0.25f, 0.0f, 0.25f, 0.75f, 1.0f, 0.75f)};
    post.faces = shape_box_faces(post.boxes);
    parts.push_back(std::move(post));
    parts.push_back(hand(ShapeFace::Back, 0.875f));
    parts.push_back(hand(ShapeFace::Front, 0.875f));
    parts.push_back(hand(ShapeFace::Right, 0.875f));
    parts.push_back(hand(ShapeFace::Left, 0.875f));
    return parts;
}

// The fence as data/block_shapes.json spells it: a post, and one claimed arm per
// side carrying two rails — thin enough that what it draws over a boundary is a
// pair of narrow bands rather than the whole face.
std::vector<ShapePart> fence_parts() {
    const auto part = [](std::vector<BlockAABB> boxes, ShapeRule rule) {
        ShapePart p;
        p.boxes = std::move(boxes);
        p.rule = rule;
        p.faces = shape_box_faces(p.boxes);
        return p;
    };
    std::vector<ShapePart> parts;
    parts.push_back(part({box(0.375f, 0.0f, 0.375f, 0.625f, 1.0f, 0.625f)}, ShapeRule::None));
    parts.push_back(part({box(0.4375f, 0.75f, 0.0f, 0.5625f, 0.9375f, 0.5625f),
                          box(0.4375f, 0.375f, 0.0f, 0.5625f, 0.5625f, 0.5625f)},
                         ShapeRule::Fence));
    parts.push_back(part({box(0.4375f, 0.75f, 0.4375f, 0.5625f, 0.9375f, 1.0f),
                          box(0.4375f, 0.375f, 0.4375f, 0.5625f, 0.5625f, 1.0f)},
                         ShapeRule::Fence));
    parts.push_back(part({box(0.4375f, 0.75f, 0.4375f, 1.0f, 0.9375f, 0.5625f),
                          box(0.4375f, 0.375f, 0.4375f, 1.0f, 0.5625f, 0.5625f)},
                         ShapeRule::Fence));
    parts.push_back(part({box(0.0f, 0.75f, 0.4375f, 0.5625f, 0.9375f, 0.5625f),
                          box(0.0f, 0.375f, 0.4375f, 0.5625f, 0.5625f, 0.5625f)},
                         ShapeRule::Fence));
    return parts;
}

// The loader's own two steps, which is what makes a hand-built shape behave like a
// loaded one: rules that answer with faces of their own override the declared
// ones, and the static lists are the canonical flattening of the parts.
BlockType finish_shape(BlockType bt, ShapeRule connector) {
    bt.connector = connector;
    for (ShapePart& part : bt.parts) {
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

BlockType make_wall(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.parts = wall_parts();
    return finish_shape(std::move(bt), ShapeRule::WallArm);
}

BlockType make_fence(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.parts = fence_parts();
    return finish_shape(std::move(bt), ShapeRule::Fence);
}

// A slab with a step standing on the back half of it — a stair's two boxes, so the
// shape has a face ON the cell boundary (the slab's) and a face one half-cell in
// (the step's) that is visible from behind.
BlockType make_step_probe(const char* name) {
    BlockType bt{};
    bt.name = name;
    bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::NoOcclusion;
    bt.visible_faces = {true, true, true, true, true, true};
    bt.selection_boxes = {box(0.0f, 0.0f, 0.0f, 1.0f, 0.5f, 1.0f),
                          box(0.0f, 0.5f, 0.5f, 1.0f, 1.0f, 1.0f)};
    bt.full_cube_ = false;
    bt.greedy_mergeable = false;
    return bt;
}

// One emitted quad: the plane its normal runs along, which way it faces, and the
// rectangle it covers on that plane in the two axes the normal is not on.
struct Quad {
    int axis = 0;
    int sign = 1;
    float plane = 0.0f;
    float a0 = 0.0f, a1 = 0.0f, b0 = 0.0f, b1 = 0.0f;
};

std::vector<Quad> quads_of(const MeshBuilder& mb) {
    std::vector<Quad> out;
    const auto& verts = mb.get_vertices();
    size_t i = 0;
    while (i + 3 < verts.size()) {
        float v[4][3];
        for (int k = 0; k < 4; ++k) {
            v[k][0] = static_cast<float>(verts[i + k].x) / 256.0f;
            v[k][1] = static_cast<float>(verts[i + k].y) / 256.0f;
            v[k][2] = static_cast<float>(verts[i + k].z) / 256.0f;
        }
        Quad q{};
        const int nx = static_cast<int>(verts[i].nx);
        const int ny = static_cast<int>(verts[i].ny);
        const int nz = static_cast<int>(verts[i].nz);
        const int normal = ny != 0 ? 1 : (nx != 0 ? 0 : 2);
        q.axis = normal;
        q.sign = (ny != 0 ? ny : (nx != 0 ? nx : nz)) > 0 ? 1 : -1;
        q.plane = v[0][normal];
        const int ta = normal == 0 ? 1 : 0;
        const int tb = normal == 2 ? 1 : 2;
        q.a0 = q.a1 = v[0][ta];
        q.b0 = q.b1 = v[0][tb];
        for (int k = 1; k < 4; ++k) {
            q.a0 = std::min(q.a0, v[k][ta]);
            q.a1 = std::max(q.a1, v[k][ta]);
            q.b0 = std::min(q.b0, v[k][tb]);
            q.b1 = std::max(q.b1, v[k][tb]);
        }
        out.push_back(q);
        i += 4;
    }
    return out;
}

// The area two quads share, or 0 when they are not on the same plane. Positive
// means z-fighting: the same pixels written twice, by two different draws.
float shared_area(const Quad& a, const Quad& b) {
    if (a.axis != b.axis || std::fabs(a.plane - b.plane) > 1e-4f) return 0.0f;
    const float da = std::min(a.a1, b.a1) - std::max(a.a0, b.a0);
    const float db = std::min(a.b1, b.b1) - std::max(a.b0, b.b0);
    if (da <= 1e-4f || db <= 1e-4f) return 0.0f;
    return da * db;
}

int quads_on_plane(const std::vector<Quad>& quads, int axis, float plane, int sign) {
    int count = 0;
    for (const Quad& q : quads) {
        if (q.axis == axis && q.sign == sign && std::fabs(q.plane - plane) < 1e-4f) ++count;
    }
    return count;
}

// Sample centres offset off every 16th, so a point never lands exactly on a seam
// between two pieces and counts for both.
bool rect_covers(float x0, float x1, float y0, float y1, float px, float py) {
    constexpr float kE = 1e-4f;
    return px >= x0 - kE && px <= x1 + kE && py >= y0 - kE && py <= y1 + kE;
}

BlockID register_shape_or_fail(BlockRegistry& reg, const BlockType& shape) {
    const BlockID id = reg.register_block(shape);
    if (id == BlockIDs::AIR) CHECK(false);
    return id;
}

// The mesher as the GAME runs it: the greedy passes first, then the per-AABB
// pass. The unit tests of the shape rules turn greedy off to isolate them, so the
// shipped path is the one that has to be checked for a seam.
MeshBuilder mesh_of(const ChunkData& chunk, bool greedy = true) {
    MeshBuilder mb;
    mb.set_greedy_enabled(greedy);
    mb.build_mesh(chunk);
    return mb;
}

}  // namespace

// The case a player walks up to: one wall cell with a wall on every one of its four
// horizontal sides. It draws a reach into each of them, and the four reaches meet
// in the middle of the cell with all four of their tops on the same plane and in
// the same place — four coplanar quads on top of each other, which is the shimmer.
// The centre cell carries no post (the reaches already fill the middle), so nothing
// swallows the overlap: the pieces have to be handed out, one owner per region.
// Checked on the shipped mesh path and sampled point by point, because a count of
// quads says nothing about where they are.
TEST_CASE("a wall surrounded on all four sides draws each piece of its middle once") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall = register_shape_or_fail(reg, make_wall("test_seam_cross"));
    if (wall == BlockIDs::AIR) return;

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(6, 15, 6, wall);  // the cell itself
    chunk.set_block(5, 15, 6, wall);  // and one on each of its four sides
    chunk.set_block(7, 15, 6, wall);
    chunk.set_block(6, 15, 5, wall);
    chunk.set_block(6, 15, 7, wall);
    chunk.compute_section_flags();
    const std::vector<Quad> quads = quads_of(mesh_of(chunk));
    CHECK(quads.size() > 0);

    for (size_t a = 0; a < quads.size(); ++a) {
        for (size_t b = a + 1; b < quads.size(); ++b) {
            CHECK(shared_area(quads[a], quads[b]) <= 1e-6f);
        }
    }

    // The reaches' own plane, sampled over the centre cell: the four of them reach
    // in 6/16 wide, so the surface they make is a plus — the strips along either
    // axis through the middle — and every point of it is drawn exactly once. This
    // is the assertion that fails loudly when a second reach's top is drawn over
    // the first instead of being cut back to the part it owns.
    constexpr float kCellX = 6.0f;
    constexpr float kCellZ = 6.0f;
    constexpr float kTop = 15.875f;
    constexpr int kSteps = 64;
    int covered = 0;
    int drawn = 0;
    for (int iz = 0; iz < kSteps; ++iz) {
        for (int ix = 0; ix < kSteps; ++ix) {
            const float lx = (static_cast<float>(ix) + 0.41f) / kSteps;
            const float lz = (static_cast<float>(iz) + 0.41f) / kSteps;
            const bool by_reach = (lx > 0.3125f && lx < 0.6875f) || (lz > 0.3125f && lz < 0.6875f);
            int hits = 0;
            for (const Quad& q : quads) {
                if (q.axis != 1 || q.sign != +1 || std::fabs(q.plane - kTop) > 1e-4f) continue;
                if (rect_covers(q.a0 - kCellX, q.a1 - kCellX, q.b0 - kCellZ, q.b1 - kCellZ, lx,
                                lz)) {
                    ++hits;
                }
            }
            CHECK(hits == (by_reach ? 1 : 0));
            if (by_reach) ++covered;
            if (hits == 1) ++drawn;
        }
    }
    CHECK(covered > 0);
    CHECK(drawn == covered);

    // ...and the middle is still filled, once: the centre 6/16 square is where all
    // four reaches overlap, so it is the hardest point of the cell for the rule —
    // each of these two is inside two reaches and has to be drawn by one of them.
    for (const float p : {0.55f, 0.45f}) {
        int hits = 0;
        for (const Quad& q : quads) {
            if (q.axis != 1 || q.sign != +1 || std::fabs(q.plane - kTop) > 1e-4f) continue;
            if (rect_covers(q.a0 - kCellX, q.a1 - kCellX, q.b0 - kCellZ, q.b1 - kCellZ, p, p)) {
                ++hits;
            }
        }
        CHECK(hits == 1);
    }
}

// A wall standing in a wall: every cell of the patch draws an arm into each side it
// meets, so the planes between them are the ones that used to be drawn twice. The
// count of fighting quads has to be zero — the whole complaint — and the patch's
// own surface still has to be there, or the answer would be "culled too much".
TEST_CASE("two wall cells never both draw the face they share") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID wall = register_shape_or_fail(reg, make_wall("test_seam_wall"));
    if (wall == BlockIDs::AIR) return;

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    for (int32_t z = 5; z <= 7; ++z) {
        for (int32_t x = 5; x <= 7; ++x) chunk.set_block(x, 15, z, wall);
    }
    chunk.compute_section_flags();
    // Both paths: the shipped one (greedy passes, then the per-AABB pass) and the
    // one the shape tests use. A wall is not greedy-mergeable, so the two have to
    // agree quad for quad — and the shipped path is the only one a player sees.
    const std::vector<Quad> quads = quads_of(mesh_of(chunk, true));
    const std::vector<Quad> quads_plain = quads_of(mesh_of(chunk, false));
    CHECK(quads.size() > 0);
    CHECK(quads_plain.size() == quads.size());

    // The property, stated as geometry rather than as an observation: no two quads
    // on one plane may share any area, whichever way they face.
    for (size_t a = 0; a < quads.size(); ++a) {
        for (size_t b = a + 1; b < quads.size(); ++b) {
            CHECK(shared_area(quads[a], quads[b]) <= 1e-6f);
            CHECK(shared_area(quads_plain[a], quads_plain[b]) <= 1e-6f);
        }
    }

    // The planes between two of the patch's cells carry a solid arm on both sides,
    // so they are inside the wall and nothing may be drawn on them at all — this is
    // the check that the arms, and not the static lists, decided it.
    for (const float plane : {6.0f, 7.0f}) {
        CHECK(quads_on_plane(quads, 0, plane, +1) == 0);
        CHECK(quads_on_plane(quads, 0, plane, -1) == 0);
        CHECK(quads_on_plane(quads, 2, plane, +1) == 0);
        CHECK(quads_on_plane(quads, 2, plane, -1) == 0);
    }

    // ...and the patch is still a wall: the reaches' tops at 14/16, the posts at the
    // cell top, and a post's own side where a cell of the patch meets open air (a
    // cell's sides that face nothing carry no reach, so its post is the geometry).
    CHECK(quads_on_plane(quads, 1, 15.875f, +1) > 0);
    CHECK(quads_on_plane(quads, 1, 16.0f, +1) > 0);
    CHECK(quads_on_plane(quads, 2, 5.25f, -1) > 0);  // the corner cell's post side
    CHECK(quads_on_plane(quads, 0, 7.75f, +1) > 0);  // ...and the far column's
}

// The hard half: a neighbour whose geometry covers PART of the face. The fence
// draws two narrow rails over the boundary and the shape beside it draws a whole
// slab face under them, so neither face covers the other and culling alone cannot
// answer — one of the two has to give up exactly the overlap, which is the half of
// the rule that keeps the plane drawn once.
TEST_CASE("a fence and the shape beside it draw their shared plane exactly once") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence = register_shape_or_fail(reg, make_fence("test_seam_fence"));
    const BlockID probe = register_shape_or_fail(reg, make_step_probe("test_seam_probe"));
    if (fence == BlockIDs::AIR || probe == BlockIDs::AIR) return;

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(6, 15, 5, fence);
    chunk.set_block(6, 15, 6, probe);
    chunk.compute_section_flags();
    const std::vector<Quad> quads = quads_of(mesh_of(chunk));

    for (size_t a = 0; a < quads.size(); ++a) {
        for (size_t b = a + 1; b < quads.size(); ++b) {
            CHECK(shared_area(quads[a], quads[b]) <= 1e-6f);
        }
    }

    // The plane between them, sampled: every point the two shapes cover there has
    // to be drawn exactly once, and every point they do not cover has to be drawn
    // not at all. One sample per 1/64 of the face, off the seams.
    constexpr float kPlane = 6.0f;
    constexpr float kCellX = 6.0f;      // the probe's own column
    constexpr float kCellY = 15.0f;     // ...and its own cell row
    constexpr int kSteps = 64;
    int covered = 0;
    int drawn = 0;
    for (int iy = 0; iy < kSteps; ++iy) {
        for (int ix = 0; ix < kSteps; ++ix) {
            const float lx = (static_cast<float>(ix) + 0.37f) / kSteps;
            const float ly = (static_cast<float>(iy) + 0.37f) / kSteps;
            // The fence's two rails reach across; the probe's slab reaches its own
            // boundary with the whole footprint under the rails' height.
            const bool by_fence = rect_covers(0.4375f, 0.5625f, 0.375f, 0.5625f, lx, ly) ||
                                  rect_covers(0.4375f, 0.5625f, 0.75f, 0.9375f, lx, ly);
            const bool by_probe = rect_covers(0.0f, 1.0f, 0.0f, 0.5f, lx, ly);
            const bool expected = by_fence || by_probe;
            int hits = 0;
            for (const Quad& q : quads) {
                if (q.axis != 2 || std::fabs(q.plane - kPlane) > 1e-4f) continue;
                if (rect_covers(q.a0 - kCellX, q.a1 - kCellX, q.b0 - kCellY, q.b1 - kCellY, lx,
                                ly)) {
                    ++hits;
                }
            }
            if (expected) ++covered;
            if (hits == 1) ++drawn;
            CHECK(hits == (expected ? 1 : 0));
        }
    }
    CHECK(covered > 0);
    CHECK(drawn == covered);
}

// The other side of that rule: only a face lying ON the boundary is shared with the
// neighbour. The probe's step stands one half-cell in with its back face open to
// the air, and the fence's rails reach the boundary the step does not touch — so
// trimming by them would punch a hole in a face that is plainly visible.
TEST_CASE("a cell's own inner face is not trimmed by the neighbour's geometry") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence = register_shape_or_fail(reg, make_fence("test_seam_fence2"));
    const BlockID probe = register_shape_or_fail(reg, make_step_probe("test_seam_probe2"));
    if (fence == BlockIDs::AIR || probe == BlockIDs::AIR) return;

    ChunkData chunk;
    chunk.fill_blocks(BlockIDs::AIR);
    chunk.set_block(6, 15, 5, fence);
    chunk.set_block(6, 15, 6, probe);
    chunk.compute_section_flags();
    const std::vector<Quad> quads = quads_of(mesh_of(chunk));

    // The step's back face at z = 6.5, one quad spanning the step's whole footprint
    // from the top of the slab to the top of the cell.
    const int step_faces = quads_on_plane(quads, 2, 6.5f, -1);
    CHECK(step_faces == 1);

    // ...and it really is the step's face, not a piece of something else.
    bool whole = false;
    for (const Quad& q : quads) {
        if (q.axis != 2 || q.sign != -1 || std::fabs(q.plane - 6.5f) > 1e-4f) continue;
        whole = std::fabs(q.a0 - 6.0f) < 1e-4f && std::fabs(q.a1 - 7.0f) < 1e-4f &&
                std::fabs(q.b0 - 15.5f) < 1e-4f && std::fabs(q.b1 - 16.0f) < 1e-4f;
    }
    CHECK(whole);
}
