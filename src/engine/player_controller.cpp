#include "engine/player_controller.hpp"
#include "engine/collision_resolver.hpp"
#include <cmath>
#include <algorithm>

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
    accumulator_ = 0.0f;
}

float PlayerSim::get_accumulator_fraction() const {
    return accumulator_ / TICK_DT;
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
                                           CollisionResolver& cr, float step_height) {
    prev_position_ = position_;
    accumulator_ += static_cast<float>(frame_delta);

    // Safety cap: never run more than 4 ticks per frame to avoid spiral of death
    int max_ticks = 4;
    while (accumulator_ >= TICK_DT && max_ticks > 0) {
        tick(input, cr, step_height);
        accumulator_ -= TICK_DT;
        --max_ticks;
    }
    if (max_ticks == 0 && accumulator_ >= TICK_DT) {
        accumulator_ = 0.0f;
    }
}

void PlayerSim::tick(const PlayerInput& input, CollisionResolver& cr, float step_height) {
    // --- Determine state and move multiplier ---
    bool sneaking = input.sneak_held && on_floor_;
    bool sprinting = input.sprint_held && on_floor_ && !sneaking;

    if (sneaking) {
        state_ = MoveState::SNEAKING;
    } else if (sprinting) {
        state_ = MoveState::SPRINTING;
    } else if (on_floor_) {
        state_ = MoveState::WALKING;
    } else {
        state_ = MoveState::AIRBORNE;
    }

    float move_multiplier = 0.0f;
    if (sneaking) move_multiplier = SNEAK_MULT;
    else if (sprinting) move_multiplier = SPRINT_MULT;
    else if (on_floor_) move_multiplier = WALK_MULT;

    // --- Drag (applied FIRST — velocity from previous tick is damped before new accel) ---
    // Vanilla order: Entity.move() applies ground friction to motionX/Z at the END of each tick,
    // so entering the next tick the velocity is already damped. We replicate this by dragging first.
    velocity_.y *= VERTICAL_DRAG;
    if (on_floor_) {
        float ground_friction = DEFAULT_SLIPPERINESS * 0.91f;
        velocity_.x *= ground_friction;
        velocity_.z *= ground_friction;
    } else {
        velocity_.x *= AIR_FRICTION;
        velocity_.z *= AIR_FRICTION;
    }

    // --- Horizontal acceleration ---
    // wish_direction is already camera-relative and XZ-normalized from the caller
    float accel;
    if (on_floor_) {
        accel = GROUND_ACCEL * move_multiplier
              * std::pow(DEFAULT_SLIPPERINESS / DEFAULT_SLIPPERINESS, 3.0f); // slipperiness lookup deferred
    } else {
        accel = AIR_ACCEL * move_multiplier;
    }

    velocity_.x += input.wish_direction.x * accel;
    velocity_.z += input.wish_direction.z * accel;

    // --- Jump ---
    if (input.jump_pressed && on_floor_) {
        velocity_.y = JUMP_VELOCITY;
        // Sprint-jump horizontal boost
        if (sprinting) {
            float boost_x = -std::sin(input.yaw) * SPRINT_JUMP_BOOST;
            float boost_z = -std::cos(input.yaw) * SPRINT_JUMP_BOOST;
            velocity_.x += boost_x;
            velocity_.z += boost_z;
        }
    }

    // --- Collision resolution ---
    Vector3 size = (state_ == MoveState::SNEAKING) ? SNEAKING_SIZE : STANDING_SIZE;
    // velocity_ IS the per-tick motion vector — no TICK_DT multiplication
    auto result = cr.resolve(position_, velocity_, size, step_height);
    position_ = result.position;

    if (result.collided_x) velocity_.x = 0.0f;
    if (result.collided_y) velocity_.y = 0.0f;
    if (result.collided_z) velocity_.z = 0.0f;

    on_floor_ = result.on_floor;

    // --- Gravity (applied AFTER move, like vanilla) ---
    velocity_.y -= GRAVITY;
}

} // namespace VoxelEngine
