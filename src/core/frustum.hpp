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
    void update(const std::array<godot::Plane, 6>& planes) {
        for (size_t i = 0; i < 6; ++i) {
            planes_[i] = planes[i];
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
