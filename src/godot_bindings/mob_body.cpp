#include "godot_bindings/mob_body.hpp"

#include <godot_cpp/core/class_db.hpp>

#include "engine/collision_resolver.hpp"

using namespace godot;
using namespace VoxelEngine;

void MobBody::_bind_methods() {
    ClassDB::bind_method(D_METHOD("reset", "position"), &MobBody::reset);
    ClassDB::bind_method(D_METHOD("step", "delta", "wish", "jump", "sprint", "sneak", "yaw",
                                  "speed_multiplier"),
                         &MobBody::step);
    ClassDB::bind_method(D_METHOD("get_position"), &MobBody::get_position);
    ClassDB::bind_method(D_METHOD("get_tick_position"), &MobBody::get_tick_position);
    ClassDB::bind_method(D_METHOD("get_velocity"), &MobBody::get_velocity);
    ClassDB::bind_method(D_METHOD("is_on_ground"), &MobBody::is_on_ground);
    ClassDB::bind_method(D_METHOD("is_in_water"), &MobBody::is_in_water);
    ClassDB::bind_method(D_METHOD("is_sneaking"), &MobBody::is_sneaking);
    ClassDB::bind_method(D_METHOD("is_sprinting"), &MobBody::is_sprinting);
    ClassDB::bind_method(D_METHOD("get_eye_height"), &MobBody::get_eye_height);
    ClassDB::bind_method(D_METHOD("get_hurt_resist_ticks"), &MobBody::get_hurt_resist_ticks);
    ClassDB::bind_method(D_METHOD("get_air_ticks"), &MobBody::get_air_ticks);
    ClassDB::bind_method(D_METHOD("get_tick_count"), &MobBody::get_tick_count);
    ClassDB::bind_method(D_METHOD("shove", "attacker_position", "extra_velocity"),
                         &MobBody::shove);
    ClassDB::bind_method(D_METHOD("push", "velocity"), &MobBody::push);
    ClassDB::bind_method(D_METHOD("consume_damage"), &MobBody::consume_damage);
    ClassDB::bind_method(D_METHOD("is_liquid_at", "x", "y", "z"), &MobBody::is_liquid_at);
    ClassDB::bind_method(D_METHOD("is_solid_at", "x", "y", "z"), &MobBody::is_solid_at);
}

void MobBody::reset(const Vector3& position) {
    sim_.reset(position);
}

void MobBody::step(double delta, const Vector3& wish, bool jump, bool sprint, bool sneak,
                   float yaw, float speed_multiplier) {
    if (resolver_ == nullptr) return;
    PlayerInput input;
    // The sim wants a flat, normalized wish direction: it is the same field the player
    // node fills from the movement keys, with a route's heading in place of keys.
    input.wish_direction = Vector3(wish.x, 0.0f, wish.z);
    if (input.wish_direction.length_squared() > 1.0f) {
        input.wish_direction = input.wish_direction.normalized();
    }
    input.move_forward_held = input.wish_direction.length_squared() > 1e-8f;
    input.jump_pressed = jump;
    input.sprint_held = sprint;
    input.sneak_held = sneak;
    input.yaw = yaw;
    sim_.accumulate_and_tick(delta, input, *resolver_, PlayerSim::STEP_HEIGHT, speed_multiplier);
}

Vector3 MobBody::get_position() const {
    return sim_.get_render_position(sim_.get_accumulator_fraction());
}

Vector3 MobBody::get_tick_position() const {
    return sim_.get_position();
}

Vector3 MobBody::get_velocity() const {
    return sim_.get_velocity();
}

bool MobBody::is_on_ground() const {
    return sim_.is_on_floor();
}

bool MobBody::is_in_water() const {
    return sim_.is_in_water();
}

bool MobBody::is_sneaking() const {
    return sim_.is_sneaking();
}

bool MobBody::is_sprinting() const {
    return sim_.is_sprinting();
}

float MobBody::get_eye_height() const {
    return sim_.get_eye_height();
}

int32_t MobBody::get_hurt_resist_ticks() const {
    return sim_.get_hurt_resist_ticks();
}

int32_t MobBody::get_air_ticks() const {
    return sim_.get_air_ticks();
}

int64_t MobBody::get_tick_count() const {
    return static_cast<int64_t>(sim_.get_tick_count());
}

bool MobBody::shove(const Vector3& attacker_position, const Vector3& extra_velocity) {
    return sim_.shove(attacker_position, extra_velocity);
}

void MobBody::push(const Vector3& velocity) {
    sim_.push(velocity);
}

float MobBody::consume_damage() {
    const DamageEvents events = sim_.consume_damage_events();
    // The raw fall figure is the HUD's, not the health's: the landing has already entered the
    // queue as a source and been resolved against the hurt window, so it is spent here.
    sim_.consume_pending_fall_damage();
    return events.health_delta;
}

bool MobBody::is_liquid_at(int32_t x, int32_t y, int32_t z) const {
    if (resolver_ == nullptr) return false;
    return resolver_->is_liquid_at(x, y, z);
}

bool MobBody::is_solid_at(int32_t x, int32_t y, int32_t z) const {
    if (resolver_ == nullptr) return false;
    return resolver_->is_solid_at(x, y, z);
}
