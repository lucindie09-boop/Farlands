#include "engine/entity_push.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

namespace {

// The reference's epsilon: a pair closer than this has no direction to be pushed in, so it is
// left where it is and the next tick's walk separates it. Kept at the same value so a pair that
// stacks exactly does not get a random shove here that it would not get in the game.
constexpr float kMinGap = 0.009999999776482582f;

godot::Vector3 box_min(const PushBody& body) {
    return godot::Vector3(body.feet.x - body.size.x * 0.5f, body.feet.y,
                          body.feet.z - body.size.z * 0.5f);
}

godot::Vector3 box_max(const PushBody& body) {
    return godot::Vector3(body.feet.x + body.size.x * 0.5f, body.feet.y + body.size.y,
                          body.feet.z + body.size.z * 0.5f);
}

bool boxes_intersect(const PushBody& a, const PushBody& b) {
    const godot::Vector3 a_min = box_min(a);
    const godot::Vector3 a_max = box_max(a);
    const godot::Vector3 b_min = box_min(b);
    const godot::Vector3 b_max = box_max(b);
    if (a_max.x <= b_min.x || b_max.x <= a_min.x) return false;
    if (a_max.y <= b_min.y || b_max.y <= a_min.y) return false;
    if (a_max.z <= b_min.z || b_max.z <= a_min.z) return false;
    return true;
}

} // namespace

PushPair push_apart(const PushBody& a, const PushBody& b, float strength) {
    PushPair out;
    // Not touching is not a push, whatever the centres say: a goblin three blocks away is not
    // pressed against anything.
    if (!boxes_intersect(a, b)) return out;
    out.touching = true;

    const float dx = b.feet.x - a.feet.x;
    const float dz = b.feet.z - a.feet.z;
    const float gap = std::max(std::fabs(dx), std::fabs(dz));
    // Exactly stacked: the reference picks no direction rather than an arbitrary one, so the
    // pair stays put until one of them walks.
    if (gap < kMinGap) return out;

    // The reference's arithmetic, kept to the letter because the FEEL is the point: the direction
    // is scaled by the square root of the gap, not by the gap, and the factor is capped at one.
    // A pair a block or more apart therefore gets the full push and a pair that barely overlaps
    // gets less, which is the reason a crowd pressed into a doorway does not explode apart.
    const float root = std::sqrt(gap);
    const float scale = std::min(1.0f / root, 1.0f);
    const float push_x = (dx / root) * scale * strength;
    const float push_z = (dz / root) * scale * strength;

    out.a = godot::Vector3(-push_x, 0.0f, -push_z);
    out.b = godot::Vector3(push_x, 0.0f, push_z);
    return out;
}

} // namespace VoxelEngine
