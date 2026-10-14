#include "doctest.h"

#include "player_controller_test_support.hpp"

#include "core/block_types.hpp"
#include "core/chunk_map.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/player_controller.hpp"

#include <cmath>

using namespace VoxelEngine;
using namespace godot;
using namespace player_controller_test;

// The hurt window lives in the sim and needs a tick, so these stand on a flat
// floor and drive the sim with the real 20 Hz accumulator. The pure arithmetic
// is pinned in test_player_damage.cpp; here it is the stateful half: what
// registers, what a repeat does, and how a landing enters the same door.

namespace {

// A sim standing still, past the initial settle, with an empty window.
void settle(PlayerSim& pc, CollisionResolver& cr) {
    PlayerInput idle;
    for (int i = 0; i < 5; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
}

} // namespace

TEST_CASE("Damage window: a queued hit lands fully and opens both windows") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);

    // Nothing lands until a tick resolves the queue.
    DamageSource hit;
    hit.amount = 6.0f;
    pc.queue_damage(hit);
    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(0.0f));

    PlayerInput idle;
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(6.0f));
    CHECK(e.hurt);
    // Opened this tick, so the full windows are still standing.
    CHECK(pc.get_hurt_resist_ticks() == kHurtResistTime);
    CHECK(pc.get_hurt_anim_ticks() == kHurtAnimTime);
    // A second drain is empty: consuming clears the accumulators.
    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(0.0f));
    CHECK_FALSE(pc.consume_damage_events().hurt);
}

TEST_CASE("Damage window: an equal repeat inside the first half is resisted") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 6.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    (void)pc.consume_damage_events();
    const int after_first = pc.get_hurt_resist_ticks();
    CHECK(after_first == kHurtResistTime);

    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(0.0f));
    CHECK_FALSE(e.hurt);
    // The window is not reset; it merely ticks down.
    CHECK(pc.get_hurt_resist_ticks() == after_first - 1);
}

TEST_CASE("Damage window: a bigger repeat applies only its excess, no feedback") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource first;
    first.amount = 6.0f;
    pc.queue_damage(first);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    (void)pc.consume_damage_events();

    DamageSource bigger;
    bigger.amount = 10.0f;
    pc.queue_damage(bigger);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(4.0f));  // 10 - 6
    CHECK_FALSE(e.hurt);
    CHECK(pc.get_hurt_resist_ticks() == kHurtResistTime - 1);
}

TEST_CASE("Damage window: once the first half passes, a hit lands fully again") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 6.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    (void)pc.consume_damage_events();

    // Let the resistance window fall to or below the midpoint.
    for (int i = 0; i < kHurtResistWindow; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_hurt_resist_ticks() <= kHurtResistWindow);

    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(6.0f));
    CHECK(e.hurt);
    CHECK(pc.get_hurt_resist_ticks() == kHurtResistTime);
}

TEST_CASE("Damage window: absorption soaks the hit before health") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    pc.set_absorption(3.0f);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 5.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(2.0f));
    CHECK(pc.get_absorption() == doctest::Approx(0.0f));
}

TEST_CASE("Damage window: peaceful drops the hit and opens nothing") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    DamageInputs inputs;
    inputs.difficulty = 0;
    pc.set_damage_inputs(inputs);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 5.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(0.0f));
    CHECK_FALSE(e.hurt);
    CHECK(pc.get_hurt_resist_ticks() == 0);
}

TEST_CASE("Damage window: a landing enters the same queue and window") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    // Floor top sits at y=1, so starting at 8.5 is a 7.5-block drop -> 4.
    pc.reset(Vector3(0.5f, 8.5f, 0.5f));
    PlayerInput idle;
    for (int i = 0; i < 60 && !pc.is_on_floor(); ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.is_on_floor());

    // The fall-specific view agrees with the unified one; reading it first,
    // because draining the unified events drains this slice too.
    CHECK(pc.consume_pending_fall_damage() == 4);
    DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(4.0f));
    CHECK(e.hurt);
}

TEST_CASE("Damage window: an attacker hit shoves the body away and up") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    // Resting: no horizontal motion (vertical is only this tick's gravity).
    CHECK(pc.get_velocity().x == doctest::Approx(0.0f));
    CHECK(pc.get_velocity().z == doctest::Approx(0.0f));

    PlayerInput idle;
    DamageSource hit;
    hit.amount = 4.0f;
    hit.by_attacker = true;
    hit.attacker_position = Vector3(0.5f, 1.0f, 1.5f);  // attacker at larger z
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    // Pushed away from the attacker (-z) by the full base speed, and lifted.
    const Vector3 v = pc.get_velocity();
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.z == doctest::Approx(-PlayerSim::KNOCKBACK_SPEED));
    CHECK(v.y > 0.0f);
    CHECK(v.y <= PlayerSim::KNOCKBACK_SPEED + 1e-5f);
    (void)pc.consume_damage_events();
}

TEST_CASE("Damage window: a landing shoves vertically with no horizontal impulse") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    // Resting: no horizontal motion (vertical is only this tick's gravity).
    CHECK(pc.get_velocity().x == doctest::Approx(0.0f));
    CHECK(pc.get_velocity().z == doctest::Approx(0.0f));

    PlayerInput idle;
    DamageSource landing;
    landing.kind = DamageKind::Fall;
    landing.amount = 4.0f;  // by_attacker defaults to false
    pc.queue_damage(landing);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    // A landing has no attacker direction, so every axis is halved and only the
    // vertical gets the lift: the horizontal stays zero (no push) and the
    // vertical is the resting gravity halved plus the base speed, capped. A fast
    // fall ends up pushed down instead (v.y*0.5 + 0.4 still negative).
    const Vector3 v = pc.get_velocity();
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.z == doctest::Approx(0.0f));
    CHECK(v.y > 0.0f);  // from rest, the lift wins
    CHECK(v.y <= PlayerSim::KNOCKBACK_SPEED + 1e-5f);
    CHECK(pc.consume_pending_fall_damage() == 4);
}

TEST_CASE("Damage window: a resisted repeat does not shove again") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 4.0f;
    hit.by_attacker = true;
    hit.attacker_position = Vector3(0.5f, 1.0f, 1.5f);
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    (void)pc.consume_damage_events();

    // Let the body coast a few ticks, still inside the first half of the window.
    for (int i = 0; i < 6; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    const Vector3 before = pc.get_velocity();
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    // A resisted hit is a no-op: no damage, no feedback, no new shove. The
    // velocity only differs by this tick's own drag on the coasting body, so a
    // component cannot have jumped by anywhere near the knockback speed.
    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(0.0f));
    CHECK_FALSE(e.hurt);
    const Vector3 after = pc.get_velocity();
    CHECK(std::abs(after.x - before.x) < PlayerSim::KNOCKBACK_SPEED);
    CHECK(std::abs(after.z - before.z) < PlayerSim::KNOCKBACK_SPEED);
}

TEST_CASE("Damage window: a non-fall environmental hit never shoves") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    // A landing is the one environmental source that shoves (its own test); a
    // fire, a drown or a cactus still does not.
    DamageSource fire;
    fire.kind = DamageKind::Fire;
    fire.amount = 4.0f;  // by_attacker defaults to false
    pc.queue_damage(fire);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    CHECK(pc.consume_damage_events().health_delta == doctest::Approx(4.0f));
    const Vector3 v = pc.get_velocity();
    CHECK(v.x == doctest::Approx(0.0f));
    CHECK(v.z == doctest::Approx(0.0f));
    CHECK(v.y <= 0.0f);  // only the resting gravity, no lift
}

TEST_CASE("Damage window: an absorbed hit still reports its full weight") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    pc.set_absorption(4.0f);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 4.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);

    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(0.0f));  // buffer ate it all
    CHECK(e.amount == doctest::Approx(4.0f));        // but the hit has a size
    CHECK(e.hurt);                                   // and still counts as a hit
}

TEST_CASE("Damage window: a repeat that lands only excess reports that excess") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource first;
    first.amount = 6.0f;
    pc.queue_damage(first);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    const DamageEvents opened = pc.consume_damage_events();
    CHECK(opened.amount == doctest::Approx(6.0f));

    DamageSource bigger;
    bigger.amount = 10.0f;
    pc.queue_damage(bigger);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    const DamageEvents e = pc.consume_damage_events();
    CHECK(e.health_delta == doctest::Approx(4.0f));
    CHECK(e.amount == doctest::Approx(4.0f));  // only the part that landed
    CHECK_FALSE(e.hurt);
}

TEST_CASE("Damage window: the hurt view samples the clock forward, with no lag") {
    ChunkMap cm;
    fill_flat_floor(cm);
    CollisionResolver cr(&cm);
    PlayerSim pc;
    pc.reset(Vector3(0.5f, 1.0f, 0.5f));
    settle(pc, cr);
    PlayerInput idle;

    DamageSource hit;
    hit.amount = 6.0f;
    pc.queue_damage(hit);
    pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    (void)pc.consume_damage_events();

    // On the hit's own tick the lean is full, and it is already falling: no
    // one-tick hold at the top (the fixed-step render lag).
    CHECK(pc.get_hurt_anim_ticks() == kHurtAnimTime);
    CHECK(pc.get_render_hurt_anim_ticks(0.0f) == doctest::Approx(static_cast<float>(kHurtAnimTime)));
    CHECK(pc.get_render_hurt_anim_ticks(0.5f) == doctest::Approx(static_cast<float>(kHurtAnimTime) - 0.5f));

    // A decay tick left part-way through (0.02 s of the 0.05 s step remains):
    // the value falls smoothly from this tick toward the next, meeting it exactly.
    pc.accumulate_and_tick(1.0 / 20.0 + 0.02, idle, cr);
    const float partial = pc.get_accumulator_fraction();
    CHECK(pc.get_hurt_anim_ticks() == kHurtAnimTime - 1);
    CHECK(pc.get_render_hurt_anim_ticks(0.0f) == doctest::Approx(static_cast<float>(kHurtAnimTime - 1)));
    CHECK(pc.get_render_hurt_anim_ticks(partial) == doctest::Approx(static_cast<float>(kHurtAnimTime - 1) - partial));
    CHECK(pc.get_render_hurt_anim_ticks(1.0f) == doctest::Approx(static_cast<float>(kHurtAnimTime - 2)));

    // Once the window is spent the sample never goes negative.
    for (int i = 0; i < kHurtAnimTime; ++i) {
        pc.accumulate_and_tick(1.0 / 20.0, idle, cr);
    }
    CHECK(pc.get_hurt_anim_ticks() == 0);
    CHECK(pc.get_render_hurt_anim_ticks(0.5f) == doctest::Approx(0.0f));
}
