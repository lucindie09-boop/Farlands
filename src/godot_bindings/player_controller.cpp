#include "godot_bindings/player_controller.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "engine/collision_resolver.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace VoxelEngine;

PlayerController::PlayerController() = default;
PlayerController::~PlayerController() = default;

void PlayerController::_bind_methods() {
    ClassDB::bind_method(D_METHOD("toggle_fly_mode"), &PlayerController::toggle_fly_mode);
    ClassDB::bind_method(D_METHOD("break_block"), &PlayerController::break_block);
    ClassDB::bind_method(D_METHOD("place_block"), &PlayerController::place_block);
    ClassDB::bind_method(D_METHOD("get_selected_block"), &PlayerController::get_selected_block);
    ClassDB::bind_method(D_METHOD("set_selected_block", "block_id"), &PlayerController::set_selected_block);
    ClassDB::bind_method(D_METHOD("set_sensitivity", "s"), &PlayerController::set_sensitivity);
    ClassDB::bind_method(D_METHOD("get_sensitivity"), &PlayerController::get_sensitivity);
    ClassDB::bind_method(D_METHOD("set_fly_speed", "s"), &PlayerController::set_fly_speed);
    ClassDB::bind_method(D_METHOD("get_fly_speed"), &PlayerController::get_fly_speed);

    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sensitivity", PROPERTY_HINT_RANGE, "0.001,0.01,0.001"),
                 "set_sensitivity", "get_sensitivity");
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fly_speed", PROPERTY_HINT_RANGE, "1.0,50.0,0.5"),
                 "set_fly_speed", "get_fly_speed");
}

void PlayerController::set_sensitivity(float s) { sensitivity_ = s; }
float PlayerController::get_sensitivity() const { return sensitivity_; }
void PlayerController::set_fly_speed(float s) { fly_speed_ = s; }
float PlayerController::get_fly_speed() const { return fly_speed_; }

void PlayerController::_ready() {
    camera_ = get_node<Camera3D>("Camera3D");
    if (camera_) {
        camera_->set_position(Vector3(0, 1.62f, 0));
    }

    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (cm_node) {
        ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
        if (cm) {
            collision_resolver_ = cm->get_collision_resolver();
        }
    }

    sim_.reset(get_global_position());
}

void PlayerController::_process(double delta) {
    if (!collision_resolver_) return;

    if (fly_mode_) {
        Input* input = Input::get_singleton();
        Vector3 input_dir;
        if (input) {
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
        pos += input_dir * fly_speed_ * static_cast<float>(delta);
        set_global_position(pos);
        sim_.reset(pos);
        if (camera_) camera_->set_position(Vector3(0, 1.62f, 0));
        return;
    }

    PlayerInput pi;
    Input* input = Input::get_singleton();
    if (input) {
        Basis basis = get_basis();
        pi.move_forward_held = input->is_action_pressed("move_forward");
        if (input->is_action_pressed("move_forward")) pi.wish_direction -= basis.get_column(2);
        if (input->is_action_pressed("move_back"))    pi.wish_direction += basis.get_column(2);
        if (input->is_action_pressed("move_left"))    pi.wish_direction -= basis.get_column(0);
        if (input->is_action_pressed("move_right"))   pi.wish_direction += basis.get_column(0);
        pi.wish_direction.y = 0.0f;
        if (pi.wish_direction.length_squared() > 0.001f) {
            pi.wish_direction = pi.wish_direction.normalized();
        }
        if (input->is_action_just_pressed("jump")) sim_.queue_jump();
        pi.jump_pressed = input->is_action_pressed("jump");
        pi.sprint_held = input->is_action_pressed("sprint");
        pi.sneak_held = input->is_action_pressed("sneak");
        pi.yaw = get_rotation().y;
    }

    sim_.accumulate_and_tick(delta, pi, *collision_resolver_);

    float partial = sim_.get_accumulator_fraction();
    set_global_position(sim_.get_render_position(partial));
    if (camera_) {
        Vector3 cam_pos = camera_->get_position();
        cam_pos.y = sim_.get_eye_height();
        camera_->set_position(cam_pos);
    }
}

void PlayerController::_input(const Ref<InputEvent>& p_event) {
    Input* input = Input::get_singleton();
    if (!input) return;

    if (input->get_mouse_mode() != Input::MOUSE_MODE_CAPTURED) {
        if (p_event->is_action_pressed("ui_cancel")) {
            input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
            return;
        }
        if (p_event->is_action_pressed("mouse_click_left")) {
            input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
            return;
        }
        return;
    }

    Ref<InputEventMouseMotion> mm = p_event;
    if (mm.is_valid()) {
        rotate_y(-mm->get_relative().x * sensitivity_);
        pitch_ -= mm->get_relative().y * sensitivity_;
        pitch_ = CLAMP(pitch_, -1.4f, 1.4f);
        if (camera_) {
            Vector3 cam_rot = camera_->get_rotation();
            cam_rot.x = pitch_;
            camera_->set_rotation(cam_rot);
        }
    }

    if (p_event->is_action_pressed("ui_cancel")) {
        input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
        return;
    }

    if (p_event->is_action_pressed("mouse_click_left")) {
        break_block();
    }

    if (p_event->is_action_pressed("mouse_click_right")) {
        place_block();
    }

    if (p_event->is_action_pressed("fly_toggle")) {
        toggle_fly_mode();
    }

    Ref<InputEventKey> ke = p_event;
    if (ke.is_valid() && ke->is_pressed() && !ke->is_echo()) {
        int pk = ke->get_physical_keycode();
        if (pk >= KEY_1 && pk <= KEY_9) selected_block_type_ = pk - KEY_1 + 1;
        else if (pk == KEY_0) selected_block_type_ = 0;
    }
}

void PlayerController::toggle_fly_mode() {
    fly_mode_ = !fly_mode_;
    sim_.reset(get_global_position());
}

void PlayerController::break_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (result.get("success", false)) {
        Vector3 pos = result["position"];
        cm->set_block(static_cast<int>(std::floor(pos.x)),
                      static_cast<int>(std::floor(pos.y)),
                      static_cast<int>(std::floor(pos.z)),
                      0);
    }
}

void PlayerController::place_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (result.get("success", false)) {
        Vector3 place_pos = result["place_position"];
        int bx = static_cast<int>(std::floor(place_pos.x));
        int by = static_cast<int>(std::floor(place_pos.y));
        int bz = static_cast<int>(std::floor(place_pos.z));

        Vector3 ppos = get_global_position();
        int px = static_cast<int>(std::floor(ppos.x));
        int py = static_cast<int>(std::floor(ppos.y));
        int pz = static_cast<int>(std::floor(ppos.z));
        if (bx == px && bz == pz && (by == py || by == py + 1)) return;

        cm->set_block(bx, by, bz, selected_block_type_);
    }
}

int PlayerController::get_selected_block() const { return selected_block_type_; }
void PlayerController::set_selected_block(int block_id) { selected_block_type_ = block_id; }
