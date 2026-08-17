#include "mesh/mesh_builder.hpp"

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
                        for (int32_t dz = 0; dz < stride_xz_; ++dz) {
                            for (int32_t dx = 0; dx < stride_xz_; ++dx) {
                                BlockID sample = chunk.get_block_unsafe(x_src + dx, y, z_src + dz);
                                if (sample != BlockIDs::AIR) {
                                    if (representative == BlockIDs::AIR) representative = sample;
                                }
                            }
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
                solid_cache[y][z][0] = accessor.neg_x
                    ? accessor.neg_x->get_block_unsafe(CHUNK_WIDTH - 1, y, z_src)
                    : BlockIDs::AIR;
                solid_cache[y][z][SC_W - 1] = accessor.pos_x
                    ? accessor.pos_x->get_block_unsafe(0, y, z_src)
                    : BlockIDs::AIR;
            }
        }

        // Z boundaries
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            for (int32_t x = 1; x <= CHUNK_WIDTH; x++) {
                int32_t x_src = ((x - 1) / stride_xz_) * stride_xz_;
                solid_cache[y][0][x] = accessor.neg_z
                    ? accessor.neg_z->get_block_unsafe(x_src, y, CHUNK_DEPTH - 1)
                    : BlockIDs::AIR;
                solid_cache[y][SC_D - 1][x] = accessor.pos_z
                    ? accessor.pos_z->get_block_unsafe(x_src, y, 0)
                    : BlockIDs::AIR;
            }
        }

        // Four corner columns (x=0 or SC_W-1, z=0 or SC_D-1)
        for (int32_t y = 0; y < CHUNK_HEIGHT; y++) {
            solid_cache[y][0][0] = accessor.neg_x_neg_z
                ? accessor.neg_x_neg_z->get_block_unsafe(CHUNK_WIDTH - 1, y, CHUNK_DEPTH - 1)
                : BlockIDs::AIR;
            solid_cache[y][0][SC_W - 1] = accessor.pos_x_neg_z
                ? accessor.pos_x_neg_z->get_block_unsafe(0, y, CHUNK_DEPTH - 1)
                : BlockIDs::AIR;
            solid_cache[y][SC_D - 1][0] = accessor.neg_x_pos_z
                ? accessor.neg_x_pos_z->get_block_unsafe(CHUNK_WIDTH - 1, y, 0)
                : BlockIDs::AIR;
            solid_cache[y][SC_D - 1][SC_W - 1] = accessor.pos_x_pos_z
                ? accessor.pos_x_pos_z->get_block_unsafe(0, y, 0)
                : BlockIDs::AIR;
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
// -------------------------------------------------------------------------
bool MeshBuilder::should_cull_aabb_face(const float self_min[3], const float self_max[3],
                                         FaceDirection dir, const BlockType& neighbor_type) const {
    if (neighbor_type.is_full_cube()) return true;

    for (const auto& nb : neighbor_type.selection_boxes) {
        bool covers = true;
        switch (dir) {
            case FaceDirection::Top:
            case FaceDirection::Bottom:
                // Check XZ overlap: neighbor must cover same XZ area
                covers = nb.min[0] <= self_min[0] && nb.max[0] >= self_max[0] &&
                         nb.min[2] <= self_min[2] && nb.max[2] >= self_max[2];
                break;
            case FaceDirection::Right:
            case FaceDirection::Left:
                // Check YZ overlap
                covers = nb.min[1] <= self_min[1] && nb.max[1] >= self_max[1] &&
                         nb.min[2] <= self_min[2] && nb.max[2] >= self_max[2];
                break;
            case FaceDirection::Front:
            case FaceDirection::Back:
                // Check XY overlap
                covers = nb.min[0] <= self_min[0] && nb.max[0] >= self_max[0] &&
                         nb.min[1] <= self_min[1] && nb.max[1] >= self_max[1];
                break;
        }
        if (covers) return true;
    }
    return false;
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
                        const BlockType& bt = registry.get_block_fast(block_id);
                        if (bt.is_full_cube()) continue;
                        for (const auto& box : bt.selection_boxes) {
                            for (int i = 0; i < 6; i++) {
                                FaceDirection dir = kAllDirections[i];
                                if (dir == FaceDirection::Bottom) continue;
                                int32_t dir_idx = static_cast<int32_t>(dir);
                                int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                                int32_t ny = y + kDirectionOffsets[dir_idx][1];
                                int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;
                                BlockID neighbor = accessor.get_block(nx, ny, nz);
                                const BlockType& neighbor_type = registry.get_block(neighbor);
                                if (neighbor == BlockIDs::AIR ||
                                    !should_cull_aabb_face(box.min, box.max, dir, neighbor_type)) {
                                    add_aabb_face(chunk, accessor, x, y, z, dir, block_id, registry,
                                                  box.min, box.max);
                                }
                            }
                        }
                    }
                }
            }
        }
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

                        const BlockType& bt = registry.get_block_fast(block_id);

                        // Non-full blocks: emit faces per selection AABB
                        if (!bt.is_full_cube()) {
                            for (const auto& box : bt.selection_boxes) {
                                for (int i = 0; i < 6; i++) {
                                    FaceDirection dir = kAllDirections[i];
                                    if (dir == FaceDirection::Bottom) continue;
                                    int32_t dir_idx = static_cast<int32_t>(dir);
                                    int32_t nx = x + kDirectionOffsets[dir_idx][0] * stride_xz_;
                                    int32_t ny = y + kDirectionOffsets[dir_idx][1];
                                    int32_t nz = z + kDirectionOffsets[dir_idx][2] * stride_xz_;
                                    BlockID neighbor = accessor.get_block(nx, ny, nz);
                                    const BlockType& neighbor_type = registry.get_block(neighbor);
                                    if (neighbor == BlockIDs::AIR ||
                                        !should_cull_aabb_face(box.min, box.max, dir, neighbor_type)) {
                                        add_aabb_face(chunk, accessor, x, y, z, dir, block_id, registry,
                                                      box.min, box.max);
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
            if (!HasProperty(neighbor_type.properties, BlockProperty::Liquid) ||
                !HasProperty(current_type.properties, BlockProperty::Liquid)) {
                return false;
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
