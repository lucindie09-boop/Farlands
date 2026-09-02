#ifndef FARLANDS_BLOCK_OUTLINE_MESH_HPP
#define FARLANDS_BLOCK_OUTLINE_MESH_HPP

#include <cstdint>
#include <vector>

namespace VoxelEngine {

struct OutlineBox {
    float min[3];
    float max[3];
};

struct OutlineMeshData {
    std::vector<float> verts;   // xyz triplets
    std::vector<uint32_t> indices;
};

// Builds the block-outline edge geometry. Port of the old GDScript algorithm:
// for each axis, faces from + and - directions are XORed per plane so faces
// covered on both sides (internal walls, e.g. shared slab seams) are removed,
// then each surviving edge is extruded into a quad of the given thickness.
// boxes are block-local AABBs (all coordinates in [0,1]); thickness is the
// outline thickness setting (0.1 default; half-extent = thickness * 0.01).
// int voxel-space overlays — all coordinates are floats, pure math, no Godot
// runtime dependencies so the standalone test binary can cover it.
OutlineMeshData build_outline_mesh(const std::vector<OutlineBox>& boxes, float thickness);

// Union AABB of the boxes (center + size). Both zero when the list is empty.
void outline_fill_bounds(const std::vector<OutlineBox>& boxes, float center[3], float size[3]);

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_OUTLINE_MESH_HPP