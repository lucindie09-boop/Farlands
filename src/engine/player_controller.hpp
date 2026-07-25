#ifndef FUK_MINECRAFT_PLAYER_CONTROLLER_HPP
#define FUK_MINECRAFT_PLAYER_CONTROLLER_HPP

#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>

namespace VoxelEngine {

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
                             CollisionResolver& cr, float step_height = STEP_HEIGHT);

    float get_accumulator_fraction() const;
    godot::Vector3 get_render_position(float partial_tick) const;
    godot::Vector3 get_camera_position(float partial_tick) const;
    MoveState get_state() const { return state_; }
    bool is_on_floor() const { return on_floor_; }
    float get_eye_height() const;
    godot::Vector3 get_velocity() const { return velocity_; }
    godot::Vector3 get_position() const { return position_; }

private:
    void tick(const PlayerInput& input, CollisionResolver& cr, float step_height);

    godot::Vector3 position_;
    godot::Vector3 prev_position_;
    godot::Vector3 velocity_;
    MoveState state_ = MoveState::AIRBORNE;
    bool on_floor_ = false;
    float accumulator_ = 0.0f;
};

} // namespace VoxelEngine
#endif
