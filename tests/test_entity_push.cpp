#include "doctest.h"

#include "engine/entity_push.hpp"

#include <cmath>
#include <vector>

using VoxelEngine::kEntityPush;
using VoxelEngine::PushBody;
using VoxelEngine::PushPair;
using VoxelEngine::push_apart;
using VoxelEngine::push_apart_all;

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

TEST_CASE("the batched push is the pairwise push, summed per body") {
    // A crowd packed inside one body's width of its neighbours plus one player leaning on it:
    // exactly the shape the goblin script used to walk pair by pair, Dictionary by Dictionary.
    const std::vector<PushBody> crowd = {
        at(0.0f, 10.0f, 0.0f), at(0.35f, 10.0f, 0.1f), at(-0.2f, 10.0f, 0.3f),
        at(0.1f, 10.0f, -0.4f), at(0.5f, 10.0f, 0.5f), at(0.0f, 13.0f, 0.0f),
    };
    std::vector<godot::Vector3> batched(crowd.size());
    push_apart_all(crowd.data(), crowd.size(), batched.data(), batched.size());

    for (size_t i = 0; i < crowd.size(); ++i) {
        godot::Vector3 expected(0.0f, 0.0f, 0.0f);
        for (size_t j = 0; j < crowd.size(); ++j) {
            if (i == j) continue;
            // `crowd[i]` is always the first argument, so its own half is `a` whichever side of
            // the pair it was listed on -- the sum of a body's own pairs, which is what the
            // per-pair script loop built.
            const PushPair pair = push_apart(crowd[i], crowd[j]);
            if (!pair.touching) continue;
            expected += pair.a;
        }
        CHECK(batched[i].x == doctest::Approx(expected.x).epsilon(0.000001));
        CHECK(batched[i].z == doctest::Approx(expected.z).epsilon(0.000001));
        CHECK(batched[i].y == doctest::Approx(0.0f));
    }
    // The body one storey up is in the crowd and is not pushed by it.
    CHECK(batched.back().x == doctest::Approx(0.0f));
    CHECK(batched.back().z == doctest::Approx(0.0f));
    // ...and a batched crowd does move: the middle of a packed group is pushed somewhere.
    CHECK(std::fabs(batched[0].x) + std::fabs(batched[0].z) > 0.0f);
}

TEST_CASE("the batched push does not depend on the order the bodies are listed in") {
    const std::vector<PushBody> forward = {
        at(0.0f, 10.0f, 0.0f), at(0.3f, 10.0f, 0.0f), at(0.0f, 10.0f, 0.3f)};
    const std::vector<PushBody> reversed = {forward[2], forward[1], forward[0]};
    std::vector<godot::Vector3> a(3);
    std::vector<godot::Vector3> b(3);
    push_apart_all(forward.data(), forward.size(), a.data(), a.size());
    push_apart_all(reversed.data(), reversed.size(), b.data(), b.size());

    CHECK(b[0].x == doctest::Approx(a[2].x));
    CHECK(b[0].z == doctest::Approx(a[2].z));
    CHECK(b[1].x == doctest::Approx(a[1].x));
    CHECK(b[1].z == doctest::Approx(a[1].z));
    CHECK(b[2].x == doctest::Approx(a[0].x));
    CHECK(b[2].z == doctest::Approx(a[0].z));
    // One shared push per pair still comes to nothing overall: the sums cancel.
    godot::Vector3 total(0.0f, 0.0f, 0.0f);
    for (const godot::Vector3& d : a) total += d;
    CHECK(total.x == doctest::Approx(0.0f).epsilon(0.000001));
    CHECK(total.z == doctest::Approx(0.0f).epsilon(0.000001));
}

TEST_CASE("a crowd with nothing touching is pushed nowhere") {
    const std::vector<PushBody> spread = {
        at(0.0f, 10.0f, 0.0f), at(3.0f, 10.0f, 0.0f), at(0.0f, 10.0f, 3.0f)};
    std::vector<godot::Vector3> deltas(spread.size());
    push_apart_all(spread.data(), spread.size(), deltas.data(), deltas.size());
    for (const godot::Vector3& d : deltas) {
        CHECK(d.x == doctest::Approx(0.0f));
        CHECK(d.z == doctest::Approx(0.0f));
    }
}

TEST_CASE("the player's delta is the sum of their own pairs") {
    // The player is the LAST entry, as the GDScript side lists them: one body leaning on two goblins
    // takes the sum of both pushes, and each goblin takes the player's share of its own pair.
    const std::vector<PushBody> crowd = {
        at(0.0f, 10.0f, 0.0f), at(1.2f, 10.0f, 0.0f), PushBody{godot::Vector3(0.5f, 10.0f, 0.2f), kBody}};
    std::vector<godot::Vector3> deltas(crowd.size());
    push_apart_all(crowd.data(), crowd.size(), deltas.data(), deltas.size());

    godot::Vector3 player_expected(0.0f, 0.0f, 0.0f);
    for (size_t i = 0; i + 1 < crowd.size(); ++i) {
        const PushPair pair = push_apart(crowd[i], crowd.back());
        if (pair.touching) player_expected += pair.b;
    }
    CHECK(deltas.back().x == doctest::Approx(player_expected.x).epsilon(0.000001));
    CHECK(deltas.back().z == doctest::Approx(player_expected.z).epsilon(0.000001));
    CHECK(deltas.back().y == doctest::Approx(0.0f));
    // ...and the crowd did push them: a player in the middle of two goblins is never left alone.
    CHECK(std::fabs(deltas.back().x) + std::fabs(deltas.back().z) > 0.0f);
}
