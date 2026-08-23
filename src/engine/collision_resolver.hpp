#ifndef FARLANDS_COLLISION_RESOLVER_HPP
#define FARLANDS_COLLISION_RESOLVER_HPP

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/aabb.hpp>

namespace VoxelEngine {

class ChunkMap;

class CollisionResolver {
public:
    struct CollisionResult {
        godot::Vector3 position;
        bool collided_x = false;
        bool collided_y = false;
        bool collided_z = false;
        bool on_floor = false;
        bool stepped_up = false;
    };

    explicit CollisionResolver(ChunkMap* cm) : chunk_map_(cm) {}

    CollisionResult resolve(const godot::Vector3& position,
                            const godot::Vector3& motion,
                            const godot::Vector3& size,
                            float step_height = 0.0f) const;
    bool is_aabb_solid(const godot::AABB& aabb) const;
    bool is_aabb_solid_fast(const godot::AABB& aabb) const;
    bool is_solid_at(int32_t wx, int32_t wy, int32_t wz) const;
    float get_slipperiness_at(int32_t wx, int32_t wy, int32_t wz) const;

private:
    ChunkMap* chunk_map_;
};

} // namespace VoxelEngine
#endif
