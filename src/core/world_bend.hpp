#ifndef FARLANDS_WORLD_BEND_HPP
#define FARLANDS_WORLD_BEND_HPP

// World Bend's culling side.
//
// The bend moves the world's vertices in the vertex shader, and a vertex shader
// runs *after* the engine has already decided which chunks to draw. An instance
// is culled against its AABB — `GeometryInstance3D::custom_aabb`, or the mesh's
// own — carried into world space by the instance's transform, and that box
// describes where the chunk *was*: the box the mesher built, in the place the
// chunk sits. So the bend pulls ground into view that the frustum has already
// thrown away, and what a player sees past the frustum's own edge is a hole
// where the far field should be. Straight down, where the whole point of the
// effect is that the ground closes up around you, the hole is a ring.
//
// shaders/world_bend.gdshaderinc is the authority on what the bend does; this is
// the same arithmetic a second time, answering the one question culling has to
// ask: how far can the bend move a point of a box this big, this far away? It
// answers with a *bound*, not with the exact image of the box, and deliberately:
// a box grown by too much costs a few extra draws, and a box grown by too little
// is a hole in the world.
//
// The bound, from the include's own two terms:
//
//   - The horizontal one. `closed = mix(reach, atan(reach/radius) * radius,
//     amount)` shrinks every point's distance from the camera's vertical axis,
//     so a point can move inward by at most `amount * (reach - atan(reach/radius)
//     * radius)` — the pull at the far corner, since that quantity grows with
//     distance. Nothing ever moves *outward*: `atan(r) <= r`, so the mix can only
//     shrink. Growing the box by that pull in x and z therefore covers the move,
//     whichever way round the camera is.
//   - The vertical one. `lift` is added to y and is never negative, so the
//     bottom of the box is already a bound and only the top needs growing — by
//     the lift at the far corner, which is where the lift is largest.
//
// The far corner is a corner rather than a guess: the distance from a point to
// the camera's vertical axis is `sqrt(dx^2 + dz^2)`, and over a box that is
// largest at the corner where |dx| and |dz| are each largest, and those are the
// same corner. Both terms are monotone increasing in that distance, so one
// evaluation bounds every point of the box.

#include <algorithm>
#include <cmath>

#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/vector3.hpp>

namespace VoxelEngine {

// The bend's four knobs, exactly the uniforms the two world materials carry and
// shader_overlay.gd pushes. `enabled` is the registry's switch (`world_bend_on`);
// with it off nothing moves and there is nothing to compensate for.
struct WorldBendParams {
    bool enabled = false;
    float amount = 0.7f;
    float radius = 256.0f;
    float rise = 1.0f;

    bool active() const { return enabled && amount > 0.0f; }
};

// The farthest any point of `world_aabb` can be from the camera's vertical axis,
// which is where both of the bend's terms are largest.
inline float world_bend_reach(const godot::AABB& world_aabb, const godot::Vector3& camera) {
    const godot::Vector3 far_corner = world_aabb.position + world_aabb.size;
    const float dx = std::max(std::fabs(world_aabb.position.x - camera.x),
                              std::fabs(far_corner.x - camera.x));
    const float dz = std::max(std::fabs(world_aabb.position.z - camera.z),
                              std::fabs(far_corner.z - camera.z));
    return std::sqrt(dx * dx + dz * dz);
}

// The largest amount the bend can move a point of `world_aabb`, horizontally,
// which is also the largest amount it can move it at all (the lift is measured
// separately because it only ever grows the box upward).
inline float world_bend_pull(const godot::AABB& world_aabb, const godot::Vector3& camera,
                             const WorldBendParams& params) {
    if (!params.active()) return 0.0f;
    const float radius = std::max(params.radius, 1.0f);
    const float reach = world_bend_reach(world_aabb, camera);
    const float wrapped = std::atan(reach / radius) * radius;
    return params.amount * (reach - wrapped);
}

// How far up the bend can lift a point of `world_aabb` — the include's
// `amount * rise * radius * (1 - 1/sqrt(1 + ratio^2))` at the far corner.
inline float world_bend_lift(const godot::AABB& world_aabb, const godot::Vector3& camera,
                             const WorldBendParams& params) {
    if (!params.active()) return 0.0f;
    const float radius = std::max(params.radius, 1.0f);
    const float ratio = world_bend_reach(world_aabb, camera) / radius;
    return params.amount * params.rise * radius * (1.0f - 1.0f / std::sqrt(1.0f + ratio * ratio));
}

// The largest amount the bend can move a point of `world_aabb` at all, in any
// direction: what a single scalar margin has to allow for. The cull box below
// spends the two terms separately instead, because the lift is only ever upward.
inline float world_bend_margin(const godot::AABB& world_aabb, const godot::Vector3& camera,
                              const WorldBendParams& params) {
    return world_bend_pull(world_aabb, camera, params) + world_bend_lift(world_aabb, camera, params);
}

// The box to hand to the RenderingServer for culling: the mesh's own local-space
// box, grown by how far the bend can move it. `origin` is the instance's world
// translation — the chunk's or the far region's corner, the same number the
// instance transform is built from — and `slack` is the margin the caller wants
// left over for the camera movement it is going to tolerate before recomputing.
inline godot::AABB world_bend_cull_aabb(const godot::AABB& local_aabb, const godot::Vector3& origin,
                                        const godot::Vector3& camera, const WorldBendParams& params,
                                        float slack) {
    // Inactive is also how a box is put back: the margin is nothing, so the box
    // handed back is the mesh's own and a world that was bent and is now not
    // stops being drawn outside its own bounds.
    if (!params.active()) {
        return local_aabb;
    }
    const godot::AABB world_aabb(local_aabb.position + origin, local_aabb.size);
    const float pull = world_bend_pull(world_aabb, camera, params) + slack;
    const float lift = world_bend_lift(world_aabb, camera, params) + slack;

    godot::AABB grown = local_aabb;
    grown.position.x -= pull;
    grown.position.z -= pull;
    grown.size.x += pull * 2.0f;
    grown.size.z += pull * 2.0f;
    // Upward only: the lift is added to y and is never negative, so the bottom
    // of the box is already a bound on where the bottom of the mesh can be.
    grown.size.y += lift;
    return grown;
}

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_BEND_HPP
