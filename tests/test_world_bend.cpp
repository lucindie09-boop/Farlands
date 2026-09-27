// World Bend's culling bound: the AABB a chunk is culled against has to contain
// where the vertex shader will actually put that chunk.
//
// The bend's authority is shaders/world_bend.gdshaderinc; nothing here re-decides
// what it does. What is tested is the second implementation of it in
// core/world_bend.hpp — the one culling uses — against two properties that
// culling cannot be wrong about:
//
//   1. every point of a box, warped, is inside the box the cull AABB gives back.
//      This is the property a hole in the world is made of. The warp below is
//      transcribed from the include line by line, and the sample is a grid of
//      points through the box, corners included, over a spread of boxes,
//      cameras and knob settings — including the awkward ones: a box straddling
//      the camera's own vertical axis (where the warp's direction is undefined),
//      a camera inside the box, the radius at its floor and at its ceiling, and
//      the bend switched off.
//   2. with the bend off, the box comes back untouched, whatever the camera. A
//      switch that leaves a chunk drawn outside its own bounds is a switch that
//      changed the frame while claiming to be off.
//
// What is *not* tested here is that the engine culls with the box: that is
// `.freebuff/probe_bend_cull.gd`, which has a real world and a real frustum.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/world_bend.hpp"

using namespace VoxelEngine;
using godot::AABB;
using godot::Vector3;

namespace {

// The include's `world_bend_position`, point for point: the same relative
// vector, the same arctan wrap, the same convex mix, the same levelled-off lift,
// and the same two guards.
Vector3 warp(const Vector3& world, const Vector3& camera, const WorldBendParams& p) {
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

bool contains(const AABB& box, const Vector3& point) {
    const Vector3 far_corner = box.position + box.size;
    return point.x >= box.position.x && point.x <= far_corner.x
        && point.y >= box.position.y && point.y <= far_corner.y
        && point.z >= box.position.z && point.z <= far_corner.z;
}

// How far a point sits outside a box, in blocks, in its worst axis. Zero for a
// point inside it.
float outside_by(const AABB& box, const Vector3& point) {
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

// The chunk-shaped boxes the renderer hands in: a 32-block chunk box, and the
// far-field regions, which are several chunks across.
struct Case {
    AABB local;
    Vector3 origin;
    Vector3 camera;
    WorldBendParams params;
};

std::vector<Case> cases() {
    std::vector<Case> out;
    const AABB chunk(Vector3(0, 0, 0), Vector3(32, 32, 32));
    const AABB region(Vector3(0, 0, 0), Vector3(256, 32, 256));
    const std::vector<Vector3> cameras = {
        Vector3(0, 70, 0),            // straight above the chunk
        Vector3(16, 70, 16),          // inside its own column
        Vector3(500, 70, 500),        // far away and off-axis
        Vector3(-2000, 70, 900),
        Vector3(0.5f, 70.5f, 0.5f),   // the awkward one: on a block corner
    };
    const std::vector<WorldBendParams> params = {
        {true, 0.7f, 256.0f, 1.0f},   // the registry's defaults
        {true, 1.0f, 256.0f, 1.0f},   // the top of the slider, level cap
        {true, 1.0f, 64.0f, 1.0f},    // the radius at its floor: the tightest bend
        {true, 1.0f, 1024.0f, 1.0f},  // and at its ceiling: the widest
        {true, 0.7f, 256.0f, 2.0f},   // the rise at its own top
        {true, 0.7f, 256.0f, 0.0f},   // and at zero, where the bend is a pull only
        {false, 1.0f, 256.0f, 1.0f},  // switched off
        {true, 0.0f, 256.0f, 1.0f},   // amount zero: off by another name
    };
    // Origins a chunk can actually have: under the camera, far out along one
    // axis, out at a corner, and far enough away that the bend has levelled off.
    const std::vector<Vector3> origins = {
        Vector3(0, 64, 0),
        Vector3(-32, 64, -32),
        Vector3(480, 64, 0),
        Vector3(0, 64, -512),
        Vector3(-1024, 64, 1024),
    };
    for (const auto& camera : cameras) {
        for (const auto& param : params) {
            for (const auto& origin : origins) {
                out.push_back({chunk, origin, camera, param});
                if (out.size() % 7 == 0) out.push_back({region, origin, camera, param});
            }
        }
    }
    return out;
}

}  // namespace

TEST_CASE("world bend: the cull box contains everything the shader moves into it") {
    // A 5x5x5 grid through each box, corners included: 125 samples of the warp
    // per case, against the box culling would use.
    const int steps = 5;
    int checked = 0;
    float worst = 0.0f;
    for (const auto& c : cases()) {
        const AABB world_box(c.local.position + c.origin, c.local.size);
        const AABB cull = world_bend_cull_aabb(c.local, c.origin, c.camera, c.params, 0.0f);
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < steps; ++j) {
                for (int k = 0; k < steps; ++k) {
                    const Vector3 point(
                        world_box.position.x + world_box.size.x * static_cast<float>(i) / (steps - 1),
                        world_box.position.y + world_box.size.y * static_cast<float>(j) / (steps - 1),
                        world_box.position.z + world_box.size.z * static_cast<float>(k) / (steps - 1));
                    const Vector3 moved = warp(point, c.camera, c.params);
                    // The cull box is in the instance's local space; the camera
                    // and the warp are in world space, which is the same space
                    // shifted by the origin.
                    const Vector3 moved_local = moved - c.origin;
                    const float outside = outside_by(cull, moved_local);
                    worst = std::max(worst, outside);
                    CHECK_MESSAGE(outside <= 0.0001f,
                                  "a point of the box lands " << outside << " blocks outside the cull box"
                                  << " (camera " << c.camera.x << "," << c.camera.z
                                  << ", origin " << c.origin.x << "," << c.origin.z
                                  << ", amount " << c.params.amount
                                  << ", radius " << c.params.radius
                                  << ", rise " << c.params.rise
                                  << ", enabled " << c.params.enabled << ")");
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 1000);
    INFO("worst overshoot over " << checked << " warped points: " << worst);
}

TEST_CASE("world bend: with the bend off the cull box is the mesh's own") {
    WorldBendParams off{false, 0.7f, 256.0f, 1.0f};
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    for (float distance : {0.0f, 32.0f, 256.0f, 4096.0f}) {
        const AABB cull = world_bend_cull_aabb(local, Vector3(0, 64, 0), Vector3(distance, 70, 0), off, 2.0f);
        CHECK(cull.position.x == doctest::Approx(local.position.x));
        CHECK(cull.position.y == doctest::Approx(local.position.y));
        CHECK(cull.position.z == doctest::Approx(local.position.z));
        CHECK(cull.size.x == doctest::Approx(local.size.x));
        CHECK(cull.size.y == doctest::Approx(local.size.y));
        CHECK(cull.size.z == doctest::Approx(local.size.z));
    }
}

TEST_CASE("world bend: the cull box grows with distance and stays finite") {
    WorldBendParams params{true, 1.0f, 256.0f, 1.0f};
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    float previous = -1.0f;
    for (float distance : {32.0f, 128.0f, 256.0f, 512.0f, 1024.0f}) {
        const AABB world_box(Vector3(distance, 64, 0), local.size);
        const float margin = world_bend_margin(world_box, Vector3(0, 70, 0), params);
        // The pull and the lift both grow with distance, so a chunk further out
        // is never given a smaller box than a nearer one.
        CHECK(margin > previous);
        // And the lift is capped by rise * radius, so no box is ever unbounded.
        CHECK(margin < 256.0f + distance);
        previous = margin;
    }
    // The chunk the player is standing over is where the bend is nearly the
    // identity, and its box is grown by nearly nothing: a block or so of lift at
    // the box's own far corner (22 blocks out), against the 250-odd blocks a
    // chunk at 500 needs. That difference is the whole reason this is a margin
    // per chunk rather than one number for the world.
    const AABB underfoot(Vector3(0, 64, 0), Vector3(32, 32, 32));
    const float near_margin = world_bend_margin(underfoot, Vector3(16, 70, 16), params);
    const AABB far_box(Vector3(512, 64, 0), Vector3(32, 32, 32));
    const float far_margin = world_bend_margin(far_box, Vector3(16, 70, 16), params);
    CHECK(near_margin < 2.0f);
    CHECK(far_margin > near_margin * 100.0f);
}

TEST_CASE("world bend: the lift only ever grows the box upward") {
    WorldBendParams params{true, 1.0f, 256.0f, 1.0f};
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    const AABB cull = world_bend_cull_aabb(local, Vector3(0, 64, 0), Vector3(0, 70, -512), params, 0.0f);
    // The bend adds to y and never takes away, so the bottom of the box is
    // already a bound and growing it downward would only make culling worse.
    CHECK(cull.position.y == doctest::Approx(local.position.y));
    CHECK(cull.size.y > local.size.y);
}
