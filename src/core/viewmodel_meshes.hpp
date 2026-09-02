#ifndef FARLANDS_CORE_VIEWMODEL_MESHES_HPP
#define FARLANDS_CORE_VIEWMODEL_MESHES_HPP

#include <cstdint>
#include <vector>

// Pure, Godot-free mesh geometry ported out of viewmodel.gd. The thin
// GDScript scene keeps all node/material state and feeds these routines raw
// geometry params; they return interleaved-array surface data (xyz floats,
// uv floats, xyz normal floats, triangle indices) that the binding packs into
// ArrayMesh surfaces.

namespace VoxelEngine {

struct MeshGeometry {
    std::vector<float> verts;     // xyz triplets
    std::vector<float> uvs;       // uv pairs
    std::vector<float> normals;   // xyz triplets
    std::vector<int32_t> indices; // triangle indices
};

// The cube held/blocks render: unit cube centred on the origin, texture-top =
// world-top on every face. Mirrors viewmodel.gd `_build_cube_mesh`, which is
// the same layout the Block Maker / block-break overlay cubes use.
MeshGeometry build_unit_cube_mesh();

// Boxes in block space: six floats per box (min_x..max_z, each in 0..1). The
// GDScript source subtracted 0.5 to centre them, so this does too. Mirrors
// viewmodel.gd `_build_shaped_block_mesh` winding/UVs.
MeshGeometry build_box_mesh(const std::vector<float>& boxes);

// Extruded sprite (held item): every texel with alpha > 0 gets a front/back
// quad plus silhouette rims on edges facing an empty neighbour. `rgba` is
// width*height*4 RGBA8 bytes. Mirrors viewmodel.gd `_generate_item_mesh`.
MeshGeometry build_sprite_mesh(const uint8_t* rgba, int width, int height);

} // namespace VoxelEngine

#endif // FARLANDS_CORE_VIEWMODEL_MESHES_HPP