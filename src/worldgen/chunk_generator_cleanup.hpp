#pragma once

#include "worldgen/chunk_generator.hpp"

#include <cstdint>

namespace VoxelEngine {

// Post-density repair of a freshly written dense block buffer: the isolated
// voxel scan, the thin-solid-sheet-in-water repair and the water gap flood
// fill. Split out of generate_chunk() (see chunk_generator_cleanup.cpp) to keep
// that file under the 500-line cap; the passes read the density field for
// classification and only ever write the dense buffer.
void cleanup_generated_columns(BlockID* dense_buffer,
                               const float* density_buffer,
                               int32_t density_stride_y,
                               int32_t world_y_start,
                               int32_t top_wy,
                               float sea_level,
                               const ChunkGenerator::ChunkColumn (*columns)[CHUNK_DEPTH]);

} // namespace VoxelEngine
