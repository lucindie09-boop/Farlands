// The interactive loop: `_process` (move, look, camera, mining tick) and `_input`
// (mouse look, click and key actions), plus the mouse-mode helper both the menus and
// this loop call. Kept apart from godot_bindings/player_controller.cpp, which owns
// the node's lifecycle and its bindings.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/chunk_manager.hpp"
#include "godot_bindings/player_controller_internal.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;
using namespace player_detail;
void PlayerController::_process(double delta) {
    refresh_cached_nodes();
    if (needs_spawn_calc_ && chunk_manager_) {
        // Scan the column at (0, 0) from top down to find the first solid block.
        for (int32_t y = WORLD_HEIGHT_Y - 1; y >= 0; --y) {
            if (chunk_manager_->get_block(0, y, 0) != 0) {
                Vector3 spawn(0, y + 1, 0);
                set_global_position(spawn);
                sim_.reset(spawn);
                spawn_point_ = spawn;
                needs_spawn_calc_ = false;
                break;
            }
        }
        if (needs_spawn_calc_) return;  // chunks not loaded yet, try next frame
    }

    // The held item can light the player independently of movement, so this runs
    // before the frozen/dead early-outs: the light must follow a hotbar switch
    // and must be back in the right state when the player respawns.
    update_held_light();

    if (!collision_resolver_ || dead_) return;

    update_break_progress(static_cast<float>(delta));

    float speed_multiplier = 1.0f;
    if (chunk_manager_) speed_multiplier = chunk_manager_->get_move_speed_multiplier();

    if (fly_mode_) {
        Input* input = Input::get_singleton();
        Vector3 input_dir;
        // Suppress movement while a UI overlay (inventory/table/chat/settings/
        // the wand's menu) is open
        if (input && !inventory_open_ && !table_menu_open_ && !chat_open_ && !settings_open_ &&
            !wand_menu_open_) {
            Basis basis = get_basis();
            if (input->is_action_pressed("move_forward")) input_dir -= basis.get_column(2);
            if (input->is_action_pressed("move_back"))    input_dir += basis.get_column(2);
            if (input->is_action_pressed("move_left"))    input_dir -= basis.get_column(0);
            if (input->is_action_pressed("move_right"))   input_dir += basis.get_column(0);
            if (input->is_action_pressed("jump"))         input_dir += Vector3(0, 1, 0);
            if (input->is_action_pressed("sneak"))        input_dir += Vector3(0, -1, 0);
        }
        input_dir = input_dir.normalized();
        Vector3 pos = get_global_position();
        pos += input_dir * fly_speed_ * speed_multiplier * static_cast<float>(delta);
        set_global_position(pos);
        sim_.reset(pos);
        rendered_eye_height_ = 1.62f;
        update_camera_transform(1.62f, static_cast<float>(delta));
        return;
    }

    PlayerInput pi;
    Input* input = Input::get_singleton();
    // Suppress movement while a UI overlay (inventory/table/chat/settings/the
    // wand's menu) is open
    if (input && !inventory_open_ && !table_menu_open_ && !chat_open_ && !settings_open_ &&
        !wand_menu_open_) {
        Basis basis = get_basis();
        pi.move_forward_held = input->is_action_pressed("move_forward");
        if (input->is_action_pressed("move_forward")) pi.wish_direction -= basis.get_column(2);
        if (input->is_action_pressed("move_back"))    pi.wish_direction += basis.get_column(2);
        if (input->is_action_pressed("move_left"))    pi.wish_direction -= basis.get_column(0);
        if (input->is_action_pressed("move_right"))   pi.wish_direction += basis.get_column(0);
        pi.wish_direction.y = 0.0f;
        if (pi.wish_direction.length_squared() > 1.0f) {
            pi.wish_direction.normalize();
        }
        if (input->is_action_just_pressed("jump")) sim_.queue_jump();
        pi.jump_pressed = input->is_action_pressed("jump");
        pi.sprint_held = input->is_action_pressed("sprint");
        pi.sneak_held = input->is_action_pressed("sneak");
        pi.yaw = get_rotation().y;
    }

    sim_.accumulate_and_tick(delta, pi, *collision_resolver_, PlayerSim::STEP_HEIGHT, speed_multiplier);

    // Apply any fall damage queued by landings during this frame's ticks.
    int fall_damage = sim_.consume_pending_fall_damage();
    if (fall_damage > 0) {
        set_health(health_ - fall_damage);
    }

    // Update player animation based on movement
    bool is_walking = pi.wish_direction.length_squared() > 0.01f && sim_.is_on_floor();
    update_player_animation(is_walking);

    float partial = sim_.get_accumulator_fraction();
    set_global_position(sim_.get_render_position(partial));

    // Minecraft-style body yaw (body-yaw): while walking the body eases
    // toward the travel direction; standing still it holds, so the head turns
    // up to ±75° before dragging the body along. Applied to the model pivot —
    // the camera and the head's camera-tracking stay on the true look dir.
    {
        const float look_yaw = get_rotation().y;
        const float wish_len = pi.wish_direction.length();
        // Body turn follows horizontal movement whether on the ground or
        // midair (vanilla gates the body turn on movement only; the
        // on-ground check there only drives the limb-swing speed).
        if (wish_len > 0.01f) {
            // pi.wish_direction is already a world-space XZ direction (built
            // from the player's world basis columns above), so it must NOT be
            // rotated by the player's basis again — that double-rotation made
            // the torso face the wrong way whenever the player was turned.
            const Vector3 world_wish = pi.wish_direction;
            const float travel_yaw = std::atan2(-world_wish.x, -world_wish.z);
            const float ease = 1.0f - std::pow(1.0f - kBodyTurnPerTick,
                                               static_cast<float>(delta) * 20.0f);
            body_yaw_ += wrap_pi(travel_yaw - body_yaw_) * ease;
        }
        const float look_diff = wrap_pi(look_yaw - body_yaw_);
        if (look_diff > kBodyMaxYaw) body_yaw_ = look_yaw - kBodyMaxYaw;
        else if (look_diff < -kBodyMaxYaw) body_yaw_ = look_yaw + kBodyMaxYaw;
        if (model_pivot_) {
            model_pivot_->set_rotation(Vector3(0, wrap_pi(body_yaw_ - look_yaw), 0));
        }
    }

    update_camera_transform(sim_.get_eye_height(), static_cast<float>(delta));
}

void PlayerController::_input(const Ref<InputEvent>& p_event) {
    Input* input = Input::get_singleton();
    if (!input) return;

    // Dead: freeze everything (look/move/break/place/hotbar) until respawn.
    if (dead_) return;

    // Third-person toggle works even while a UI overlay is open (and without
    // mouse capture); only the dead check above gates it.
    if (p_event->is_action_pressed("toggle_third_person")) {
        toggle_third_person();
    }

    // Skip mouse mode switching when inventory, table menu, chat or settings is open
    if (inventory_open_ || table_menu_open_ || chat_open_ || settings_open_ || wand_menu_open_) {
        return;
    }

    if (p_event->is_action_pressed("ui_cancel")) {
        // A UI (chat/inventory) may have already consumed this Escape to close
        // itself and restore mouse capture. Don't show the cursor for it.
        if (get_viewport()->is_input_handled()) {
            return;
        }
        input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
        return;
    }

    if (input->get_mouse_mode() != Input::MOUSE_MODE_CAPTURED) {
        if (p_event->is_action_pressed("mouse_click_left")) {
            input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
        }
        return;
    }
    
    Ref<InputEventMouseMotion> mm = p_event;
    if (mm.is_valid()) {
        rotate_y(-mm->get_relative().x * sensitivity_);
        pitch_ -= mm->get_relative().y * sensitivity_;
        // Minecraft clamps the look to ±90° (straight up / straight down).
        pitch_ = CLAMP(pitch_, -kMaxLookPitch, kMaxLookPitch);
        // Camera rotation/position is applied every frame in _process, which
        // knows the current view mode (first / back / front).
    }

    // Hold-to-break: progress accumulates in _process via update_break_progress;
    // the LMB click here only re-captures the mouse (handled above) and punches
    // the pose-clone dummy when it's under the crosshair (Minecraft attack).
    // A held wand takes the click instead — its left button commits whatever it
    // is showing, and it never mines.
    if (p_event->is_action_pressed("mouse_click_left")) {
        if (wand_held()) {
            emit_signal("wand_confirm");
        } else if (try_punch_dummy()) {
            punch_cooldown_ = kPunchInterval;
        }
    }

    if (p_event->is_action_pressed("mouse_click_right")) {
        if (wand_held()) {
            emit_signal("wand_use");
        } else {
            use_item();
        }
    }

    if (p_event->is_action_pressed("wand_menu") && wand_held()) {
        emit_signal("wand_menu");
    }

    if (p_event->is_action_pressed("fly_toggle")) {
        toggle_fly_mode();
    }

    Ref<InputEventKey> ke = p_event;
    if (ke.is_valid() && ke->is_pressed() && !ke->is_echo()) {
        int pk = ke->get_physical_keycode();
        if (pk >= KEY_1 && pk <= KEY_9) inventory_.select_slot(pk - KEY_1);
    }
}

void PlayerController::update_mouse_mode() {
    Input* input = Input::get_singleton();
    if (!input) return;
    if (inventory_open_ || table_menu_open_ || chat_open_ || settings_open_ || wand_menu_open_ || dead_) {
        input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
    } else {
        input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
    }
}
