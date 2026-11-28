#ifndef FARLANDS_ENTITY_PUSH_HPP
#define FARLANDS_ENTITY_PUSH_HPP

// -----------------------------------------------------------------------------
// Entity against entity: the half of a body's motion that is not the world.
//
// The voxel resolver stops a body at terrain and stops at nothing else, so two
// bodies in one place were not a thing the engine could see: a goblin walked
// through the player and through its own pack, and five of them read as one
// silhouette. The reference game's answer is `Entity::push(Entity)`, and this is
// that arithmetic, kept out of the tick so it can be checked on its own:
//
//   * the two boxes have to INTERSECT. The reference gets this from the query that
//     finds the neighbours (`getEntities(this, getBoundingBox())`); here the boxes
//     are the caller's, so the test is part of the call;
//   * the pair is decided on the LARGER of the two centre gaps, horizontally, and a
//     pair whose centres are coincident is left alone — there is no direction to
//     pick. That is the reference's own epsilon and its own hole, and it is why the
//     gap to measure is the walk's, not this function's;
//   * the push is normalised by the SQUARE ROOT of that gap rather than by its
//     length, and the factor is capped at one, so a pair a block apart or further is
//     pushed the full `kEntityPush` and a pair that barely overlaps is nudged less;
//   * each body takes its delta as VELOCITY (`Entity::push` adds to the delta
//     movement) and the world then stops it at terrain, which is what makes a pack
//     spread out instead of stacking up.
//
// Godot's Vector3 only: no scene, no registry, no resolver, so the test binary reads
// the numbers.
// -----------------------------------------------------------------------------

#include <godot_cpp/variant/vector3.hpp>

#include <cstddef>

namespace VoxelEngine {

// One tick's full-strength push, in blocks a tick: 0.05 at 20 Hz is a block a second, which is
// the reference's own number and the reason a pressed-in crowd separates rather than jitters.
inline constexpr float kEntityPush = 0.05f;

// A body as the collision resolver sees it: feet at the BOTTOM CENTRE and the size of the box
// above them, which is 0.6 x 1.8 x 0.6 for both the player and a goblin.
struct PushBody {
    godot::Vector3 feet;
    godot::Vector3 size;
};

struct PushPair {
    bool touching = false;  // the two boxes intersect at all
    // Velocity to add to each body, horizontal only. `b` is the exact negation of `a`: one push
    // shared, so the pair separates by twice the delta no matter which side asks.
    godot::Vector3 a;
    godot::Vector3 b;
};

// The push for one pair, or `touching == false` when the boxes do not intersect (and both
// deltas zero when they intersect but share a centre).
[[nodiscard]] PushPair push_apart(const PushBody& a, const PushBody& b,
                                  float strength = kEntityPush);

// The whole crowd, in one call: every pair in `bodies` (and the player, when they are one of
// them) summed into one delta per body in `out`.
//
// The pairwise walk a script does at 100 bodies is ~10,000 array visits and ~5,000 native calls a
// tick, each returning a Dictionary — and all of it on the main thread. This is the same solve,
// in one call, with nothing allocated: a broad phase rejects pairs too far apart to touch, and the
// surviving pairs go through `push_apart`, whose arithmetic is unchanged. Each pair pushes both
// bodies once, so a body's delta is the sum of its neighbours' pushes exactly as the per-pair form
// would leave it — `tests/test_entity_push.cpp` pins that.
//
// The player is a body in the list rather than a special case: their own summed delta is simply
// theirs. `out_count` entries are written (missing ones are skipped rather than computed).
void push_apart_all(const PushBody* bodies, std::size_t count, godot::Vector3* out,
                    std::size_t out_count, float strength = kEntityPush);

} // namespace VoxelEngine

#endif // FARLANDS_ENTITY_PUSH_HPP
