#include "mesh/mesh_builder.hpp"
#include "core/light_packing.hpp"
#include <cstddef>
#include <cstring>

namespace VoxelEngine {

static constexpr uint8_t kLightMergeThreshold = 2;

static inline bool lights_similar_enough(uint16_t a, uint16_t b) {
    uint8_t dr = (unpack_r(a) > unpack_r(b)) ? (unpack_r(a) - unpack_r(b)) : (unpack_r(b) - unpack_r(a));
    uint8_t dg = (unpack_g(a) > unpack_g(b)) ? (unpack_g(a) - unpack_g(b)) : (unpack_g(b) - unpack_g(a));
    uint8_t db = (unpack_b(a) > unpack_b(b)) ? (unpack_b(a) - unpack_b(b)) : (unpack_b(b) - unpack_b(a));
    uint8_t ds = (unpack_sky(a) > unpack_sky(b)) ? (unpack_sky(a) - unpack_sky(b)) : (unpack_sky(b) - unpack_sky(a));
    // Sky must count too: at a cave mouth / overhang the block channels can be
    // identical while sky steps 15->0, and a merged face spanning that boundary
    // would render flat (a hard border) instead of a smooth gradient.
    return dr <= kLightMergeThreshold && dg <= kLightMergeThreshold
        && db <= kLightMergeThreshold && ds <= kLightMergeThreshold;
}

// -------------------------------------------------------------------------
// Passive greedy meshing
// -------------------------------------------------------------------------
void MeshBuilder::flush_horizontal_merge(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                          int32_t z_start, int32_t z_end,
                                          int32_t y, int32_t x, FaceDirection direction,
                                          BlockID block_id, uint16_t light_key, int rotation,
                                          const float ao[4], const BlockRegistry& registry) {
    if (z_start < 0 || z_end <= z_start) return;

    // Partial rebuilds: only flush runs that intersect the dirty region.
    if (partial_mode_ && !(z_start < partial_bounds_.z_max && z_end > partial_bounds_.z_min)) return;

    if (z_end - z_start > 1) {
        bool has_occlusion = (ao[0] < 1.0f || ao[1] < 1.0f || ao[2] < 1.0f || ao[3] < 1.0f);
        if (!has_occlusion) {
            Face face;
            face.x = x;
            face.y = y;
            face.z = z_start;
            face.direction = direction;
            face.block_id = block_id;
            face.u_max = stride_xz_ - 1;
            face.v_max = (z_end - 1) - z_start;
            add_greedy_face(chunk, accessor, face, light_key, rotation, ao, registry);
            return;
        }
    }

    for (int32_t cz = z_start; cz < z_end; cz += stride_xz_) {
        // Partial rebuilds: a run crossing the region boundary must not re-emit
        // the cells outside it — those are carried forward from the cache.
        if (partial_mode_ &&
            !(cz < partial_bounds_.z_max && cz + stride_xz_ > partial_bounds_.z_min)) {
            continue;
        }
        add_face(chunk, accessor, x, y, cz, direction, block_id, registry);
    }
}

void MeshBuilder::passive_greedy_mesh_horizontal(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                                 FaceDirection direction, const BlockRegistry& registry) {
    if (direction != FaceDirection::Top && direction != FaceDirection::Bottom) {
        return;
    }

    const int dir_idx = static_cast<int>(direction);
    const int32_t dx = kDirectionOffsets[dir_idx][0];
    const int32_t dy = kDirectionOffsets[dir_idx][1];
    const int32_t dz = kDirectionOffsets[dir_idx][2];

    for (int32_t s = 0; s < CHUNK_SECTIONS; s++) {
        if (chunk.is_section_all_air(s)) continue;
        int32_t y0 = s * SECTION_HEIGHT;
        int32_t y1 = y0 + SECTION_HEIGHT;
        for (int32_t y = y0; y < y1; y++) {
            const int32_t nybase = y + dy;
            for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
                // Partial rebuilds: skip rows whose faces cannot intersect the region.
                if (partial_mode_ &&
                    !(y >= partial_bounds_.y_min && y < partial_bounds_.y_max &&
                      x < partial_bounds_.x_max && x + stride_xz_ > partial_bounds_.x_min)) {
                    continue;
                }
                int32_t merge_start = -1;
                BlockID current_block = BlockIDs::AIR;
                uint16_t current_light_key = 0;
                int current_rotation = 0;
                float current_ao[4]{};

                for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
                    BlockID block_id = solid_at(y, z + 1, x + 1);

                    if (block_id == BlockIDs::AIR) {
                        flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                               current_block, current_light_key, current_rotation, current_ao, registry);
                        merge_start = -1;
                        continue;
                    }

                    // Non-full blocks can't participate in greedy merging
                    if (!registry.get_block_fast(block_id).is_full_cube()) {
                        flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                               current_block, current_light_key, current_rotation, current_ao, registry);
                        merge_start = -1;
                        continue;
                    }

                    if (direction == FaceDirection::Top && y < CHUNK_HEIGHT - 1
                        && solid_at(y + 1, z + 1, x + 1) != BlockIDs::AIR) {
                        const BlockType& nbt = registry.get_block(solid_at(y + 1, z + 1, x + 1));
                        const BlockType& bt = registry.get_block(block_id);
                        if (nbt.is_full_cube()
                            && HasProperty(bt.properties, BlockProperty::Solid)
                            && bt.top_face_offset == 0.0f
                            && !HasProperty(nbt.properties, BlockProperty::Transparent)) {
                            flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                                   current_block, current_light_key, current_rotation, current_ao, registry);
                            merge_start = -1;
                            continue;
                        }
                    }

                    BlockID neighbor = accessor.get_block(x, nybase, z);

                    bool cull = should_cull_against_neighbor(chunk, block_id, neighbor, direction, x, y, z, registry);

                    if (cull) {
                        flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                               current_block, current_light_key, current_rotation, current_ao, registry);
                        merge_start = -1;
                        continue;
                    }

                    int rotation = get_face_rotation(block_id, x, y, z, direction, dir_idx);
                    const uint16_t light_key = accessor.get_light_packed(x + dx, nybase, z + dz);

                    if (merge_start != -1 && block_id == current_block
                        && (z - merge_start) < kMaxGreedyMergeDistance
                        && lights_similar_enough(light_key, current_light_key)
                        && rotation == current_rotation) {
                        float block_ao[4];
                        ao.compute_face(accessor, registry, x, y, z, direction, block_ao, stride_xz_, true);
                        if (block_ao[0] == current_ao[0] && block_ao[1] == current_ao[1]
                            && block_ao[2] == current_ao[2] && block_ao[3] == current_ao[3]) {
                            continue;
                        }
                        flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                               current_block, current_light_key, current_rotation, current_ao, registry);
                        merge_start = z;
                        current_block = block_id;
                        current_rotation = rotation;
                        current_light_key = light_key;
                        memcpy(current_ao, block_ao, sizeof(current_ao));
                        continue;
                    }

                    if (merge_start != -1) {
                        flush_horizontal_merge(chunk, accessor, merge_start, z, y, x, direction,
                                               current_block, current_light_key, current_rotation, current_ao, registry);
                    }

                    merge_start = z;
                    current_block = block_id;
                    current_rotation = rotation;
                    current_light_key = light_key;
                    ao.compute_face(accessor, registry, x, y, z, direction, current_ao, stride_xz_, true);
                }

                if (merge_start != -1) {
                    flush_horizontal_merge(chunk, accessor, merge_start, CHUNK_DEPTH, y, x, direction,
                                           current_block, current_light_key, current_rotation, current_ao, registry);
                }
            }
        }
    }
}

// -------------------------------------------------------------------------
// Passive vertical greedy meshing (1D Y-axis merge for side faces)
// -------------------------------------------------------------------------
void MeshBuilder::flush_vertical_merge(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                          int32_t y_start, int32_t y_end,
                                          int32_t x, int32_t z, FaceDirection direction,
                                          BlockID block_id, uint16_t light_key, int rotation,
                                          const float ao[4], const BlockRegistry& registry) {
    if (y_start < 0 || y_end <= y_start) return;

    // Partial rebuilds: only flush runs that intersect the dirty region.
    if (partial_mode_ && !(y_start < partial_bounds_.y_max && y_end > partial_bounds_.y_min)) return;

    if (y_end - y_start > 1) {
        ++greedy_v_stats_local.merge_attempts;
        ++greedy_v_stats_local.merge_successes;
        Face face;
        face.x = x;
        face.y = y_start;
        face.z = z;
        face.direction = direction;
        face.block_id = block_id;
        face.u_max = stride_xz_ - 1;
        face.v_max = (y_end - 1) - y_start;
        add_greedy_face(chunk, accessor, face, light_key, rotation, ao, registry);
    } else {
        add_face(chunk, accessor, x, y_start, z, direction, block_id, registry);
    }
}

void MeshBuilder::passive_greedy_mesh_vertical(const ChunkData& chunk, const ChunkNeighborAccessor& accessor,
                                                const BlockRegistry& registry) {
    const ChunkData* right_chunk = accessor.pos_x;
    const ChunkData* left_chunk = accessor.neg_x;
    const ChunkData* front_chunk = accessor.pos_z;
    const ChunkData* back_chunk = accessor.neg_z;

    static constexpr int kDirCount = 4;
    static constexpr FaceDirection kDirs[kDirCount] = {
        FaceDirection::Right, FaceDirection::Left,
        FaceDirection::Front, FaceDirection::Back
    };
    static constexpr int kDirIndices[kDirCount] = {
        static_cast<int>(FaceDirection::Right),
        static_cast<int>(FaceDirection::Left),
        static_cast<int>(FaceDirection::Front),
        static_cast<int>(FaceDirection::Back)
    };
    static constexpr int kNxOff[kDirCount] = {1, -1, 0, 0};
    static constexpr int kNzOff[kDirCount] = {0, 0, 1, -1};
    const ChunkData* kBChunks[kDirCount] = {
        right_chunk, left_chunk, front_chunk, back_chunk
    };

    struct DirMergeState {
        int32_t merge_start = -1;
        BlockID current_block = BlockIDs::AIR;
        uint16_t current_light_key = 0;
        int current_rotation = 0;
        float current_ao[4]{};
        bool ao_valid = false;
    };

    for (int32_t x = 0; x < CHUNK_WIDTH; x += stride_xz_) {
        for (int32_t z = 0; z < CHUNK_DEPTH; z += stride_xz_) {
            // Partial rebuilds: skip columns whose faces cannot intersect the region.
            if (partial_mode_ &&
                !(x < partial_bounds_.x_max && x + stride_xz_ > partial_bounds_.x_min &&
                  z < partial_bounds_.z_max && z + stride_xz_ > partial_bounds_.z_min)) {
                continue;
            }
            DirMergeState dirs[kDirCount];

            const bool boundary[kDirCount] = {
                (x + stride_xz_ >= CHUNK_WIDTH),  // Right
                (x < stride_xz_),                   // Left
                (z + stride_xz_ >= CHUNK_DEPTH),   // Front
                (z < stride_xz_)                    // Back
            };

            for (int32_t s = 0; s < CHUNK_SECTIONS; ++s) {
                int32_t y0 = s * SECTION_HEIGHT;
                int32_t y1 = y0 + SECTION_HEIGHT;
                if (chunk.is_section_all_air(s)) {
                    for (int d = 0; d < kDirCount; d++) {
                        auto& dst = dirs[d];
                        flush_vertical_merge(chunk, accessor, dst.merge_start, y0, x, z,
                                            kDirs[d], dst.current_block, dst.current_light_key,
                                            dst.current_rotation, dst.current_ao, registry);
                        dst.merge_start = -1;
                        dst.ao_valid = false;
                    }
                    continue;
                }

                for (int32_t y = y0; y < y1; y++) {
                    BlockID block_id = solid_at(y, z + 1, x + 1);

                    if (stride_xz_ > 1) {
                        greedy_v_stats_local.lod_cells_visited++;
                        if (block_id == BlockIDs::AIR) greedy_v_stats_local.lod_cells_skipped_air++;
                    }

                    if (block_id == BlockIDs::AIR) {
                        for (int d = 0; d < kDirCount; d++) {
                            auto& dst = dirs[d];
                            flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                kDirs[d], dst.current_block, dst.current_light_key,
                                                dst.current_rotation, dst.current_ao, registry);
                            dst.merge_start = -1;
                            dst.ao_valid = false;
                        }
                        continue;
                    }

                    // Non-full blocks can't participate in greedy merging
                    if (!registry.get_block_fast(block_id).is_full_cube()) {
                        for (int d = 0; d < kDirCount; d++) {
                            auto& dst = dirs[d];
                            flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                kDirs[d], dst.current_block, dst.current_light_key,
                                                dst.current_rotation, dst.current_ao, registry);
                            dst.merge_start = -1;
                            dst.ao_valid = false;
                        }
                        continue;
                    }

                    if (stride_xz_ == 1) {
                    const int sx0 = x + 1 - 1, sx1 = x + 1 + 1;
                    const int sz0 = z + 1 - 1, sz1 = z + 1 + 1;
                    if ((solid_at(y, z + 1, sx0) != BlockIDs::AIR)
                        && (solid_at(y, z + 1, sx1) != BlockIDs::AIR)
                        && (solid_at(y, sz0, x + 1) != BlockIDs::AIR)
                        && (solid_at(y, sz1, x + 1) != BlockIDs::AIR)
                        && !boundary[0] && !boundary[1] && !boundary[2] && !boundary[3]) {
                        const BlockType& bt = registry.get_block(block_id);
                        if (HasProperty(bt.properties, BlockProperty::Solid)
                            && bt.top_face_offset == 0.0f) {
                            const BlockType& n_xneg = registry.get_block_fast(solid_at(y, z + 1, sx0));
                            const BlockType& n_xpos = registry.get_block_fast(solid_at(y, z + 1, sx1));
                            const BlockType& n_zneg = registry.get_block_fast(solid_at(y, sz0, x + 1));
                            const BlockType& n_zpos = registry.get_block_fast(solid_at(y, sz1, x + 1));
                            if (n_xneg.is_full_cube()
                                && n_xpos.is_full_cube()
                                && n_zneg.is_full_cube()
                                && n_zpos.is_full_cube()
                                && n_xneg.top_face_offset == 0.0f
                                && n_xpos.top_face_offset == 0.0f
                                && n_zneg.top_face_offset == 0.0f
                                && n_zpos.top_face_offset == 0.0f) {
                                for (int d = 0; d < kDirCount; d++) {
                                    auto& dst = dirs[d];
                                    flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                        kDirs[d], dst.current_block, dst.current_light_key,
                                                        dst.current_rotation, dst.current_ao, registry);
                                    dst.merge_start = -1;
                                    dst.ao_valid = false;
                                }
                                continue;
                            }
                        }
                    }
                    } // if (stride_xz_ == 1)

                    for (int d = 0; d < kDirCount; d++) {
                        auto& dst = dirs[d];

                        bool cull;
                        if (boundary[d]) {
                            if (!kBChunks[d]) {
                                cull = false;
                            } else {
                                cull = boundary_face_fully_occluded(chunk, kBChunks[d], kDirs[d],
                                                                    x, y, z, stride_xz_, block_id, registry);
                            }
                        } else {
                            const int sx = x + 1 + kNxOff[d] * stride_xz_;
                            const int sz = z + 1 + kNzOff[d] * stride_xz_;
                            BlockID neighbor = solid_at(y, sz, sx);
                            cull = should_cull_against_neighbor(chunk, block_id, neighbor, kDirs[d], x, y, z, registry);
                        }
                        if (stride_xz_ > 1) {
                            if (cull) greedy_v_stats_local.lod_faces_culled++;
                            else greedy_v_stats_local.lod_faces_emitted++;
                        }
                        if (cull) {
                            flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                kDirs[d], dst.current_block, dst.current_light_key,
                                                dst.current_rotation, dst.current_ao, registry);
                            dst.merge_start = -1;
                            dst.ao_valid = false;
                            continue;
                        }

                        uint16_t light_key = 0;
                        if (boundary[d]) {
                            if (kBChunks[d]) {
                                switch (kDirs[d]) {
                                    case FaceDirection::Right: light_key = kBChunks[d]->get_light_packed_word_unsafe(0, y, z); break;
                                    case FaceDirection::Left:  light_key = kBChunks[d]->get_light_packed_word_unsafe(CHUNK_WIDTH - 1, y, z); break;
                                    case FaceDirection::Front: light_key = kBChunks[d]->get_light_packed_word_unsafe(x, y, 0); break;
                                    case FaceDirection::Back:  light_key = kBChunks[d]->get_light_packed_word_unsafe(x, y, CHUNK_DEPTH - 1); break;
                                    default: break;
                                }
                            }
                        } else {
                            light_key = chunk.get_light_packed_word_unsafe(x + kNxOff[d] * stride_xz_, y, z + kNzOff[d] * stride_xz_);
                        }

                        const int rotation = get_face_rotation(block_id, x, y, z, kDirs[d], kDirIndices[d]);

                        if (dst.merge_start != -1) {
                            const bool same_block = block_id == dst.current_block;
                            const bool within_distance = (y - dst.merge_start) < kMaxGreedyMergeDistance;
                            const bool same_light = lights_similar_enough(light_key, dst.current_light_key);
                            const bool same_rotation = rotation == dst.current_rotation;
                            if (same_block && within_distance && same_light && same_rotation) {
                                float block_ao[4];
                                this->ao.compute_face(accessor, registry, x, y, z, kDirs[d], block_ao, stride_xz_, true);
                                if (!dst.ao_valid) {
                                    this->ao.compute_face(accessor, registry, x, dst.merge_start, z, kDirs[d], dst.current_ao, stride_xz_, true);
                                    dst.ao_valid = true;
                                }
                                if (block_ao[0] == dst.current_ao[0] && block_ao[1] == dst.current_ao[1]
                                    && block_ao[2] == dst.current_ao[2] && block_ao[3] == dst.current_ao[3]) {
                                    continue;
                                }
                                ++greedy_v_stats_local.reject_ao_mismatch;
                                flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                    kDirs[d], dst.current_block, dst.current_light_key,
                                                    dst.current_rotation, dst.current_ao, registry);
                                dst.merge_start = y;
                                dst.current_block = block_id;
                                dst.current_rotation = rotation;
                                dst.current_light_key = light_key;
                                memcpy(dst.current_ao, block_ao, sizeof(dst.current_ao));
                                dst.ao_valid = true;
                                continue;
                            }
                            if (!same_block) {
                                ++greedy_v_stats_local.reject_block_mismatch;
                            } else if (!same_light) {
                                ++greedy_v_stats_local.reject_light_mismatch;
                            } else if (!same_rotation) {
                                ++greedy_v_stats_local.reject_rotation_mismatch;
                            } else if (!within_distance) {
                                ++greedy_v_stats_local.reject_distance_limit;
                            }
                            flush_vertical_merge(chunk, accessor, dst.merge_start, y, x, z,
                                                kDirs[d], dst.current_block, dst.current_light_key,
                                                dst.current_rotation, dst.current_ao, registry);
                        }

                        dst.merge_start = y;
                        dst.current_block = block_id;
                        dst.current_rotation = rotation;
                        dst.current_light_key = light_key;
                        dst.ao_valid = false;
                    }
                }
            }

            for (int d = 0; d < kDirCount; d++) {
                auto& dst = dirs[d];
                if (dst.merge_start != -1) {
                    flush_vertical_merge(chunk, accessor, dst.merge_start, CHUNK_HEIGHT, x, z,
                                        kDirs[d], dst.current_block, dst.current_light_key,
                                        dst.current_rotation, dst.current_ao, registry);
                }
            }
        }
    }
}

} // namespace VoxelEngine
