#pragma once

#include <cstdint>

namespace VoxelEngine {

class ChunkGenerator;

// True when the chunk at (cx, cy, cz) would be entirely solid if it were
// generated: the whole chunk sits below every column's lowest possible
// surface (macro height − density margin), so the density field has no air
// inside it — and chunks below the bedrock layer are bedrock, also solid.
// Out-of-world chunks return false so boundary faces at the world edges still
// render. Used by mesh culling to treat an ungenerated underground neighbor
// as opaque instead of rendering a box wall into the void.
bool chunk_would_be_fully_solid(const ChunkGenerator& gen, int32_t cx, int32_t cy, int32_t cz);

// The same rule, for a caller that already has the column's lowest possible
// surface (`HeightRange::min_h`, as WorldUpdater's column cache keeps it). The
// chunk form above spends a ~25 us lattice sample to find that one float; a probe
// that asks about several chunks of one column should sample it once.
bool chunk_would_be_fully_solid_at_column(const ChunkGenerator& gen, int32_t cy,
                                          float column_min_h);

} // namespace VoxelEngine
