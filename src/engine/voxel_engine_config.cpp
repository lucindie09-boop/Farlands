// World configuration and persistence: the config files (terrain, biomes,
// vegetation, item registry), the world's metadata and the inventory's, the dirty
// chunk flush, and the biome lookup the tools ask for. Kept apart from
// engine/voxel_engine_controller.cpp, which owns none of the file handling.

#include "engine/voxel_engine_controller.hpp"

#include "core/item_registry.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"

#include <cmath>

namespace VoxelEngine {
using namespace godot;

void VoxelEngineController::load_world_configs() {
    TerrainParams params = world_updater.get_terrain_params();
    params.load_from_json("res://data/terrain_config.json");
    world_updater.set_terrain_params(params);

    BiomeConfig biomes;
    if (!BiomeConfig::load("res://data/biomes.json", biomes)) {
        biomes.reset_defaults();
    }
    world_updater.set_biome_config(biomes);
    // Kept as well as handed over: the far mode samples this configuration on
    // worker threads and the updater does not hand its copy back out.
    lod_grid_biomes = biomes;

    VegetationConfig vegetation;
    vegetation.load("res://data/vegetation.json");
    world_updater.set_vegetation_config(vegetation);

    if (!ItemRegistry::get_instance().load_from_json("res://data/items.json")) {
        WARN_PRINT("items.json missing or unparseable; item recipes will not resolve");
    }
    // Blocks load before items, so a block whose "drops" names an item (the torch
    // block drops the torch item) could not be resolved then. Finish those now that
    // the item ids exist.
    BlockRegistry::get_instance().resolve_pending_drops([](const char* name) {
        return ItemRegistry::get_instance().get_item_id_by_name(name);
    });

    recipe_book.clear();
    if (!recipe_book.load_from_json("res://data/recipes.json")) {
        WARN_PRINT("recipes.json missing or unparseable; crafting disabled");
    }
}

void VoxelEngineController::save_world_metadata() {
    const TerrainParams& params = world_updater.get_terrain_params();
    chunk_world.save_world_metadata(params);
}

bool VoxelEngineController::load_world_metadata() {
    TerrainParams params;
    int32_t chunk_version;
    if (!chunk_world.load_world_metadata(params, chunk_version)) {
        return false;
    }
    // Check for seed mismatch before applying
    if (params.seed != seed) {
        WARN_PRINT("World metadata seed mismatch: saved=" + String::num_int64(params.seed) + ", current=" + String::num_int64(seed) + ". Loading saved world may generate incoherent terrain.");
    }
    // Apply loaded params to controller state
    seed = params.seed;
    sea_level = params.sea_level;
    biome_size = params.biome_size;
    // Update world_updater with loaded params
    world_updater.set_seed(seed);
    world_updater.set_sea_level(sea_level);
    world_updater.set_biome_size(biome_size);
    return true;
}

bool VoxelEngineController::world_metadata_exists() const {
    return chunk_world.world_metadata_exists();
}

void VoxelEngineController::save_inventory(const Inventory& inventory) {
    chunk_world.save_inventory(inventory);
}

bool VoxelEngineController::load_inventory(Inventory& inventory) {
    return chunk_world.load_inventory(inventory);
}

void VoxelEngineController::flush_dirty_chunks(bool wait_for_completion, double timeout_sec) {
    chunk_world.flush_dirty_chunks(wait_for_completion, timeout_sec);
}

Dictionary VoxelEngineController::find_biome(const String& biome_name, int32_t center_x,
                                            int32_t center_z, int32_t max_radius_blocks) {
    Dictionary result;
    BiomeType target;
    if (!biome_from_name(biome_name.utf8().get_data(), target)) {
        result["found"] = false;
        return result;
    }
    int32_t out_x = 0;
    int32_t out_z = 0;
    float out_height = 0.0f;
    const bool found = world_updater.find_nearest_biome(
        target, center_x, center_z, max_radius_blocks, out_x, out_z, out_height);
    result["found"] = found;
    result["x"] = out_x;
    result["y"] = static_cast<int32_t>(std::round(out_height));
    result["z"] = out_z;
    return result;
}

} // namespace VoxelEngine
