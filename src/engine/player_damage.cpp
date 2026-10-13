#include "engine/player_damage.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

namespace {

// The reference caps each defensive total before it uses it, and the cap is part
// of the rule rather than something a caller has to remember. Values here match
// the divisors in the stages below: an armor or protection total of 20 leaves a
// factor of 5/25, i.e. an 80% reduction, and resistance is measured in points
// of five per potion level with the same ceiling.
constexpr float kMaxArmorPoints = 20.0f;
constexpr float kMaxProtectionPoints = 20.0f;
constexpr float kMaxResistanceLevel = 5.0f;
constexpr float kMaxResistancePoints = 20.0f;

} // namespace

float scale_by_difficulty(float amount, int difficulty) {
    switch (difficulty) {
        case 0: // peaceful: nothing hurts
            return 0.0f;
        case 1: // easy: half, plus one
            return amount * 0.5f + 1.0f;
        case 3: // hard: one and a half
            return amount * 1.5f;
        default: // normal, and anything unexpected
            return amount;
    }
}

float apply_blocking(float amount) {
    // Raising a blocker turns a hit into (amount + 1) * 0.5, so a small hit is
    // cut less than proportionally and even a zero hit is not fully free.
    return (amount + 1.0f) * 0.5f;
}

float apply_armor(float amount, float armor_points) {
    const float armor = std::clamp(armor_points, 0.0f, kMaxArmorPoints);
    return amount * (25.0f - armor) / 25.0f;
}

float apply_resistance(float amount, float resistance_level) {
    // Resistance is worth five points per level, and the points are capped at
    // 20, so level I takes 20% off, level IV 80%, and beyond that nothing more.
    const float level = std::clamp(resistance_level, 0.0f, kMaxResistanceLevel);
    const float points = std::min(level * 5.0f, kMaxResistancePoints);
    return amount * (25.0f - points) / 25.0f;
}

float apply_protection(float amount, float protection_points) {
    const float points = std::clamp(protection_points, 0.0f, kMaxProtectionPoints);
    return amount * (25.0f - points) / 25.0f;
}

DamageResult apply_absorption(float amount, float absorption) {
    DamageResult result;
    result.total = amount;
    const float buffer = std::max(absorption, 0.0f);
    // The buffer eats first and never goes negative; whatever it cannot cover
    // reaches health. buffer >= amount covers the exact and over cases at once.
    if (buffer >= amount) {
        result.absorption_delta = amount;
    } else {
        result.absorption_delta = buffer;
        result.health_delta = amount - buffer;
    }
    return result;
}

DamageResult reduce_damage(float amount, const DamageSource& source, const DamageInputs& inputs) {
    if (amount <= 0.0f) {
        return DamageResult{};
    }
    if (!source.unblockable) {
        if (inputs.blocking) {
            amount = apply_blocking(amount);
        }
        amount = apply_armor(amount, inputs.armor_points);
    }
    if (!source.absolute) {
        amount = apply_resistance(amount, inputs.resistance_level);
        amount = apply_protection(amount, inputs.protection_points);
    }
    return apply_absorption(amount, inputs.absorption);
}

DamageResult compute_damage(const DamageSource& source, const DamageInputs& inputs) {
    // The difficulty stage runs before every reduction, so the amount the
    // reductions see is the difficulty-scaled one (which is also the amount the
    // live window compares against).
    return reduce_damage(scale_by_difficulty(source.amount, inputs.difficulty), source, inputs);
}

godot::Vector3 knockback_direction(const godot::Vector3& victim_position,
                                   const godot::Vector3& attacker_position) {
    const float dx = victim_position.x - attacker_position.x;
    const float dz = victim_position.z - attacker_position.z;
    const float len_sq = dx * dx + dz * dz;
    if (len_sq < 1e-8f) {
        return godot::Vector3(1.0f, 0.0f, 0.0f);
    }
    const float inv = 1.0f / std::sqrt(len_sq);
    return godot::Vector3(dx * inv, 0.0f, dz * inv);
}

} // namespace VoxelEngine
