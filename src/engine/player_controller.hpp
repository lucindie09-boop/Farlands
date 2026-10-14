#ifndef FARLANDS_PLAYER_CONTROLLER_HPP
#define FARLANDS_PLAYER_CONTROLLER_HPP

#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>
#include <vector>

#include "engine/player_damage.hpp"

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
    bool move_forward_held = false;
    float yaw = 0.0f;  // facing angle for sprint-jump boost
};

// What the hits resolved since the last drain did, in half-hearts. The node
// turns `health_delta` into a health change, `amount` into the size of the hit
// for feedback (it survives absorption, so a hit fully soaked by the yellow
// buffer still reports its weight), and `hurt` into the visible hurt response.
// All three are zero on a frame with no damage, so a caller can act every frame
// unconditionally.
//
// `by_attacker` and `attacker_position` describe the hit that produced `hurt`,
// for the visible lean: an attacker-caused hit leans the view back toward the
// attacker, while an environmental one has no bearing of its own and rolls the
// view instead. They are copied only from a hit that actually registered, so a
// resisted repeat leaves the previous values alone.
struct DamageEvents {
    float health_delta = 0.0f;
    float amount = 0.0f;
    bool hurt = false;
    bool by_attacker = false;
    godot::Vector3 attacker_position;
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
    // Air: 15 s of breath (300 ticks) before drowning, refilled at 4 ticks per
    // tick once the eye is clear. A drowned or suffocated hit is one source among
    // the others, so the hurt window paces the repeats rather than a timer here.
    static constexpr int MAX_AIR = 300;
    static constexpr int AIR_REFILL_PER_TICK = 4;
    static constexpr float DROWN_DAMAGE = 2.0f;      // half-hearts per drowned hit
    static constexpr float SUFFOCATE_DAMAGE = 1.0f;  // half-hearts per suffocated hit
    // Receiving knockback (blocks/tick): the same 0.4 base the reference uses
    // and the combat dummy mirrors, so a hit shoves either body identically.
    static constexpr float KNOCKBACK_SPEED = 0.4f;
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
    // The visible hurt, sampled between sim ticks for the render rate. The clock
    // falls one per tick, so it is sampled forward to the next tick: full the
    // instant the hit lands, then falling immediately with no fixed-step lag.
    float get_render_hurt_anim_ticks(float partial_tick) const;
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

    // --- Receiving combat (see engine/player_damage.hpp) ---
    // Queue one hit. It is resolved at the end of a tick, so a caller can queue
    // from outside the tick (an input handler, later an entity) and every source
    // is judged against the same window state in a defined order. Calling it
    // more than once is fine; the sources are resolved in the order queued.
    void queue_damage(const DamageSource& source);
    // Everything the resolved hits have done since the last call: health to
    // remove this frame and whether any hit produced feedback. Consuming clears
    // the accumulators.
    DamageEvents consume_damage_events();
    // The defensive snapshot the reductions read (armor, effects, blocking,
    // difficulty). Absorption is NOT here because damage spends it; see below.
    void set_damage_inputs(const DamageInputs& inputs) { damage_inputs_ = inputs; }
    const DamageInputs& get_damage_inputs() const { return damage_inputs_; }
    // The yellow-heart buffer, in half-hearts. Spent before health by a hit.
    void set_absorption(float half_hearts) { absorption_ = half_hearts; }
    float get_absorption() const { return absorption_; }
    // The horizontal bearing (world, unit) a registered hit with no attacker
    // shoves along -- a landing, whose source and victim share a spot. The sim
    // never draws a random itself, so the node picks the bearing per frame and
    // hands it in; an attacker hit ignores this and shoves away from the body.
    void set_knockback_bearing(const godot::Vector3& direction) { knockback_bearing_ = direction; }
    // Ticks left in each window: the resistance window gates repeats, the anim
    // window drives the visible hurt. Exposed for tests and HUD-adjacent code.
    int get_hurt_resist_ticks() const { return hurt_resist_ticks_; }
    int get_hurt_anim_ticks() const { return hurt_anim_ticks_; }

    // Remaining breath in ticks (0..MAX_AIR), for the bubble HUD. 0 means the
    // eye has been under a liquid long enough that drowning is now ticking.
    int get_air_ticks() const { return air_ticks_; }

    void queue_jump() { jump_queued_ = true; }

private:
    void tick(const PlayerInput& input, CollisionResolver& cr, float step_height, float speed_multiplier);
    // Resolve every queued source against the hurt window and accumulate what it
    // did. Called at the end of a tick, after that tick's fall damage (if any)
    // has been queued, so a landing hurts on the tick it happens.
    void resolve_incoming_damage();
    // Shove the body on a registered full hit: halve the current velocity on
    // every axis, add `direction`'s horizontal knockback and the vertical one
    // capped at the base speed -- the reference's base knockback, and the same
    // one dummy.gd applies. The caller has already decided the hit registers, so
    // this is unconditional. An attacker hit passes the direction away from the
    // attacker; a landing has no body to be shoved away from, so it passes the
    // random bearing the node picked.
    void apply_knockback(const godot::Vector3& direction);
    // Queue this tick's environmental sources: drowning once the breath is gone
    // with the eye under a liquid, suffocation with the head inside a full solid,
    // and contact damage for a body touching a block that deals it (cactus). They
    // enter the same queue as everything else, so the hurt window paces the
    // repeats and the reductions can soften them. Called at the end of a tick,
    // after the move, right before the queue is resolved.
    void apply_environmental_hazards(CollisionResolver& cr);

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
    int air_ticks_ = MAX_AIR;         // remaining breath, in ticks

    // Receiving-combat state. The `incoming_damage_` queue is drained every
    // tick, so it never grows without bound even if a caller keeps queueing.
    std::vector<DamageSource> incoming_damage_;
    DamageInputs damage_inputs_;      // armor/effects/blocking/difficulty
    float absorption_ = 0.0f;         // yellow-heart buffer, in half-hearts
    int hurt_resist_ticks_ = 0;       // 0..kHurtResistTime
    int hurt_anim_ticks_ = 0;         // 0..kHurtAnimTime
    float last_damage_ = 0.0f;        // the difficulty-scaled amount of the last registered hit
    float pending_health_damage_ = 0.0f;  // this frame's health damage, all sources
    float pending_hit_amount_ = 0.0f;     // this frame's hit size for feedback, all sources
    bool pending_hurt_ = false;       // a full hit registered this frame
    bool pending_hit_by_attacker_ = false;   // ...and whether an attacker caused it
    godot::Vector3 pending_hit_attacker_;    // ...and where that attacker was
    godot::Vector3 knockback_bearing_;       // bearing a no-attacker hit shoves along (set by the node)
};

} // namespace VoxelEngine
#endif
