#include "engine/player_controller.hpp"
#include "engine/collision_resolver.hpp"
#include <cmath>
#include <algorithm>
#include <godot_cpp/variant/utility_functions.hpp>

bool VoxelEngine::g_engine_running = false;

namespace VoxelEngine {

using namespace godot;

const Vector3 PlayerSim::STANDING_SIZE = Vector3(0.6f, 1.8f, 0.6f);
const Vector3 PlayerSim::SNEAKING_SIZE = Vector3(0.6f, 1.5f, 0.6f);

void PlayerSim::reset(const Vector3& initial_pos) {
    position_ = initial_pos;
    prev_position_ = initial_pos;
    velocity_ = Vector3();
    state_ = MoveState::AIRBORNE;
    on_floor_ = false;
    in_water_ = false;
    sprint_active_ = false;
    prev_sprint_active_ = false;
    accumulator_ = 0.0f;
    fall_distance_ = 0.0f;
    pending_fall_damage_ = 0;
}

float PlayerSim::get_accumulator_fraction() const {
    return accumulator_ / TICK_DT;
}

int PlayerSim::consume_pending_fall_damage() {
    int damage = pending_fall_damage_;
    pending_fall_damage_ = 0;
    return damage;
}

Vector3 PlayerSim::get_render_position(float partial_tick) const {
    return prev_position_.lerp(position_, partial_tick);
}

Vector3 PlayerSim::get_camera_position(float partial_tick) const {
    Vector3 render_pos = get_render_position(partial_tick);
    render_pos.y += get_eye_height();
    return render_pos;
}

float PlayerSim::get_eye_height() const {
    return (state_ == MoveState::SNEAKING) ? SNEAKING_EYE : STANDING_EYE;
}

void PlayerSim::accumulate_and_tick(double frame_delta, const PlayerInput& input,
                                           CollisionResolver& cr, float step_height,
                                           float speed_multiplier) {
    accumulator_ += static_cast<float>(frame_delta);

    // Safety cap: never run more than 4 ticks per frame to avoid spiral of death
    int max_ticks = 4;
    while (accumulator_ >= TICK_DT && max_ticks > 0) {
        tick(input, cr, step_height, speed_multiplier);
        accumulator_ -= TICK_DT;
        --max_ticks;
    }
    if (max_ticks == 0 && accumulator_ >= TICK_DT) {
        accumulator_ = 0.0f;
    }
}

void PlayerSim::tick(const PlayerInput& input, CollisionResolver& cr, float step_height, float speed_multiplier) {
    prev_position_ = position_;   // snapshot for interpolation — once per tick, not per frame

    // --- Liquid state, sampled once at the start of the tick ---
    // ANY part of the body inside a liquid counts, not just the feet. The case
    // that needs the second sample is a body standing ON something under water: on
    // a submerged slab or stair the cell its feet occupy holds the block itself,
    // so a feet-only test would report the body dry — standing there with its head
    // under water, unaffected by drag and able to breathe. Two samples (foot level,
    // head level) cover the 1.8-tall body, which spans at most three cells.
    //
    // Sampling before anything moves means the tick that enters the water already
    // feels it, which is what stops a body plunging in at full speed for one tick
    // and what cancels the fall damage on the way in. The body height comes from
    // this tick's *starting* state, which only differs by 0.3 while sneaking.
    {
        const float body_h = (state_ == MoveState::SNEAKING) ? SNEAKING_SIZE.y : STANDING_SIZE.y;
        const int32_t ix = static_cast<int32_t>(std::floor(position_.x));
        const int32_t iz = static_cast<int32_t>(std::floor(position_.z));
        in_water_ = cr.is_liquid_at(ix, static_cast<int32_t>(std::floor(position_.y + 0.1f)), iz)
                 || cr.is_liquid_at(ix, static_cast<int32_t>(std::floor(position_.y + body_h - 0.1f)), iz);
    }

    // --- Sprint state machine (vanilla: sticky flag, one-tick stale for airborne) ---
    prev_sprint_active_ = sprint_active_;
    bool sneaking = input.sneak_held && on_floor_;
    if (sneaking || !input.sprint_held || !input.move_forward_held) {
        sprint_active_ = false;
    } else if (input.sprint_held && input.move_forward_held && on_floor_) {
        sprint_active_ = true;
    }

    // State and multiplier: airborne uses prev_sprint_active_ (one-tick stale)
    bool effective_sprint = on_floor_ ? sprint_active_ : prev_sprint_active_;
    if (sneaking) {
        state_ = MoveState::SNEAKING;
    } else if (effective_sprint) {
        state_ = MoveState::SPRINTING;
    } else if (on_floor_) {
        state_ = MoveState::WALKING;
    } else {
        state_ = MoveState::AIRBORNE;
    }

    float move_multiplier = 0.0f;
    if (on_floor_) {
        if (sneaking) move_multiplier = SNEAK_MULT;
        else if (effective_sprint) move_multiplier = SPRINT_MULT;
        else move_multiplier = WALK_MULT;
    } else {
        move_multiplier = prev_sprint_active_ ? SPRINT_MULT : WALK_MULT;
    }
    move_multiplier *= speed_multiplier;

    // --- Slipperiness lookup ---
    float slipperiness = DEFAULT_SLIPPERINESS;
    if (on_floor_) {
        int32_t fx = static_cast<int32_t>(std::floor(position_.x));
        int32_t fy = static_cast<int32_t>(std::floor(position_.y)) - 1;
        int32_t fz = static_cast<int32_t>(std::floor(position_.z));
        slipperiness = cr.get_slipperiness_at(fx, fy, fz);
    }

    // --- Jump / swim up (applied BEFORE friction, matching the vanilla tick order) ---
    bool want_jump = jump_queued_ || input.jump_pressed;
    if (in_water_) {
        // Swimming up: a small upward impulse every tick the jump control is
        // held, which is what makes a body float back to the surface instead of
        // being stuck at the bottom of a pool.
        if (want_jump) velocity_.y += WATER_RISE;
        jump_queued_ = false;
    } else if (want_jump && on_floor_) {
        velocity_.y = JUMP_VELOCITY;
        jump_queued_ = false;
        if (sprint_active_) {
            float boost_x = -std::sin(input.yaw) * SPRINT_JUMP_BOOST;
            float boost_z = -std::cos(input.yaw) * SPRINT_JUMP_BOOST;
            velocity_.x += boost_x;
            velocity_.z += boost_z;
        }
    } else {
        jump_queued_ = false;  // consumed while not on floor — discard
    }

    // --- Horizontal friction (applied AFTER jump, matching vanilla travel() order) ---
    if (in_water_) {
        // Water drag replaces both the ground and the air case: being in a
        // liquid is what matters, not whether a floor is under the feet.
        velocity_.x *= WATER_DRAG;
        velocity_.z *= WATER_DRAG;
    } else if (on_floor_) {
        float ground_friction = slipperiness * 0.91f;
        velocity_.x *= ground_friction;
        velocity_.z *= ground_friction;
    } else {
        velocity_.x *= AIR_FRICTION;
        velocity_.z *= AIR_FRICTION;
    }

    // --- Horizontal acceleration ---
    // wish_direction is already camera-relative and XZ-normalized from the caller
    float accel;
    if (in_water_) {
        accel = WATER_ACCEL * move_multiplier;
    } else if (on_floor_) {
        accel = GROUND_ACCEL * move_multiplier
              * std::pow(DEFAULT_SLIPPERINESS / slipperiness, 3.0f);
    } else {
        accel = AIR_ACCEL * move_multiplier;
    }

    velocity_.x += input.wish_direction.x * accel;
    velocity_.z += input.wish_direction.z * accel;

    // --- Sneak edge-guard: vanilla-style graduated clamp ---
    // Instead of an all-or-nothing zero, shrink the intended step in small
    // increments until the leading edge of the footprint still has floor
    // beneath it, or we've reduced the step to zero. Independent per axis so
    // sliding sideways along a cliff still works.
    if (sneaking && move_multiplier > 0.0f) {
        int32_t floor_y = static_cast<int32_t>(std::floor(position_.y)) - 1;
        const float half_w = STANDING_SIZE.x * 0.5f;
        const float epsilon = 0.001f;
        const float step = 0.05f;

        auto footing_ok = [&](float center_x, float center_z) {
            int32_t bx_min = static_cast<int32_t>(std::floor(center_x - half_w + epsilon));
            int32_t bx_max = static_cast<int32_t>(std::floor(center_x + half_w - epsilon));
            int32_t bz_min = static_cast<int32_t>(std::floor(center_z - half_w + epsilon));
            int32_t bz_max = static_cast<int32_t>(std::floor(center_z + half_w - epsilon));
            for (int32_t bx = bx_min; bx <= bx_max; ++bx)
                for (int32_t bz = bz_min; bz <= bz_max; ++bz)
                    if (cr.is_solid_at(bx, floor_y, bz)) return true;
            return false;
        };

        if (std::abs(velocity_.x) > epsilon) {
            float remaining = velocity_.x;
            while (std::abs(remaining) > epsilon &&
                   !footing_ok(position_.x + remaining, position_.z)) {
                remaining -= (remaining > 0.0f ? step : -step);
                if ((remaining > 0.0f) != (velocity_.x > 0.0f)) { remaining = 0.0f; break; }
            }
            velocity_.x = remaining;
        }

        if (std::abs(velocity_.z) > epsilon) {
            float remaining = velocity_.z;
            while (std::abs(remaining) > epsilon &&
                   !footing_ok(position_.x, position_.z + remaining)) {
                remaining -= (remaining > 0.0f ? step : -step);
                if ((remaining > 0.0f) != (velocity_.z > 0.0f)) { remaining = 0.0f; break; }
            }
            velocity_.z = remaining;
        }
    }

    // --- Collision resolution ---
    Vector3 size = (state_ == MoveState::SNEAKING) ? SNEAKING_SIZE : STANDING_SIZE;
    // velocity_ IS the per-tick motion vector — no TICK_DT multiplication

    // Step-up must never engage while airborne. Gate on "grounded before this
    // tick's motion" AND "not currently launching upward" (a jump above sets
    // velocity_.y = JUMP_VELOCITY > 0). Passing 0.0f here disables the whole
    // step-up branch for this tick — we don't rely on resolve()'s post-move
    // on_floor probe to reject it after the fact, since that only works today
    // because every block is a uniform full cube.
    bool step_up_eligible = on_floor_ && velocity_.y <= 0.0f;
    float effective_step_height = step_up_eligible ? step_height : 0.0f;

    auto result = cr.resolve(position_, velocity_, size, effective_step_height);
    position_ = result.position;

    if (result.collided_x) velocity_.x = 0.0f;
    if (result.collided_y) velocity_.y = 0.0f;
    if (result.collided_z) velocity_.z = 0.0f;

    // Sprint cancels on horizontal wall collision
    if (result.collided_x || result.collided_z) sprint_active_ = false;

    // --- Swim up out of the water onto a ledge ---
    // Horizontal collision while swimming and holding jump lifts the body. A
    // pool walled with full blocks is otherwise a trap: the surface cannot be
    // climbed from the inside, so no amount of swimming gets you out.
    if (in_water_ && want_jump && (result.collided_x || result.collided_z)) {
        velocity_.y = WATER_LEDGE_BOOST;
    }

    on_floor_ = result.on_floor;

    // --- Fall damage tracking (vanilla: landings over 3 blocks hurt) ---
    // prev_position_ -> position_ is this tick's actual motion; on the
    // landing tick it includes the collision-clipped remainder, so it must be
    // added BEFORE the landing check or every fall loses its final segment.
    float fallen = prev_position_.y - position_.y;
    if (fallen > 0.0f) {
        fall_distance_ += fallen;
    }
    if (in_water_) {
        // Water breaks a fall without hurting: the distance is forgotten rather
        // than converted into damage, so jumping off a cliff into deep water is
        // safe (matching the reference).
        fall_distance_ = 0.0f;
    } else if (on_floor_) {
        if (fall_distance_ > SAFE_FALL_DISTANCE) {
            pending_fall_damage_ +=
                static_cast<int>(std::floor(fall_distance_ - SAFE_FALL_DISTANCE));
        }
        fall_distance_ = 0.0f;
    }

    // --- Vertical motion (applied AFTER move, matching vanilla tick order) ---
    if (in_water_) {
        velocity_.y -= WATER_SINK;
        velocity_.y *= WATER_DRAG;
    } else {
        velocity_.y -= GRAVITY;
        velocity_.y *= VERTICAL_DRAG;
    }

    // Per-tick state dump removed (was spamming the console every tick).
    // if (g_engine_running) {
    //     godot::UtilityFunctions::print(
    //         godot::vformat("pos=(%.2f,%.2f,%.2f) vel=(%.3f,%.3f,%.3f) floor=%d sprint=%d/%d yaw=%.2f wish=(%.3f,%.3f)",
    //            position_.x, position_.y, position_.z,
    //            velocity_.x, velocity_.y, velocity_.z,
    //            (int)on_floor_, (int)sprint_active_, (int)prev_sprint_active_, input.yaw,
    //            input.wish_direction.x, input.wish_direction.z));
    // }
}

} // namespace VoxelEngine
