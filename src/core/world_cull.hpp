#ifndef FARLANDS_WORLD_CULL_HPP
#define FARLANDS_WORLD_CULL_HPP

// The world's geometry effects and the boxes the world is culled against.
//
// Two entries of the Shaders menu move the world's vertices rather than drawing
// over them: **World Bend**, which closes the ground in around the player and
// lifts the far field, and the **Horizon Curve**, which leaves every distance
// where it is and sinks the far field the way a sphere's own surface falls away.
// Both are culled the same way and for the same reason - a vertex shader runs
// after the engine has decided what to draw, so the box a chunk is culled against
// describes where the chunk was - and both therefore need a second implementation
// of their arithmetic, on the CPU, answering the one question culling has to ask.
//
// The bend's is below; the horizon's is at the bottom, and where the bend is a
// pull and a lift the horizon is a single downward move, which makes its bound
// the simpler of the two. `world_cull_aabb` at the very bottom is the box the
// mesh manager hands to the RenderingServer, with both effects applied.
//
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
#include <godot_cpp/variant/vector2.hpp>
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

// The farthest any point of `world_aabb` can be from the camera's vertical axis.
// This is the one measurement both effects are monotone in: the bend's pull and
// lift and the horizon's drop are all functions of the distance from that axis
// that only grow with it, so each of them is largest somewhere the same box
// evaluation, at this one number, bounds.
inline float world_cull_reach(const godot::AABB& world_aabb, const godot::Vector3& camera) {
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
    const float reach = world_cull_reach(world_aabb, camera);
    const float wrapped = std::atan(reach / radius) * radius;
    return params.amount * (reach - wrapped);
}

// How far up the bend can lift a point of `world_aabb` — the include's
// `amount * rise * radius * (1 - 1/sqrt(1 + ratio^2))` at the far corner.
inline float world_bend_lift(const godot::AABB& world_aabb, const godot::Vector3& camera,
                             const WorldBendParams& params) {
    if (!params.active()) return 0.0f;
    const float radius = std::max(params.radius, 1.0f);
    const float ratio = world_cull_reach(world_aabb, camera) / radius;
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

// ---------------------------------------------------------------------------
// The Horizon Curve's culling side
// ---------------------------------------------------------------------------
//
// shaders/world_horizon.gdshaderinc is the authority on what the curve does and
// this re-decides nothing; it is the same arithmetic a second time, for the same
// reason the bend's is here. The curve sinks a chunk's vertices by up to a
// hundred-odd blocks, and a box that still says where the chunk was is a chunk
// that gets culled with its own far field - the very ground the effect exists to
// show.
//
// Its bound is the simpler of the two, because the curve does one thing. Every
// point of a box moves down by at most
//
//     radius * (1 - 1/sqrt(1 + (reach / radius)²))
//
// which is the include's own expression evaluated at the box's far corner, where
// the distance from the camera's vertical axis is largest and the drop largest
// with it - and nothing at all sideways or upward. So the box culling is handed
// is the mesh's own with its bottom taken down by that much. The plan of the box,
// and the top of it, are untouched, which is the shape of the effect itself: the
// curve leaves every distance measured on the ground, and every vertical edge,
// exactly where they were.
//
// The drop is also far less sensitive to the camera than the bend is: its largest
// derivative with respect to a point's distance from the camera is 0.385 blocks
// of drop per block of distance (at reach = radius / sqrt(2)), whatever the
// radius is, so a refresh per whole block of camera movement is if anything more
// exactly right here than it is for the bend.
struct WorldHorizonParams {
    bool enabled = false;
    float radius = 1500.0f;

    bool active() const { return enabled && radius > 0.0f; }
};

// How far the curve sinks a point `reach` blocks from the camera's vertical axis:
// the include's `world_horizon_drop`, and a bound on every point of a box whose
// farthest corner is that far from the axis, since the drop only grows with it.
inline float world_horizon_drop(float reach, const WorldHorizonParams& params) {
    if (!params.active()) return 0.0f;
    const float radius = std::max(params.radius, 1.0f);
    const float ratio = reach / radius;
    return radius * (1.0f - 1.0f / std::sqrt(1.0f + ratio * ratio));
}

// The most the curve can sink any point of `world_aabb`. Never negative, so the
// top of the box is already a bound and only the bottom has to move.
inline float world_horizon_sag(const godot::AABB& world_aabb, const godot::Vector3& camera,
                               const WorldHorizonParams& params) {
    return world_horizon_drop(world_cull_reach(world_aabb, camera), params);
}

// The curve on its own: the mesh's own box with its bottom taken down by the sag
// plus the caller's slack. Separate from `world_cull_aabb` below so that the
// horizon can be pinned by itself, with the bend out of the picture.
inline godot::AABB world_horizon_cull_aabb(const godot::AABB& local_aabb, const godot::Vector3& origin,
                                           const godot::Vector3& camera, const WorldHorizonParams& params,
                                           float slack) {
    if (!params.active()) {
        return local_aabb;
    }
    const godot::AABB world_aabb(local_aabb.position + origin, local_aabb.size);
    const float sag = world_horizon_sag(world_aabb, camera, params) + slack;
    godot::AABB grown = local_aabb;
    grown.position.y -= sag;
    grown.size.y += sag;
    return grown;
}

// The box to hand the RenderingServer with both of the world's effects on it, and
// the mesh's own box back when neither of them is on. The two are applied one
// after the other rather than merged into a single margin, and that is exact
// rather than merely tidy: the bend's box already contains everything the bend
// can do to a point of the box, the curve moves the world only downward and by no
// more than the sag at the *original* box's far corner (both effects are
// functions of where a point really is), so lowering that box's bottom by the sag
// contains everything the curve can do to a point of it afterwards. Neither
// effect can move a point of a box outside the box the other one already
// bounds.
inline godot::AABB world_cull_aabb(const godot::AABB& local_aabb, const godot::Vector3& origin,
                                   const godot::Vector3& camera, const WorldBendParams& bend,
                                   const WorldHorizonParams& horizon, float slack) {
    if (!bend.active() && !horizon.active()) {
        return local_aabb;
    }
    godot::AABB box = world_bend_cull_aabb(local_aabb, origin, camera, bend, slack);
    if (horizon.active()) {
        const godot::AABB world_aabb(local_aabb.position + origin, local_aabb.size);
        const float sag = world_horizon_sag(world_aabb, camera, horizon) + slack;
        box.position.y -= sag;
        box.size.y += sag;
    }
    return box;
}

// How far a cull box has grown from the mesh's own, as the two numbers the
// per-block refresh watches and the whole of what it remembers about an
// instance: sideways (twice the bend's pull, because both faces of the box move
// outward) and taller (the bend's lift plus the curve's sag).
//
// It is a *magnitude* and not the box's own position change, which is the same
// information with a sign on it and which is negative whenever the box is grown
// - the bend pushes the box's corner out to one side and the curve pushes it
// down, so a growth written that way never looks like a growth, and the test the
// refresh makes for "has this box really moved?" is never true while an effect
// is on.
//
// Growth upward and growth downward are not kept apart because both of them only
// ever add to the box's height, and each term grows monotonically with the one
// distance - the reach, from the camera to the box's far corner - that either
// effect is a function of. So a height that has not moved is neither effect
// having moved, and the box a growth stands for is a function of that reach and
// the knobs alone. Both components are zero exactly when the box is the mesh's
// own, and neither is ever negative, which is what lets a negative remembered
// value mean "never touched".
inline godot::Vector2 world_cull_growth(const godot::AABB& local_aabb, const godot::AABB& grown) {
    return godot::Vector2(grown.size.x - local_aabb.size.x,
                          grown.size.y - local_aabb.size.y);
}

// Whether a growth that has been worked out before is close enough to this one
// that the box it produced does not have to be written to the RenderingServer
// again. A component that has never been touched is negative, which no real
// growth is, so it is never close to one - the first box is always written.
inline bool world_cull_growth_unchanged(const godot::Vector2& was, const godot::Vector2& now,
                                        float epsilon) {
    return was.x >= 0.0f && was.y >= 0.0f
        && std::fabs(was.x - now.x) < epsilon && std::fabs(was.y - now.y) < epsilon;
}

} // namespace VoxelEngine

#endif // FARLANDS_WORLD_CULL_HPP
