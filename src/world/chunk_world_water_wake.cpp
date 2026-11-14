#include "world/chunk_world.hpp"
#include "core/block_types.hpp"
#include "fluids/generated_water_wake.hpp"

#include <vector>

namespace VoxelEngine {

namespace {

// Cached for the same reason the editor caches them: a name lookup walks the
// registry through Godot's String, and this runs for every edit. An empty answer
// (registry not loaded) is retried on the next call.
BlockID generated_water_id() {
    static BlockID id = BlockIDs::AIR;
    if (id == BlockIDs::AIR) {
        id = BlockRegistry::get_instance().get_block_id_by_name("surface_water");
    }
    return id;
}

BlockID live_water_id() {
    static BlockID id = BlockIDs::AIR;
    if (id == BlockIDs::AIR) {
        id = BlockRegistry::get_instance().get_block_id_by_name("water");
    }
    return id;
}

} // namespace

int ChunkWorld::wake_generated_water(int32_t world_x, int32_t world_y, int32_t world_z) {
    if (waking_generated_water_) {
        return 0;
    }
    const int radius = fluids::water_wake_radius();
    if (radius <= 0) {
        return 0;
    }
    const BlockID generated = generated_water_id();
    const BlockID live = live_water_id();
    if (generated == BlockIDs::AIR || live == BlockIDs::AIR) {
        return 0;
    }

    waking_generated_water_ = true;
    std::vector<fluids::WakeCell> converted;

    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z,
                         local_x, local_y, local_z);

    {
        // The converted cells are within `radius` of the edit, so they span at most
        // one chunk either way: the same 3x3x3 band place_block already takes.
        uint64_t keys[27];
        int index = 0;
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    keys[index++] = chunk_map.get_chunk_key(chunk_x + dx, chunk_y + dy, chunk_z + dz);
                }
            }
        }
        auto lock = chunk_map.lock_keys_exclusive(keys);

        auto read = [&](int32_t wx, int32_t wy, int32_t wz) -> BlockID {
            int32_t cx, cy, cz, lx, ly, lz;
            world_to_chunk_local(wx, wy, wz, cx, cy, cz, lx, ly, lz);
            const ChunkData* data = chunk_map.get_chunk_data_fast(cx, cy, cz);
            return data != nullptr ? data->get_block_unsafe(lx, ly, lz) : static_cast<BlockID>(BlockIDs::AIR);
        };
        auto convert = [&](const fluids::WakeCell& cell) {
            int32_t cx, cy, cz, lx, ly, lz;
            world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
            ChunkData* data = chunk_map.get_chunk_data_fast(cx, cy, cz);
            if (data == nullptr) {
                return;
            }
            data->set_block(lx, ly, lz, live);
            if (ChunkRenderData* render_data = chunk_map.get_chunk_render_data_fast(cx, cy, cz)) {
                render_data->is_mesh_dirty = true;
                render_data->mesh_version++;
                render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(lx, ly, lz));
                render_data->mark_block_dirty(lx, ly, lz);
            }
            converted.push_back(cell);
        };
        fluids::find_generated_water(read, convert, generated, world_x, world_y, world_z, radius);
    }

    for (const fluids::WakeCell& cell : converted) {
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(cell.x, cell.y, cell.z, cx, cy, cz, lx, ly, lz);
        add_block_edit(cx, cy, cz, lx, ly, lz, live, false);
        // The listener wakes the simulation for each converted cell; the guard above
        // makes its own call back into this function a no-op.
        notify_block_change(cell.x, cell.y, cell.z);
    }
    waking_generated_water_ = false;
    return static_cast<int>(converted.size());
}

} // namespace VoxelEngine
