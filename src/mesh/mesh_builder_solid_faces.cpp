// How a box's face is actually drawn: as ONE rectangle normally, and as several when
// a sibling box of the same shape covers part of it — the within-a-cell half of the
// culling the mesher does against neighbouring cells. Also what the neighbour's own
// resolved geometry covers of the plane the two cells share. The emit half asks these
// questions; the declarations they answer are in mesh_builder_solid_internal.hpp.

#include "mesh/mesh_builder_solid_internal.hpp"

#include "core/block_types.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

// ---------------------------------------------------------------------------
// Coplanar sibling faces
// ---------------------------------------------------------------------------
// A box's face is a rectangle in the two axes its normal does not run along, so
// that pair of axes is all the sibling test below needs.

[[nodiscard]] FaceAxes face_axes(VoxelEngine::FaceDirection dir) {
    using VoxelEngine::FaceDirection;
    switch (dir) {
        case FaceDirection::Top:    return {1, +1, 0, 2};
        case FaceDirection::Bottom: return {1, -1, 0, 2};
        case FaceDirection::Right:  return {0, +1, 1, 2};
        case FaceDirection::Left:   return {0, -1, 1, 2};
        case FaceDirection::Front:  return {2, +1, 0, 1};
        case FaceDirection::Back:   return {2, -1, 0, 1};
    }
    return {1, +1, 0, 2};
}

namespace {

// The part of `rect` that `cut` does not cover, as up to four rectangles:
// a strip either side of the cut, and a strip above and below it within the
// cut's own span. Returns how many were written, or -1 if they do not fit — the
// caller then keeps the face whole, which is the safe direction: a redundant
// quad is invisible, a missing one is a hole.
[[nodiscard]] int subtract_rect(const FaceRect& rect, const FaceRect& cut, FaceRect* out,
                               int cap) {
    const bool apart = cut.a1 <= rect.a0 + kFacePieceEpsilon ||
                       cut.a0 >= rect.a1 - kFacePieceEpsilon ||
                       cut.b1 <= rect.b0 + kFacePieceEpsilon ||
                       cut.b0 >= rect.b1 - kFacePieceEpsilon;
    if (apart) {
        if (cap < 1) return -1;
        out[0] = rect;
        return 1;
    }

    int n = 0;
    if (cut.a0 > rect.a0 + kFacePieceEpsilon) {
        if (n >= cap) return -1;
        out[n++] = {rect.a0, cut.a0, rect.b0, rect.b1};
    }
    if (cut.a1 < rect.a1 - kFacePieceEpsilon) {
        if (n >= cap) return -1;
        out[n++] = {cut.a1, rect.a1, rect.b0, rect.b1};
    }
    const float a0 = cut.a0 > rect.a0 ? cut.a0 : rect.a0;
    const float a1 = cut.a1 < rect.a1 ? cut.a1 : rect.a1;
    if (a1 > a0 + kFacePieceEpsilon) {
        if (cut.b0 > rect.b0 + kFacePieceEpsilon) {
            if (n >= cap) return -1;
            out[n++] = {a0, a1, rect.b0, cut.b0};
        }
        if (cut.b1 < rect.b1 - kFacePieceEpsilon) {
            if (n >= cap) return -1;
            out[n++] = {a0, a1, cut.b1, rect.b1};
        }
    }
    return n;
}

} // namespace

// How many quads a box's face on `dir` is drawn as: one normally, and more when
// a sibling box of the same shape covers part of it.
//
// This is the within-a-cell half of the culling the mesher already does against
// neighbouring cells, and it exists for the same reason. A wall standing with
// four sides connected draws four reaches whose tops are all at the same height
// and overlap in the middle of the cell, and two coplanar quads in the same
// place fight for the depth buffer all the way to the pixel — visible as a
// shimmer along the seam where they cross. The same is true of a rail's end cap
// and the post it is set into, and of a stair's slab under its own step.
//
// Two rules, and both are load-bearing:
//   - a sibling hides the part of this face it shares only when it FILLS THE
//     SLIVER just outside the face: its body has to come up to the plane (flush
//     or straight through it) from the outside, not merely lie somewhere along
//     the axis. That covers both a sibling standing in front of the face and
//     one flush with it. It also has to exclude a sibling that is entirely on
//     the far side of the plane, inside this box's own body: a box's own end
//     face is still the boundary there (the crucible's two facing walls are the
//     case in point — both inner faces are visible across the cavity, so
//     neither may be trimmed by the other, however far along the axis that one
//     reaches).
//   - a sibling whose body passes STRAIGHT THROUGH the plane (it starts on the
//     inner side and ends on the outer one) hides the region whatever its
//     order: the face piece is inside the sibling's body, so nobody draws it.
//     Being a straddler is what makes it >order-independent< — a rail set into
//     a post's side has its top plane crossed by the post's whole body whether
//     the post is listed before or after it in the shape.
//   - a sibling that only comes up to the plane is FLUSH with it — its own face
//     sits on the same plane — so exactly ONE of the two may draw the shared
//     region, and the EARLIER wins. Order is the resolved parts order, which is
//     the file's; without the rule, the four wall reaches would trim each
//     other's shared middle away and leave a hole. A stair is the same rule on
//     the OTHER side of the plane: its upper step's underside is flush with the
//     lower box's top, so the LOWER (earlier) box draws that whole surface and
//     the step's underside is suppressed by the buried-underside rule — the two
//     are one decision, not two ways to reach it.
// The set tested is the RESOLVED one, so a claimed part that is absent stops
// hiding anything, and the pieces a face is reduced to are texture-exact:
// per-AABB UVs are read off the box's own extents, so a piece of a face samples
// exactly the texels the whole face did.
[[nodiscard]] int visible_face_rects(const VoxelEngine::ShapeBoxes& boxes,
                                     const VoxelEngine::BlockAABB& box,
                                     VoxelEngine::FaceDirection dir, uint8_t box_index,
                                     FaceRect* out) {
    const FaceAxes ax = face_axes(dir);
    const float plane = ax.sign > 0 ? box.max[ax.normal] : box.min[ax.normal];

    FaceRect cur[kFacePieceCap];
    FaceRect next[kFacePieceCap];
    int count = 1;
    cur[0] = {box.min[ax.tangent_a], box.max[ax.tangent_a], box.min[ax.tangent_b],
              box.max[ax.tangent_b]};

    for (uint8_t j = 0; j < boxes.count(); ++j) {
        if (j == box_index) continue;
        const VoxelEngine::BlockAABB& other = boxes[j];
        // Does this sibling fill the sliver of space just outside the face? It
        // has to reach the plane (so it is flush with the face, or swallows the
        // region) and start on the outer side of it (so it is not simply a
        // sibling further along the axis, behind the face). A sibling that
        // misses either test is nowhere near this face and hides nothing.
        if (other.max[ax.normal] < plane - kFacePieceEpsilon) continue;
        if (other.min[ax.normal] > plane + kFacePieceEpsilon) continue;
        // A sibling that passes straight through the plane swallows the region
        // whatever its order — the face piece is inside its body. That is a
        // straddler: its body starts below the plane AND ends above it, which
        // has nothing to do with which way the face points, so the test is the
        // same for both signs. A sibling that only comes up to the plane (from
        // either side) is flush with it: its own face sits on the plane too, so
        // exactly one of the two draws the shared region, and the EARLIER one
        // wins (see the note above).
        const bool through_plane =
            other.min[ax.normal] < plane - kFacePieceEpsilon &&
            other.max[ax.normal] > plane + kFacePieceEpsilon;
        if (!through_plane && j > box_index) continue;
        const FaceRect cut{other.min[ax.tangent_a], other.max[ax.tangent_a],
                           other.min[ax.tangent_b], other.max[ax.tangent_b]};

        int written = 0;
        bool overflowed = false;
        for (int i = 0; i < count; ++i) {
            const int n = subtract_rect(cur[i], cut, next + written, kFacePieceCap - written);
            if (n < 0) {
                overflowed = true;
                break;
            }
            written += n;
        }
        if (overflowed || written == 0) {
            // Out of room, or the face is entirely covered: the first keeps the
            // face whole (see subtract_rect), the second drops it.
            if (overflowed) {
                out[0] = cur[0];
                return 1;
            }
            return 0;
        }
        for (int i = 0; i < written; ++i) cur[i] = next[i];
        count = written;
    }

    for (int i = 0; i < count; ++i) out[i] = cur[i];
    return count;
}

// -------------------------------------------------------------------------
// AABB face culling: check if a neighbor block's selection boxes cover
// this face area, making the face invisible.
// The neighbor must both (a) reach the face boundary plane and (b) cover
// the face's extent in the other two axes.
// -------------------------------------------------------------------------
// One neighbour box against one face, in the neighbour's own cell frame: the box
// has to reach the shared plane from its side, and to span the face's extent in
// the other two axes. Shared by the static list below and by the resolved boxes
// the boundary culling asks for, so the two can only ever differ in WHICH boxes
// they are asked about.
[[nodiscard]] bool neighbor_box_covers(const float nb_min[3], const float nb_max[3],
                                              const float self_min[3], const float self_max[3],
                                              FaceDirection dir) {
    switch (dir) {
        case FaceDirection::Top:
            // Face at y=self_max[1], the neighbour's own face on the plane below it
            return nb_min[1] <= self_max[1] - 1.0f &&
                   nb_min[0] <= self_min[0] && nb_max[0] >= self_max[0] &&
                   nb_min[2] <= self_min[2] && nb_max[2] >= self_max[2];
        case FaceDirection::Bottom:
            // Face at y=self_min[1], the neighbour's own face on the plane above it
            return nb_max[1] >= self_min[1] + 1.0f &&
                   nb_min[0] <= self_min[0] && nb_max[0] >= self_max[0] &&
                   nb_min[2] <= self_min[2] && nb_max[2] >= self_max[2];
        case FaceDirection::Right:
            return nb_min[0] <= self_max[0] - 1.0f &&
                   nb_min[1] <= self_min[1] && nb_max[1] >= self_max[1] &&
                   nb_min[2] <= self_min[2] && nb_max[2] >= self_max[2];
        case FaceDirection::Left:
            return nb_max[0] >= self_min[0] + 1.0f &&
                   nb_min[1] <= self_min[1] && nb_max[1] >= self_max[1] &&
                   nb_min[2] <= self_min[2] && nb_max[2] >= self_max[2];
        case FaceDirection::Front:
            return nb_min[2] <= self_max[2] - 1.0f &&
                   nb_min[0] <= self_min[0] && nb_max[0] >= self_max[0] &&
                   nb_min[1] <= self_min[1] && nb_max[1] >= self_max[1];
        case FaceDirection::Back:
            return nb_max[2] >= self_min[2] + 1.0f &&
                   nb_min[0] <= self_min[0] && nb_max[0] >= self_max[0] &&
                   nb_min[1] <= self_min[1] && nb_max[1] >= self_max[1];
    }
    return false;
}

// -------------------------------------------------------------------------
// What the block next door draws on the face the two cells share
// -------------------------------------------------------------------------
// The static list is exact for a block whose boxes never change. It is not for
// the connector families: a wall's held model is its post and the two arms along
// X, so a face reaching the cell's +/-Z boundary finds no box in that list and is
// drawn — while the wall next door draws its own arm into exactly that plane.
// Two coincident quads facing each other fight for the depth buffer all the way
// to the pixel, at every wall-to-wall junction in the world. So for a shape that
// is RESOLVED per cell the mesher asks the neighbour cell the question its own
// mesh answers: which boxes is that block drawing, against its own neighbours.
//
// The culling test is then the same one, against those boxes. On top of it, the
// two cells cannot both draw the sliver they share, and they cannot talk to each
// other, so the owner is a property of the boundary: the cell on the NEGATIVE
// side keeps the whole of its own face, and the cell on the positive side (a
// Back/Left/Bottom face, i.e. this one) gives up whatever the neighbour covers.
// Every region of the plane is then drawn exactly once — wherever the union of
// the two cells has a boundary there — and never twice.
[[nodiscard]] static bool is_negative_boundary_face(VoxelEngine::FaceDirection dir) {
    return dir == VoxelEngine::FaceDirection::Back || dir == VoxelEngine::FaceDirection::Left ||
           dir == VoxelEngine::FaceDirection::Bottom;
}

// Whether a box of the neighbour cell reaches the plane the two cells share. The
// tangential axes are the same in both cells' frames — only the axis the face
// lies on differs — so no shifting is needed, and the reach is the box's own
// extent toward the boundary it shares with this cell.
[[nodiscard]] static bool neighbor_box_reaches(const VoxelEngine::BlockAABB& nb,
                                               VoxelEngine::FaceDirection dir) {
    switch (dir) {
        case VoxelEngine::FaceDirection::Back:   return nb.max[2] >= 1.0f - kFacePieceEpsilon;
        case VoxelEngine::FaceDirection::Left:   return nb.max[0] >= 1.0f - kFacePieceEpsilon;
        case VoxelEngine::FaceDirection::Bottom: return nb.max[1] >= 1.0f - kFacePieceEpsilon;
        default:                                 return false;
    }
}

// Does the neighbour's resolved geometry cover this whole face, making the plane
// run through the neighbour rather than along a boundary?
[[nodiscard]] bool neighbor_boxes_cover(const VoxelEngine::ShapeBoxes& boxes,
                                               const float self_min[3], const float self_max[3],
                                               VoxelEngine::FaceDirection dir) {
    for (const VoxelEngine::BlockAABB& nb : boxes) {
        if (neighbor_box_covers(nb.min, nb.max, self_min, self_max, dir)) return true;
    }
    return false;
}
// Take the neighbour's coverage out of the pieces of a face this cell does not
// own. Pieces that end up empty are dropped; a piece that does not fit the buffer
// is kept whole, which is subtract_rect's rule and the safe direction — a
// redundant quad is invisible, a missing one is a hole.
[[nodiscard]] int drop_neighbor_coverage(FaceDirection dir, const ShapeBoxes& neighbor,
                                                const BlockAABB& box, FaceRect* rects, int count) {
    if (!is_negative_boundary_face(dir) || count <= 0) return count;
    const FaceAxes ax = face_axes(dir);
    // Only a face lying ON the boundary the neighbour is across is shared with it.
    // A connector's own inner face at z=0.5 is this cell's business however far
    // the neighbour's arm reaches.
    if (box.min[ax.normal] > kFacePieceEpsilon) return count;
    FaceRect next[kFacePieceCap];
    for (const auto& nb : neighbor) {
        if (!neighbor_box_reaches(nb, dir)) continue;
        const FaceRect cut{nb.min[ax.tangent_a], nb.max[ax.tangent_a], nb.min[ax.tangent_b],
                           nb.max[ax.tangent_b]};
        int written = 0;
        bool overflowed = false;
        for (int i = 0; i < count; ++i) {
            const int n = subtract_rect(rects[i], cut, next + written, kFacePieceCap - written);
            if (n < 0) {
                overflowed = true;
                break;
            }
            written += n;
        }
        if (overflowed) continue;
        if (written == 0) return 0;
        for (int i = 0; i < written; ++i) rects[i] = next[i];
        count = written;
    }
    return count;
}

} // namespace VoxelEngine
