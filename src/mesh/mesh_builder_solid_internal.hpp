#pragma once

// Shape geometry the solid mesher's halves share: the face-rectangle types the
// coplanar-sibling culling works in, and the questions mesh_builder_solid_emit.cpp
// asks of mesh_builder_solid_faces.cpp — which rectangles a box's face is drawn
// as, what the neighbour's resolved geometry covers, and what is left of a face
// once that coverage is taken out.
//
// These lived in one file's anonymous namespace. A file-local name cannot be shared,
// so the types and constants are defined here and the four functions are declared
// here and defined in mesh_builder_solid_faces.cpp — WITHOUT `static`, which would
// give them internal linkage in that one file. `subtract_rect`, used only by those
// functions, stayed behind in its own anonymous namespace.

#include <cstdint>

#include "mesh/mesh_builder.hpp"

namespace VoxelEngine {

// A box's face is a rectangle in the two axes its normal does not run along, so
// that pair of axes is all the sibling test below needs.
struct FaceAxes {
    uint8_t normal;
    int sign;  // +1 or -1, the way the face points
    uint8_t tangent_a;
    uint8_t tangent_b;
};

[[nodiscard]] FaceAxes face_axes(VoxelEngine::FaceDirection dir);

// A face rectangle in its own two axes. Authored geometry is exact 16ths, so
// this only absorbs the float round-trip through the JSON loader; it doubles as
// the sliver threshold, because a piece narrower than it is not a quad.
constexpr float kFacePieceEpsilon = 1e-5f;
constexpr int kFacePieceCap = 16;

struct FaceRect {
    float a0;
    float a1;
    float b0;
    float b1;
};

[[nodiscard]] int visible_face_rects(const VoxelEngine::ShapeBoxes& boxes,
                                     const VoxelEngine::BlockAABB& box,
                                     VoxelEngine::FaceDirection dir, uint8_t box_index,
                                     FaceRect* out);

[[nodiscard]] bool neighbor_box_covers(const float nb_min[3], const float nb_max[3],
                                              const float self_min[3], const float self_max[3],
                                              FaceDirection dir);

[[nodiscard]] bool neighbor_boxes_cover(const VoxelEngine::ShapeBoxes& boxes,
                                               const float self_min[3], const float self_max[3],
                                               VoxelEngine::FaceDirection dir);

[[nodiscard]] int drop_neighbor_coverage(FaceDirection dir, const ShapeBoxes& neighbor,
                                         const BlockAABB& box, FaceRect* rects, int count);

} // namespace VoxelEngine
