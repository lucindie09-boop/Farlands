#ifndef FARLANDS_BIOME_SEARCH_CONFIG_HPP
#define FARLANDS_BIOME_SEARCH_CONFIG_HPP

#include <cstdint>

namespace VoxelEngine {

// The biome locator's geometry, shared by the walk and by whoever asks it for
// a radius. Ring r of a 16-block walk costs 8r probes, so rings 0..54 cost
// 1 + 4*54*55 = 11,881: kRadiusBlocks is the largest reach the probe budget
// covers in full. The chat asks for this rather than a horizon it would give
// up short of; chunk_generator_columns.cpp pins the arithmetic.
namespace biome_search {

inline constexpr int32_t kStep = 16;
inline constexpr int32_t kMaxProbes = 12000;
inline constexpr int32_t kRadiusBlocks = 864;

}  // namespace biome_search
}  // namespace VoxelEngine

#endif  // FARLANDS_BIOME_SEARCH_CONFIG_HPP
