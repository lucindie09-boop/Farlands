// The two world effects' vertex stages, transcribed, for the tests that have to
// hold the CPU's culling arithmetic up against what the GPU will actually do to a
// vertex.
//
// Nothing here decides anything. The authorities are
// shaders/world_bend.gdshaderinc and shaders/world_horizon.gdshaderinc, and these
// functions are those files copied line by line - the same relative vectors, the
// same guards, the same expressions - so that a change to either include has
// exactly one other place to be made. Both of the tests in this directory include
// this file, which is what keeps the transcription one file rather than one per
// test.
//
// `bend_then_curve` is the pair of them in the order the world's two materials
// apply them: the bend moves the vertex, and the curve drops the result by the
// sagitta at the position the vertex really has (both effects are functions of
// where a point is, not of where the other one put it).

#ifndef FARLANDS_TESTS_WORLD_EFFECT_WARP_HPP
#define FARLANDS_TESTS_WORLD_EFFECT_WARP_HPP

#include <algorithm>
#include <cmath>

#include "core/world_cull.hpp"

namespace warptest {

using godot::AABB;
using godot::Vector3;

// shaders/world_bend.gdshaderinc's `world_bend_position`, point for point: the
// same relative vector, the same arctan wrap, the same convex mix, the same
// levelled-off lift, and the same two guards.
inline Vector3 bend_warp(const Vector3& world, const Vector3& camera,
                         const VoxelEngine::WorldBendParams& p) {
    if (!p.enabled || p.amount <= 0.0f) return world;
    const Vector3 relative = world - camera;
    const float horizontal = std::sqrt(relative.x * relative.x + relative.z * relative.z);
    if (horizontal < 0.0001f) return world;
    const float radius = std::max(p.radius, 1.0f);
    const float wrapped = std::atan(horizontal / radius) * radius;
    const float closed = horizontal + p.amount * (wrapped - horizontal);
    const float ratio = horizontal / radius;
    const float lift = p.amount * p.rise * radius * (1.0f - 1.0f / std::sqrt(1.0f + ratio * ratio));
    const float scale = closed / horizontal;
    return Vector3(camera.x + relative.x * scale,
                   world.y + lift,
                   camera.z + relative.z * scale);
}

// shaders/world_horizon.gdshaderinc's `world_horizon_drop`, point for point: the
// distance from the camera's vertical axis, the ratio, and the sphere's own
// sagitta. A float and not a position, because the include's own is a float and
// the consumer subtracts it from y.
inline float horizon_drop_at(const Vector3& world, const Vector3& camera,
                             const VoxelEngine::WorldHorizonParams& p) {
    if (!p.enabled || p.radius <= 0.0f) return 0.0f;
    const Vector3 relative = world - camera;
    const float horizontal = std::sqrt(relative.x * relative.x + relative.z * relative.z);
    const float radius = std::max(p.radius, 1.0f);
    const float ratio = horizontal / radius;
    return radius * (1.0f - 1.0f / std::sqrt(1.0f + ratio * ratio));
}

inline Vector3 horizon_warp(const Vector3& world, const Vector3& camera,
                            const VoxelEngine::WorldHorizonParams& p) {
    return Vector3(world.x, world.y - horizon_drop_at(world, camera, p), world.z);
}

// Both of them, in the order the world's materials apply them, for the test that
// has to hold the combined cull box up against both effects at once.
inline Vector3 bend_then_curve(const Vector3& world, const Vector3& camera,
                               const VoxelEngine::WorldBendParams& b,
                               const VoxelEngine::WorldHorizonParams& h) {
    const Vector3 bent = bend_warp(world, camera, b);
    return Vector3(bent.x, bent.y - horizon_drop_at(world, camera, h), bent.z);
}

inline bool contains(const AABB& box, const Vector3& point) {
    const Vector3 far_corner = box.position + box.size;
    return point.x >= box.position.x && point.x <= far_corner.x
        && point.y >= box.position.y && point.y <= far_corner.y
        && point.z >= box.position.z && point.z <= far_corner.z;
}

// How far a point sits outside a box, in blocks, in its worst axis. Zero for a
// point inside it.
inline float outside_by(const AABB& box, const Vector3& point) {
    const Vector3 far_corner = box.position + box.size;
    float worst = 0.0f;
    worst = std::max(worst, box.position.x - point.x);
    worst = std::max(worst, point.x - far_corner.x);
    worst = std::max(worst, box.position.y - point.y);
    worst = std::max(worst, point.y - far_corner.y);
    worst = std::max(worst, box.position.z - point.z);
    worst = std::max(worst, point.z - far_corner.z);
    return worst;
}

}  // namespace warptest

#endif // FARLANDS_TESTS_WORLD_EFFECT_WARP_HPP
