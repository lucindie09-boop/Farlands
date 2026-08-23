#ifndef FARLANDS_VEGETATION_GENERATOR_HPP
#define FARLANDS_VEGETATION_GENERATOR_HPP
#include <cstdint>
#include "core/chunk_data.hpp"
#include "worldgen/chunk_generator.hpp"

namespace VoxelEngine {

// Places trees / boulders / cacti on the density surface of a freshly
// generated chunk. Which feature set a column gets depends on its biome
// (BiomeConfig::vegetation) and the global knobs in VegetationConfig.
class VegetationGenerator {
public:
    using CrossChunkWriter = ChunkGenerator::CrossChunkWriter;

    void generate_vegetation(ChunkData& chunk,
                             const ChunkGenerator::ChunkColumn (&columns)[CHUNK_WIDTH][CHUNK_DEPTH],
                             int32_t chunk_x, int32_t chunk_z,
                             int32_t world_y_start, int32_t world_y_end,
                             const BiomeConfig& biomes,
                             const VegetationConfig& veg_config,
                             const CrossChunkWriter& cross_writer = nullptr);

private:
    static uint32_t hash_pos(int32_t x, int32_t z);
    // Weighted variant pick over [oak, spruce] (order-fixed, see BiomeVegetation).
    static int pick_variant(const std::array<float, 2>& weights, uint32_t seed);

    // Weighted variant pick, then dispatch to the matching shape.
    static void place_tree(ChunkData& chunk,
                           int32_t local_x, int32_t local_z,
                           int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
                           uint32_t seed, int32_t chunk_x, int32_t chunk_z,
                           const BiomeVegetation& veg, const VegetationConfig& veg_config,
                           const CrossChunkWriter& cross_writer);
    static void place_oak(ChunkData& chunk,
                          int32_t local_x, int32_t local_z,
                          int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
                          uint32_t seed, int32_t chunk_x, int32_t chunk_z,
                          int32_t trunk_height, const CrossChunkWriter& cross_writer);
    static void place_spruce(ChunkData& chunk,
                             int32_t local_x, int32_t local_z,
                             int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
                             uint32_t seed, int32_t chunk_x, int32_t chunk_z,
                             const CrossChunkWriter& cross_writer);
    static void place_cactus(ChunkData& chunk, int32_t local_x, int32_t local_z,
                             int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
                             const DesertVegConfig& cfg);
    static void place_boulder(ChunkData& chunk, int32_t local_x, int32_t local_z,
                              int32_t surface_y, int32_t world_y_start, int32_t world_y_end,
                              uint32_t seed, int32_t chunk_x, int32_t chunk_z,
                              int32_t radius, const CrossChunkWriter& cross_writer);
};

} // namespace VoxelEngine
#endif // FARLANDS_VEGETATION_GENERATOR_HPP
