#include "world/chunk_world.hpp"
#include "core/edit_map.hpp"

namespace VoxelEngine {

using namespace godot;

void ChunkWorld::flush_dirty_chunks(bool wait_for_completion, double timeout_sec) {
    std::vector<uint64_t> keys;
    {
        std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
        keys.assign(dirty_chunks.begin(), dirty_chunks.end());
    }

    if (!thread_pool) {
        // Defensive fallback (thread pool unavailable): synchronous save.
        for (uint64_t key : keys) {
            int32_t cx = 0, cy = 0, cz = 0;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            save_chunk_to_disk(cx, cy, cz);
        }
        {
            std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
            for (uint64_t key : keys) dirty_chunks.erase(key);
        }
        return;
    }

    for (uint64_t key : keys) {
        // Periodic flush: skip chunks already being written (they stay dirty and
        // get picked up next cycle). Quit flush: supersede instead so the very
        // latest state is what actually reaches disk before teardown.
        if (!wait_for_completion && is_save_in_flight(key)) {
            continue;
        }

        int32_t cx = 0, cy = 0, cz = 0;
        ChunkMap::decode_chunk_key(key, cx, cy, cz);

        // Deep-copy the edit map so the background writer never reads a half-mutated map.
        std::unique_ptr<EditMap> snapshot;
        {
            std::lock_guard<std::mutex> lock(edit_maps_mutex);
            auto it = chunk_edit_maps.find(key);
            if (it != chunk_edit_maps.end()) {
                snapshot = std::make_unique<EditMap>();
                snapshot->edits = it->second.edits; // Deep copy the unordered_map
            }
        }
        if (!snapshot || snapshot->empty()) {
            // No edits to save — this shouldn't happen since we only mark dirty when there are edits
            std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
            dirty_chunks.erase(key);
            continue;
        }

        // Snapshot is taken: safe to clear the dirty flag immediately. Any later
        // edit re-marks it through the normal edit path.
        {
            std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
            dirty_chunks.erase(key);
        }

        enqueue_edit_map_save(key, cx, cy, cz, std::move(snapshot));
    }

    if (wait_for_completion) {
        wait_for_saves(timeout_sec);
    }
}

void ChunkWorld::enqueue_edit_map_save(uint64_t key, int32_t cx, int32_t cy, int32_t cz,
                                      std::unique_ptr<EditMap> snapshot) {
    if (!thread_pool) return;
    const uint64_t epoch = async_epoch.load(std::memory_order_acquire);
    const uint64_t generation = next_save_generation.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(saves_in_flight_mutex);
        // Overwrites any older in-flight task for this chunk -> it aborts at its
        // generation gate instead of clobbering this newer snapshot.
        saves_in_flight[key] = generation;
    }
    outstanding_saves.fetch_add(1, std::memory_order_acq_rel);
    thread_pool->fire_and_forget([this, key, cx, cy, cz,
                                  snapshot = std::move(snapshot), epoch, generation]() mutable {
        save_edit_map_snapshot(key, cx, cy, cz, std::move(snapshot), epoch, generation);
    });
}

void ChunkWorld::save_edit_map_snapshot(uint64_t key, int32_t cx, int32_t cy, int32_t cz,
                                       std::unique_ptr<EditMap> snapshot,
                                       uint64_t epoch, uint64_t generation) {
    bool wrote = false;
    // Epoch gate: abort if the world was reset while we were queued, so stale
    // data never lands in a fresh world's chunk files.
    if (async_epoch.load(std::memory_order_acquire) == epoch && snapshot) {
        // file_access_mutex serializes all writes; the generation gate is checked
        // while holding it so a superseded task aborts instead of racing a newer one.
        std::lock_guard<std::mutex> lock(file_access_mutex);
        {
            std::lock_guard<std::mutex> ilock(saves_in_flight_mutex);
            auto it = saves_in_flight.find(key);
            wrote = (it != saves_in_flight.end() && it->second == generation);
        }
        if (wrote) {
            write_edit_map_file_locked(cx, cy, cz, *snapshot);
        }
    }
    {
        std::lock_guard<std::mutex> ilock(saves_in_flight_mutex);
        auto it = saves_in_flight.find(key);
        if (it != saves_in_flight.end() && it->second == generation) {
            saves_in_flight.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> lock(saves_cv_mutex);
        outstanding_saves.fetch_sub(1, std::memory_order_acq_rel);
        saves_cv.notify_all();
    }
}

bool ChunkWorld::is_save_in_flight(uint64_t key) const {
    std::lock_guard<std::mutex> lock(saves_in_flight_mutex);
    return saves_in_flight.find(key) != saves_in_flight.end();
}

void ChunkWorld::wait_for_saves(double timeout_sec) {
    std::unique_lock<std::mutex> lock(saves_cv_mutex);
    saves_cv.wait_for(lock, std::chrono::duration<double>(timeout_sec), [this]() {
        return outstanding_saves.load(std::memory_order_acquire) == 0;
    });
}

bool ChunkWorld::is_chunk_dirty(uint64_t key) const {
    std::lock_guard<std::mutex> lock(dirty_chunks_mutex);
    return dirty_chunks.find(key) != dirty_chunks.end();
}

void ChunkWorld::save_chunk_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    uint64_t key = chunk_map.get_chunk_key(chunk_x, chunk_y, chunk_z);
    
    std::lock_guard<std::mutex> lock(edit_maps_mutex);
    auto it = chunk_edit_maps.find(key);
    if (it != chunk_edit_maps.end()) {
        save_edit_map_to_disk(chunk_x, chunk_y, chunk_z, it->second);
    }
}

void ChunkWorld::save_world_metadata(const TerrainParams& params) {
    String filename = "user://chunks/world.meta";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }

    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::WRITE);
    if (!file.is_valid()) return;

    // Header: magic + version
    file->store_32(0x574F524C); // "WORL" magic
    file->store_32(2);           // metadata version

    // Terrain params
    file->store_32(params.seed);
    file->store_float(params.sea_level);
    file->store_32(params.bedrock_height);
    file->store_float(params.cave_threshold);
    file->store_float(params.cave_scale);
    file->store_float(params.continentalness_scale);
    file->store_float(params.ocean_threshold);
    file->store_float(params.land_threshold);
    file->store_float(params.shelf_width);
    file->store_float(params.shelf_depth);
    file->store_float(params.deep_ocean_depth);
    file->store_float(params.beach_width);
    file->store_32(params.subsurface_cover_depth);
    file->store_float(params.climate_temp_scale);
    file->store_float(params.climate_humidity_scale);
    file->store_float(params.biome_size);

    // Chunk save format version (for future compatibility)
    file->store_32(3); // current chunk format version

    file->close();
}

bool ChunkWorld::load_world_metadata(TerrainParams& out_params, int32_t& out_version) {
    String filename = "user://chunks/world.meta";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    if (!FileAccess::file_exists(filename)) return false;

    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::READ);
    if (!file.is_valid()) return false;

    // Header: magic + version
    uint32_t magic = file->get_32();
    if (magic != 0x574F524C) { // "WORL"
        file->close();
        return false;
    }

    int32_t meta_version = file->get_32();
    if (meta_version < 1 || meta_version > 2) {
        file->close();
        return false;
    }

    // Terrain params
    out_params.seed = file->get_32();
    out_params.sea_level = file->get_float();
    if (meta_version == 1) {
        // Skip old fields for backward compatibility
        file->get_float(); // base_height
        file->get_float(); // height_scale
        file->get_float(); // mountain_scale
    }
    out_params.bedrock_height = file->get_32();
    out_params.cave_threshold = file->get_float();
    out_params.cave_scale = file->get_float();
    out_params.continentalness_scale = file->get_float();
    out_params.ocean_threshold = file->get_float();
    out_params.land_threshold = file->get_float();
    out_params.shelf_width = file->get_float();
    out_params.shelf_depth = file->get_float();
    out_params.deep_ocean_depth = file->get_float();
    out_params.beach_width = file->get_float();
    out_params.subsurface_cover_depth = file->get_32();
    out_params.climate_temp_scale = file->get_float();
    out_params.climate_humidity_scale = file->get_float();
    out_params.biome_size = file->get_float();

    // Chunk save format version
    out_version = file->get_32();

    file->close();
    return true;
}

void ChunkWorld::save_inventory(const Inventory& inventory) {
    String filename = "user://chunks/inventory.bin";
    
    std::lock_guard<std::mutex> lock(file_access_mutex);
    
    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }
    
    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::WRITE);
    if (!file.is_valid()) return;
    
    // Header: magic + version
    file->store_32(0x494E5645); // "INVE" magic
    file->store_32(1);           // inventory version
    
    // Save hotbar
    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory.get_hotbar_slot(i);
        file->store_32(slot.block_id);
        file->store_32(slot.count);
    }
    
    // Save main inventory
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        const auto& slot = inventory.get_inventory_slot(i);
        file->store_32(slot.block_id);
        file->store_32(slot.count);
    }
    
    // Save selected slot
    file->store_32(inventory.get_selected_slot());
    
    file->close();
}

bool ChunkWorld::load_inventory(Inventory& inventory) {
    String filename = "user://chunks/inventory.bin";
    
    std::lock_guard<std::mutex> lock(file_access_mutex);
    
    if (!FileAccess::file_exists(filename)) return false;
    
    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::READ);
    if (!file.is_valid()) return false;
    
    // Header: magic + version
    uint32_t magic = file->get_32();
    if (magic != 0x494E5645) { // "INVE"
        file->close();
        return false;
    }
    
    int32_t inv_version = file->get_32();
    if (inv_version != 1) {
        file->close();
        return false;
    }
    
    // Clear existing inventory
    inventory.clear();
    
    // Load hotbar (restore exact slot positions)
    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        BlockID block_id = file->get_32();
        int count = file->get_32();
        inventory.set_hotbar_slot(i, block_id, count);
    }
    
    // Load main inventory (restore exact slot positions)
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        BlockID block_id = file->get_32();
        int count = file->get_32();
        inventory.set_inventory_slot(i, block_id, count);
    }
    
    // Load selected slot
    int selected_slot = file->get_32();
    inventory.select_slot(selected_slot);
    
    file->close();
    return true;
}

bool ChunkWorld::world_metadata_exists() const {
    String filename = "user://chunks/world.meta";
    return FileAccess::file_exists(filename);
}

bool ChunkWorld::load_edit_map_from_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, EditMap& out_edit_map) {
    String save_dir = "user://chunks/";
    String filename = save_dir + "chunk_" + String::num_int64(chunk_x) + "_" + String::num_int64(chunk_y) + "_" + String::num_int64(chunk_z) + ".edit";

    std::lock_guard<std::mutex> lock(file_access_mutex);

    if (!FileAccess::file_exists(filename)) {
        return false;
    }

    Ref<FileAccess> file = FileAccess::open(filename, FileAccess::READ);
    if (!file.is_valid()) return false;

    int64_t file_size = file->get_length();
    if (file_size == 0) {
        file->close();
        return false;
    }

    std::vector<uint8_t> data(file_size);
    file->get_buffer(data.data(), file_size);
    file->close();

    return deserialize_edit_map(data.data(), data.size(), out_edit_map);
}

void ChunkWorld::save_edit_map_to_disk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, const EditMap& edit_map) {
    std::lock_guard<std::mutex> lock(file_access_mutex);
    write_edit_map_file_locked(chunk_x, chunk_y, chunk_z, edit_map);
}

void ChunkWorld::write_edit_map_file_locked(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z, const EditMap& edit_map) {
    String save_dir = "user://chunks/";
    String filename = save_dir + "chunk_" + String::num_int64(chunk_x) + "_" + String::num_int64(chunk_y) + "_" + String::num_int64(chunk_z) + ".edit";
    String temp_filename = filename + ".tmp";
    String backup_filename = filename + ".bak";

    Ref<DirAccess> dir = DirAccess::open("user://");
    if (dir.is_valid()) {
        dir->make_dir_recursive("chunks");
    }

    // Serialize edit map to byte buffer
    std::vector<uint8_t> data;
    serialize_edit_map(edit_map, data);

    // If edit map is empty, delete the file instead of writing an empty one
    if (edit_map.empty()) {
        if (FileAccess::file_exists(filename)) {
            dir->remove(filename);
        }
        if (FileAccess::file_exists(backup_filename)) {
            dir->remove(backup_filename);
        }
        return;
    }

    // Write to temporary file first (atomic write pattern)
    Ref<FileAccess> file = FileAccess::open(temp_filename, FileAccess::WRITE);
    if (!file.is_valid()) return;

    file->store_buffer(data.data(), data.size());
    file->close();

    // Create backup of existing file before overwriting
    if (dir.is_valid() && FileAccess::file_exists(filename)) {
        // Remove old backup if it exists
        if (FileAccess::file_exists(backup_filename)) {
            dir->remove(backup_filename);
        }
        // Move current file to backup
        dir->rename(filename, backup_filename);
    }

    // Atomic rename: temp -> target
    if (dir.is_valid()) {
        dir->rename(temp_filename, filename);
    }
}

} // namespace VoxelEngine
