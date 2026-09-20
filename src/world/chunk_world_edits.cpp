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
    // A pinned chunk is one a caller is still writing into: unloading it here
    // would only mean generating it again for the same write. Refused, and the
    // caller retries — the pin is released as soon as the caller is done.
    if (is_chunk_pinned(key)) return false;
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
    {
        std::lock_guard<std::mutex> lock(urgent_chunk_mutex);
        urgent_chunk_requests.clear();
        urgent_chunk_set.clear();
    }
    unpin_all_chunks();
    pending_vegetation_placements.clear();
    chunk_map.clear();
    async_epoch.store(0, std::memory_order_release);
}

void ChunkWorld::request_urgent_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    const uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    std::lock_guard<std::mutex> lock(urgent_chunk_mutex);
    // Asking twice for the same chunk is normal (a paste re-asks every frame until
    // the chunk arrives); the set is what makes it a no-op rather than a queue of
    // duplicates that would each be generated and thrown away.
    if (!urgent_chunk_set.insert(key).second) return;
    urgent_chunk_requests.push_back(key);
}

std::vector<ChunkPos> ChunkWorld::take_urgent_chunk_requests(size_t max) {
    std::vector<ChunkPos> out;
    std::lock_guard<std::mutex> lock(urgent_chunk_mutex);
    while (out.size() < max && !urgent_chunk_requests.empty()) {
        const uint64_t key = urgent_chunk_requests.front();
        urgent_chunk_requests.pop_front();
        urgent_chunk_set.erase(key);
        ChunkPos pos{};
        ChunkMap::decode_chunk_key(key, pos.x, pos.y, pos.z);
        out.push_back(pos);
    }
    return out;
}

size_t ChunkWorld::urgent_chunk_count() const {
    std::lock_guard<std::mutex> lock(urgent_chunk_mutex);
    return urgent_chunk_requests.size();
}

void ChunkWorld::pin_chunk(uint64_t key) {
    std::lock_guard<std::mutex> lock(pinned_chunk_mutex);
    pinned_chunks.insert(key);
}

void ChunkWorld::unpin_chunk(uint64_t key) {
    std::lock_guard<std::mutex> lock(pinned_chunk_mutex);
    pinned_chunks.erase(key);
}

bool ChunkWorld::is_chunk_pinned(uint64_t key) const {
    std::lock_guard<std::mutex> lock(pinned_chunk_mutex);
    return pinned_chunks.find(key) != pinned_chunks.end();
}

void ChunkWorld::unpin_all_chunks() {
    std::lock_guard<std::mutex> lock(pinned_chunk_mutex);
    pinned_chunks.clear();
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

void ChunkWorld::add_block_edit(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, int32_t local_x, int32_t local_y, int32_t local_z, BlockID block_id, bool notify) {
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    {
        std::lock_guard<std::mutex> lock(edit_maps_mutex);
        chunk_edit_maps[key].set_block(local_x, local_y, local_z, block_id);
    }
    mark_chunk_dirty(chunk_x, chunk_y, chunk_z);

    // Outside the edit-map lock on purpose: the listener reads the chunk map,
    // and taking a chunk lock while holding the edit-map lock is exactly the
    // lock order that deadlocks against a caller that does the reverse.
    if (notify && edit_listener) {
        edit_listener(chunk_x * CHUNK_WIDTH + local_x,
                      chunk_y * CHUNK_HEIGHT + local_y,
                      chunk_z * CHUNK_DEPTH + local_z);
    }
}

void ChunkWorld::add_block_edits(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                                 const std::vector<EditCell>& edits) {
    if (edits.empty()) return;
    const uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    const size_t incoming = edits.size();
    {
        std::lock_guard<std::mutex> lock(edit_maps_mutex);
        auto it = chunk_edit_maps.find(key);
        if (it == chunk_edit_maps.end()) {
            it = chunk_edit_maps.emplace(key, EditMap{}).first;
        }
        EditMap& map = it->second;
        // Sized once for the run: without this a chunkful of edits rehashes the map
        // a dozen times on the way in, which is a surprising share of a paste.
        map.edits.reserve(map.edits.size() + incoming);
        for (const EditCell& cell : edits) {
            map.set_block(cell.x, cell.y, cell.z, cell.block);
        }
    }

    // Once for the chunk, not once per cell: a dirty mark takes its own lock, and a
    // bulk writer marks the chunks it touches dirty itself anyway.
    mark_chunk_dirty(chunk_x, chunk_y, chunk_z);
}

void ChunkWorld::notify_block_change(int32_t world_x, int32_t world_y, int32_t world_z) {
    if (!edit_listener) return;
    edit_listener(world_x, world_y, world_z);
}

void ChunkWorld::apply_edit_map_to_chunk(uint64_t key, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, ChunkData& chunk_data) {
    // Positions to wake once the edit-map lock is released. Local coordinates
    // packed three to an int so this stays allocation-light on a hot path.
    std::vector<int32_t> fluid_cells;
    {
        std::lock_guard<std::mutex> lock(edit_maps_mutex);
        auto it = chunk_edit_maps.find(key);
        if (it != chunk_edit_maps.end()) {
            // Pure shared apply logic (see core/edit_map.*) — also exercised by tests.
            VoxelEngine::apply_edit_map_to_chunk(it->second, chunk_data);

            // Seed the fluid simulation from what the map records. This is the
            // whole of "a flood survives a reload": the fluid states ARE the
            // work list, so a chunk that comes back with water in it wakes that
            // water and its neighbours and the flow carries on from where it
            // stopped, with nothing saved about schedules.
            const BlockRegistry& registry = BlockRegistry::get_instance();
            for (const auto& entry : it->second.edits) {
                if (!registry.get_block(entry.second).is_fluid_state()) continue;
                int32_t local_x = 0, local_y = 0, local_z = 0;
                EditMap::unpack_coord(entry.first, local_x, local_y, local_z);
                fluid_cells.push_back(local_x);
                fluid_cells.push_back(local_y);
                fluid_cells.push_back(local_z);
            }
        }
    }

    // This runs on a chunk-generation WORKER (generate_chunk -> here), so the wake
    // goes through the worker listener, which posts rather than applies. Calling
    // the applying one from here raced the main thread's own sim ticks — see
    // FluidSim::post_block_changed for the crash that came out of it.
    if (worker_edit_listener) {
        for (size_t i = 0; i + 2 < fluid_cells.size(); i += 3) {
            worker_edit_listener(chunk_x * CHUNK_WIDTH + fluid_cells[i],
                                 chunk_y * CHUNK_HEIGHT + fluid_cells[i + 1],
                                 chunk_z * CHUNK_DEPTH + fluid_cells[i + 2]);
        }
    }
}

} // namespace VoxelEngine
