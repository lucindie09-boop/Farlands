// Deciding that a face is invisible: one neighbour box against the face it would
// draw into, the same test asked of a whole neighbour chunk, and the two helpers the
// partial-remesh path uses to carry quads forward. The predicate both this and the
// faces half call is declared in mesh_builder_solid_internal.hpp.

#include "mesh/mesh_builder_solid_internal.hpp"

#include "core/block_types.hpp"

#include <algorithm>

namespace VoxelEngine {

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
