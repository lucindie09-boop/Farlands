#include "engine/collision_resolver.hpp"
#include "engine/collision_resolver_internal.hpp"
#include "core/chunk_map.hpp"
#include "core/chunk_coords.hpp"
#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include <cmath>
#include <vector>

// The turned-box half of the collision resolver: a rotated body against the
// world's cells, by the separating axis test. Split out of collision_resolver.cpp
// for the 500-line cap; the AABB path, the swept resolve and the point contacts
// stayed there. The text moved verbatim (see docs/file_size_plan.md).

namespace VoxelEngine {

using namespace godot;

namespace {

// How far a turned box reaches along each world axis.
Vector3 turned_reach(const Basis& basis, const Vector3& half) {
    const Vector3 r0 = basis[0];
    const Vector3 r1 = basis[1];
    const Vector3 r2 = basis[2];
    return Vector3(
        std::abs(r0.x) * half.x + std::abs(r0.y) * half.y + std::abs(r0.z) * half.z,
        std::abs(r1.x) * half.x + std::abs(r1.y) * half.y + std::abs(r1.z) * half.z,
        std::abs(r2.x) * half.x + std::abs(r2.y) * half.y + std::abs(r2.z) * half.z);
}

// A turned box against an upright one, by the separating axis test: the overlap
// along the axis they overlap LEAST is the penetration, and that axis is the
// normal that pushes them apart. `a_*` is the turned box, `b_*` the cell.
struct BoxOverlap {
    bool overlap = false;
    Vector3 normal;
    float depth = 0.0f;
};

BoxOverlap turned_vs_upright(const Vector3& a_centre, const Vector3& a_half, const Basis& a_basis,
                             const Vector3& b_centre, const Vector3& b_half) {
    BoxOverlap out;
    const Vector3 d = b_centre - a_centre;
    const Basis& a = a_basis;
    const Basis b;
    const float eps = 1e-6f;

    auto overlap_on = [&](const Vector3& axis) {
        if (axis.length_squared() < eps) return true;
        const Vector3 n = axis.normalized();
        const float ra = std::abs(n.dot(a.get_column(0))) * a_half.x
                       + std::abs(n.dot(a.get_column(1))) * a_half.y
                       + std::abs(n.dot(a.get_column(2))) * a_half.z;
        const float rb = std::abs(n.dot(b.get_column(0))) * b_half.x
                       + std::abs(n.dot(b.get_column(1))) * b_half.y
                       + std::abs(n.dot(b.get_column(2))) * b_half.z;
        const float overlap = ra + rb - std::abs(n.dot(d));
        if (overlap <= 0.0f) {
            out.overlap = false;
            return false;
        }
        if (overlap < out.depth || out.depth == 0.0f) {
            out.depth = overlap;
            // Point the normal out of the cell, toward the box's centre, so
            // pushing the box along it separates the two.
            out.normal = n.dot(d) >= 0.0f ? -n : n;
        }
        return true;
    };

    out.overlap = true;
    for (int i = 0; i < 3; ++i) {
        if (!overlap_on(a.get_column(i))) return out;
        if (!overlap_on(b.get_column(i))) return out;
        for (int j = 0; j < 3; ++j) {
            if (!overlap_on(a.get_column(i).cross(b.get_column(j)))) return out;
        }
    }
    return out;
}

} // namespace

CollisionResolver::TurnedContact CollisionResolver::turned_boxes_contact_fast(
    const Vector3& centre, const std::vector<Vector3>& offsets,
    const std::vector<Vector3>& halves, const Basis& basis) const {
    TurnedContact best;
    if (!chunk_map_ || offsets.empty()) return best;
    const BlockRegistry& registry = BlockRegistry::get_instance();

    // The whole body's reach, so the cells to visit are known once.
    Vector3 reach(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < offsets.size(); ++i) {
        reach = reach.max(turned_reach(basis, halves[i]) + (basis.xform(offsets[i])).abs());
    }
    const int32_t min_x = static_cast<int32_t>(std::floor(centre.x - reach.x));
    const int32_t min_y = static_cast<int32_t>(std::floor(centre.y - reach.y));
    const int32_t min_z = static_cast<int32_t>(std::floor(centre.z - reach.z));
    const int32_t max_x = static_cast<int32_t>(std::floor(centre.x + reach.x));
    const int32_t max_y = static_cast<int32_t>(std::floor(centre.y + reach.y));
    const int32_t max_z = static_cast<int32_t>(std::floor(centre.z + reach.z));

    for (int32_t y = min_y; y <= max_y; ++y) {
        for (int32_t z = min_z; z <= max_z; ++z) {
            for (int32_t x = min_x; x <= max_x; ++x) {
                const BlockID bid = static_cast<BlockID>(chunk_map_->get_block_world_fast(x, y, z));
                if (bid == BlockIDs::AIR) continue;
                const BlockType& bt = registry.get_block_fast(bid);
                if (!bt.stops_bodies()) continue;
                if (bt.is_full_cube()) {
                    const Vector3 cell_centre(x + 0.5f, y + 0.5f, z + 0.5f);
                    const Vector3 cell_half(0.5f, 0.5f, 0.5f);
                    for (size_t i = 0; i < offsets.size(); ++i) {
                        const Vector3 box_centre = centre + basis.xform(offsets[i]);
                        const BoxOverlap hit = turned_vs_upright(
                            box_centre, halves[i], basis, cell_centre, cell_half);
                        if (hit.overlap && hit.depth > best.depth) {
                            best.into = true;
                            best.depth = hit.depth;
                            best.normal = hit.normal;
                        }
                    }
                    continue;
                }
                CollisionShapeContext shape_ctx{chunk_map_, x, y, z};
                ShapeBoxes collision_boxes;
                resolve_shape_boxes(bt, registry,
                                    ShapeNeighborFn{&collision_shape_neighbor, &shape_ctx},
                                    ShapeBoxKind::Collision, collision_boxes);
                for (uint8_t bi = 0; bi < collision_boxes.count(); ++bi) {
                    const BlockAABB& box = collision_boxes[bi];
                    const Vector3 cell_centre(x + (box.min[0] + box.max[0]) * 0.5f,
                                              y + (box.min[1] + box.max[1]) * 0.5f,
                                              z + (box.min[2] + box.max[2]) * 0.5f);
                    const Vector3 cell_half((box.max[0] - box.min[0]) * 0.5f,
                                            (box.max[1] - box.min[1]) * 0.5f,
                                            (box.max[2] - box.min[2]) * 0.5f);
                    for (size_t i = 0; i < offsets.size(); ++i) {
                        const Vector3 box_centre = centre + basis.xform(offsets[i]);
                        const BoxOverlap hit = turned_vs_upright(
                            box_centre, halves[i], basis, cell_centre, cell_half);
                        if (hit.overlap && hit.depth > best.depth) {
                            best.into = true;
                            best.depth = hit.depth;
                            best.normal = hit.normal;
                        }
                    }
                }
            }
        }
    }
    return best;
}

CollisionResolver::TurnedContact CollisionResolver::turned_boxes_contact(
    const Vector3& centre, const std::vector<Vector3>& offsets,
    const std::vector<Vector3>& halves, const Basis& basis) const {
    constexpr float kPad = 1.0f;
    Vector3 reach(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i < offsets.size(); ++i) {
        reach = reach.max(turned_reach(basis, halves[i]) + (basis.xform(offsets[i])).abs());
    }
    auto lock = chunk_map_->lock_keys(chunk_keys_for_box(
        chunk_map_, centre - reach - Vector3(kPad, kPad, kPad),
        centre + reach + Vector3(kPad, kPad, kPad)));
    return turned_boxes_contact_fast(centre, offsets, halves, basis);
}

} // namespace VoxelEngine