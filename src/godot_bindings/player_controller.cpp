#include "godot_bindings/player_controller.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "engine/collision_resolver.hpp"
#include "world/chunk_world.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/input_event_mouse_motion.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;

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
    
    // Inventory API
    ClassDB::bind_method(D_METHOD("get_hotbar_slot_count", "slot"), &PlayerController::get_hotbar_slot_count);
    ClassDB::bind_method(D_METHOD("get_hotbar_slot_block_id", "slot"), &PlayerController::get_hotbar_slot_block_id);
    ClassDB::bind_method(D_METHOD("get_selected_hotbar_slot"), &PlayerController::get_selected_hotbar_slot);
    ClassDB::bind_method(D_METHOD("select_hotbar_slot", "slot"), &PlayerController::select_hotbar_slot);
    ClassDB::bind_method(D_METHOD("set_hotbar_slot", "slot", "block_id", "count"), &PlayerController::set_hotbar_slot);
    ClassDB::bind_method(D_METHOD("get_inventory_slot_count", "slot"), &PlayerController::get_inventory_slot_count);
    ClassDB::bind_method(D_METHOD("get_inventory_slot_block_id", "slot"), &PlayerController::get_inventory_slot_block_id);
    ClassDB::bind_method(D_METHOD("set_inventory_slot", "slot", "block_id", "count"), &PlayerController::set_inventory_slot);
    ClassDB::bind_method(D_METHOD("save_inventory"), &PlayerController::save_inventory);
    ClassDB::bind_method(D_METHOD("load_inventory"), &PlayerController::load_inventory);
    ClassDB::bind_method(D_METHOD("set_inventory_open", "open"), &PlayerController::set_inventory_open);
    ClassDB::bind_method(D_METHOD("is_inventory_open"), &PlayerController::is_inventory_open);
    
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
    g_engine_running = true;
    camera_ = get_node<Camera3D>("Camera3D");
    if (camera_) {
        camera_->set_position(Vector3(0, 1.62f, 0));
    }

    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (cm_node) {
        chunk_manager_ = Object::cast_to<ChunkManager>(cm_node);
        if (chunk_manager_) {
            collision_resolver_ = chunk_manager_->get_collision_resolver();
        }
    }

    sim_.reset(get_global_position());
    
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
    if (!collision_resolver_) return;

    float speed_multiplier = 1.0f;
    if (chunk_manager_) speed_multiplier = chunk_manager_->get_move_speed_multiplier();

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
        pos += input_dir * fly_speed_ * speed_multiplier * static_cast<float>(delta);
        set_global_position(pos);
        sim_.reset(pos);
        rendered_eye_height_ = 1.62f;
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

    float partial = sim_.get_accumulator_fraction();
    set_global_position(sim_.get_render_position(partial));
    if (camera_) {
        float target_eye = sim_.get_eye_height();
        rendered_eye_height_ += (target_eye - rendered_eye_height_) * static_cast<float>(1.0 - std::pow(0.0001, delta));
        Vector3 cam_pos = camera_->get_position();
        cam_pos.y = rendered_eye_height_;
        camera_->set_position(cam_pos);
    }
}

void PlayerController::_input(const Ref<InputEvent>& p_event) {
    Input* input = Input::get_singleton();
    if (!input) return;

    // Skip mouse mode switching when inventory is open
    if (inventory_open_) {
        return;
    }

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
    
    // Skip mouse look when inventory is open
    if (inventory_open_) {
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
        if (pk >= KEY_1 && pk <= KEY_9) inventory_.select_slot(pk - KEY_1);
    }
}

void PlayerController::toggle_fly_mode() {
    fly_mode_ = !fly_mode_;
    sim_.reset(get_global_position());
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
        
        // Only break if we can add it to inventory (and it's not air)
        if (block_type != 0 && inventory_.can_add_block(block_type, 1)) {
            // Break the block
            cm->set_block(bx, by, bz, 0);
            
            // Add to inventory
            inventory_.add_block(block_type, 1);
        }
    }
}

void PlayerController::place_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    // Get the block type from inventory
    BlockID block_to_place = inventory_.get_selected_block();
    if (block_to_place == 0) return; // No block selected
    
    // Check if we have enough blocks
    if (inventory_.get_selected_count() <= 0) return;

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

        // Place the block
        cm->set_block(bx, by, bz, block_to_place);
        
        // Consume from inventory
        inventory_.consume_block(block_to_place, 1);
    }
}

int PlayerController::get_selected_block() const { return inventory_.get_selected_block(); }
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

void PlayerController::set_inventory_open(bool open) {
    inventory_open_ = open;
    Input* input = Input::get_singleton();
    if (input) {
        if (open) {
            input->set_mouse_mode(Input::MOUSE_MODE_VISIBLE);
        } else {
            input->set_mouse_mode(Input::MOUSE_MODE_CAPTURED);
        }
    }
}

bool PlayerController::is_inventory_open() const {
    return inventory_open_;
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
