#include "world/chunk_world.hpp"
#include <godot_cpp/classes/rendering_server.hpp>
#include "mesh/mesh_manager.hpp"

namespace VoxelEngine {

using namespace godot;

void ChunkWorld::mark_chunk_dirty(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
    dirty_chunks.insert(key);
}

void ChunkWorld::free_loaded_chunks() {
    RenderingServer* rs = RenderingServer::get_singleton();
    chunk_map.for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
        if (render_data->instance_rid.is_valid()) {
            rs->free_rid(render_data->instance_rid);
        }
        if (render_data->mesh_rid.is_valid()) {
            rs->free_rid(render_data->mesh_rid);
        }
    });
    chunk_map.clear();
}

bool ChunkWorld::try_unload_chunk(uint64_t key, MeshManager* mesh_mgr) {
    int32_t cx = 0, cy = 0, cz = 0;
    ChunkMap::decode_chunk_key(key, cx, cy, cz);
bool needs_save = false;
    auto render_data = chunk_map.find_and_erase_if(key, [this, key, &needs_save](const ChunkRenderData& rd) {
        if (rd.pending_mesh_builds.load(std::memory_order_relaxed) != 0) {
            return false;
        }
        if (rd.pending_mesh_uploads.load(std::memory_order_relaxed) != 0) {
            return false;
        }
needs_save = is_chunk_dirty(key);
        return true;
    });

    if (!render_data) {
        return !chunk_map.contains(key);
    }

if (needs_save) {
    // Hand the edit map to the background saver. If a save for this chunk
    // is already in flight it is superseded (generation bump), guaranteeing the
    // newest data is the one that reaches disk.
    std::lock_guard<std::mutex> lock(edit_maps_mutex);
    auto it = chunk_edit_maps.find(key);
    if (it != chunk_edit_maps.end()) {
        auto snapshot = std::make_unique<EditMap>();
        snapshot->edits = it->second.edits; // Deep copy
        enqueue_edit_map_save(key, cx, cy, cz, std::move(snapshot));
    }
}
    if (mesh_mgr) {
        mesh_mgr->notify_chunk_unloaded(cx, cy, cz, render_data.get());
        mesh_mgr->erase_urgent(key);
        ChunkRenderData* below = chunk_map.get_chunk_render_data(cx, cy - 1, cz);
        if (below && below->data && !below->data->is_all_air()) {
            below->is_mesh_dirty = true;
            mesh_mgr->queue_dirty_chunk(cx, cy - 1, cz);
        }
    }
    light_propagated_chunks.erase(key);
    {
        std::lock_guard<std::mutex> lock(pending_placement_mutex);
        pending_block_placements.erase(key);
    }
    RenderingServer* rs = RenderingServer::get_singleton();
    if (render_data->instance_rid.is_valid()) {
        rs->free_rid(render_data->instance_rid);
    }
    if (render_data->mesh_rid.is_valid()) {
        rs->free_rid(render_data->mesh_rid);
    }
    return true;
}

void ChunkWorld::clear() {
    {
        std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
        dirty_chunks.clear();
    }
    {
        std::lock_guard<std::mutex> lock(saves_in_flight_mutex);
        saves_in_flight.clear();
    }
    {
        std::lock_guard<std::mutex> lock(edit_maps_mutex);
        chunk_edit_maps.clear();
    }
    chunk_scheduler.clear();
    pending_chunk_installs.clear();
    pending_chunk_lighting.clear();
    pending_chunk_dirty_mesh.clear();
    light_propagated_chunks.clear();
    pending_block_placements.clear();
    pending_vegetation_placements.clear();
    chunk_map.clear();
    async_epoch.store(0, std::memory_order_release);
}

void ChunkWorld::queue_pending_placement(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);
    std::lock_guard<std::mutex> lock(pending_placement_mutex);
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    pending_block_placements[key].push_back({world_x, world_y, world_z, block_id});
}

void ChunkWorld::apply_pending_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data) {
    // Apply genuine player edits made at the loading frontier (chunk not yet loaded).
    // These ARE persisted to the edit map via add_block_edit.
    std::lock_guard<std::mutex> lock(pending_placement_mutex);
    auto it = pending_block_placements.find(key);
    if (it != pending_block_placements.end()) {
        for (const auto& placement : it->second) {
            int32_t lx = placement.world_x - chunk_x * CHUNK_WIDTH;
            int32_t ly = placement.world_y - chunk_y * CHUNK_HEIGHT;
            int32_t lz = placement.world_z - chunk_z * CHUNK_DEPTH;
            if (lx >= 0 && lx < CHUNK_WIDTH &&
                ly >= 0 && ly < CHUNK_HEIGHT &&
                lz >= 0 && lz < CHUNK_DEPTH) {
                render_data.data->set_block(lx, ly, lz, static_cast<BlockID>(placement.block_id));
                // Also persist this edit in the edit map (genuine player edit at loading frontier)
                add_block_edit(chunk_x, chunk_y, chunk_z, lx, ly, lz, static_cast<BlockID>(placement.block_id));
            }
        }
        pending_block_placements.erase(it);
        render_data.data->compute_section_flags();
        render_data.data->compute_fully_solid();
    }
}

void ChunkWorld::queue_vegetation_placement(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);
    std::lock_guard<std::mutex> lock(vegetation_placement_mutex);
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    pending_vegetation_placements[key].push_back({world_x, world_y, world_z, static_cast<int>(block_id)});
}

void ChunkWorld::apply_vegetation_placements(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkRenderData& render_data) {
    std::lock_guard<std::mutex> lock(vegetation_placement_mutex);
    auto it = pending_vegetation_placements.find(key);
    if (it != pending_vegetation_placements.end()) {
        for (const auto& placement : it->second) {
            int32_t lx = placement.world_x - chunk_x * CHUNK_WIDTH;
            int32_t ly = placement.world_y - chunk_y * CHUNK_HEIGHT;
            int32_t lz = placement.world_z - chunk_z * CHUNK_DEPTH;
            if (lx >= 0 && lx < CHUNK_WIDTH &&
                ly >= 0 && ly < CHUNK_HEIGHT &&
                lz >= 0 && lz < CHUNK_DEPTH) {
                render_data.data->set_block(lx, ly, lz, static_cast<BlockID>(placement.block_id));
                // DO NOT persist vegetation overflow - it's regenerated on load
            }
        }
        pending_vegetation_placements.erase(it);
        render_data.data->compute_section_flags();
        render_data.data->compute_fully_solid();
    }
}

void ChunkWorld::add_block_edit(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, int32_t local_x, int32_t local_y, int32_t local_z, BlockID block_id) {
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    std::lock_guard<std::mutex> lock(edit_maps_mutex);
    chunk_edit_maps[key].set_block(local_x, local_y, local_z, block_id);
    mark_chunk_dirty(chunk_x, chunk_y, chunk_z);
}

void ChunkWorld::apply_edit_map_to_chunk(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& chunk_data) {
    std::lock_guard<std::mutex> lock(edit_maps_mutex);
    auto it = chunk_edit_maps.find(key);
    if (it != chunk_edit_maps.end()) {
        for (const auto& entry : it->second.edits) {
            int32_t lx, ly, lz;
            EditMap::unpack_coord(entry.first, lx, ly, lz);
            chunk_data.set_block(lx, ly, lz, entry.second);
        }
        chunk_data.compute_section_flags();
        chunk_data.compute_fully_solid();
    }
}

} // namespace VoxelEngine
