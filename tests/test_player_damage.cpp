#include "doctest.h"

#include "engine/player_damage.hpp"

using namespace VoxelEngine;

// The damage chain is pure arithmetic, so these pin each stage on its own and
// then the composed order. Nothing here needs a world or a tick.

TEST_CASE("Damage: difficulty scaling") {
    // Peaceful is the only difficulty that can zero a hit outright.
    CHECK(scale_by_difficulty(20.0f, 0) == doctest::Approx(0.0f));
    CHECK(scale_by_difficulty(1.0f, 0) == doctest::Approx(0.0f));

    // Easy adds a floor of one, so a light hit still stings.
    CHECK(scale_by_difficulty(4.0f, 1) == doctest::Approx(3.0f));
    CHECK(scale_by_difficulty(1.0f, 1) == doctest::Approx(1.5f));
    CHECK(scale_by_difficulty(0.0f, 1) == doctest::Approx(1.0f));

    // Normal passes the amount through untouched.
    CHECK(scale_by_difficulty(7.0f, 2) == doctest::Approx(7.0f));

    // Hard is one and a half times, and stays fractional.
    CHECK(scale_by_difficulty(4.0f, 3) == doctest::Approx(6.0f));
    CHECK(scale_by_difficulty(3.0f, 3) == doctest::Approx(4.5f));

    // An unexpected difficulty behaves as normal rather than dropping the hit.
    CHECK(scale_by_difficulty(5.0f, 99) == doctest::Approx(5.0f));
}

TEST_CASE("Damage: blocking halves plus one") {
    // (amount + 1) * 0.5: a small hit is cut less than proportionally.
    CHECK(apply_blocking(20.0f) == doctest::Approx(10.5f));
    CHECK(apply_blocking(4.0f) == doctest::Approx(2.5f));
    CHECK(apply_blocking(1.0f) == doctest::Approx(1.0f));
    CHECK(apply_blocking(0.0f) == doctest::Approx(0.5f));
}

TEST_CASE("Damage: armor gives 4% per point, capped at 20") {
    CHECK(apply_armor(20.0f, 0.0f) == doctest::Approx(20.0f));
    CHECK(apply_armor(20.0f, 10.0f) == doctest::Approx(12.0f));
    CHECK(apply_armor(20.0f, 20.0f) == doctest::Approx(4.0f));

    // Below zero and above the cap clamp instead of amplifying or inverting.
    CHECK(apply_armor(20.0f, -5.0f) == doctest::Approx(20.0f));
    CHECK(apply_armor(20.0f, 100.0f) == doctest::Approx(4.0f));
}

TEST_CASE("Damage: resistance is 20% per level, capped") {
    CHECK(apply_resistance(20.0f, 0.0f) == doctest::Approx(20.0f)); // none
    CHECK(apply_resistance(20.0f, 1.0f) == doctest::Approx(16.0f)); // 20% off
    CHECK(apply_resistance(20.0f, 4.0f) == doctest::Approx(4.0f));  // 80% off

    // Level V would be 25 points; the 20-point cap holds it at 80% off, and a
    // wilder level cannot push the factor past the cap either.
    CHECK(apply_resistance(20.0f, 5.0f) == doctest::Approx(4.0f));
    CHECK(apply_resistance(20.0f, 100.0f) == doctest::Approx(4.0f));
}

TEST_CASE("Damage: protection points are capped like armor") {
    CHECK(apply_protection(20.0f, 0.0f) == doctest::Approx(20.0f));
    CHECK(apply_protection(20.0f, 10.0f) == doctest::Approx(12.0f));
    CHECK(apply_protection(20.0f, 20.0f) == doctest::Approx(4.0f));
    CHECK(apply_protection(20.0f, 100.0f) == doctest::Approx(4.0f));
}

TEST_CASE("Damage: absorption is spent before health") {
    // Buffer smaller than the hit: it soaks its own amount, health takes the rest.
    {
        DamageResult r = apply_absorption(5.0f, 2.0f);
        CHECK(r.total == doctest::Approx(5.0f));
        CHECK(r.absorption_delta == doctest::Approx(2.0f));
        CHECK(r.health_delta == doctest::Approx(3.0f));
    }
    // Exactly enough: health takes nothing.
    {
        DamageResult r = apply_absorption(5.0f, 5.0f);
        CHECK(r.absorption_delta == doctest::Approx(5.0f));
        CHECK(r.health_delta == doctest::Approx(0.0f));
    }
    // More than enough: the buffer never overspends.
    {
        DamageResult r = apply_absorption(5.0f, 10.0f);
        CHECK(r.absorption_delta == doctest::Approx(5.0f));
        CHECK(r.health_delta == doctest::Approx(0.0f));
    }
    // No buffer: everything reaches health.
    {
        DamageResult r = apply_absorption(5.0f, 0.0f);
        CHECK(r.absorption_delta == doctest::Approx(0.0f));
        CHECK(r.health_delta == doctest::Approx(5.0f));
    }
}

TEST_CASE("Damage: the two deltas always sum to the total") {
    const float amounts[] = {0.5f, 1.0f, 3.0f, 4.5f, 20.0f};
    const float buffers[] = {0.0f, 0.5f, 2.0f, 20.0f, 50.0f};
    for (float amount : amounts) {
        for (float buffer : buffers) {
            DamageResult r = apply_absorption(amount, buffer);
            CHECK(r.health_delta + r.absorption_delta == doctest::Approx(r.total));
        }
    }
}

TEST_CASE("Damage: the reduction order composes inside out") {
    // Verified by hand through the chain: blocking, then armor, then
    // resistance, then protection.
    DamageSource source;
    source.amount = 20.0f;

    DamageInputs inputs;
    inputs.blocking = true;
    inputs.armor_points = 20.0f;
    inputs.resistance_level = 1.0f;
    inputs.protection_points = 20.0f;
    inputs.difficulty = 2; // normal, so the amount reaches the reductions as-is

    DamageResult r = compute_damage(source, inputs);
    // 20 -> blocking 10.5 -> armor *0.2 = 2.1 -> resistance *0.8 = 1.68
    // -> protection *0.2 = 0.336
    CHECK(r.total == doctest::Approx(0.336f));
    CHECK(r.health_delta == doctest::Approx(0.336f));
    CHECK(r.absorption_delta == doctest::Approx(0.0f));
}

TEST_CASE("Damage: unblockable skips blocking and armor") {
    DamageSource source;
    source.amount = 20.0f;
    source.unblockable = true;

    DamageInputs inputs;
    inputs.blocking = true;
    inputs.armor_points = 20.0f;
    inputs.resistance_level = 1.0f;   // still applies
    inputs.protection_points = 20.0f; // still applies

    DamageResult r = compute_damage(source, inputs);
    // 20 -> resistance *0.8 = 16 -> protection *0.2 = 3.2
    CHECK(r.total == doctest::Approx(3.2f));
}

TEST_CASE("Damage: absolute skips resistance and protection") {
    DamageSource source;
    source.amount = 20.0f;
    source.absolute = true;

    DamageInputs inputs;
    inputs.blocking = true;          // still applies
    inputs.armor_points = 20.0f;     // still applies
    inputs.resistance_level = 1.0f;
    inputs.protection_points = 20.0f;

    DamageResult r = compute_damage(source, inputs);
    // 20 -> blocking 10.5 -> armor *0.2 = 2.1
    CHECK(r.total == doctest::Approx(2.1f));
}

TEST_CASE("Damage: peaceful deals nothing at all") {
    DamageSource source;
    source.amount = 20.0f;

    DamageInputs inputs;
    inputs.difficulty = 0;
    inputs.absorption = 10.0f;

    DamageResult r = compute_damage(source, inputs);
    CHECK(r.total == doctest::Approx(0.0f));
    CHECK(r.health_delta == doctest::Approx(0.0f));
    CHECK(r.absorption_delta == doctest::Approx(0.0f));
}

TEST_CASE("Damage: fall reaches the chain through the same door") {
    // Fall is just another source; it is not special-cased by the reductions.
    DamageSource fall;
    fall.kind = DamageKind::Fall;
    fall.amount = 4.0f;

    DamageInputs inputs;
    inputs.difficulty = 2;

    DamageResult r = compute_damage(fall, inputs);
    CHECK(r.total == doctest::Approx(4.0f));
    CHECK(r.health_delta == doctest::Approx(4.0f));
}

TEST_CASE("Knockback direction: flat, unit, directly away from the attacker") {
    // Victim to one side of the attacker: the direction is the horizontal line
    // between them, ignoring both bodies' y (a hit from above still shoves
    // sideways) and the attacker's facing.
    const godot::Vector3 victim(5.0f, 3.0f, 5.0f);
    const godot::Vector3 attacker(5.0f, 1.0f, 1.0f);  // attacker at smaller z
    const godot::Vector3 dir = knockback_direction(victim, attacker);
    CHECK(dir.x == doctest::Approx(0.0f));
    CHECK(dir.y == doctest::Approx(0.0f));
    CHECK(dir.z == doctest::Approx(1.0f));   // pushed to larger z, away
    CHECK(dir.length() == doctest::Approx(1.0f));
}

TEST_CASE("Knockback direction: diagonal offsets normalize") {
    const godot::Vector3 victim(3.0f, 0.0f, 4.0f);
    const godot::Vector3 attacker(0.0f, 0.0f, 0.0f);
    const godot::Vector3 dir = knockback_direction(victim, attacker);
    CHECK(dir.x == doctest::Approx(0.6f));
    CHECK(dir.y == doctest::Approx(0.0f));
    CHECK(dir.z == doctest::Approx(0.8f));
    CHECK(dir.length() == doctest::Approx(1.0f));
}

TEST_CASE("Knockback direction: stacked bodies fall back to a fixed axis") {
    // Attacker and victim share an x/z: the true direction is undefined, so the
    // helper returns a unit vector rather than a NaN.
    const godot::Vector3 p(2.0f, 1.0f, 2.0f);
    const godot::Vector3 dir = knockback_direction(p, p);
    CHECK(dir.y == doctest::Approx(0.0f));
    CHECK(dir.length() == doctest::Approx(1.0f));
    CHECK(dir.x == doctest::Approx(1.0f));
}

TEST_CASE("Hurt lean: the angle is zero at both ends and peaks partway through") {
    // A hit's lean is 0 the instant it lands and 0 when the window closes; the
    // view swings out and settles rather than snapping to full tilt.
    CHECK(hurt_lean_angle(0.0f) == doctest::Approx(0.0f));
    CHECK(hurt_lean_angle(1.0f) == doctest::Approx(0.0f));

    // The peak is where the shaped fraction reaches a quarter turn: f^4 == 0.5.
    const float f_peak = 0.8408964f;
    CHECK(hurt_lean_angle(f_peak) == doctest::Approx(kHurtLeanMaxRadians));
    CHECK(hurt_lean_angle(f_peak) > hurt_lean_angle(0.5f));
    CHECK(hurt_lean_angle(0.5f) > 0.0f);

    // Outside the window it clamps instead of wrapping back into a lean.
    CHECK(hurt_lean_angle(-0.5f) == doctest::Approx(0.0f));
    CHECK(hurt_lean_angle(2.0f) == doctest::Approx(0.0f));
}

TEST_CASE("Hurt lean: the view tips toward the push direction") {
    const godot::Basis upright;  // no pitch, no yaw
    const float f = 0.8408964f;  // full lean

    // Pushed toward +Z, the view's up tips toward +Z.
    {
        const godot::Basis leaned = hurt_lean_view(upright, godot::Vector3(0, 0, 1), f);
        const godot::Vector3 up = leaned.get_column(1);
        CHECK(up.z > 0.0f);
        CHECK(up.x == doctest::Approx(0.0f));
    }
    // Pushed toward +X, the view's up tips toward +X (the lean axis flips).
    {
        const godot::Basis leaned = hurt_lean_view(upright, godot::Vector3(1, 0, 0), f);
        CHECK(leaned.get_column(1).x > 0.0f);
    }
    // A vertical push has no horizontal bearing: the view is left alone.
    {
        const godot::Basis leaned = hurt_lean_view(upright, godot::Vector3(0, 5, 0), f);
        CHECK(leaned.get_column(1).y == doctest::Approx(1.0f));
    }
    // Outside the window the base orientation is returned unchanged, so a caller
    // can apply it every frame.
    {
        const godot::Basis base = godot::Basis::from_euler(godot::Vector3(0.3f, 0.0f, 0.0f));
        const godot::Basis leaned = hurt_lean_view(base, godot::Vector3(0, 0, 1), 0.0f);
        for (int i = 0; i < 3; ++i) {
            CHECK(leaned.get_column(i).is_equal_approx(base.get_column(i)));
        }
    }
}
