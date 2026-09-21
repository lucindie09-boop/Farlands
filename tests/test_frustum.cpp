// The frustum's plane convention, pinned with numbers the engine itself produced.
//
// These six planes are what `Camera3D::get_frustum()` returned for a camera at
// (500, 100, 500) facing -Z with fov 70, near 0.05, far 2048 (measured by
// `.freebuff/probe_frustum.gd`, which also asked the engine's own
// `is_position_in_frustum` what the truth was). Their normals point OUTWARD.
//
// The class assumes the opposite (a point inside has a positive distance to all
// six), so `Frustum::update` negates them on the way in. Before it did, every
// box in the world tested as invisible: the generation sweep's frustum pass
// examined 2,376,704 candidates across one session and passed none, and no mesh
// was ever queued as `in_frustum`. The assertions below are written from the
// engine's answers, not from a reading of the docs, so this cannot drift back
// into passing by assuming the same wrong thing in two places.
#include "doctest.h"
#include "core/frustum.hpp"

using VoxelEngine::Frustum;

namespace {

// Verbatim from the camera probe.
godot::Plane near_plane()  { return godot::Plane(godot::Vector3(0.0f, 0.0f, 1.0f), 499.95f); }
godot::Plane far_plane()   { return godot::Plane(godot::Vector3(-0.0f, -0.0f, -1.0f), 1546.05f); }
godot::Plane left()        { return godot::Plane(godot::Vector3(-0.82f, 0.0f, 0.57f), -122.79f); }
godot::Plane top()         { return godot::Plane(godot::Vector3(0.0f, 0.82f, 0.57f), 368.70f); }
godot::Plane right()       { return godot::Plane(godot::Vector3(0.82f, 0.0f, 0.57f), 696.36f); }
godot::Plane bottom()      { return godot::Plane(godot::Vector3(0.0f, -0.82f, 0.57f), 204.87f); }

Frustum camera_frustum() {
    // In the order `Camera3D::get_frustum()` documents: near, far, left, top,
    // right, bottom.
    std::array<godot::Plane, 6> planes{ near_plane(), far_plane(), left(), top(), right(), bottom() };
    Frustum f;
    f.update(planes);
    return f;
}

constexpr float kHalf = 16.0f;  // half a chunk, the half-extent callers use

bool visible(const Frustum& f, godot::Vector3 center) {
    return f.is_aabb_visible(godot::AABB(center - godot::Vector3(kHalf, kHalf, kHalf),
                                         godot::Vector3(kHalf * 2.0f, kHalf * 2.0f, kHalf * 2.0f)));
}

} // namespace

TEST_CASE("the camera's outward planes become inward ones") {
    Frustum f = camera_frustum();
    // CHECK, not REQUIRE: the suite is built with exceptions disabled, where
    // doctest's aborting asserts are unavailable.
    CHECK(f.is_initialized());
    // The engine's own words for the same plane: `is_position_in_frustum` says a
    // box three chunks in front of the camera is visible, so with the class's
    // convention that box must have a POSITIVE distance — which needs d == -499.95.
    CHECK(near_plane().distance_to(godot::Vector3(500.0f, 100.0f, 404.0f)) < 0.0f);
    CHECK(visible(f, godot::Vector3(500.0f, 100.0f, 404.0f)));
}

TEST_CASE("a box in front of the camera is visible and one behind it is not") {
    Frustum f = camera_frustum();
    // Three chunks in front (the camera looks down -Z).
    CHECK(visible(f, godot::Vector3(500.0f, 100.0f, 500.0f - 3.0f * 32.0f)));
    // Forty chunks in front: still inside (far plane is 2048 away).
    CHECK(visible(f, godot::Vector3(500.0f, 100.0f, 500.0f - 40.0f * 32.0f)));
    // Three chunks behind the eye.
    CHECK_FALSE(visible(f, godot::Vector3(500.0f, 100.0f, 500.0f + 3.0f * 32.0f)));
    // Beyond the far plane.
    CHECK_FALSE(visible(f, godot::Vector3(500.0f, 100.0f, 500.0f - 90.0f * 32.0f)));
}

TEST_CASE("a box the camera is standing in is visible") {
    // The chunk grid path, which is what the sweeps actually call: the chunk
    // holding the eye straddles the near plane, so it is visible — the case that
    // matters most, because it is the chunk the player is inside of.
    Frustum f = camera_frustum();
    CHECK(f.is_chunk_visible(15, 3, 15));  // spans (480..512)^3, eye at (500,100,500)
    // ...and the one directly behind the camera is not.
    CHECK_FALSE(f.is_chunk_visible(15, 3, 18));  // spans z 576..608
}

TEST_CASE("an unset frustum treats everything as visible") {
    // The updater and the mesh queue both rely on this: a frustum that was never
    // fed must not silently cull the world.
    Frustum f;
    CHECK_FALSE(f.is_initialized());
    CHECK(f.is_chunk_visible(0, 0, 0));
    CHECK(f.is_aabb_visible(godot::AABB(godot::Vector3(), godot::Vector3(1.0f, 1.0f, 1.0f))));
}
