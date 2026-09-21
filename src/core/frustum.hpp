#ifndef FARLANDS_FRUSTUM_HPP
#define FARLANDS_FRUSTUM_HPP

#include <godot_cpp/variant/plane.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <array>
#include <cmath>
#include <cstddef>

namespace VoxelEngine {

class Frustum {
public:
    // Godot's `Camera3D::get_frustum()` hands back planes whose normals point
    // OUTWARD — a point inside the frustum is on the NEGATIVE side of all six —
    // which is the opposite of the convention the tests below use. Measured
    // rather than read from the docs: a camera at (500,100,500) facing -Z
    // returns its near plane as normal (0,0,1) d=499.95, and the engine's own
    // `Camera3D::is_position_in_frustum` reports true for a box three chunks in
    // FRONT of that camera and false for one behind it. Testing
    // `distance_to(center) < -r` against those planes therefore rejects
    // everything in the world.
    //
    // That is not hypothetical: it is what this class did. The generation
    // sweep's frustum pass spent 2,376,704 candidate checks in one 9,379-frame
    // session and passed exactly ZERO chunks (`/genstats`), so a third of the
    // generation budget bought nothing, and every chunk handed to the mesh
    // queue carried `in_frustum = false`. Flipping them once here keeps a single
    // convention in this class (inside = positive distance) and fixes every
    // caller at the same time.
    void update(const std::array<godot::Plane, 6>& planes) {
        for (size_t i = 0; i < 6; ++i) {
            planes_[i] = -planes[i];
        }
        count = 6;
    }

    void clear() { count = 0; }

    bool is_initialized() const { return count > 0; }

    bool is_aabb_visible(const godot::AABB& aabb) const {
        if (count == 0) return true;
        godot::Vector3 center = aabb.get_center();
        godot::Vector3 half_size = aabb.size * 0.5;
        for (size_t i = 0; i < count; ++i) {
            const auto& p = planes_[i];
            float r = half_size.x * std::abs(p.normal.x)
                    + half_size.y * std::abs(p.normal.y)
                    + half_size.z * std::abs(p.normal.z);
            if (p.distance_to(center) < -r) return false;
        }
        return true;
    }

    bool is_chunk_visible(int32_t cx, int32_t cy, int32_t cz) const {
        if (count == 0) return true;
        godot::Vector3 min(
            static_cast<float>(cx * 32),
            static_cast<float>(cy * 32),
            static_cast<float>(cz * 32)
        );
        godot::AABB aabb(min, godot::Vector3(32.0f, 32.0f, 32.0f));
        return is_aabb_visible(aabb);
    }

private:
    std::array<godot::Plane, 6> planes_{};
    size_t count = 0;
};

} // namespace VoxelEngine

#endif // FARLANDS_FRUSTUM_HPP
