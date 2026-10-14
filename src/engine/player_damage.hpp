#ifndef FARLANDS_PLAYER_DAMAGE_HPP
#define FARLANDS_PLAYER_DAMAGE_HPP

#include <cstdint>

#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/vector3.hpp>

// The receiving half of combat, with no Godot node and no state: one incoming
// hit (a DamageSource) enters a defensive snapshot (DamageInputs) and leaves as
// what it cost the player (a DamageResult). Every rule is a small pure function
// so each is its own test, and the ordering lives in exactly one place
// (reduce_damage) instead of being implied by the caller.
//
// The live side lives elsewhere: the per-tick hurt window, the timers and the
// knockback into the player's own velocity belong to PlayerSim, which owns a
// tick; this header only answers "given this hit and these numbers, how much
// health does it take". Keeping the arithmetic here and the state there is what
// lets the whole chain be pinned without a world or a frame.

namespace VoxelEngine {

// What harmed the player. The kind is recorded so an event, a death message or
// a future status effect can key on it; none of the reduction stages branch on
// it, they branch on the two flags below.
enum class DamageKind : std::uint8_t {
    Generic,
    Fall,
    Drown,
    Suffocate,
    Void,
    Fire,
    Cactus,
    Starve,
};

// One incoming hit, before any reduction. `amount` is in half-hearts and stays
// a float so the stages keep their fractions instead of truncating at each
// step; the difficulty stage is the only one that can zero it outright.
//
// `unblockable` and `absolute` are two flags rather than one because the
// reference's are: the first is the armor-piercing family (void, some magic)
// and skips blocking and armor, the second is the potion-and-enchantment
// family and skips resistance and protection. An attacker-caused hit carries
// the attacker's position so the receiving side can push the body away from it
// (used by the per-tick knockback, not by anything in this file).
struct DamageSource {
    DamageKind kind = DamageKind::Generic;
    float amount = 0.0f;
    bool unblockable = false;
    bool absolute = false;
    bool by_attacker = false;
    godot::Vector3 attacker_position;
};

// The player's defensive numbers at the moment of the hit. Every field is a
// plain number with no notion of where it came from: armor from worn pieces,
// resistance from the potion (its level, I = 1), protection from enchantments,
// absorption from the yellow-heart buffer (in half-hearts, spent before
// health). `difficulty` is 0 peaceful, 1 easy, 2 normal, 3 hard.
struct DamageInputs {
    float armor_points = 0.0f;       // capped at 20
    float resistance_level = 0.0f;   // potion level, I = 1
    float protection_points = 0.0f;  // capped at 20
    float absorption = 0.0f;         // half-hearts in the buffer
    bool blocking = false;
    int difficulty = 2;
};

// What a hit cost, in half-hearts. `total` is the amount after every reduction
// and before the absorption buffer; `health_delta` is what reaches health and
// `absorption_delta` what the buffer ate, so the two always sum to `total`.
struct DamageResult {
    float total = 0.0f;
    float health_delta = 0.0f;
    float absorption_delta = 0.0f;
};

// The hurt window, in ticks. A hit that registers opens a 20-tick resistance
// window and a 10-tick feedback (tint/animation) window. Inside the FIRST half
// of the resistance window (more than `kHurtResistWindow` ticks left) only a
// hit LARGER than the one that opened it lands, and only by its excess; that is
// what stops a flurry from stacking. They live here so every receiving body --
// the player sim now, the combat dummy next -- reads one set of numbers instead
// of two that can drift.
constexpr int kHurtResistTime = 20;
constexpr int kHurtResistWindow = 10;
constexpr int kHurtAnimTime = 10;

// The visible face of the hurt window: a short lean on the view. `window_fraction`
// is the hurt-animation window as a fraction -- 1 the instant a hit lands, falling
// to 0 as the window closes. The lean is 0 at both ends of the window and peaks
// partway through, so the view swings out and settles instead of snapping to full
// tilt; the amplitude and the shape are the reference's (a 14-degree lean traced
// by sin(f^4 * pi)). Kept as a pure function of the fraction so the curve can be
// pinned without a camera, a world or a frame.
inline constexpr float kHurtLeanMaxRadians = 14.0f * 3.14159265358979f / 180.0f;
float hurt_lean_angle(float window_fraction);

// The camera's own orientation for the current view after a hit's lean. `base` is
// that orientation before the lean (the pitch and any view-mode yaw the caller
// already computed) and `local_push` is the bearing the hit shoves the body,
// expressed in the player's frame (its Y is ignored): away from an attacker, or
// along a side axis when the hit has none. The arc is a roll about the view axis,
// carried round into that bearing, so the top of the view tips toward the hit
// (opposite the shove) and follows the head as the player looks up or down instead
// of leaning along a fixed world axis; a push along a side axis is a pure sideways
// roll (the no-attacker case). At `window_fraction` 0 the base is returned
// unchanged, so a caller may apply it every frame unconditionally.
godot::Basis hurt_lean_view(const godot::Basis& base, const godot::Vector3& local_push,
                            float window_fraction);

// --- The chain, one pure stage per function ---
// Each takes the amount leaving the previous stage and returns the amount
// entering the next. None mutates and none reads state, so a stage can be
// pinned on its own. The caps the reference applies (armor, protection,
// resistance points) are part of the rule and live here, not in the caller.
float scale_by_difficulty(float amount, int difficulty);
float apply_blocking(float amount);
float apply_armor(float amount, float armor_points);
float apply_resistance(float amount, float resistance_level);
float apply_protection(float amount, float protection_points);
DamageResult apply_absorption(float amount, float absorption);

// The reductions only, for an amount already scaled by difficulty:
//   blocking -> armor -> resistance -> protection -> absorption.
// `source.unblockable` drops the first two, `source.absolute` the next two.
// The order is the reference's and it matters: blocking and armor scale the
// amount that resistance and protection then scale again. Split from
// compute_damage because the live window compares the difficulty-scaled amount
// and only runs the reductions once it has decided the hit registers.
DamageResult reduce_damage(float amount, const DamageSource& source, const DamageInputs& inputs);

// The whole chain in one call, difficulty first:
//   difficulty -> blocking -> armor -> resistance -> protection -> absorption.
DamageResult compute_damage(const DamageSource& source, const DamageInputs& inputs);

// The horizontal unit direction a hit shoves the victim: directly away from the
// attacker along the two bodies' actual x/z positions, never their facing (the
// reference does the same, so a hit from the side or behind still reads as one
// straight push away). The result is flat (y == 0) and unit-length. When the two
// share an x/z the direction is undefined, so a fixed axis is returned; the sim
// only reaches that when an attacker and victim are stacked, where any direction
// reads as a shove. Kept pure so the stateful knockback in PlayerSim has nothing
// to test but the velocity it writes.
godot::Vector3 knockback_direction(const godot::Vector3& victim_position,
                                   const godot::Vector3& attacker_position);

} // namespace VoxelEngine

#endif // FARLANDS_PLAYER_DAMAGE_HPP
