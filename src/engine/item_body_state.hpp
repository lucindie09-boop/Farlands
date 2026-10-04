#ifndef FARLANDS_ITEM_BODY_STATE_HPP
#define FARLANDS_ITEM_BODY_STATE_HPP

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/quaternion.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector4.hpp>

#include <cstdint>

// The rigid bodies of the dropped items, as the native physics reads and writes
// them: a table of SHAPES (a body's boxes and surface points, which never move
// while the item is in the world) and parallel arrays of STATE (where every body
// is and how it is moving, which changes every substep).
//
// Both halves of an item's substep work on exactly these -- the world step
// (engine/item_body_solver.hpp) and the pair solve (engine/item_pair_solver.hpp)
// -- so a body is gathered and read back once per substep and nothing is copied
// between the halves. The caller owns all of it; the solvers only read and write
// through the pointers.

namespace VoxelEngine {

// The shape of every body a pass may touch, flattened. A body names its shape by
// row; a row owns the body's boxes (the exact guard and the push-out) and its
// surface points (the impulses, the turning and the friction).
struct ItemShapeTable {
    // Box i of shape s is at [box_start[s] + i], in the body's own space.
    const godot::Vector3* box_offsets = nullptr;  // from the body's origin
    const godot::Vector3* box_halves = nullptr;
    const int32_t* box_start = nullptr;
    const int32_t* box_count = nullptr;
    // Point i of shape s is at [point_start[s] + i], in the body's own space.
    const godot::Vector3* points = nullptr;
    const int32_t* point_start = nullptr;
    const int32_t* point_count = nullptr;
    int32_t shape_count = 0;
};

// The bodies of one substep, in parallel arrays. Everything is read and written
// in place; the last four are outputs:
//   * touched  -- took part in an overlapping pair this substep, so it is
//     resting against something and may come to rest;
//   * woken    -- was a sleeping body the pair solve woke;
//   * grounded -- touches the WORLD (the pair solve sets it for a pair contact
//     too, as the GDScript it replaces did);
//   * rest     -- seconds it has been slow and touching, in and out: the caller
//     keeps it between substeps, the settle reads it.
struct ItemBodies {
    godot::Vector3* positions = nullptr;
    godot::Vector4* rotations = nullptr;  // quaternion (x, y, z, w)
    godot::Vector3* velocities = nullptr;
    godot::Vector3* spins = nullptr;
    const godot::Vector3* inertia = nullptr;  // diagonal tensor, body space
    const float* reach = nullptr;             // broad-phase radius, from the origin
    const int32_t* shapes = nullptr;          // row in the shape table
    uint8_t* asleep = nullptr;                // in and out
    float* rest = nullptr;                    // in and out
    uint8_t* touched = nullptr;               // out
    uint8_t* woken = nullptr;                 // out
    uint8_t* grounded = nullptr;              // out
    int32_t count = 0;
};

// The body's rotation, as the basis its own space is turned by. The quaternion
// is the same one the node carries.
inline godot::Basis item_basis_of(const godot::Vector4& rotation) {
    return godot::Basis(godot::Quaternion(rotation.x, rotation.y, rotation.z, rotation.w));
}

inline godot::Vector4 item_rotation_of(const godot::Quaternion& q) {
    return godot::Vector4(q.x, q.y, q.z, q.w);
}

// A body's inverse inertia, applied to a world-space vector: the tensor is
// diagonal in the body's own space, so the vector is turned in, divided per
// axis, and turned back out. The same as `basis * diag(1/inertia) * basis^T * v`,
// with two transforms instead of two matrix products.
inline godot::Vector3 item_inertia_xform(const godot::Basis& basis, const godot::Basis& transposed,
                                         const godot::Vector3& inertia, const godot::Vector3& v) {
    const godot::Vector3 local = transposed.xform(v);
    return basis.xform(godot::Vector3(local.x / inertia.x, local.y / inertia.y,
                                      local.z / inertia.z));
}

} // namespace VoxelEngine

#endif // FARLANDS_ITEM_BODY_STATE_HPP
