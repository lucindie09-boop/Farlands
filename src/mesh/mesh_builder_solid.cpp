#include "mesh/mesh_builder.hpp"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"

namespace {

int lod_representative_priority(VoxelEngine::BlockID block_id, const VoxelEngine::BlockRegistry& registry) {
    if (block_id == VoxelEngine::BlockIDs::AIR) return 0;
    const VoxelEngine::BlockType& block_type = registry.get_block_fast(block_id);
    if (!VoxelEngine::HasProperty(block_type.properties, VoxelEngine::BlockProperty::Transparent)) {
        return 3;
    }
    if (!VoxelEngine::HasProperty(block_type.properties, VoxelEngine::BlockProperty::Liquid)) {
        return 2;
    }
    return 1;
}

// Per-IDs priority table (air=0, liquid=1, transparent=2, opaque=3) so the
// LOD representative scan is a flat array load per sample instead of a
// BlockRegistry type lookup, keeping the coarse-footprint passes cheap at
// stride 2/4/8. Opaque (priority 3) is the maximum, so a scan that finds an
// opaque voxel can stop immediately: later samples are at best equal and the
// first max-priority win is deterministic.
struct LodPriorityLut {
    uint8_t priority[256];
};

void build_lod_priority_lut(LodPriorityLut& lut, const VoxelEngine::BlockRegistry& registry) {
    for (uint32_t i = 0; i < 256; i++) {
        lut.priority[i] = static_cast<uint8_t>(
            lod_representative_priority(static_cast<VoxelEngine::BlockID>(i), registry));
    }
}

// True when a sibling box of the same shape sits directly beneath this one and
// spans its whole footprint, so this box's underside is buried inside the shape
// (a stair's upper step over its own lower step, a wall's cap over its post).
// Emitting that face would be a hidden quad.
//
// The set tested is the RESOLVED one, not the static list: if a claimed part is
// absent, the box above it is no longer buried and its underside has to be drawn.
bool box_underside_covered(const VoxelEngine::ShapeBoxes& boxes,
                           const VoxelEngine::BlockAABB& box) {
    for (const VoxelEngine::BlockAABB& other : boxes) {
        if (&other == &box) continue;
        const float dy = other.max[1] - box.min[1];
        if (dy < -0.0005f || dy > 0.0005f) continue;
        if (other.min[0] <= box.min[0] && other.max[0] >= box.max[0] &&
            other.min[2] <= box.min[2] && other.max[2] >= box.max[2]) {
            return true;
        }
    }
    return false;
}

// Bottom faces of a box resting on the cell floor are never visible from a
// ground-level view and have been skipped since the first per-AABB emitter. A
// box raised off the floor (a wall torch, a fence rail, a lantern) does get its
// underside emitted, unless a sibling box covers it.
bool skip_bottom_face(const VoxelEngine::ShapeBoxes& boxes,
                      const VoxelEngine::BlockAABB& box) {
    return box.min[1] <= 0.0f || box_underside_covered(boxes, box);
}

// ---------------------------------------------------------------------------
// Coplanar sibling faces
// ---------------------------------------------------------------------------
// A box's face is a rectangle in the two axes its normal does not run along, so
// that pair of axes is all the sibling test below needs.
struct FaceAxes {
    uint8_t normal;
    int sign;  // +1 or -1, the way the face points
    uint8_t tangent_a;
    uint8_t tangent_b;
};

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

// A face rectangle in its own two axes. Authored geometry is exact 16ths, so
// this only absorbs the float round-trip through the JSON loader; it doubles as
// the sliver threshold, because a piece narrower than it is not a quad.
constexpr float kFacePieceEpsilon = 1e-5f;
constexpr int kFacePieceCap = 16;

struct FaceRect {
    float a0;
    float a1;
    float b0;
    float b1;
};

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

} // namespace

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Solid cache population and access
// -------------------------------------------------------------------------
void MeshBuilder::populate_solid_cache(const ChunkData& chunk, const BlockRegistry& registry) {
    if (partial_mode_) {
        // Tight-populate only the box around the re-emit region. Everything
        // outside solid_bounds_ is read live via solid_at(), so it must never be
        // populated (and stale values there are never read).
        if (debug_poison_solid_cache_) {
            for (auto& plane : solid_cache)
                for (auto& row : plane)
                    row.fill(0xFFFFu);
        }
        for (int32_t y = solid_bounds_.y_min; y < solid_bounds_.y_max; ++y) {
            for (int32_t z = solid_bounds_.z_min; z < solid_bounds_.z_max; ++z) {
                const int32_t zi = z + 1;
                for (int32_t x = solid_bounds_.x_min; x < solid_bounds_.x_max; ++x) {
                    solid_cache[y][zi][x + 1] = chunk.get_block_unsafe(x, y, z);
                }
            }
        }
        return;
    }
    for (auto& plane : solid_cache)
        for (auto& row : plane)
            row.fill(0);

    LodPriorityLut priority_lut;
    build_lod_priority_lut(priority_lut, registry);

    // solid_cache is laid out [y][z][x] (see header) so this population pass
    // walks it with x as the fastest-varying index.
    {
        ScopedTimer cache_timer(perf_timer, TimerID::SolidCachePopulation);
        // Populate interior (x: 1..SC_W-2 = CHUNK_WIDTH, z: 1..SC_D-2 = CHUNK_DEPTH)
        for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
            if (chunk.is_section_all_air(s)) continue;
            int32_t y0 = s * SECTION_HEIGHT;
            int32_t y1 = y0 + SECTION_HEIGHT;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t z = 1; z <= CHUNK_DEPTH; z += stride_xz_) {
                    int32_t z_src = z - 1;
                    for (int32_t x = 1; x <= CHUNK_WIDTH; x += stride_xz_) {
                        int32_t x_src = x - 1;
                        BlockID representative = BlockIDs::AIR;
                        int representative_priority = 0;
                        for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                            for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                                BlockID sample = chunk.get_block_unsafe(x_src + dx, y, z_src + dz);
                                const uint8_t sp = priority_lut.priority[sample];
                                if (sp > representative_priority) {
                                    representative = sample;
                                    representative_priority = sp;
                                    if (representative_priority == 3) break;
                                }
                            }
                            if (representative_priority == 3) break;
                        }
                        // LOD meshing must be conservative: if any voxel in the coarse footprint
                        // is present, keep the cell alive so exposed faces are not dropped.
                        solid_cache[y][z][x] = representative;
                    }
                }
            }
        }

        // X boundaries — store the actual BlockID (or BlockIDs::AIR if neighbor null)
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t z = 1; z <= CHUNK_DEPTH; z++) {
                int32_t z_src = ((z - 1) / stride_xz_) * stride_xz_;
                BlockID neg_x_rep = BlockIDs::AIR;
                BlockID pos_x_rep = BlockIDs::AIR;
                int neg_x_priority = 0;
                int pos_x_priority = 0;
                if (accessor.neg_x) {
                    for (int32_t dz = 0; dz < stride_xz_ && z_src + dz < CHUNK_DEPTH; ++dz) {
                        const BlockID sample = accessor.neg_x->get_block_unsafe(CHUNK_WIDTH - 1, y, z_src + dz);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > neg_x_priority) {
                            neg_x_rep = sample;
                            neg_x_priority = sp;
                            if (neg_x_priority == 3) break;
                        }
                    }
                }
                if (accessor.pos_x) {
                    for (int32_t dz = 0; dz < stride_xz_ && z_src + dz < CHUNK_DEPTH; ++dz) {
                        const BlockID sample = accessor.pos_x->get_block_unsafe(0, y, z_src + dz);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > pos_x_priority) {
                            pos_x_rep = sample;
                            pos_x_priority = sp;
                            if (pos_x_priority == 3) break;
                        }
                    }
                }
                solid_cache[y][z][0] = neg_x_rep;
                solid_cache[y][z][SC_W - 1] = pos_x_rep;
            }
        }

        // Z boundaries
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t x = 1; x <= CHUNK_WIDTH; x++) {
                int32_t x_src = ((x - 1) / stride_xz_) * stride_xz_;
                BlockID neg_z_rep = BlockIDs::AIR;
                BlockID pos_z_rep = BlockIDs::AIR;
                int neg_z_priority = 0;
                int pos_z_priority = 0;
                if (accessor.neg_z) {
                    for (int32_t dx = 0; dx < stride_xz_ && x_src + dx < CHUNK_WIDTH; ++dx) {
                        const BlockID sample = accessor.neg_z->get_block_unsafe(x_src + dx, y, CHUNK_DEPTH - 1);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > neg_z_priority) {
                            neg_z_rep = sample;
                            neg_z_priority = sp;
                            if (neg_z_priority == 3) break;
                        }
                    }
                }
                if (accessor.pos_z) {
                    for (int32_t dx = 0; dx < stride_xz_ && x_src + dx < CHUNK_WIDTH; ++dx) {
                        const BlockID sample = accessor.pos_z->get_block_unsafe(x_src + dx, y, 0);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > pos_z_priority) {
                            pos_z_rep = sample;
                            pos_z_priority = sp;
                            if (pos_z_priority == 3) break;
                        }
                    }
                }
                solid_cache[y][0][x] = neg_z_rep;
                solid_cache[y][SC_D - 1][x] = pos_z_rep;
            }
        }

        // Four corner columns (x=0 or SC_W-1, z=0 or SC_D-1)
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            BlockID neg_x_neg_z_rep = BlockIDs::AIR;
            BlockID pos_x_neg_z_rep = BlockIDs::AIR;
            BlockID neg_x_pos_z_rep = BlockIDs::AIR;
            BlockID pos_x_pos_z_rep = BlockIDs::AIR;
            int neg_x_neg_z_priority = 0;
            int pos_x_neg_z_priority = 0;
            int neg_x_pos_z_priority = 0;
            int pos_x_pos_z_priority = 0;
            if (accessor.neg_x_neg_z) {
                for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                    const int32_t neg_z = CHUNK_DEPTH - stride_xz_ + dz;
                    for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                        const int32_t neg_x = CHUNK_WIDTH - stride_xz_ + dx;
                        const BlockID sample = accessor.neg_x_neg_z->get_block_unsafe(neg_x, y, neg_z);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > neg_x_neg_z_priority) {
                            neg_x_neg_z_rep = sample;
                            neg_x_neg_z_priority = sp;
                            if (neg_x_neg_z_priority == 3) break;
                        }
                    }
                    if (neg_x_neg_z_priority == 3) break;
                }
            }
            if (accessor.pos_x_neg_z) {
                for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                    const int32_t neg_z = CHUNK_DEPTH - stride_xz_ + dz;
                    for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                        const int32_t pos_x = dx;
                        const BlockID sample = accessor.pos_x_neg_z->get_block_unsafe(pos_x, y, neg_z);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > pos_x_neg_z_priority) {
                            pos_x_neg_z_rep = sample;
                            pos_x_neg_z_priority = sp;
                            if (pos_x_neg_z_priority == 3) break;
                        }
                    }
                    if (pos_x_neg_z_priority == 3) break;
                }
            }
            if (accessor.neg_x_pos_z) {
                for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                    const int32_t pos_z = dz;
                    for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                        const int32_t neg_x = CHUNK_WIDTH - stride_xz_ + dx;
                        const BlockID sample = accessor.neg_x_pos_z->get_block_unsafe(neg_x, y, pos_z);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > neg_x_pos_z_priority) {
                            neg_x_pos_z_rep = sample;
                            neg_x_pos_z_priority = sp;
                            if (neg_x_pos_z_priority == 3) break;
                        }
                    }
                    if (neg_x_pos_z_priority == 3) break;
                }
            }
            if (accessor.pos_x_pos_z) {
                for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                    const int32_t pos_z = dz;
                    for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                        const int32_t pos_x = dx;
                        const BlockID sample = accessor.pos_x_pos_z->get_block_unsafe(pos_x, y, pos_z);
                        const uint8_t sp = priority_lut.priority[sample];
                        if (sp > pos_x_pos_z_priority) {
                            pos_x_pos_z_rep = sample;
                            pos_x_pos_z_priority = sp;
                            if (pos_x_pos_z_priority == 3) break;
                        }
                    }
                    if (pos_x_pos_z_priority == 3) break;
                }
            }
            solid_cache[y][0][0] = neg_x_neg_z_rep;
            solid_cache[y][0][SC_W - 1] = pos_x_neg_z_rep;
            solid_cache[y][SC_D - 1][0] = neg_x_pos_z_rep;
            solid_cache[y][SC_D - 1][SC_W - 1] = pos_x_pos_z_rep;
        }
    }
}

BlockID MeshBuilder::solid_at(int32_t y, int32_t zi, int32_t xi) const {
    if (partial_mode_) {
        const int32_t x = xi - 1;
        const int32_t z = zi - 1;
        if (x < solid_bounds_.x_min || x >= solid_bounds_.x_max ||
            y < solid_bounds_.y_min || y >= solid_bounds_.y_max ||
            z < solid_bounds_.z_min || z >= solid_bounds_.z_max) {
            return accessor.get_block(x, y, z);
        }
    }
    return solid_cache[y][zi][xi];
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
[[nodiscard]] static bool neighbor_box_covers(const float nb_min[3], const float nb_max[3],
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
[[nodiscard]] static bool neighbor_boxes_cover(const VoxelEngine::ShapeBoxes& boxes,
                                               const float self_min[3], const float self_max[3],
                                               VoxelEngine::FaceDirection dir) {
    for (const VoxelEngine::BlockAABB& nb : boxes) {
        if (neighbor_box_covers(nb.min, nb.max, self_min, self_max, dir)) return true;
    }
    return false;
}

bool MeshBuilder::should_cull_aabb_face(const float self_min[3], const float self_max[3],
                                         FaceDirection dir, const BlockType& neighbor_type) const {
    // A transparent neighbour never covers a face, however full its box is: you see
    // the far face THROUGH it. Without this, a block that is only full-height but
    // drawn with transparency hides the faces of everything it touches — a column
    // of falling water (`water_fallen`) takes the side off a slab, and a leaf block
    // takes the face off any partial block next to it. Note this is the path the
    // per-AABB emitters use directly; the full-cube emitters go through
    // should_cull_against_neighbor, which already returns false for a transparent
    // neighbour unless the two blocks hold the same substance.
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) return false;

    if (neighbor_type.is_full_cube()) {
        // Full cube only covers the face if the face reaches the cell boundary.
        // A wall back face at z=0.5, pole side at x=0.625, stair internal face etc.
        // don't reach the boundary, so the full cube neighbor doesn't cover them.
        switch (dir) {
            case FaceDirection::Top:    return self_max[1] >= 1.0f;
            case FaceDirection::Bottom: return self_min[1] <= 0.0f;
            case FaceDirection::Right:  return self_max[0] >= 1.0f;
            case FaceDirection::Left:   return self_min[0] <= 0.0f;
            case FaceDirection::Front:  return self_max[2] >= 1.0f;
            case FaceDirection::Back:   return self_min[2] <= 0.0f;
        }
        return true;
    }

    for (const auto& nb : neighbor_type.selection_boxes) {
        if (neighbor_box_covers(nb.min, nb.max, self_min, self_max, dir)) return true;
    }
    return false;
}

// Take the neighbour's coverage out of the pieces of a face this cell does not
// own. Pieces that end up empty are dropped; a piece that does not fit the buffer
// is kept whole, which is subtract_rect's rule and the safe direction — a
// redundant quad is invisible, a missing one is a hole.
[[nodiscard]] static int drop_neighbor_coverage(FaceDirection dir, const ShapeBoxes& neighbor,
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

// The six neighbours of the cell being meshed, resolved on demand. A cell asks a
// neighbour at most once per direction, and only for a shape that is resolved at
// all: this is ~4KB of stack, which is the price of not re-resolving the same
// neighbour once per box of the cell.
struct NeighborFaces {
    ShapeBoxes boxes[6];
    uint8_t state[6] = {0, 0, 0, 0, 0, 0};  // 0 = not asked yet, 1 = resolved
};

// shape_resolver.hpp re-declares the six faces as ShapeFace so core/ need not
// include the mesh layer. Same order, same values; this is what keeps that true.
static_assert(static_cast<uint8_t>(ShapeFace::Top) == static_cast<uint8_t>(FaceDirection::Top));
static_assert(static_cast<uint8_t>(ShapeFace::Bottom) == static_cast<uint8_t>(FaceDirection::Bottom));
static_assert(static_cast<uint8_t>(ShapeFace::Right) == static_cast<uint8_t>(FaceDirection::Right));
static_assert(static_cast<uint8_t>(ShapeFace::Left) == static_cast<uint8_t>(FaceDirection::Left));
static_assert(static_cast<uint8_t>(ShapeFace::Front) == static_cast<uint8_t>(FaceDirection::Front));
static_assert(static_cast<uint8_t>(ShapeFace::Back) == static_cast<uint8_t>(FaceDirection::Back));

// The neighbour a shape part's claim is tested against. Same accessor the face
// culling reads, so a fence arm and the culling of the faces around it agree.
void MeshBuilder::resolve_neighbor_shape(const ChunkNeighborAccessor& accessor,
                                         const BlockRegistry& registry,
                                         const BlockType& neighbor_type, int32_t nx, int32_t ny,
                                         int32_t nz, ShapeBoxes& out) {
    // The cell next door, asked the same question the block's own mesh answers.
    // Stride 1 because the per-AABB emitters are the only callers and both guard
    // on `stride_xz_ <= 1`; above that a shape is drawn as a full cube instead.
    ShapeNeighborContext ctx{&accessor, nx, ny, nz, 1};
    resolve_shape_boxes(neighbor_type, registry,
                        ShapeNeighborFn{&shape_neighbor_lookup, &ctx},
                        ShapeBoxKind::Selection, out);
}

BlockID MeshBuilder::shape_neighbor_lookup(void* ctx, ShapeFace face) {
    const ShapeNeighborContext& c = *static_cast<ShapeNeighborContext*>(ctx);
    const int32_t d = static_cast<int32_t>(face);
    return c.accessor->get_block(c.x + kDirectionOffsets[d][0] * c.stride_xz,
                                 c.y + kDirectionOffsets[d][1],
                                 c.z + kDirectionOffsets[d][2] * c.stride_xz);
}

// -------------------------------------------------------------------------
// Face emission driver
// -------------------------------------------------------------------------
void MeshBuilder::emit_faces(const ChunkData& chunk, const BlockRegistry& registry) {
    if (passive_greedy_enabled) {
        {
            ScopedTimer greedy_h_timer(perf_timer, TimerID::GreedyMeshHorizontal);
            passive_greedy_mesh_horizontal(chunk, accessor, FaceDirection::Top, registry);
            // Bottom faces are never visible from a ground-level / top-down view.
            // Skipping them saves ~1/6 of mesh build time and reduces GPU upload bytes.
            // passive_greedy_mesh_horizontal(chunk, accessor, FaceDirection::Bottom, registry);
        }
        {
            ScopedTimer greedy_v_timer(perf_timer, TimerID::GreedyMeshVertical);
            passive_greedy_mesh_vertical(chunk, accessor, registry);
        }
        // Non-full blocks can't participate in greedy merging (the greedy passes
        // skip them), so emit their faces separately via per-AABB geometry.
        // At LOD stride > 1 all blocks are treated as full cubes by the greedy
        // passes, so this pass is unnecessary.
        if (stride_xz_ <= 1) {
        for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
            if (chunk.is_section_all_air(s)) continue;
            int32_t y0 = s * SECTION_HEIGHT;
            int32_t y1 = y0 + SECTION_HEIGHT;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
                    for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
                        if (partial_mode_ &&
                            !(x < partial_bounds_.x_max && x + stride_xz_ > partial_bounds_.x_min &&
                              y >= partial_bounds_.y_min && y < partial_bounds_.y_max &&
                              z < partial_bounds_.z_max && z + stride_xz_ > partial_bounds_.z_min)) {
                            continue;
                        }
                        const BlockID block_id = solid_at(y, z + 1, x + 1);
                        if (block_id == BlockIDs::AIR) continue;
                        // Liquids are drawn by the fluid pass, never here (see
                        // is_fluid_drawn).
                        if (is_fluid_drawn(block_id, registry)) continue;
                        const BlockType& bt = registry.get_block_fast(block_id);
                        if (bt.greedy_mergeable) continue;
                        // Neighbour-dependent parts resolve here, against the same
                        // accessor the culling below uses.
                        ShapeNeighborContext shape_ctx{&accessor, x, y, z, stride_xz_};
                        ShapeBoxes boxes;
                        resolve_shape_boxes(bt, registry,
                                            ShapeNeighborFn{&shape_neighbor_lookup, &shape_ctx},
                                            ShapeBoxKind::Selection, boxes);
                        NeighborFaces neighbor_faces;
                        for (uint8_t bi = 0; bi < boxes.count(); ++bi) {
                            const BlockAABB& box = boxes[bi];
                            // Raised boxes emit their underside; floor boxes and
                            // boxes whose underside is buried in a sibling box do
                            // not. See skip_bottom_face above.
                            const bool skip_bottom = skip_bottom_face(boxes, box);
                            for (int i = 0; i < 6; i++) {
                                FaceDirection dir = kAllDirections[i];
                                if (dir == FaceDirection::Bottom && skip_bottom) continue;
                                int32_t dir_idx = static_cast<int32_t>(dir);
                                int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                                int32_t ny = y + kDirectionOffsets[dir_idx][1];
                                int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;
                                BlockID neighbor = accessor.get_block(nx, ny, nz);
                                const ShapeBoxes* neighbor_boxes = nullptr;
                                bool face_hidden = false;
                                if (neighbor != BlockIDs::AIR) {
                                    const BlockType& neighbor_type = registry.get_block(neighbor);
                                    if (neighbor_type.parts.empty() ||
                                        HasProperty(neighbor_type.properties,
                                                    BlockProperty::Transparent)) {
                                        face_hidden = should_cull_aabb_face(box.min, box.max, dir,
                                                                           neighbor_type);
                                    } else {
                                        // A shape resolved per cell: ask what it is
                                        // drawing over there. See the note above the
                                        // culling.
                                        if (neighbor_faces.state[dir_idx] == 0) {
                                            resolve_neighbor_shape(accessor, registry, neighbor_type,
                                                                   nx, ny, nz,
                                                                   neighbor_faces.boxes[dir_idx]);
                                            neighbor_faces.state[dir_idx] = 1;
                                        }
                                        const ShapeBoxes& nb_boxes =
                                            neighbor_faces.boxes[dir_idx];
                                        face_hidden = neighbor_boxes_cover(nb_boxes, box.min,
                                                                           box.max, dir);
                                        if (!face_hidden) neighbor_boxes = &nb_boxes;
                                    }
                                }
                                if (!face_hidden) {
                                    // One quad per visible piece: a sibling box of the
                                    // same shape can cover part of this face, and two
                                    // coplanar quads in the same place z-fight. See
                                    // visible_face_rects. A face the cell next door also
                                    // reaches loses the part the neighbour draws: only
                                    // one of the two may draw the sliver they share,
                                    // see drop_neighbor_coverage.
                                    const FaceAxes ax = face_axes(dir);
                                    FaceRect rects[kFacePieceCap];
                                    int rect_count =
                                        visible_face_rects(boxes, box, dir, bi, rects);
                                    if (neighbor_boxes != nullptr) {
                                        rect_count = drop_neighbor_coverage(dir, *neighbor_boxes, box,
                                                                           rects, rect_count);
                                    }
                                    for (int r = 0; r < rect_count; ++r) {
                                        float lo[3] = {box.min[0], box.min[1], box.min[2]};
                                        float hi[3] = {box.max[0], box.max[1], box.max[2]};
                                        lo[ax.tangent_a] = rects[r].a0;
                                        hi[ax.tangent_a] = rects[r].a1;
                                        lo[ax.tangent_b] = rects[r].b0;
                                        hi[ax.tangent_b] = rects[r].b1;
                                        add_aabb_face(chunk, accessor, x, y, z, dir, block_id,
                                                      registry, lo, hi);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        } // stride_xz_ <= 1
    } else {
        for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
            if (chunk.is_section_all_air(s)) continue;
            int32_t y0 = s * SECTION_HEIGHT;
            int32_t y1 = y0 + SECTION_HEIGHT;
            for (int32_t y = y0; y < y1; y++) {
                for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
                    for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
                        if (partial_mode_ &&
                            !(x < partial_bounds_.x_max && x + stride_xz_ > partial_bounds_.x_min &&
                              y >= partial_bounds_.y_min && y < partial_bounds_.y_max &&
                              z < partial_bounds_.z_max && z + stride_xz_ > partial_bounds_.z_min)) {
                            continue;
                        }
                        const BlockID block_id = solid_at(y, z + 1, x + 1);
                        if (block_id == BlockIDs::AIR) continue;
                        // Liquids are drawn by the fluid pass at full detail, so
                        // this path must not draw them too. At LOD stride > 1 the
                        // fluid pass does not run at all (a corner height means
                        // nothing at that scale), and THIS path is then the only
                        // thing that draws water — so the skip has to be tied to
                        // the stride. Unguarded, it left LOD chunks with no water
                        // geometry whatsoever.
                        if (stride_xz_ <= 1 && is_fluid_drawn(block_id, registry)) continue;

                        const BlockType& bt = registry.get_block_fast(block_id);

                        // Non-full blocks: emit faces per selection AABB
                        // (at LOD stride > 1, treat them as full cubes instead)
                        if (stride_xz_ <= 1 && !bt.is_full_cube()) {
                            // Neighbour-dependent parts resolve here, against the
                            // same accessor the culling below uses.
                            ShapeNeighborContext shape_ctx{&accessor, x, y, z, stride_xz_};
                            ShapeBoxes boxes;
                            resolve_shape_boxes(bt, registry,
                                                ShapeNeighborFn{&shape_neighbor_lookup, &shape_ctx},
                                                ShapeBoxKind::Selection, boxes);
                            NeighborFaces neighbor_faces;
                            for (uint8_t bi = 0; bi < boxes.count(); ++bi) {
                                const BlockAABB& box = boxes[bi];
                                // Raised boxes emit their underside (see the
                                // passive path's note); floor boxes and buried
                                // undersides do not.
                                const bool skip_bottom = skip_bottom_face(boxes, box);
                                for (int i = 0; i < 6; i++) {
                                    FaceDirection dir = kAllDirections[i];
                                    if (dir == FaceDirection::Bottom && skip_bottom) continue;
                                    int32_t dir_idx = static_cast<int32_t>(dir);
                                    int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                                    int32_t ny = y + kDirectionOffsets[dir_idx][1];
                                    int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;
                                    BlockID neighbor = accessor.get_block(nx, ny, nz);
                                    const ShapeBoxes* neighbor_boxes = nullptr;
                                    bool face_hidden = false;
                                    if (neighbor != BlockIDs::AIR) {
                                        const BlockType& neighbor_type =
                                            registry.get_block(neighbor);
                                        if (neighbor_type.parts.empty() ||
                                            HasProperty(neighbor_type.properties,
                                                        BlockProperty::Transparent)) {
                                            face_hidden = should_cull_aabb_face(box.min, box.max, dir,
                                                                               neighbor_type);
                                        } else {
                                            // A shape resolved per cell: ask what it is
                                            // drawing over there. See the note above the
                                            // culling.
                                            if (neighbor_faces.state[dir_idx] == 0) {
                                                resolve_neighbor_shape(
                                                    accessor, registry, neighbor_type, nx, ny, nz,
                                                    neighbor_faces.boxes[dir_idx]);
                                                neighbor_faces.state[dir_idx] = 1;
                                            }
                                            const ShapeBoxes& nb_boxes =
                                                neighbor_faces.boxes[dir_idx];
                                            face_hidden = neighbor_boxes_cover(nb_boxes, box.min,
                                                                               box.max, dir);
                                            if (!face_hidden) neighbor_boxes = &nb_boxes;
                                        }
                                    }
                                    if (!face_hidden) {
                                        // One quad per visible piece; see
                                        // visible_face_rects for why. A face the cell
                                        // next door also reaches loses the part the
                                        // neighbour draws; see drop_neighbor_coverage.
                                        const FaceAxes ax = face_axes(dir);
                                        FaceRect rects[kFacePieceCap];
                                        int rect_count =
                                            visible_face_rects(boxes, box, dir, bi, rects);
                                        if (neighbor_boxes != nullptr) {
                                            rect_count = drop_neighbor_coverage(
                                                dir, *neighbor_boxes, box, rects, rect_count);
                                        }
                                        for (int r = 0; r < rect_count; ++r) {
                                            float lo[3] = {box.min[0], box.min[1], box.min[2]};
                                            float hi[3] = {box.max[0], box.max[1], box.max[2]};
                                            lo[ax.tangent_a] = rects[r].a0;
                                            hi[ax.tangent_a] = rects[r].a1;
                                            lo[ax.tangent_b] = rects[r].b0;
                                            hi[ax.tangent_b] = rects[r].b1;
                                            add_aabb_face(chunk, accessor, x, y, z, dir, block_id,
                                                          registry, lo, hi);
                                        }
                                    }
                                }
                            }
                            continue;
                        }

                        if (stride_xz_ == 1) {
                            bool all_surrounded = true;
                            for (int i = 0; i < 6; i++) {
                                int32_t nx = x + kDirectionOffsets[i][0];
                                int32_t ny = y + kDirectionOffsets[i][1];
                                int32_t nz = z + kDirectionOffsets[i][2];
                                if (nx < 0 || nx >= CHUNK_WIDTH ||
                                    ny < 0 || ny >= CHUNK_HEIGHT ||
                                    nz < 0 || nz >= CHUNK_DEPTH) {
                                    all_surrounded = false;
                                    break;
                                }
                                BlockID neighbor = solid_at(ny, nz + 1, nx + 1);
                                if (!should_cull_against_neighbor(chunk, block_id, neighbor, kAllDirections[i], x, y, z, registry)) {
                                    all_surrounded = false;
                                    break;
                                }
                            }
                            if (all_surrounded) continue;
                        }
                        for (int i = 0; i < 6; i++) {
                            FaceDirection dir = kAllDirections[i];
                            if (dir == FaceDirection::Bottom) continue;
                            int32_t dir_idx = static_cast<int32_t>(dir);
                            int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                            int32_t ny = y + kDirectionOffsets[dir_idx][1];
                            int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;

                            BlockID neighbor = accessor.get_block(nx, ny, nz);
                            if (!should_cull_against_neighbor(chunk, block_id, neighbor, dir, x, y, z, registry)) {
                                add_face(chunk, accessor, x, y, z, dir, block_id, registry);
                            }
                        }
                    }
                }
            }
        }
    }

    // Liquids last and separately: a liquid surface is a quad with four
    // independent corner heights (see mesh_fluid.hpp), which neither the greedy
    // passes nor the per-AABB path can produce — so at FULL detail those two skip
    // every liquid (is_fluid_drawn) and this pass draws them all. At LOD stride >
    // 1 this pass does not run (a corner height means nothing at that scale) and
    // the generic emitters draw liquids as plain boxes into the water buffer
    // instead, which is why their skip is tied to the stride.
    {
        ScopedTimer fluid_timer(perf_timer, TimerID::FluidMesh);
        passive_fluid_mesh(chunk, accessor, registry);
    }
}

// -------------------------------------------------------------------------
// Quad carry-forward helpers (partial remeshing)
// -------------------------------------------------------------------------
void MeshBuilder::append_quad(const CachedQuad& q) {
    auto& dest_vertices = q.water ? water_vertices : vertices;
    auto& dest_indices = q.water ? water_indices : indices;
    const uint32_t base = static_cast<uint32_t>(dest_vertices.size());
    for (int i = 0; i < 4; ++i) dest_vertices.push_back(q.verts[i]);
    for (int i = 0; i < 6; ++i) dest_indices.push_back(base + q.idx[i]);
    quads.push_back(q);
}

bool MeshBuilder::should_drop_quad(const CachedQuad& q) const {
    const SubChunkBounds& b = partial_bounds_;
    if (b.x_max <= b.x_min || b.y_max <= b.y_min || b.z_max <= b.z_min) return false;
    if (q.x + q.ex <= b.x_min || q.x >= b.x_max) return false;
    if (q.y + q.ey <= b.y_min || q.y >= b.y_max) return false;
    if (q.z + q.ez <= b.z_min || q.z >= b.z_max) return false;
    return true;
}

// -------------------------------------------------------------------------
// Face culling against neighbor chunks
// -------------------------------------------------------------------------
bool MeshBuilder::should_cull_against_neighbor(const ChunkData& chunk, BlockID current, BlockID neighbor,
                                                FaceDirection direction, int32_t x, int32_t y, int32_t z,
                                                const BlockRegistry& registry) const {
    if (neighbor == BlockIDs::AIR) {
         return false;
    }
    const BlockType& neighbor_type = registry.get_block(neighbor);
    const BlockType& current_type = registry.get_block(current);
    if (HasProperty(neighbor_type.properties, BlockProperty::Transparent)) {
        if (current != neighbor) {
            const bool both_liquid =
                HasProperty(neighbor_type.properties, BlockProperty::Liquid) &&
                HasProperty(current_type.properties, BlockProperty::Liquid);
            if (!both_liquid) {
                return false;
            }
            // Different-ID liquids stack throughout oceans (WATER bodies capped
            // by SURFACE_WATER). Their lowered selection boxes make both non-full
            // cubes whose AABBs never reach each other's planes, so the generic
            // path below would emit an interior top face one block below every
            // water surface. Vertical liquid-liquid interfaces are always
            // interior; lateral ones keep the height-aware handling below.
            if (direction == FaceDirection::Top || direction == FaceDirection::Bottom) {
                return true;
            }
        }
    }
    if (current == neighbor && current_type.cull_against_same) return true;

    // For non-full blocks, use AABB face culling
    if (!current_type.is_full_cube() && !current_type.selection_boxes.empty()) {
        for (const auto& box : current_type.selection_boxes) {
            if (!should_cull_aabb_face(box.min, box.max, direction, neighbor_type)) {
                return false;
            }
        }
        return true;
    }

    if (is_side_face(direction)) {
        float current_height = 1.0f - current_type.top_face_offset;
        float neighbor_height = 1.0f - neighbor_type.top_face_offset;
        if (neighbor_height < current_height) return false;
        if (neighbor_height > current_height) return true;
    }
    // Non-full neighbor blocks: only cull if their AABBs fully cover this face area
    if (!neighbor_type.is_full_cube() && !neighbor_type.selection_boxes.empty()) {
        float full_min[3] = {0.0f, 0.0f, 0.0f};
        float full_max[3] = {1.0f, 1.0f, 1.0f};
        return should_cull_aabb_face(full_min, full_max, direction, neighbor_type);
    }
    // Lowered blocks (top_face_offset > 0) are full-cube but their visual
    // top is below the cell boundary, so they don't cover the face above.
    if (neighbor_type.top_face_offset > 0.0f && direction == FaceDirection::Bottom) {
        return false;
    }
    return true;
}

bool MeshBuilder::boundary_face_fully_occluded(const ChunkData& current_chunk, const ChunkData* neighbor,
                                               FaceDirection dir, int32_t x, int32_t y, int32_t z,
                                               int32_t stride, BlockID current_block,
                                               const BlockRegistry& registry) const {
    if (!neighbor) return false;
    for (int32_t i = 0; i < stride; i++) {
        int32_t nx, nz;
        switch (dir) {
            case FaceDirection::Right: nx = 0;                nz = z + i; break;
            case FaceDirection::Left:  nx = CHUNK_WIDTH - 1;  nz = z + i; break;
            case FaceDirection::Front: nx = x + i;            nz = 0;     break;
            case FaceDirection::Back:  nx = x + i;            nz = CHUNK_DEPTH - 1; break;
            default: return false;
        }
        BlockID nb = neighbor->get_block_unsafe(nx, y, nz);
        if (!should_cull_against_neighbor(current_chunk, current_block, nb, dir, x, y, z, registry)) {
            return false;
        }
    }
    return true;
}

} // namespace VoxelEngine
