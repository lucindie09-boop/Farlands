// The face-emission driver: the walk over a chunk's cells, what each block's face
// asks its neighbours, and the quads that come out of it. The geometry questions this
// asks (how many rectangles a face is, what the neighbour covers) are answered by
// mesh_builder_solid_faces.cpp through mesh_builder_solid_internal.hpp.

#include "mesh/mesh_builder_solid_internal.hpp"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

namespace {

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

} // namespace

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
            // Bottom faces are never visible from a ground-level / top-down view,
            // so the Bottom arm is deliberately not emitted: skipping it saves
            // ~1/6 of mesh build time and reduces GPU upload bytes.
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

} // namespace VoxelEngine
