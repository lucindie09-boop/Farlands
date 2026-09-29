#ifndef FARLANDS_CHUNK_GENERATOR_LATTICE_HPP
#define FARLANDS_CHUNK_GENERATOR_LATTICE_HPP

// The cache a chunk column is read from: the climate fields, the macro land shape and
// the blended amplification knobs of one chunk, each sampled once per 4-block node
// (9x9 over a 32-block chunk) and bilinearly read back per column.
//
// The node values come from ChunkGenerator's own samplers, which is what makes the
// cache and the per-call path agree: a chunk edge meets its neighbour's queries
// without a seam, and the tools that cross-check the two compare bit-identical values
// (see the *_lattice_debug accessors). ChunkGenerator is a friend for exactly that
// reason -- the samplers are private, and a lattice filled from anywhere else could
// drift from them.
//
// The builders used to be ChunkGenerator members taking the node arrays as
// parameters; they are methods here now, reading the generator they are handed.

#include "core/chunk_data.hpp"
#include "worldgen/biome_config.hpp"

#include <cstdint>

namespace VoxelEngine {

class ChunkGenerator;

class ChunkGeneratorLattice {
public:
    static constexpr int32_t SPACING = 4;
    static constexpr int32_t NODES = CHUNK_WIDTH / SPACING + 1;

    // Filled once per chunk. build_amplification reads the climate nodes, so it
    // comes after build_climate.
    void build_climate(const ChunkGenerator& gen, int32_t chunk_x, int32_t chunk_z);
    void build_land_shape(const ChunkGenerator& gen, int32_t chunk_x, int32_t chunk_z);
    void build_amplification(const ChunkGenerator& gen, int32_t chunk_x, int32_t chunk_z);

    // Bilinear reads at a world column.
    [[nodiscard]] float temperature(int32_t world_x, int32_t world_z) const;
    [[nodiscard]] float humidity(int32_t world_x, int32_t world_z) const;
    [[nodiscard]] float land_shape(int32_t world_x, int32_t world_z) const;
    [[nodiscard]] BiomeAmplification amplification(int32_t world_x, int32_t world_z) const;

    // Raw nodes, for the callers that bound a cell from its four corners rather
    // than reading a point (the height-range estimate).
    [[nodiscard]] float land_node(int32_t i, int32_t j) const { return land_[i][j]; }
    [[nodiscard]] const BiomeAmplification& amp_node(int32_t i, int32_t j) const {
        return amp_[i][j];
    }

private:
    // The lattice cell a world column falls in, with the fractions across it.
    struct Node {
        int32_t ix = 0;
        int32_t iz = 0;
        float fx = 0.0f;
        float fz = 0.0f;
    };

    [[nodiscard]] Node node_for(int32_t world_x, int32_t world_z) const;

    int32_t origin_x_ = 0;
    int32_t origin_z_ = 0;
    float temp_[NODES][NODES]{};
    float hum_[NODES][NODES]{};
    float land_[NODES][NODES]{};
    BiomeAmplification amp_[NODES][NODES]{};
};

} // namespace VoxelEngine

#endif  // FARLANDS_CHUNK_GENERATOR_LATTICE_HPP
