#ifndef FARLANDS_BIOME_CONFIG_HPP
#define FARLANDS_BIOME_CONFIG_HPP

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
    Ocean = 0,
    Hills = 1,
    Count = 2
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
    // Weighted variant pick: [oak, spruce]. Fixed-size, order-fixed contract
    // (index 0 = oak, index 1 = spruce). No birch — birch needs its own
    // block/texture definitions before it can be placed (vegetation_generator.cpp).
    std::array<float, 2> tree_variants{1.0f, 0.0f};
};

// Per-biome terrain amplification knobs (1.0 / 0.0 = neutral, worldgen
// unchanged).
//
// height:    scales a column's displacement around sea level — < 1 flattens
//            toward sea level, > 1 exaggerates relief. Applied AFTER biome
//            classification (land/ocean split stays where the raw height put
//            it). Feeds sample_column and the scheduler's quick_height_estimate.
// weirdness: scales the 3D-shaping weirdness mask — 0 pins every column to
//            the minimum shaping strength, > 1 pushes more columns toward the
//            maximum (saturated masks). Feeds the density field in
//            generate_chunk, find_surface_y and single-point density queries.
// min_weirdness: floor for the amplified shaping MASK, expressed as an offset
//            above neutral (1.0 = no floor; each +0.1 raises the floor by
//            0.1, so 1.1 floors the mask at 0.1). Values at or below 1.0 are
//            inert. Applied as clamp01(max(raw_mask * weirdness,
//            min_weirdness - 1.0)).
struct BiomeAmplification {
    float height        = 1.0f;
    float weirdness     = 1.0f;
    float min_weirdness = 1.0f;
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
    std::array<BiomeAmplification, static_cast<size_t>(BiomeType::Count)> amplification;

    BiomeConfig();

    void reset_defaults();
    // Fills defaults first, then applies overrides from the JSON file.
    static bool load(const godot::String& json_path, BiomeConfig& out);
};

// Canonical biome name (index == static_cast<int>(BiomeType)), matching the
// biome keys in data/biomes.json.
const char* biome_name(BiomeType b);

// Case-insensitive name -> enum lookup. Returns false for unknown names.
bool biome_from_name(const char* name, BiomeType& out);

} // namespace VoxelEngine

#endif // FARLANDS_BIOME_CONFIG_HPP
