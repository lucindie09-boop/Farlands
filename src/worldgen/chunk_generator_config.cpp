// Constructing, configuring and asking the generator about the world: the one-line
// accessors that do no sampling work. Everything that samples is in
// worldgen/chunk_generator_sampling.cpp.

#include "worldgen/chunk_generator.hpp"

namespace VoxelEngine {

ChunkGenerator::ChunkGenerator(const TerrainParams& p)
    : terrain_noise(p.seed)
    , cave_noise(p.seed + 2000)
    , density_noise(p.seed + 7000)
    , weirdness_noise(p.seed + 9000)
    , temp_noise(p.seed + 3000)
    , humidity_noise(p.seed + 4000)
    , climate_warp_noise(p.seed + 5000)
    , params(p)
    , rng(p.seed)
 {
}

BiomeType ChunkGenerator::get_biome(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z).biome;
}

float ChunkGenerator::get_terrain_height(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z).height;
}

// -------------------------------------------------------------------------
// Parameter management
// -------------------------------------------------------------------------
void ChunkGenerator::set_params(const TerrainParams& p) {
    bool seed_changed = (p.seed != params.seed);
    params = p;
    if (seed_changed) {
        terrain_noise     = FastNoise(p.seed);
        cave_noise        = FastNoise(p.seed + 2000);
        density_noise     = FastNoise(p.seed + 7000);
        weirdness_noise   = FastNoise(p.seed + 9000);
        temp_noise        = FastNoise(p.seed + 3000);
        humidity_noise    = FastNoise(p.seed + 4000);
        climate_warp_noise = FastNoise(p.seed + 5000);
        rng.seed(p.seed);
    }
}

const TerrainParams& ChunkGenerator::get_params() const {
    return params;
}

void ChunkGenerator::set_biome_config(const BiomeConfig& config) {
    biome_config = config;
}

const BiomeConfig& ChunkGenerator::get_biome_config() const {
    return biome_config;
}

void ChunkGenerator::set_vegetation_config(const VegetationConfig& config) {
    vegetation_config = config;
}

const VegetationConfig& ChunkGenerator::get_vegetation_config() const {
    return vegetation_config;
}

} // namespace VoxelEngine
