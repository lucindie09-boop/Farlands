// The Horizon Curve's culling bound: the AABB a chunk is culled against has to
// contain where the vertex shader will actually put that chunk.
//
// The curve's authority is shaders/world_horizon.gdshaderinc; nothing here
// re-decides what it does. What is tested is the second implementation of it in
// core/world_cull.hpp — the one culling uses — against the properties culling
// cannot be wrong about:
//
//   1. every point of a box, sunk by the curve, is inside the box the cull AABB
//      gives back. This is the property a hole in the world is made of. The warp
//      below is the include's own expression, and the sample is a grid of points
//      through the box, corners included, over a spread of boxes, cameras and
//      radii — including the awkward ones: a box straddling the camera's own
//      vertical axis, a camera inside the box, the radius at its floor (a planet
//      a thousand blocks across, where the far field plunges hundreds of blocks)
//      and at its ceiling, a camera two kilometres out, and the curve switched
//      off.
//   2. with the curve off the box comes back untouched, whatever the camera. A
//      switch that leaves a chunk drawn outside its own bounds is a switch that
//      changed the frame while claiming to be off.
//   3. the growth is downward and nothing else: the plan of the box, and its top,
//      are the mesh's own to the last bit. That is not a detail — it is the shape
//      of the effect, which moves every vertex down by a function of its distance
//      from the camera's vertical axis and touches nothing else, and it is why
//      the world's layout and every vertical edge in it survive.
//   4. the two effects together: with both on, every point warped by both is
//      inside the one box `world_cull_aabb` gives back. The two are independent
//      of each other's order — each is a function of where a point really is —
//      and this is where that stops being a claim in a comment.
//
// What is *not* tested here is that the engine culls with the box: that is
// `.freebuff/probe_horizon_geo.gd` (and `probe_bend_cull.gd`) with a real world
// and a real frustum.

#include "doctest.h"

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/world_cull.hpp"
#include "world_effect_warp.hpp"

using namespace VoxelEngine;
using godot::AABB;
using godot::Vector2;
using godot::Vector3;
using warptest::contains;
using warptest::horizon_drop_at;
using warptest::horizon_warp;
using warptest::outside_by;

namespace {

// The chunk-shaped boxes the renderer hands in: a 32-block chunk box, and the
// far-field regions, which are several chunks across and reach a long way out.
struct Case {
    AABB local;
    Vector3 origin;
    Vector3 camera;
    WorldHorizonParams params;
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
    const std::vector<WorldHorizonParams> params = {
        {true, 1500.0f},    // the registry's default: the far field a long way down
        {true, 1000.0f},    // the slider at its floor: a world 2,000 blocks across
        {true, 32000.0f},   // and at its ceiling, where the curve is nearly flat
        {true, 8000.0f},    // the gentler curve the effect shipped at first
        {false, 1500.0f},   // switched off
        {true, 0.0f},       // radius zero: off by another name
    };
    // Origins a chunk can actually have: under the camera, far out along one
    // axis, out at a corner, and far enough away that the drop has levelled off.
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

TEST_CASE("horizon curve: the cull box contains everything the shader sinks into it") {
    // A 5x5x5 grid through each box, corners included: 125 samples of the warp
    // per case, against the box culling would use.
    const int steps = 5;
    int checked = 0;
    float worst = 0.0f;
    for (const auto& c : cases()) {
        const AABB world_box(c.local.position + c.origin, c.local.size);
        const AABB cull = world_cull_aabb(c.local, c.origin, c.camera, WorldBendParams{false}, c.params, 0.0f);
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < steps; ++j) {
                for (int k = 0; k < steps; ++k) {
                    const Vector3 point(
                        world_box.position.x + world_box.size.x * static_cast<float>(i) / (steps - 1),
                        world_box.position.y + world_box.size.y * static_cast<float>(j) / (steps - 1),
                        world_box.position.z + world_box.size.z * static_cast<float>(k) / (steps - 1));
                    const Vector3 moved = horizon_warp(point, c.camera, c.params);
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
                                  << ", radius " << c.params.radius
                                  << ", enabled " << c.params.enabled << ")");
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 1000);
    INFO("worst overshoot over " << checked << " warped points: " << worst);
}

TEST_CASE("horizon curve: with the curve off the cull box is the mesh's own") {
    WorldHorizonParams off{false, 1500.0f};
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    for (float distance : {0.0f, 32.0f, 256.0f, 8192.0f}) {
        const AABB cull = world_cull_aabb(local, Vector3(0, 64, 0), Vector3(distance, 70, 0),
                                          WorldBendParams{false}, off, 2.0f);
        CHECK(cull.position.x == doctest::Approx(local.position.x));
        CHECK(cull.position.y == doctest::Approx(local.position.y));
        CHECK(cull.position.z == doctest::Approx(local.position.z));
        CHECK(cull.size.x == doctest::Approx(local.size.x));
        CHECK(cull.size.y == doctest::Approx(local.size.y));
        CHECK(cull.size.z == doctest::Approx(local.size.z));
    }
}

TEST_CASE("horizon curve: the cull box grows downward and nothing else") {
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    for (float distance : {64.0f, 512.0f, 2048.0f}) {
        const AABB cull = world_horizon_cull_aabb(local, Vector3(distance, 64, 0), Vector3(0, 70, 0),
                                                  WorldHorizonParams{true, 4000.0f}, 0.0f);
        // The curve moves every vertex down by a function of its distance from
        // the camera's vertical axis and touches nothing else, so the top of the
        // box and the whole of its plan are the mesh's own - exactly, not
        // approximately, because the arithmetic only ever adds to a lower bound.
        CHECK(cull.position.x == local.position.x);
        CHECK(cull.position.z == local.position.z);
        CHECK(cull.size.x == local.size.x);
        CHECK(cull.size.y > local.size.y);
        CHECK(cull.size.z == local.size.z);
        CHECK(cull.position.y + cull.size.y == local.position.y + local.size.y);
        // And the bottom really moved: the ground this far out is drawn by tens
        // of blocks, which is the whole of why the box has to move at all.
        CHECK(cull.position.y < local.position.y - 1.0f);
    }
}

TEST_CASE("horizon curve: the drop grows with distance and is capped by the radius") {
    const WorldHorizonParams params{true, 4000.0f};
    float previous = -1.0f;
    for (float distance : {32.0f, 128.0f, 512.0f, 2048.0f, 8192.0f}) {
        const Vector3 point(distance, 64.0f, 0.0f);
        const float drop = horizon_drop_at(point, Vector3(0, 70, 0), params);
        // Further out is never sunk less than nearer in: the drop's only
        // derivative is positive, which is what makes it a curve rather than a
        // fold, and what makes the far corner a bound on the whole box.
        CHECK(drop > previous);
        // And it levels off at the radius instead of running away: a vertex sent
        // to infinity is not a curve, it is a broken frame.
        CHECK(drop < params.radius);
        previous = drop;
    }
    // The chunk a player is standing over is where the curve is nearly the
    // identity, and its box is grown by a fraction of a block: 0.03 blocks at 32
    // blocks out, against 258 at two kilometres. That difference is the whole
    // reason this is a margin per chunk rather than one number for the world, and
    // it is also why the ground underfoot, the block being mined and the reach of
    // a player's arm are untouched by the effect.
    const float underfoot = horizon_drop_at(Vector3(32, 64, 0), Vector3(16, 70, 16), params);
    const float far_out = horizon_drop_at(Vector3(2048, 64, 0), Vector3(16, 70, 16), params);
    CHECK(underfoot < 0.1f);
    CHECK(far_out > 200.0f);
}

TEST_CASE("world effects: one box holds both of them") {
    // Both effects at once, over the same spread, warped in the order the world's
    // materials apply them. The two are functions of where a point really is, so
    // the box has to hold the pair of them and not just each one on its own. The
    // cases' own radii are not used here: this is a fixed pair of settings over
    // the same spread of boxes, cameras and origins as every test above.
    const WorldBendParams bend{true, 0.7f, 256.0f, 1.0f};
    const WorldHorizonParams curve{true, 1500.0f};
    const int steps = 5;
    int checked = 0;
    float worst = 0.0f;
    for (const auto& c : cases()) {
        const AABB world_box(c.local.position + c.origin, c.local.size);
        const AABB cull = world_cull_aabb(c.local, c.origin, c.camera, bend, curve, 0.0f);
        for (int i = 0; i < steps; ++i) {
            for (int j = 0; j < steps; ++j) {
                for (int k = 0; k < steps; ++k) {
                    const Vector3 point(
                        world_box.position.x + world_box.size.x * static_cast<float>(i) / (steps - 1),
                        world_box.position.y + world_box.size.y * static_cast<float>(j) / (steps - 1),
                        world_box.position.z + world_box.size.z * static_cast<float>(k) / (steps - 1));
                    const Vector3 moved = warptest::bend_then_curve(point, c.camera, bend, curve) - c.origin;
                    const float outside = outside_by(cull, moved);
                    worst = std::max(worst, outside);
                    CHECK_MESSAGE(outside <= 0.0001f,
                                  "with both effects on a point lands " << outside << " blocks outside the"
                                  << " cull box (camera " << c.camera.x << "," << c.camera.z
                                  << ", origin " << c.origin.x << "," << c.origin.z << ")");
                    ++checked;
                }
            }
        }
    }
    CHECK(checked > 1000);
    INFO("worst overshoot over " << checked << " points with both effects on: " << worst);
}

TEST_CASE("world effects: a box is grown by both of them, and given back by both") {
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    const Vector3 origin(512, 64, 0);
    const Vector3 camera(0, 70, 0);
    const WorldBendParams bend{true, 0.7f, 256.0f, 1.0f};
    const WorldHorizonParams curve{true, 1500.0f};

    const AABB bend_only = world_cull_aabb(local, origin, camera, bend, WorldHorizonParams{false}, 0.0f);
    const AABB curve_only = world_cull_aabb(local, origin, camera, WorldBendParams{false}, curve, 0.0f);
    const AABB both = world_cull_aabb(local, origin, camera, bend, curve, 0.0f);
    // The bend grows the box sideways and upward, the curve downward, so the box
    // with both on is the box of each with the other's growth added to it.
    CHECK(both.position.x < local.position.x);
    CHECK(both.position.y < local.position.y);
    CHECK(both.size.y > bend_only.size.y);
    CHECK(both.size.y > curve_only.size.y);
    CHECK(both.size.x > curve_only.size.x);
    // Containment of the two single-effect boxes, which is the property the
    // per-instance margin bookkeeping relies on: whichever of the two effects
    // changed, the box changed with it.
    CHECK(contains(both, bend_only.position));
    CHECK(contains(both, bend_only.position + bend_only.size));
    CHECK(contains(both, curve_only.position));
    CHECK(contains(both, curve_only.position + curve_only.size));

    // Switched off, whatever the knobs are set to, is the mesh's own box again.
    const AABB off = world_cull_aabb(local, origin, camera, WorldBendParams{false, 0.7f, 256.0f, 1.0f},
                                     WorldHorizonParams{false, 1500.0f}, 2.0f);
    CHECK(off.position.x == local.position.x);
    CHECK(off.position.y == local.position.y);
    CHECK(off.position.z == local.position.z);
    CHECK(off.size.x == local.size.x);
    CHECK(off.size.y == local.size.y);
    CHECK(off.size.z == local.size.z);
}

TEST_CASE("world effects: the growth a box is remembered by moves whenever the box does") {
    // The per-block refresh leaves an instance alone when its remembered growth
    // has not really changed, so the growth has to be a faithful fingerprint of
    // the box: two camera positions that give different boxes must give
    // different growths, or the boxes go stale and the world has a hole in it.
    const AABB local(Vector3(0, 0, 0), Vector3(32, 32, 32));
    const Vector3 origin(2048, 64, 0);
    const WorldBendParams bend{true, 0.7f, 256.0f, 1.0f};
    const WorldHorizonParams curve{true, 1500.0f};
    Vector2 previous(-1.0f, -1.0f);
    const std::vector<Vector3> cameras = {
        Vector3(0, 70, 0), Vector3(64, 70, 0), Vector3(64, 70, -64),
        Vector3(-512, 70, 512), Vector3(0, 200, 0),
    };
    for (const auto& camera : cameras) {
        const AABB box = world_cull_aabb(local, origin, camera, bend, curve, 2.0f);
        const Vector2 growth = world_cull_growth(local, box);
        CHECK(growth.x > 0.0f);
        CHECK(growth.y > 0.0f);
        CHECK_FALSE(world_cull_growth_unchanged(previous, growth, 0.25f));
        previous = growth;
    }
    // A growth that has never been written is negative, which no real growth is:
    // the first box for an instance is always written.
    CHECK_FALSE(world_cull_growth_unchanged(Vector2(-1.0f, -1.0f), Vector2(0.0f, 0.0f), 0.25f));
    CHECK(world_cull_growth_unchanged(Vector2(1.0f, 2.0f), Vector2(1.1f, 1.9f), 0.25f));
    // A camera that has moved vertically only changes nothing: the curve is
    // measured from the camera's own vertical axis, so a box is the same box
    // whether the player is standing or jumping.
    const AABB at_70 = world_cull_aabb(local, origin, Vector3(64, 70, 0), bend, curve, 2.0f);
    const AABB at_90 = world_cull_aabb(local, origin, Vector3(64, 90, 0), bend, curve, 2.0f);
    CHECK(at_70.position.y == at_90.position.y);
    CHECK(at_70.size.y == at_90.size.y);
}
