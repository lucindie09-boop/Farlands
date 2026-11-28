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

void push_apart_all(const PushBody* bodies, std::size_t count, godot::Vector3* out,
                    std::size_t out_count, float strength) {
    const std::size_t written = std::min(count, out_count);
    for (std::size_t i = 0; i < written; ++i) out[i] = godot::Vector3(0.0f, 0.0f, 0.0f);
    if (bodies == nullptr || count < 2) return;

    for (std::size_t i = 0; i < count; ++i) {
        const PushBody& a = bodies[i];
        for (std::size_t j = i + 1; j < count; ++j) {
            const PushBody& b = bodies[j];
            // Broad phase, in squared distances and in both axes at once: two boxes can only
            // intersect if their centre gap is inside the sum of their half-widths on each axis,
            // and that is bounded by the diagonal of those sums. Cheap enough to run on every
            // pair, and it throws out the whole crowd's far neighbours before the exact test.
            const float dx = b.feet.x - a.feet.x;
            const float dz = b.feet.z - a.feet.z;
            const float reach_x = 0.5f * (a.size.x + b.size.x);
            const float reach_z = 0.5f * (a.size.z + b.size.z);
            if (dx * dx + dz * dz > reach_x * reach_x + reach_z * reach_z) continue;
            // Vertical, which is exact and one comparison: a body standing on another's head is
            // not touching it.
            if (a.feet.y >= b.feet.y + b.size.y || b.feet.y >= a.feet.y + a.size.y) continue;

            const PushPair pair = push_apart(a, b, strength);
            if (!pair.touching) continue;
            if (i < written) out[i] += pair.a;
            if (j < written) out[j] += pair.b;
        }
    }
}

} // namespace VoxelEngine
