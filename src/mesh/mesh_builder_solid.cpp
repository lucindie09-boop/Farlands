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

} // namespace VoxelEngine
