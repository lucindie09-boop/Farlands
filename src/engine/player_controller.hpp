#ifndef FARLANDS_PLAYER_CONTROLLER_HPP
#define FARLANDS_PLAYER_CONTROLLER_HPP

#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>

namespace VoxelEngine {

extern bool g_engine_running;

class CollisionResolver;

enum class MoveState : uint8_t {
    WALKING,
    SPRINTING,
    SNEAKING,
    AIRBORNE
};

struct PlayerInput {
    godot::Vector3 wish_direction;  // camera-relative, XZ-normalized
    bool jump_pressed = false;
    bool sprint_held = false;
    bool sneak_held = false;
    bool move_forward_held = false;
    float yaw = 0.0f;  // facing angle for sprint-jump boost
};

class PlayerSim {
public:
    // Vanilla constants — all blocks/tick unless noted
    static constexpr float TICK_RATE = 20.0f;
    static constexpr float TICK_DT = 1.0f / 20.0f;
    static constexpr float GRAVITY = 0.08f;
    static constexpr float VERTICAL_DRAG = 0.98f;
    static constexpr float AIR_FRICTION = 0.91f;
    static constexpr float DEFAULT_SLIPPERINESS = 0.6f;
    static constexpr float GROUND_ACCEL = 0.1f;
    static constexpr float AIR_ACCEL = 0.02f;
    static constexpr float JUMP_VELOCITY = 0.42f;
    static constexpr float SPRINT_JUMP_BOOST = 0.2f;
    static constexpr float STEP_HEIGHT = 0.6f;

    // --- Liquid movement (a body sinks slowly and swims up) ---
    // A liquid is swum through rather than stood on, so these replace the
    // airborne numbers while the feet are inside a liquid cell. Tuned to the
    // reference feel: sinking about 1.6 blocks/s, moving at ~1/5 land speed,
    // and rising while jump is held. Without them a body falls through water at
    // full speed and walks on the bottom, i.e. water reads as air.
    static constexpr float WATER_SINK = 0.02f;        // downward pull per tick (vs GRAVITY 0.08)
    static constexpr float WATER_DRAG = 0.8f;         // per-tick retention (vs VERTICAL_DRAG 0.98 air, 0.91 ground)
    static constexpr float WATER_ACCEL = 0.02f;       // horizontal acceleration (vs GROUND_ACCEL 0.1)
    static constexpr float WATER_RISE = 0.04f;        // upward impulse per tick while jump is held
    static constexpr float WATER_LEDGE_BOOST = 0.3f;  // upward kick when swimming into a wall
    // Vanilla fall damage: landings further than 3 blocks hurt, 1 half-heart
    // per extra block (floor(fall_distance - 3)).
    static constexpr float SAFE_FALL_DISTANCE = 3.0f;
    static constexpr float WALK_MULT = 1.0f;
    static constexpr float SPRINT_MULT = 1.3f;
    static constexpr float SNEAK_MULT = 0.3f;

    // Hitboxes (non-constexpr: godot::Vector3 constructor is not constexpr in godot-cpp)
    static const godot::Vector3 STANDING_SIZE;
    static const godot::Vector3 SNEAKING_SIZE;
    static constexpr float STANDING_EYE = 1.62f;
    static constexpr float SNEAKING_EYE = 1.27f;

    void reset(const godot::Vector3& initial_pos);
    void accumulate_and_tick(double frame_delta, const PlayerInput& input,
                             CollisionResolver& cr, float step_height = STEP_HEIGHT,
                             float speed_multiplier = 1.0f);

    float get_accumulator_fraction() const;
    godot::Vector3 get_render_position(float partial_tick) const;
    godot::Vector3 get_camera_position(float partial_tick) const;
    // Freezes the interpolated view exactly on position_ — used on death, so
    // the camera lands where the body did instead of mid-lerp above it.
    void snap_render_position() { prev_position_ = position_; }
    MoveState get_state() const { return state_; }
    bool is_on_floor() const { return on_floor_; }
    // True when this tick's feet are inside a liquid cell.
    bool is_in_water() const { return in_water_; }
    float get_eye_height() const;
    godot::Vector3 get_velocity() const { return velocity_; }
    godot::Vector3 get_position() const { return position_; }
    float get_fall_distance() const { return fall_distance_; }
    // Half-hearts queued by landings since the last call; consuming clears it.
    int consume_pending_fall_damage();

    void queue_jump() { jump_queued_ = true; }

private:
    void tick(const PlayerInput& input, CollisionResolver& cr, float step_height, float speed_multiplier);

    godot::Vector3 position_;
    godot::Vector3 prev_position_;
    godot::Vector3 velocity_;
    MoveState state_ = MoveState::AIRBORNE;
    bool on_floor_ = false;
    bool in_water_ = false;
    bool jump_queued_ = false;
    bool sprint_active_ = false;
    bool prev_sprint_active_ = false;
    float accumulator_ = 0.0f;
    float fall_distance_ = 0.0f;
    int pending_fall_damage_ = 0;
};

} // namespace VoxelEngine
#endif
