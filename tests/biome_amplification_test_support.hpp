#ifndef FARLANDS_TESTS_BIOME_AMPLIFICATION_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_BIOME_AMPLIFICATION_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the biome amplification tests.
//
// Both helpers search a wide window for a column whose biome is the target: the
// climate features these tests measure span thousands of blocks, so a small window
// can sit inside one climate band and never see the biome being asked for.
// `find_isolated_column` additionally requires the cell's neighbours to be a
// different biome, which is what the border-blend cases need.
//
// These were file-local to test_biome_amplification.cpp; the split moved them here
// as `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "core/terrain_params.hpp"

#include <cmath>

using namespace VoxelEngine;

namespace biome_amplification_test {


inline bool find_seed_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    // Wide window (coarse step): climate features span ~8000 blocks, so a
    // small window can sit entirely inside one climate band and miss a target.
    for (int32_t z = -24000; z <= 24000; z += 500) {
        for (int32_t x = -24000; x <= 24000; x += 500) {
            if (gen.get_biome(x, z) == target) {
                wx = x;
                wz = z;
                return true;
            }
        }
    }
    return false;
}

// A column whose whole amplification-blend window (2 nodes = 8 blocks at the
// default radius) is the target biome: the blended knobs there equal the
// biome's own knobs exactly, which is what the exact-height expectations in
// the amplification tests assume.
inline bool find_isolated_column(ChunkGenerator& gen, BiomeType target, int32_t& wx, int32_t& wz) {
    for (int32_t z = -24000; z <= 24000; z += 500) {
        for (int32_t x = -24000; x <= 24000; x += 500) {
            if (gen.get_biome(x, z) != target) continue;
            bool isolated = true;
            for (int32_t dz = -8; dz <= 8 && isolated; dz += 4) {
                for (int32_t dx = -8; dx <= 8 && isolated; dx += 4) {
                    if (gen.get_biome(x + dx, z + dz) != target) isolated = false;
                }
            }
            if (isolated) {
                wx = x;
                wz = z;
                return true;
            }
        }
    }
    return false;
}


} // namespace biome_amplification_test

#endif // FARLANDS_TESTS_BIOME_AMPLIFICATION_TEST_SUPPORT_HPP
