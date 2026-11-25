#ifndef FARLANDS_GODOT_BINDINGS_MOB_BODY_HPP
#define FARLANDS_GODOT_BINDINGS_MOB_BODY_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include "engine/player_controller.hpp"

namespace VoxelEngine {
class CollisionResolver;
}

// One non-player body moved by the PLAYER's own movement simulation.
//
// A walked mob could be given a second mover written for mobs -- gravity, drag,
// acceleration, step-up, swimming, all of it again -- and then every one of those
// numbers would exist twice and drift. This hands out a `PlayerSim` instead: `step()`
// is the same call `PlayerController::_physics_process` makes, against the same
// `CollisionResolver`, at the same 20 Hz, with the same step height, and
// `get_position()` is the same interpolated render position the player's node is
// placed from. Only the input differs: a route's wish direction instead of keys.
//
// The body owns its position and velocity; the caller owns the node. Nothing here
// touches the scene, and no resolver means a body that can be shoved and measured but
// not moved through a world (which is what a stubbed probe wants).
class MobBody : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) - GDCLASS generates operator=
    GDCLASS(MobBody, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // The world the body moves through. C++ only: the owning ChunkManager hands over
    // the collision resolver it already owns, so a body can never outlive the world's
    // own mapping of blocks to collision.
    void set_resolver(VoxelEngine::CollisionResolver* resolver) { resolver_ = resolver; }

    // Where the body is put, with no velocity and no fall in progress: the same reset
    // the player node does on a respawn.
    void reset(const godot::Vector3& position);

    // One frame of movement. `wish` is the flat, world-space direction the body wants
    // to travel and `jump` is the jump control held this frame; the sim takes its own
    // ticks out of `delta`, so this does nothing on a frame too short for one.
    // `speed_multiplier` scales every move speed -- 1.0 is the player's own pace, and
    // a stroll is the same physics walked slower.
    void step(double delta, const godot::Vector3& wish, bool jump, bool sprint, bool sneak,
              float yaw, float speed_multiplier);

    // Where the node goes: the tick positions interpolated by the tick the frame is in,
    // exactly as the player's own node is placed.
    [[nodiscard]] godot::Vector3 get_position() const;
    // The last tick's own feet position: what a planner is asked about, since it is the
    // position the body actually occupies rather than the one it is drawn between.
    [[nodiscard]] godot::Vector3 get_tick_position() const;
    // Blocks per tick.
    [[nodiscard]] godot::Vector3 get_velocity() const;
    [[nodiscard]] bool is_on_ground() const;
    [[nodiscard]] bool is_in_water() const;
    [[nodiscard]] bool is_sneaking() const;
    [[nodiscard]] bool is_sprinting() const;
    [[nodiscard]] float get_eye_height() const;
    [[nodiscard]] int32_t get_hurt_resist_ticks() const;
    [[nodiscard]] int32_t get_air_ticks() const;
    [[nodiscard]] int64_t get_tick_count() const;

    // The reference's base knockback plus the hurt window a hit opens, for a victim
    // whose health is its own. False while the window resists the hit, so the caller's
    // damage and this shove share one gate.
    bool shove(const godot::Vector3& attacker_position, const godot::Vector3& extra_velocity);

    // Everything the sim has queued since the last call, in half-hearts of health damage:
    // a landing past `SAFE_FALL_DISTANCE` arrives here as a Fall source like any other, so a
    // fall hurts a mob through the same hurt window a blow does. Breath and contact damage
    // ride the same queue. Consuming clears the accumulators, so a body whose damage comes
    // from elsewhere calls this and drops the answer rather than letting it grow.
    [[nodiscard]] float consume_damage();

    // Is that cell of the world a liquid? It is the same predicate the body's own water state
    // is read from, so a swimmer looking for dry ground cannot disagree with the physics about
    // what water is. `is_solid_at` is its other half, for the same reason: "can I stand here"
    // is a question about the very cells the resolver will move the body through.
    [[nodiscard]] bool is_liquid_at(int32_t x, int32_t y, int32_t z) const;
    [[nodiscard]] bool is_solid_at(int32_t x, int32_t y, int32_t z) const;

protected:
    static void _bind_methods();

private:
    VoxelEngine::PlayerSim sim_;
    VoxelEngine::CollisionResolver* resolver_ = nullptr;
};

#endif  // FARLANDS_GODOT_BINDINGS_MOB_BODY_HPP
