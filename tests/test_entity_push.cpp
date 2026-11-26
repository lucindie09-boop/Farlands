#include "doctest.h"

#include "engine/entity_push.hpp"

#include <cmath>

using VoxelEngine::kEntityPush;
using VoxelEngine::PushBody;
using VoxelEngine::PushPair;
using VoxelEngine::push_apart;

namespace {

const godot::Vector3 kBody(0.6f, 1.8f, 0.6f);

PushBody at(float x, float y, float z) {
    return PushBody{godot::Vector3(x, y, z), kBody};
}

} // namespace

TEST_CASE("a pair of bodies in one place is pushed apart, horizontally, and both ways") {
    // 0.4 apart: the boxes overlap by 0.2, so this is a pair that has to come apart.
    const PushPair pair = push_apart(at(0.0f, 10.0f, 0.0f), at(0.4f, 10.0f, 0.0f));
    CHECK(pair.touching);
    // b is on a's +x side, so a is pushed -x and b +x, and they are exact opposites: one push
    // shared, not two pushes of the same size.
    CHECK(pair.a.x < 0.0f);
    CHECK(pair.b.x > 0.0f);
    CHECK(pair.a.x == doctest::Approx(-pair.b.x));
    CHECK(pair.a.z == doctest::Approx(0.0f));
    CHECK(pair.a.y == doctest::Approx(0.0f));
    CHECK(pair.b.y == doctest::Approx(0.0f));
    // The reference's arithmetic, to the letter: normalised by the SQUARE ROOT of the larger
    // centre gap, factor capped at one. At 0.4 that is 0.4 / sqrt(0.4) * 0.05 = 0.0316.
    CHECK(std::fabs(pair.a.x) == doctest::Approx(0.4f / std::sqrt(0.4f) * kEntityPush).epsilon(0.0001));
}

TEST_CASE("the push is capped at one tick's strength") {
    // Two bodies of the same width can never reach the cap: their centres are under a width
    // apart whenever the boxes still intersect, so the push is sqrt(gap) * 0.05 and always a
    // little under it. A WIDE body can reach it -- here a four-block one and a goblin a block
    // apart, which is one block / sqrt(one block) / sqrt(one block) = exactly one -- and that
    // is what stops a pair inside one large box from being flung out of it.
    const PushBody wide{godot::Vector3(0.0f, 10.0f, 0.0f), godot::Vector3(4.0f, 1.8f, 4.0f)};
    const PushPair pair = push_apart(wide, at(1.0f, 10.0f, 0.0f));
    CHECK(pair.touching);
    CHECK(std::fabs(pair.a.x) == doctest::Approx(kEntityPush).epsilon(0.0001));
    CHECK(std::fabs(pair.b.x) == doctest::Approx(kEntityPush).epsilon(0.0001));
}

TEST_CASE("a diagonal pair is pushed along its own diagonal") {
    const PushPair pair = push_apart(at(0.0f, 10.0f, 0.0f), at(0.4f, 10.0f, 0.4f));
    CHECK(pair.touching);
    CHECK(pair.a.x < 0.0f);
    CHECK(pair.a.z < 0.0f);
    CHECK(pair.a.x == doctest::Approx(pair.a.z));
    CHECK(pair.b.x == doctest::Approx(-pair.a.x));
}

TEST_CASE("bodies that do not intersect are not pushed") {
    // Apart on x: 1.0 between centres is 0.4 between boxes.
    CHECK_FALSE(push_apart(at(0.0f, 10.0f, 0.0f), at(1.0f, 10.0f, 0.0f)).touching);
    // Stacked: the same column, but a body three blocks above is standing on the first one's
    // head, not inside it.
    CHECK_FALSE(push_apart(at(0.0f, 10.0f, 0.0f), at(0.0f, 13.0f, 0.0f)).touching);
    // ...and just touching is not overlapping: at exactly one body width apart the boxes share
    // a face and neither is inside the other.
    CHECK_FALSE(push_apart(at(0.0f, 10.0f, 0.0f), at(0.6f, 10.0f, 0.0f)).touching);
}

TEST_CASE("a pair sharing a centre is left where it is") {
    // The reference's own hole, kept: with no separation there is no direction to pick, so it
    // picks none and the next tick's walk separates the pair. Pushing at random instead would
    // make an exactly-stacked pair jitter, which is worse than standing still for one tick.
    const PushPair pair = push_apart(at(0.0f, 10.0f, 0.0f), at(0.0f, 10.0f, 0.0f));
    CHECK(pair.touching);
    CHECK(pair.a.x == doctest::Approx(0.0f));
    CHECK(pair.a.z == doctest::Approx(0.0f));
    CHECK(pair.b.x == doctest::Approx(0.0f));
    CHECK(pair.b.z == doctest::Approx(0.0f));
}
