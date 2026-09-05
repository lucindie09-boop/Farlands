#include "godot_bindings/player_controller.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "engine/collision_resolver.hpp"
#include "core/item_registry.hpp"
#include "core/chunk_coords.hpp"
#include "world/chunk_world.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "render/texture_array_generator.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <algorithm>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;

namespace {
// Minecraft's third-person camera sits 4 blocks back (thirdPersonView uses 4.0
// for both the back and front views) and is pulled in when it would clip
// through solid terrain. The front view mirrors the offset along the look dir.
constexpr float kThirdPersonOffset = 4.0f;
constexpr float kCameraStep = 0.25f;    // camera-collision sampling step, blocks
constexpr float kCameraMinDist = 0.25f; // never push the camera into the player
constexpr float kPi = 3.14159265358979f;
constexpr float kTwoPi = 6.28318530718f;
constexpr float kMaxLookPitch = kPi / 2.0f; // ±90°, matching Minecraft
// Body yaw (renderYawOffset): eases 0.3 of the remaining gap per 20 Hz tick and
// is clamped to ±35° from the look yaw — the dead zone the head can lead the
// body by before the torso is dragged along (Minecraft uses ±75°; 35° reads
// tighter and more responsive).
constexpr float kBodyTurnPerTick = 0.3f;
constexpr float kBodyMaxYaw = 35.0f * kPi / 180.0f;

float wrap_pi(float a) {
    return std::remainder(a, kTwoPi);
}
} // namespace

PlayerController::PlayerController() = default;
PlayerController::~PlayerController() {
    // Auto-save inventory on destruction (no-op if _exit_tree already saved).
    if (!inventory_saved_) {
        save_inventory();
    }
}

void PlayerController::_bind_methods() {
    ClassDB::bind_method(D_METHOD("toggle_fly_mode"), &PlayerController::toggle_fly_mode);
    ClassDB::bind_method(D_METHOD("break_block"), &PlayerController::break_block);
    ClassDB::bind_method(D_METHOD("place_block"), &PlayerController::place_block);
    ClassDB::bind_method(D_METHOD("get_selected_block"), &PlayerController::get_selected_block);
    ClassDB::bind_method(D_METHOD("set_selected_block", "block_id"), &PlayerController::set_selected_block);
    ClassDB::bind_method(D_METHOD("get_block_edit_counter"), &PlayerController::get_block_edit_counter);
    ClassDB::bind_method(D_METHOD("get_break_state"), &PlayerController::get_break_state);
    
    // Inventory API
    ClassDB::bind_method(D_METHOD("get_hotbar_slot_count", "slot"), &PlayerController::get_hotbar_slot_count);
    ClassDB::bind_method(D_METHOD("get_hotbar_slot_block_id", "slot"), &PlayerController::get_hotbar_slot_block_id);
    ClassDB::bind_method(D_METHOD("get_selected_hotbar_slot"), &PlayerController::get_selected_hotbar_slot);
    ClassDB::bind_method(D_METHOD("select_hotbar_slot", "slot"), &PlayerController::select_hotbar_slot);
    ClassDB::bind_method(D_METHOD("set_hotbar_slot", "slot", "block_id", "count"), &PlayerController::set_hotbar_slot);
    ClassDB::bind_method(D_METHOD("get_inventory_slot_count", "slot"), &PlayerController::get_inventory_slot_count);
    ClassDB::bind_method(D_METHOD("get_inventory_slot_block_id", "slot"), &PlayerController::get_inventory_slot_block_id);
    ClassDB::bind_method(D_METHOD("set_inventory_slot", "slot", "block_id", "count"), &PlayerController::set_inventory_slot);
    ClassDB::bind_method(D_METHOD("give_block", "block_id", "count"), &PlayerController::give_block);
    ClassDB::bind_method(D_METHOD("clear_inventory"), &PlayerController::clear_inventory);
    ClassDB::bind_method(D_METHOD("match_recipe", "grid_ids", "grid_counts"), &PlayerController::match_recipe);
    ClassDB::bind_method(D_METHOD("craft_recipe", "grid_ids", "grid_counts"), &PlayerController::craft_recipe);
    ClassDB::bind_method(D_METHOD("save_inventory"), &PlayerController::save_inventory);
    ClassDB::bind_method(D_METHOD("load_inventory"), &PlayerController::load_inventory);
    ClassDB::bind_method(D_METHOD("set_active_texture_pack", "pack_name"), &PlayerController::set_active_texture_pack);
    ClassDB::bind_method(D_METHOD("get_installed_pack_names"), &PlayerController::get_installed_pack_names);
    ClassDB::bind_method(D_METHOD("resolve_texture_path", "texture_name"), &PlayerController::resolve_texture_path);
    ClassDB::bind_method(D_METHOD("set_inventory_open", "open"), &PlayerController::set_inventory_open);
    ClassDB::bind_method(D_METHOD("is_inventory_open"), &PlayerController::is_inventory_open);
    ClassDB::bind_method(D_METHOD("set_table_menu_open", "open"), &PlayerController::set_table_menu_open);
    ClassDB::bind_method(D_METHOD("is_table_menu_open"), &PlayerController::is_table_menu_open);

    ClassDB::bind_method(D_METHOD("set_chat_open", "open"), &PlayerController::set_chat_open);
    ClassDB::bind_method(D_METHOD("is_chat_open"), &PlayerController::is_chat_open);
    ClassDB::bind_method(D_METHOD("set_settings_open", "open"), &PlayerController::set_settings_open);
    ClassDB::bind_method(D_METHOD("is_settings_open"), &PlayerController::is_settings_open);
    ClassDB::bind_method(D_METHOD("teleport_to", "pos"), &PlayerController::teleport_to);
    ClassDB::bind_method(D_METHOD("set_fly_mode", "on"), &PlayerController::set_fly_mode);
    ClassDB::bind_method(D_METHOD("get_fly_mode"), &PlayerController::get_fly_mode);
    
    ClassDB::bind_method(D_METHOD("set_sensitivity", "s"), &PlayerController::set_sensitivity);
    ClassDB::bind_method(D_METHOD("get_sensitivity"), &PlayerController::get_sensitivity);
    ClassDB::bind_method(D_METHOD("set_fly_speed", "s"), &PlayerController::set_fly_speed);
    ClassDB::bind_method(D_METHOD("get_fly_speed"), &PlayerController::get_fly_speed);

    ClassDB::bind_method(D_METHOD("get_health"), &PlayerController::get_health);
    ClassDB::bind_method(D_METHOD("set_health", "value"), &PlayerController::set_health);
    ClassDB::bind_method(D_METHOD("is_dead"), &PlayerController::is_dead);
    ClassDB::bind_method(D_METHOD("die"), &PlayerController::die);
    ClassDB::bind_method(D_METHOD("respawn"), &PlayerController::respawn);
    ClassDB::bind_method(D_METHOD("is_on_floor"), &PlayerController::is_on_floor);

    ClassDB::bind_method(D_METHOD("toggle_third_person"), &PlayerController::toggle_third_person);
    ClassDB::bind_method(D_METHOD("set_third_person", "on"), &PlayerController::set_third_person);
    ClassDB::bind_method(D_METHOD("get_third_person"), &PlayerController::get_third_person);
    ClassDB::bind_method(D_METHOD("set_third_person_view", "view"), &PlayerController::set_third_person_view);
    ClassDB::bind_method(D_METHOD("get_third_person_view"), &PlayerController::get_third_person_view);
    ClassDB::bind_method(D_METHOD("update_player_animation", "is_walking"), &PlayerController::update_player_animation);
    ClassDB::bind_method(D_METHOD("get_aim_origin"), &PlayerController::get_aim_origin);
    ClassDB::bind_method(D_METHOD("get_aim_direction"), &PlayerController::get_aim_direction);

    ADD_SIGNAL(MethodInfo("crafting_table_used"));
    ADD_SIGNAL(MethodInfo("block_placed"));
    ADD_SIGNAL(MethodInfo("died"));
    ADD_SIGNAL(MethodInfo("respawned"));

    ADD_PROPERTY(PropertyInfo(Variant::INT, "health", PROPERTY_HINT_RANGE, "0,20,1"),
                 "set_health", "get_health");

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sensitivity", PROPERTY_HINT_RANGE, "0.001,0.01,0.001"),
                 "set_sensitivity", "get_sensitivity");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fly_speed", PROPERTY_HINT_RANGE, "1.0,50.0,0.5"),
                 "set_fly_speed", "get_fly_speed");
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "third_person"), "set_third_person", "get_third_person");
}

void PlayerController::set_sensitivity(float s) { sensitivity_ = s; }
float PlayerController::get_sensitivity() const { return sensitivity_; }
void PlayerController::set_fly_speed(float s) { fly_speed_ = s; }
float PlayerController::get_fly_speed() const { return fly_speed_; }

int PlayerController::get_health() const { return health_; }

bool PlayerController::is_dead() const { return dead_; }

bool PlayerController::is_on_floor() const { return sim_.is_on_floor(); }

void PlayerController::set_health(int value) {
    health_ = CLAMP(value, 0, MAX_HEALTH);
    if (health_ <= 0 && !dead_) {
        die();
    }
}

void PlayerController::die() {
    if (dead_) return;
    dead_ = true;
    // The sim stops ticking when dead, so the prev->curr interpolation would
    // freeze mid-lerp short of the landing spot. Snap it onto position_.
    sim_.snap_render_position();
    update_mouse_mode();
    emit_signal("died");
}

void PlayerController::respawn() {
    if (!dead_) return;
    dead_ = false;
    health_ = MAX_HEALTH;
    // teleport_to resets the sim (clearing fall state) at the spawn point.
    teleport_to(spawn_point_);
    emit_signal("respawned");
    update_mouse_mode();
}

void PlayerController::_ready() {
    g_engine_running = true;
    camera_ = get_node<Camera3D>("Camera3D");
    if (camera_) {
        camera_->set_position(Vector3(0, 1.62f, 0));
    }

    // The visual body (player.glb) follows the player's position; hidden in
    // first person. It lives under a ModelPivot wrapper so the body can lag
    // behind the look direction (Minecraft's renderYawOffset) while the head
    // stays glued to the camera.
    if (model_pivot_ == nullptr) {
        model_pivot_ = memnew(godot::Node3D);
        model_pivot_->set_name("ModelPivot");
        add_child(model_pivot_);
    }
    model_ = Object::cast_to<Node3D>(find_child("PlayerModel", true, false));
    if (model_ && model_->get_parent() != model_pivot_) {
        model_->reparent(model_pivot_);
    }
    if (model_) {
        model_->set_visible(third_person_view_ > 0);
    }

    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (cm_node) {
        chunk_manager_ = Object::cast_to<ChunkManager>(cm_node);
        if (chunk_manager_) {
            collision_resolver_ = chunk_manager_->get_collision_resolver();
        }
    }

    sim_.reset(get_global_position());
    spawn_point_ = get_global_position();

    // Load inventory from saved data
    load_inventory();
}

void PlayerController::_exit_tree() {
    // _exit_tree fires while every node in the tree is still allocated, so the
    // cached ChunkManager pointer is valid here — unlike in the destructor,
    // where tree lookups and sibling pointers may be gone.
    save_inventory();
    inventory_saved_ = true;
}

void PlayerController::_process(double delta) {
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

    if (!collision_resolver_ || dead_) return;

    update_break_progress(static_cast<float>(delta));

    float speed_multiplier = 1.0f;
    if (chunk_manager_) speed_multiplier = chunk_manager_->get_move_speed_multiplier();

    if (fly_mode_) {
        Input* input = Input::get_singleton();
        Vector3 input_dir;
        // Suppress movement while a UI overlay (inventory/table/chat/settings) is open
        if (input && !inventory_open_ && !table_menu_open_ && !chat_open_ && !settings_open_) {
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
    // Suppress movement while a UI overlay (inventory/table/chat/settings) is open
    if (input && !inventory_open_ && !table_menu_open_ && !chat_open_ && !settings_open_) {
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

    // Minecraft-style body yaw (renderYawOffset): while walking the body eases
    // toward the travel direction; standing still it holds, so the head turns
    // up to ±75° before dragging the body along. Applied to the model pivot —
    // the camera and the head's camera-tracking stay on the true look dir.
    {
        const float look_yaw = get_rotation().y;
        const float wish_len = pi.wish_direction.length();
        // Body turn follows horizontal movement whether on the ground or
        // midair (Minecraft gates func_110146_f on movement only; the
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
    if (inventory_open_ || table_menu_open_ || chat_open_ || settings_open_) {
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
    // the LMB click here only re-captures the mouse (handled above).

    if (p_event->is_action_pressed("mouse_click_right")) {
        place_block();
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

void PlayerController::toggle_fly_mode() {
    set_fly_mode(!fly_mode_);
}

void PlayerController::set_fly_mode(bool on) {
    if (fly_mode_ == on) return;
    fly_mode_ = on;
    sim_.reset(get_global_position());
    rendered_eye_height_ = 1.62f;
}

bool PlayerController::get_fly_mode() const {
    return fly_mode_;
}

void PlayerController::toggle_third_person() {
    // Minecraft's F5 cycles: first -> behind player -> in front of player.
    set_third_person_view((third_person_view_ + 1) % 3);
}

void PlayerController::set_third_person(bool on) {
    set_third_person_view(on ? 1 : 0);
}

bool PlayerController::get_third_person() const {
    return third_person_view_ > 0;
}

void PlayerController::set_third_person_view(int view) {
    if (view < 0) view = 0;
    if (view > 2) view = 2;
    if (third_person_view_ == view) return;
    third_person_view_ = view;
    if (model_) {
        model_->set_visible(view > 0);
    }
    // Reposition the camera immediately for a smooth transition; _process
    // keeps it up to date every frame after this.
    update_camera_transform(rendered_eye_height_, 1.0f / 60.0f);
}

int PlayerController::get_third_person_view() const {
    return third_person_view_;
}

godot::Vector3 PlayerController::get_aim_origin() const {
    return get_global_position() + godot::Vector3(0.0f, sim_.get_eye_height(), 0.0f);
}

godot::Vector3 PlayerController::get_aim_direction() const {
    // Look direction = player yaw (on this node's basis) applied to the
    // pitch-rotated forward; matches the first-person camera's orientation.
    const godot::Vector3 local =
        godot::Vector3(0, 0, -1).rotated(godot::Vector3(1, 0, 0), pitch_);
    return get_global_transform().basis.xform(local).normalized();
}

void PlayerController::update_camera_transform(float eye_height, float delta) {
    if (!camera_) return;
    const Vector3 local_forward = Vector3(0, 0, -1).rotated(Vector3(1, 0, 0), pitch_);

    if (third_person_view_ == 0) {
        // First person: steady eye-height blend, no walk bob.
        const float target = eye_height;
        rendered_eye_height_ += (target - rendered_eye_height_)
            * static_cast<float>(1.0 - std::pow(0.0001, delta));
        camera_->set_position(Vector3(0, rendered_eye_height_, 0));
        camera_->set_rotation(Vector3(pitch_, 0.0f, 0.0f));
        return;
    }

    // Third person (back or front view): the camera sits kThirdPersonOffset
    // along the look direction from the eye, pulled in before any solid block
    // so it never clips through terrain. Back view looks forward past the
    // player's back; front view mirrors the offset (in front of the player,
    // looking back at them), matching Minecraft's thirdPersonView 1 and 2.
    const float dir_sign = (third_person_view_ == 1) ? -1.0f : 1.0f;
    const Transform3D g = get_global_transform();
    const Vector3 eye_world = g.xform(Vector3(0.0f, eye_height, 0.0f));
    const Vector3 dir_world = g.basis.xform(local_forward).normalized() * dir_sign;
    const float dist = camera_clear_distance(eye_world, dir_world, kThirdPersonOffset);
    camera_->set_global_position(eye_world + dir_world * dist);

    // Minecraft's third-person camera uses the player's look rotation directly
    // (the front view adds 180 to the pitch), NOT an aim-at-player direction:
    // the camera sits exactly on the look ray, so its forward is parallel to
    // the eye-ray that block targeting uses and the crosshair/outline always
    // agree with first person. Aim-derived rotations were also the cause of
    // the near-vertical "lock": looking straight down/up the horizontal
    // component of the aim direction vanishes, so the camera's yaw got driven
    // by the body's lagging yaw instead of the mouse.
    if (third_person_view_ == 1) {
        camera_->set_rotation(Vector3(pitch_, 0.0f, 0.0f));
    } else {
        camera_->set_rotation(Vector3(-pitch_, kPi, 0.0f));
    }
}

float PlayerController::camera_clear_distance(const Vector3& eye_world, const Vector3& dir,
                                              float max_dist) const {
    if (!collision_resolver_) return max_dist;
    float t = kCameraMinDist;
    while (t < max_dist) {
        const Vector3 p = eye_world + dir * t;
        if (collision_resolver_->is_solid_at(static_cast<int32_t>(std::floor(p.x)),
                                             static_cast<int32_t>(std::floor(p.y)),
                                             static_cast<int32_t>(std::floor(p.z)))) {
            return std::max(t - kCameraStep, kCameraMinDist);
        }
        t += kCameraStep;
    }
    return max_dist;
}

void PlayerController::update_player_animation(bool is_walking) {
    if (model_) {
        // Call the GDScript method on the PlayerModel node
        model_->call("set_animation_state", is_walking);
    }
}

void PlayerController::teleport_to(const Vector3& pos) {
    set_global_position(pos);
    sim_.reset(pos);
    rendered_eye_height_ = 1.62f;
}

void PlayerController::break_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (result.get("success", false)) {
        Vector3 pos = result["position"];
        int bx = static_cast<int>(std::floor(pos.x));
        int by = static_cast<int>(std::floor(pos.y));
        int bz = static_cast<int>(std::floor(pos.z));
        
        // Get the block type before breaking
        int block_type = cm->get_block(bx, by, bz);
        
        // Remap block variants to base IDs for inventory consistency
        BlockID collect_id = static_cast<BlockID>(block_type);
        int collect_count = 1;
        if (const auto* slab_fam = VoxelEngine::BlockRegistry::get_instance().get_slab_family(collect_id)) {
            // Halves drop as the bottom variant; a merged full block drops both halves
            if (collect_id == slab_fam->full)
                collect_count = 2;
            collect_id = slab_fam->bottom;
        } else if (const auto* stair_fam = VoxelEngine::BlockRegistry::get_instance().get_stair_family(collect_id)) {
            collect_id = stair_fam->base;
        } else if (const auto* wall_fam = VoxelEngine::BlockRegistry::get_instance().get_wall_family(collect_id)) {
            collect_id = wall_fam->base;
        }
        
        // Only break if we can add it to inventory (and it's not air)
        if (block_type != 0 && inventory_.can_add_block(collect_id, collect_count)) {
            // Break the block
            cm->set_block(bx, by, bz, 0);

            // Add to inventory
            inventory_.add_block(collect_id, collect_count);

            // Increment edit counter to invalidate block outline
            block_edit_counter_++;
        }
    }
}

void PlayerController::update_break_progress(float delta) {
    if (!chunk_manager_) {
        break_target_valid_ = false;
        break_progress_ = 0.0f;
        return;
    }

    Input* input = Input::get_singleton();
    const bool mouse_captured = input && input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
    const bool held = input && input->is_action_pressed("mouse_click_left");
    const bool ui_blocked = inventory_open_ || table_menu_open_ || chat_open_ || settings_open_;

    // Re-aim while LMB is held with the mouse captured and no UI open.
    bool aiming = false;
    Vector3i target;
    float hardness = -1.0f;
    BlockID collect_id = 0;
    int collect_count = 1;
    int block_type = 0;
    if (held && mouse_captured && !ui_blocked) {
        Dictionary result = chunk_manager_->raycast_from_camera(10.0);
        if (result.get("success", false)) {
            Vector3 pos = result["position"];
            target = Vector3i(static_cast<int>(std::floor(pos.x)),
                              static_cast<int>(std::floor(pos.y)),
                              static_cast<int>(std::floor(pos.z)));
            block_type = chunk_manager_->get_block(target.x, target.y, target.z);
            if (block_type != 0) {
                aiming = true;
                const VoxelEngine::BlockRegistry& reg = VoxelEngine::BlockRegistry::get_instance();
                collect_id = static_cast<BlockID>(block_type);
                hardness = reg.get_block(collect_id).hardness;
                // Mirror break_block's variant remap for the inventory gate.
                collect_count = 1;
                if (const auto* slab_fam = reg.get_slab_family(collect_id)) {
                    if (collect_id == slab_fam->full) collect_count = 2;
                    collect_id = slab_fam->bottom;
                } else if (const auto* stair_fam = reg.get_stair_family(collect_id)) {
                    collect_id = stair_fam->base;
                } else if (const auto* wall_fam = reg.get_wall_family(collect_id)) {
                    collect_id = wall_fam->base;
                }
            }
        }
    }

    // A different target (or first aim) restarts progress; aiming the same block again resumes it.
    if (aiming && (!break_target_valid_ || target != break_target_)) {
        break_target_ = target;
        break_progress_ = 0.0f;
        break_target_valid_ = true;
        break_block_id_ = block_type;
    }

    // Releasing LMB (or losing the breakable target) drops mining progress; the
    // crack vanishes and the swing stops. Progress only builds while actively aimed.
    if (!aiming || !break_target_valid_) {
        break_progress_ = 0.0f;
        break_target_valid_ = false;
        return;
    }

    // Inventory-full gate: no progress (matches break_block's insta-collect rule).
    if (!inventory_.can_add_block(collect_id, collect_count)) return;

    // Unbreakable blocks never crack or progress.
    if (hardness < 0.0f) return;

    break_progress_ += delta / hardness;
    if (break_progress_ >= 1.0f) {
        break_progress_ = 0.0f;
        break_target_valid_ = false;
        break_block();
    }
}

godot::Dictionary PlayerController::get_break_state() {
    Dictionary state;

    // Invalidate the crack if the memoized block changed or vanished in the world.
    if (break_target_valid_ && chunk_manager_) {
        if (chunk_manager_->get_block(break_target_.x, break_target_.y, break_target_.z) != break_block_id_) {
            break_target_valid_ = false;
            break_progress_ = 0.0f;
        }
    }

    // World-space overlay — stays visible behind menus (progress simply pauses
    // while a UI is open thanks to the ui_blocked gate in update_break_progress).
    const bool visible = break_target_valid_ && break_progress_ > 0.0f && !dead_;
    state["active"] = visible;
    if (visible) {
        state["x"] = break_target_.x;
        state["y"] = break_target_.y;
        state["z"] = break_target_.z;
        // Map progress [0,1) onto 10 crack stages [0,9]; stage 9 is nearly broken.
        state["stage"] = static_cast<int>(std::floor(std::min(break_progress_ * 10.0f, 9.999f)));
    }
    return state;
}

void PlayerController::place_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    // Vanilla container behavior: right-clicking a crafting table uses it
    // (opens its 3x3 menu via the crafting_table_used signal) instead of
    // placing a block against it.
    Dictionary hit = cm->raycast_from_camera(10.0);
    if (hit.get("success", false)) {
        Vector3 hit_pos = hit["position"];
        const int target = cm->get_block(static_cast<int>(std::floor(hit_pos.x)),
                                         static_cast<int>(std::floor(hit_pos.y)),
                                         static_cast<int>(std::floor(hit_pos.z)));
        if (target == static_cast<int>(VoxelEngine::BlockRegistry::get_instance().get_block_id_by_name("crafting_table"))) {
            emit_signal("crafting_table_used");
            return;
        }
    }

    // Get the block type from inventory
    BlockID block_to_place = inventory_.get_selected_block();
    if (block_to_place == 0) return; // No block selected

    // Items (sticks, tools, ...) are never placeable — their ids live in a
    // separate space above the block registry and must stay out of voxels.
    if (VoxelEngine::ItemRegistry::get_instance().is_item(block_to_place)) return;

    // Check if we have enough blocks
    if (inventory_.get_selected_count() <= 0) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (result.get("success", false)) {
        Vector3 place_pos = result["place_position"];
        int bx = static_cast<int>(std::floor(place_pos.x));
        int by = static_cast<int>(std::floor(place_pos.y));
        int bz = static_cast<int>(std::floor(place_pos.z));

        // Slab auto-detection: merge halves into double slab, or pick top/bottom orientation.
        // Works per family (plank slabs, log stumps) so the two never cross-merge.
        BlockID final_block = block_to_place;
        if (const auto* slab_fam = VoxelEngine::BlockRegistry::get_instance().get_slab_family(block_to_place);
                slab_fam && block_to_place != slab_fam->full) {
            Vector3 hit_normal = Vector3(result["hit_normal"]);
            BlockID target_block = static_cast<BlockID>(cm->get_block(bx, by, bz));

            if (target_block == slab_fam->bottom || target_block == slab_fam->top) {
                // Target cell already has a half of this family — merge
                final_block = slab_fam->full;
            } else if (std::abs(hit_normal.y) > 0.5) {
                // Horizontal face: check if clicking on an existing half to merge
                Vector3 hit_pos = result["position"];
                int pos_x = static_cast<int>(std::floor(hit_pos.x));
                int pos_y = static_cast<int>(std::floor(hit_pos.y));
                int pos_z = static_cast<int>(std::floor(hit_pos.z));
                BlockID look_block = static_cast<BlockID>(cm->get_block(pos_x, pos_y, pos_z));

                if (look_block == slab_fam->bottom || look_block == slab_fam->top) {
                    // Only merge when the face completes the full block:
                    // bottom half's top face (+Y) or top half's bottom face (-Y)
                    bool merge = (look_block == slab_fam->bottom && hit_normal.y > 0)
                              || (look_block == slab_fam->top && hit_normal.y < 0);
                    if (merge) {
                        final_block = slab_fam->full;
                        bx = pos_x; by = pos_y; bz = pos_z;
                    } else {
                        final_block = (hit_normal.y > 0) ? slab_fam->bottom : slab_fam->top;
                    }
                } else {
                    // +Y face = bottom half sits on surface, -Y face = top half hugs ceiling
                    final_block = (hit_normal.y > 0) ? slab_fam->bottom : slab_fam->top;
                }
            } else {
                // Side face: upper half of face → top half, lower half → bottom half
                Vector3 hit_point = result["hit_point"];
                double frac_y = hit_point.y - by;
                final_block = (frac_y < 0.5) ? slab_fam->bottom : slab_fam->top;
            }
        }

        // Stair auto-detection: pick rotation from face hit and player direction
        if (const auto* stair_fam = VoxelEngine::BlockRegistry::get_instance().get_stair_family(block_to_place)) {
            Vector3 hit_normal = Vector3(result["hit_normal"]);
            Vector3 ppos_stair = get_global_position();

            if (std::abs(hit_normal.y) > 0.5) {
                // Horizontal face: compute player-to-target direction for rotation
                double dx = ppos_stair.x - (bx + 0.5);
                double dz = ppos_stair.z - (bz + 0.5);
                bool up = hit_normal.y < 0; // bottom face = upside-down

                if (std::abs(dx) > std::abs(dz)) {
                    final_block = (dx > 0)
                        ? (up ? stair_fam->w_up : stair_fam->w)
                        : (up ? stair_fam->e_up : stair_fam->e);
                } else {
                    final_block = (dz > 0)
                        ? (up ? stair_fam->n_up : stair_fam->base)
                        : (up ? stair_fam->s_up : stair_fam->s);
                }
            } else {
                // Side face: step goes against the clicked face; upper portion = upside-down
                Vector3 hit_point = result["hit_point"];
                double frac_y = hit_point.y - by;
                bool up = (frac_y > 0.75);
                double nx = hit_normal.x, nz = hit_normal.z;
                if (std::abs(nx) > std::abs(nz)) {
                    final_block = (nx > 0)
                        ? (up ? stair_fam->w_up : stair_fam->w)
                        : (up ? stair_fam->e_up : stair_fam->e);
                } else {
                    final_block = (nz > 0)
                        ? (up ? stair_fam->n_up : stair_fam->base)
                        : (up ? stair_fam->s_up : stair_fam->s);
                }
            }
        }

        // Wall auto-detection: orient from face hit, merge if target has wall of same family
        if (const auto* wall_fam = VoxelEngine::BlockRegistry::get_instance().get_wall_family(block_to_place)) {
            Vector3 hit_normal = Vector3(result["hit_normal"]);
            BlockID target_block = static_cast<BlockID>(cm->get_block(bx, by, bz));
            const auto* target_wall = VoxelEngine::BlockRegistry::get_instance().get_wall_family(target_block);

            if (target_wall && target_wall == wall_fam && target_block != wall_fam->full) {
                // Side face, target cell already has a wall of same family — merge there
                final_block = wall_fam->full;
            } else if (std::abs(hit_normal.y) > 0.5) {
                // Top/bottom face: wall sits on the edge closest to hit point
                Vector3 hit_point = result["hit_point"];
                double frac_x = hit_point.x - bx;
                double frac_z = hit_point.z - bz;
                double off_x = std::abs(frac_x - 0.5);
                double off_z = std::abs(frac_z - 0.5);
                if (off_x > off_z) {
                    final_block = (frac_x > 0.5) ? wall_fam->e : wall_fam->w;
                } else {
                    final_block = (frac_z > 0.5) ? wall_fam->s : wall_fam->base;
                }
            } else {
                // Side face: wall panel hugs the clicked block (flat side toward it)
                double nx = hit_normal.x, nz = hit_normal.z;
                if (std::abs(nx) > std::abs(nz)) {
                    final_block = (nx > 0) ? wall_fam->w : wall_fam->e;
                } else {
                    final_block = (nz > 0) ? wall_fam->base : wall_fam->s;
                }
            }
        }

        Vector3 ppos = get_global_position();
        int px = static_cast<int>(std::floor(ppos.x));
        int py = static_cast<int>(std::floor(ppos.y));
        int pz = static_cast<int>(std::floor(ppos.z));
        if (bx == px && bz == pz && (by == py || by == py + 1)) return;

        // Don't overwrite an occupied voxel unless a merge changed the block type
        BlockID existing = static_cast<BlockID>(cm->get_block(bx, by, bz));
        if (existing != 0 && final_block == block_to_place) return;

        // Place the block, then verify it actually landed before consuming
        cm->set_block(bx, by, bz, final_block);
        BlockID placed = static_cast<BlockID>(cm->get_block(bx, by, bz));
        if (placed != final_block) return;

        // Consume from inventory
        inventory_.consume_block(block_to_place, 1);

        // Notify that a block actually landed (drives the place swing animation).
        emit_signal("block_placed");

        // Increment edit counter to invalidate block outline
        block_edit_counter_++;
    }
}

int PlayerController::get_selected_block() const { return inventory_.get_selected_block(); }
int PlayerController::get_block_edit_counter() const { return block_edit_counter_; }
void PlayerController::set_selected_block(int block_id) { 
    // Legacy method - set the selected hotbar slot to this block type
    // This is for backwards compatibility, but does NOT grant free blocks
    for (int i = 0; i < VoxelEngine::Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory_.get_hotbar_slot(i);
        if (slot.block_id == block_id) {
            inventory_.select_slot(i);
            return;
        }
    }
    // If not found in hotbar, select the first empty slot (without granting blocks)
    for (int i = 0; i < VoxelEngine::Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory_.get_hotbar_slot(i);
        if (slot.count == 0) {
            inventory_.select_slot(i);
            break;
        }
    }
}

// Inventory API
int PlayerController::get_hotbar_slot_count(int slot) const {
    return inventory_.get_hotbar_slot(slot).count;
}

int PlayerController::get_hotbar_slot_block_id(int slot) const {
    return inventory_.get_hotbar_slot(slot).block_id;
}

int PlayerController::get_selected_hotbar_slot() const {
    return inventory_.get_selected_slot();
}

void PlayerController::select_hotbar_slot(int slot) {
    inventory_.select_slot(slot);
}

void PlayerController::set_hotbar_slot(int slot, int block_id, int count) {
    inventory_.set_hotbar_slot(slot, block_id, count);
}

int PlayerController::get_inventory_slot_count(int slot) const {
    return inventory_.get_inventory_slot(slot).count;
}

int PlayerController::get_inventory_slot_block_id(int slot) const {
    return inventory_.get_inventory_slot(slot).block_id;
}

void PlayerController::set_inventory_slot(int slot, int block_id, int count) {
    inventory_.set_inventory_slot(slot, block_id, count);
}

bool PlayerController::give_block(int block_id, int count) {
    if (block_id <= 0 || count <= 0) return false;
    return inventory_.add_block(block_id, count);
}

void PlayerController::clear_inventory() {
    inventory_.clear();
}

// Crafting API
Dictionary PlayerController::match_recipe(const PackedInt32Array& grid_ids,
                                          const PackedInt32Array& grid_counts) {
    Dictionary out;
    out["ok"] = false;
    const int cell_count = grid_ids.size();
    if (!chunk_manager_ || cell_count != grid_counts.size()) {
        return out;
    }
    int dim;
    switch (cell_count) {
        case 4:  dim = 2; break;   // inventory menu grid
        case 9:  dim = 3; break;   // crafting table grid
        default: return out;
    }
    VoxelEngineController* controller = chunk_manager_->get_controller();
    if (!controller) {
        return out;
    }
    BlockID cells[9];
    int counts[9];
    for (int i = 0; i < cell_count; ++i) {
        const int c = grid_counts[i] > 0 ? grid_counts[i] : 0;
        const int v = grid_ids[i];
        // A cell drained to 0 by a previous craft can keep its stale id on
        // the GUI side; treat it as empty so matching never sees phantom
        // ingredients. Ids may address blocks or items (>=1024 id space).
        cells[i] = (v > 0 && v < VoxelEngine::ItemRegistry::FIRST_ITEM_ID + 256 && c > 0)
                       ? static_cast<BlockID>(v) : BlockIDs::AIR;
        counts[i] = c;
    }
    const CraftingRecipe* recipe = controller->get_recipe_book().match(cells, dim);
    if (!recipe) {
        return out;
    }
    // Gate the preview on availability: ids alone would keep showing a result
    // after craft_recipe consumed the last ingredient (the ghost-icon bug).
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int available = 0;
        for (int i = 0; i < cell_count; ++i) {
            if (cells[i] == need.block_id) available += counts[i];
        }
        if (available < need.count) {
            return out;
        }
    }
    out["ok"] = true;
    out["block_id"] = static_cast<int>(recipe->result.block_id);
    out["count"] = recipe->result.count;
    return out;
}

Dictionary PlayerController::craft_recipe(const PackedInt32Array& grid_ids,
                                          const PackedInt32Array& grid_counts) {
    Dictionary out;
    out["ok"] = false;
    const int cell_count = grid_ids.size();
    if (!chunk_manager_ || cell_count != grid_counts.size()) {
        return out;
    }
    int dim;
    switch (cell_count) {
        case 4:  dim = 2; break;
        case 9:  dim = 3; break;
        default: return out;
    }
    VoxelEngineController* controller = chunk_manager_->get_controller();
    if (!controller) {
        return out;
    }
    BlockID cells[9];
    int counts[9];
    for (int i = 0; i < cell_count; ++i) {
        const int c = grid_counts[i] > 0 ? grid_counts[i] : 0;
        const int v = grid_ids[i];
        // Same stale-drained-cell guard as match_recipe; ids may address
        // blocks or items (>=1024 id space).
        cells[i] = (v > 0 && v < VoxelEngine::ItemRegistry::FIRST_ITEM_ID + 256 && c > 0)
                       ? static_cast<BlockID>(v) : BlockIDs::AIR;
        counts[i] = c;
    }
    const CraftingRecipe* recipe = controller->get_recipe_book().match(cells, dim);
    if (!recipe) {
        return out;
    }

    // Verify every ingredient total is coverable by the grid before touching
    // any cell, then deduct greedily per total (totals have unique ids, so the
    // deductions never interact).
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int available = 0;
        for (int i = 0; i < cell_count; ++i) {
            if (cells[i] == need.block_id) available += counts[i];
        }
        if (available < need.count) {
            return out;
        }
    }
    PackedInt32Array new_counts = grid_counts;
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int remaining = need.count;
        for (int i = 0; i < cell_count && remaining > 0; ++i) {
            if (cells[i] != need.block_id) continue;
            const int take = counts[i] < remaining ? counts[i] : remaining;
            counts[i] -= take;
            remaining -= take;
            new_counts[i] = counts[i];
        }
    }
    out["ok"] = true;
    out["block_id"] = static_cast<int>(recipe->result.block_id);
    out["count"] = recipe->result.count;
    out["new_counts"] = new_counts;
    return out;
}

bool PlayerController::set_active_texture_pack(const String& pack_name) {
    TexturePackManager& tpm = TexturePackManager::get_instance();
    if (pack_name.is_empty()) {
        tpm.clear_active_pack();
    } else if (!tpm.set_active_pack(pack_name)) {
        return false;  // unknown pack name
    }

    // Re-resolve every texture name through the new pack, rebuild the albedo
    // and emissive arrays, and push them into the shared chunk materials.
    // Layer indices are keyed by name (not pack), so existing meshes remain
    // valid — only the pixels change.
    TextureArrayGenerator::get_instance().invalidate_texture_path_cache();
    TextureArrayGenerator::get_instance().force_regenerate();
    if (chunk_manager_) {
        if (VoxelEngineController* controller = chunk_manager_->get_controller()) {
            controller->get_environment_controller().get_material_manager().reload_textures();
        }
    }
    return true;
}

godot::PackedStringArray PlayerController::get_installed_pack_names() const {
    godot::PackedStringArray names;
    for (const VoxelEngine::TexturePack& pack : TexturePackManager::get_instance().packs()) {
        names.push_back(godot::String(pack.name.c_str()));
    }
    return names;
}

godot::String PlayerController::resolve_texture_path(const String& texture_name) const {
    return TexturePackManager::get_instance().resolve(texture_name);
}

void PlayerController::set_inventory_open(bool open) {
    inventory_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_inventory_open() const {
    return inventory_open_;
}

void PlayerController::set_table_menu_open(bool open) {
    table_menu_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_table_menu_open() const {
    return table_menu_open_;
}

void PlayerController::set_chat_open(bool open) {
    chat_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_chat_open() const {
    return chat_open_;
}

void PlayerController::set_settings_open(bool open) {
    settings_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_settings_open() const {
    return settings_open_;
}

void PlayerController::update_mouse_mode() {
    Input* input = Input::get_singleton();
    if (!input) return;
    if (inventory_open_ || table_menu_open_ || chat_open_ || settings_open_ || dead_) {
        input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
    } else {
        input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
    }
}

void PlayerController::save_inventory() {
    if (!chunk_manager_) return;
    auto* controller = chunk_manager_->get_controller();
    if (!controller) return;
    controller->save_inventory(inventory_);
}

bool PlayerController::load_inventory() {
    if (!chunk_manager_) return false;
    auto* controller = chunk_manager_->get_controller();
    if (!controller) return false;
    return controller->load_inventory(inventory_);
}
