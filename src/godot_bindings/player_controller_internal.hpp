#pragma once

// The player's feel constants (camera offsets, look clamp, body-turn rate, punch
// reach and cadence) and the two small helpers built on them. They lived in an
// anonymous namespace while the whole binding was one file; a file-local name
// cannot be shared, so they are `inline` here and included by the files that use
// them — godot_bindings/player_controller.cpp, _input.cpp and _interact.cpp
// — and nowhere else.
//
// `kPunchInterval` is why this header has to exist at all: `_input` arms the punch
// cooldown with it, and `update_break_progress` (mining) re-arms it while the
// trigger is held.

#include <godot_cpp/variant/vector3.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace VoxelEngine {
namespace player_detail {

// The two helpers take and return godot vectors, and the file this came from had
// `using namespace godot;` at file scope. A using-declaration keeps that readable
// without a using-directive in a header.
using godot::Vector3;

// vanilla's third-person camera sits 4 blocks back (thirdPersonView uses 4.0
// for both the back and front views) and is pulled in when it would clip
// through solid terrain. The front view mirrors the offset along the look dir.
inline constexpr float kThirdPersonOffset = 4.0f;
inline constexpr float kCameraStep = 0.25f;    // camera-collision sampling step, blocks
inline constexpr float kCameraMinDist = 0.25f; // never push the camera into the player
inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kTwoPi = 6.28318530718f;
inline constexpr float kMaxLookPitch = kPi / 2.0f; // ±90°, matching vanilla
// Body yaw (body-yaw): eases 0.3 of the remaining gap per 20 Hz tick and
// is clamped to ±35° from the look yaw — the dead zone the head can lead the
// body by before the torso is dragged along (vanilla uses ±75°; 35° reads
// tighter and more responsive).
inline constexpr float kBodyTurnPerTick = 0.3f;
inline constexpr float kBodyMaxYaw = 35.0f * kPi / 180.0f;
// Punch reach — vanilla survival attack reach (3.0 blocks; creative is 5.0).
// Block mining keeps its own (10-block) range, so this only gates punching
// the pose-clone dummy.
inline constexpr float kPunchReach = 3.0f;
// vanilla player collision box: 0.6 wide, 1.8 tall, origin at the feet. Matches
// dummy.gd's physics size and the dummy host node (position = feet center).
inline constexpr float kDummyHalfWidth = 0.3f;
inline constexpr float kDummyHeight = 1.8f;
// Vanilla attack cadence: while LMB is held the player re-attacks every
// 10 ticks (0.5 s at 20 tps); the dummy's hurt-resistance gate then
// throttles actual knockback to the same ~10-tick cadence.
inline constexpr float kPunchInterval = 0.5f;

inline float wrap_pi(float a) {
    return std::remainder(a, kTwoPi);
}

// Slab-method ray vs AABB intersection. Returns t along dir at entry, or -1.
inline float ray_aabb_hit(const Vector3& origin, const Vector3& dir,
                   const Vector3& box_min, const Vector3& box_max) {
    float tmin = -std::numeric_limits<float>::max();
    float tmax = std::numeric_limits<float>::max();
    for (int axis = 0; axis < 3; ++axis) {
        const float o = origin[axis];
        const float d = dir[axis];
        const float lo = box_min[axis];
        const float hi = box_max[axis];
        if (std::fabs(d) < 1e-8f) {
            if (o < lo || o > hi) return -1.0f;
        } else {
            float t1 = (lo - o) / d;
            float t2 = (hi - o) / d;
            if (t1 > t2) std::swap(t1, t2);
            tmin = std::max(tmin, t1);
            tmax = std::min(tmax, t2);
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin;
}

} // namespace player_detail
} // namespace VoxelEngine
