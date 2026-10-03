#ifndef FARLANDS_COLLISION_RESOLVER_HPP
#define FARLANDS_COLLISION_RESOLVER_HPP

#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/basis.hpp>

#include <vector>

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

    // A point of a TURNED body's contact with the world: where it touches, which
    // way that cell pushes it, and how deep it is.
    struct PointContact {
        godot::Vector3 point;
        godot::Vector3 normal;
        float depth = 0.0f;
    };

    // Every point of `points` that is inside solid geometry, as one contact each.
    // These are the points a solver takes its torque from -- the velocity of the
    // material point that touched is what makes a body turn, and a sweep's "which
    // axis hit" cannot say that.
    std::vector<PointContact> contacts_for_points(const godot::Vector3* points, size_t count,
                                                  float radius) const;

    // How deep a turned box is inside the world, with the normal that pushes it
    // out. An EXACT convex test, so it holds along an edge as well as at a corner:
    // sampled points cannot promise that, and a block can sit between two samples
    // and be passed straight through. This is the guard that keeps a body out of
    // the world; the point contacts above are what turn it.
    struct TurnedContact {
        bool into = false;
        godot::Vector3 normal;
        float depth = 0.0f;
    };
    TurnedContact turned_box_contact(const godot::Vector3& centre, const godot::Vector3& half,
                                     const godot::Basis& basis) const;
    TurnedContact turned_box_contact_fast(const godot::Vector3& centre, const godot::Vector3& half,
                                          const godot::Basis& basis) const;

    // True when the cell stops a body. Liquids are NOT solid (see BlockType::stops_bodies).
    bool is_solid_at(int32_t wx, int32_t wy, int32_t wz) const;
    // True when the cell holds a liquid, whatever state it is drawn in.
    bool is_liquid_at(int32_t wx, int32_t wy, int32_t wz) const;
    float get_slipperiness_at(int32_t wx, int32_t wy, int32_t wz) const;

private:
    ChunkMap* chunk_map_;
};

} // namespace VoxelEngine
#endif
