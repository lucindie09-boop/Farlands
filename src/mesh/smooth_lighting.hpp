#ifndef FARLANDS_SMOOTH_LIGHTING_HPP
#define FARLANDS_SMOOTH_LIGHTING_HPP

#include <cstdint>
#include "mesh/mesh_types.hpp"
#include "mesh/chunk_neighbor_accessor.hpp"

namespace VoxelEngine {

// Computes per-vertex light for a block face by averaging the light of the
// 4 blocks meeting at each vertex corner (face-adjacent block + 3 corner blocks).
// Returns 4 packed light keys (one per vertex in standard face vertex order).
// These can be unpacked and assigned per-vertex for smooth light gradients
// across faces instead of flat per-face lighting.
//
// `stride` is the LOD stride of the face (1 = full detail). Corner positions
// are scaled by stride on the in-plane axes so LOD faces sample light at their
// true corners, matching adjacent full-detail faces at shared grid corners.
void compute_smooth_light(
    const ChunkNeighborAccessor& accessor,
    const BlockRegistry& registry,
    int32_t x, int32_t y, int32_t z,
    FaceDirection direction,
    uint16_t light_keys_out[4],
    int32_t stride = 1
);

// Computes the light at an arbitrary grid corner (vertex position) lying on a
// face plane by averaging the 4 cells that meet at that corner on the air side
// of the face, excluding occluding (solid/opaque) samples. This is the same
// sampling compute_smooth_light uses per corner, generalized to arbitrary grid
// corners so greedy-merged faces get per-corner gradients that agree with
// adjacent per-block faces by construction.
uint16_t compute_corner_light(
    const ChunkNeighborAccessor& accessor,
    const BlockRegistry& registry,
    int32_t gx, int32_t gy, int32_t gz,
    FaceDirection direction
);

} // namespace VoxelEngine

#endif // FARLANDS_SMOOTH_LIGHTING_HPP
