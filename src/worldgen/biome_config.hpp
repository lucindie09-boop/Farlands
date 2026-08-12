#ifndef FUK_MINECRAFT_BIOME_CONFIG_HPP
#define FUK_MINECRAFT_BIOME_CONFIG_HPP

#include <array>
#include <cstdint>

#include "core/block_types.hpp"

namespace godot {
class String;
}

namespace VoxelEngine {

// ---------------------------------------------------------------------------
// Biome definitions.
//
// The biome grid is a 2D (temperature, humidity) lookup gated by
// continentalness. This header owns the BiomeType enum plus the per-biome
// material / vegetation tables, which load from data/biomes.json — the single
// source of truth for biome tuning. Defaults mirror the historical hardcoded
// values so the JSON file is purely an override surface.
// ---------------------------------------------------------------------------
enum class BiomeType : uint8_t {
    Ocean   = 0,
    Beach   = 1,
    Plains  = 2,
    Forest  = 3,
    Desert  = 4,
    Count   = 5
};

// Per-biome surface materials (resolved block IDs from block_definitions.json).
struct BiomeSurface {
    BlockID surface              = BlockIDs::GRASS;
    BlockID subsurface            = BlockIDs::DIRT;
    BlockID near_water_surface    = BlockIDs::MUD;
    BlockID near_water_subsurface = BlockIDs::DIRT;
};

// Per-biome vegetation tuning.
struct BiomeVegetation {
    // Probability that a qualifying surface column grows a tree (0 = none).
    float tree_density = 0.0f;
    // Weighted variant pick: [oak, spruce, birch].
    std::array<float, 3> tree_variants{1.0f, 0.0f, 0.0f};
};

// One entry per BiomeType value (index == static_cast<int>(BiomeType)).
struct BiomeConfig {
    // Block placed on ocean floors / under surface water.
    BlockID underwater_surface = BlockIDs::SAND;

    // 2D climate grid thresholds. Empirically ~0.43 / ~0.57 split the sampled
    // noise distribution into even thirds (see land_biome_from_grid).
    float temp_cold_max  = 0.43f;
    float temp_hot_min   = 0.57f;
    float hum_dry_max    = 0.43f;
    float hum_humid_min  = 0.57f;

    std::array<BiomeSurface, static_cast<size_t>(BiomeType::Count)> surfaces;
    std::array<BiomeVegetation, static_cast<size_t>(BiomeType::Count)> vegetation;

    BiomeConfig();

    void reset_defaults();
    // Fills defaults first, then applies overrides from the JSON file.
    static bool load(const godot::String& json_path, BiomeConfig& out);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_BIOME_CONFIG_HPP
