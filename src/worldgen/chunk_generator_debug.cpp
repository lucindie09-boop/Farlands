// The debug accessors the tools and tests compare the chunk path against. Each one
// runs the same builders and samplers the chunk path runs, so a disagreement means
// the chunk path drifted rather than that this file did.

#include "worldgen/chunk_generator.hpp"

namespace VoxelEngine {

// Debug accessors (expose private members for standalone tools)
float ChunkGenerator::sample_continentalness_debug(float x, float z) const {
    return sample_continentalness(x, z);
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column_debug(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z);
}

// Debug: climate values read through the chunk-cached lattice path (what
// generate_chunk uses), for cross-checking against the per-call samplers.
float ChunkGenerator::sample_temperature_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                       int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_climate(*this, chunk_x, chunk_z);
    return lattice.temperature(wx, wz);
}

float ChunkGenerator::sample_weirdness_debug(float x, float z) const {
    return sample_weirdness(x, z);
}

float ChunkGenerator::sample_temperature_debug(float x, float z) const {
    return sample_temperature(x, z);
}

float ChunkGenerator::sample_humidity_debug(float x, float z) const {
    return sample_humidity(x, z);
}

float ChunkGenerator::sample_land_shape_debug(float x, float z) const {
    return sample_land_shape(x, z, 0.0f, 0.0f);  // temp/humidity are unused
}

// Debug: macro land height read through the chunk-cached lattice path
// (what generate_chunk uses), for cross-checking against the per-call
// sampler.
float ChunkGenerator::sample_land_shape_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                      int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_land_shape(*this, chunk_x, chunk_z);
    return lattice.land_shape(wx, wz);
}

// Debug: blended amplification knobs at a column (per-call path).
BiomeAmplification ChunkGenerator::blend_amplification_debug(int32_t world_x, int32_t world_z) const {
    return blend_amplification_at(world_x, world_z);
}

// Debug: blended amplification read through the chunk-cached lattice path
// (what generate_chunk uses), for cross-checking against the per-call
// sampler.
BiomeAmplification ChunkGenerator::blend_amplification_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                                     int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_climate(*this, chunk_x, chunk_z);
    lattice.build_amplification(*this, chunk_x, chunk_z);
    return lattice.amplification(wx, wz);
}

BiomeType ChunkGenerator::biome_from_climate_debug(float temperature, float humidity, float cont) const {
    return biome_from_climate(temperature, humidity, cont);
}

} // namespace VoxelEngine
