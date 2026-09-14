#include "fluids/chunk_fluid.hpp"

#include "core/block_types.hpp"

#include <algorithm>

namespace VoxelEngine {
namespace fluids {

ChunkFluidWorld::CellSample ChunkFluidWorld::sample(const BlockRegistry& registry, BlockID id) noexcept {
    CellSample out;
    const BlockType& type = registry.get_block(id);
    out.blocks = type.blocks_fluid();
    if (type.is_fluid_state()) {
        out.fluid = FluidCell{ type.fluid_kind, type.fluid_depth, type.fluid_falling };
    }
    return out;
}

bool ChunkFluidWorld::read_window(const ChunkMap& map, int32_t x, int32_t y, int32_t z) noexcept {
    origin_x_ = x - kSearchRadius;
    origin_y_ = y - 1;
    origin_z_ = z - kSearchRadius;
    complete_ = true;

    // The window spans at most two chunks in x and z (11 < 2 * CHUNK_WIDTH) and
    // at most two in y, so eight keys cover it in the worst case.
    uint64_t keys[8];
    int key_count = 0;
    const int32_t min_cx = (origin_x_ >= 0 ? origin_x_ : origin_x_ - (CHUNK_WIDTH - 1)) / CHUNK_WIDTH;
    const int32_t max_cx = (origin_x_ + kWidth - 1) / CHUNK_WIDTH;
    const int32_t min_cy = (origin_y_ >= 0 ? origin_y_ : origin_y_ - (CHUNK_HEIGHT - 1)) / CHUNK_HEIGHT;
    const int32_t max_cy = (origin_y_ + kLayers - 1) / CHUNK_HEIGHT;
    const int32_t min_cz = (origin_z_ >= 0 ? origin_z_ : origin_z_ - (CHUNK_DEPTH - 1)) / CHUNK_DEPTH;
    const int32_t max_cz = (origin_z_ + kWidth - 1) / CHUNK_DEPTH;
    for (int32_t cy = min_cy; cy <= max_cy; ++cy) {
        for (int32_t cz = min_cz; cz <= max_cz; ++cz) {
            for (int32_t cx = min_cx; cx <= max_cx; ++cx) {
                if (key_count < 8) keys[key_count++] = map.get_chunk_key(cx, cy, cz);
            }
        }
    }

    auto lock = map.lock_keys(keys, static_cast<size_t>(key_count));
    const BlockRegistry& registry = *registry_;

    for (int py = 0; py < kLayers; ++py) {
        for (int pz = 0; pz < kWidth; ++pz) {
            for (int px = 0; px < kWidth; ++px) {
                const int32_t wx = origin_x_ + px;
                const int32_t wy = origin_y_ + py;
                const int32_t wz = origin_z_ + pz;
                CellSample& out = window_[window_index(py, pz, px)];

                if (wy < 0 || wy >= WORLD_HEIGHT_Y) {
                    // Outside the world is a wall, not air: bedrock under the
                    // lowest cell and no room above the highest. Reading it as
                    // passable would pour fluid out of the world forever.
                    out = CellSample{};
                    out.blocks = true;
                    continue;
                }
                int32_t cx = 0, cy = 0, cz = 0, lx = 0, ly = 0, lz = 0;
                world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
                const ChunkData* data = map.get_chunk_data_fast(cx, cy, cz);
                if (data == nullptr) {
                    // Not resident. The caller must not decide anything with this
                    // window, so it does not matter what the contents say.
                    complete_ = false;
                    out = CellSample{};
                    continue;
                }
                out = sample(registry, static_cast<BlockID>(data->get_block_unsafe(lx, ly, lz)));
            }
        }
    }
    return complete_;
}

FluidCell ChunkFluidWorld::fluid_at(int x, int y, int z) const {
    const int px = x - origin_x_;
    const int py = y - origin_y_;
    const int pz = z - origin_z_;
    if (px < 0 || py < 0 || pz < 0 || px >= kWidth || py >= kLayers || pz >= kWidth) {
        return FluidCell{};
    }
    return window_[window_index(py, pz, px)].fluid;
}

bool ChunkFluidWorld::blocked(int x, int y, int z) const {
    const int px = x - origin_x_;
    const int py = y - origin_y_;
    const int pz = z - origin_z_;
    if (px < 0 || py < 0 || pz < 0 || px >= kWidth || py >= kLayers || pz >= kWidth) {
        // Out of the window cannot happen for a rule call; "blocked" is the safe
        // answer if it ever does, because it stops fluid rather than releasing it.
        return true;
    }
    return window_[window_index(py, pz, px)].blocks;
}

int ChunkFluidWorld::apply_writes(const ChunkMap& map, FluidWriteSink* sink,
                                  const std::vector<CellWrite>& writes) {
    if (writes.empty()) return 0;

    // Group by chunk first. Writes come from one tick, so they cluster: a flood
    // front touches a handful of chunks, not hundreds.
    groups_.clear();
    for (const CellWrite& write : writes) {
        int32_t cx = 0, cy = 0, cz = 0, lx = 0, ly = 0, lz = 0;
        world_to_chunk_local(write.x, write.y, write.z, cx, cy, cz, lx, ly, lz);
        const uint64_t key = map.get_chunk_key(cx, cy, cz);
        ChunkGroup* group = nullptr;
        for (ChunkGroup& candidate : groups_) {
            if (candidate.key == key) { group = &candidate; break; }
        }
        if (group == nullptr) {
            groups_.emplace_back();
            group = &groups_.back();
            group->key = key;
            group->cx = cx;
            group->cy = cy;
            group->cz = cz;
        }
        group->records.push_back(FluidWriteRecord{ lx, ly, lz, table_->block_for(write.state), BlockIDs::AIR });
    }

    int applied = 0;
    for (ChunkGroup& group : groups_) {
        // 3x3x3 around the chunk: a write can touch light and the mesher's
        // neighbour queries, and the grid of keys is what the block editor uses
        // for the same reason.
        uint64_t keys[27];
        int index = 0;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    keys[index++] = map.get_chunk_key(group.cx + dx, group.cy + dy, group.cz + dz);
                }
            }
        }
        {
            auto lock = map.lock_keys_exclusive(keys);
            ChunkData* data = map.get_chunk_data_fast(group.cx, group.cy, group.cz);
            if (data == nullptr) {
                group.records.clear();  // unloaded under us: nothing to write into
                continue;
            }
            ChunkRenderData* render = map.get_chunk_render_data_fast(group.cx, group.cy, group.cz);
            const BlockRegistry& registry = *registry_;
            for (FluidWriteRecord& record : group.records) {
                const BlockID previous =
                    static_cast<BlockID>(data->get_block_unsafe(record.local_x, record.local_y, record.local_z));
                record.previous = previous;
                if (previous == record.block) continue;
                // Water states are all transparent and non-emissive, so a fluid
                // write cannot change lighting. Counted rather than assumed: if a
                // future fluid breaks that, this is where it shows up instead of
                // as a dark cave that will not light.
                const BlockType& before = registry.get_block(previous);
                const BlockType& after = registry.get_block(record.block);
                if (HasProperty(before.properties, BlockProperty::Opaque) !=
                        HasProperty(after.properties, BlockProperty::Opaque) ||
                    before.light_r != after.light_r || before.light_g != after.light_g ||
                    before.light_b != after.light_b) {
                    ++opacity_changes_;
                }
                data->set_block(record.local_x, record.local_y, record.local_z, record.block);
                ++applied;
                if (render != nullptr) {
                    render->is_mesh_dirty = true;
                    render->mesh_version++;
                    render->dirty_subchunks |=
                        static_cast<uint8_t>(1 << subchunk_index(record.local_x, record.local_y, record.local_z));
                    render->mark_block_dirty(record.local_x, record.local_y, record.local_z);
                }
            }
        }  // lock released before the sink runs: it must be free to lock again

        if (sink != nullptr && !group.records.empty()) {
            sink->on_chunk_updated(group.cx, group.cy, group.cz, group.records);
        } else {
            group.records.clear();
        }
    }
    return applied;
}

} // namespace fluids
} // namespace VoxelEngine
