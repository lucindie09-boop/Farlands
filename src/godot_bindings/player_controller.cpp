// The node itself: construction, the GDScript bindings, the health/death state,
// the cached sibling nodes, and the mode toggles (fly, third person, view). The
// per-frame loop lives in player_controller_input.cpp, what the player aims at in
// player_controller_camera.cpp, and the edits it makes in _interact/_place/_inventory.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/cached_node.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/variant/utility_functions.hpp>

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
    // One function per area of the API, in the order the areas are declared: a
    // new binding has one obvious home, and no single list runs off the screen.
    // Signals and properties go in last, in that order.
    bind_actions();
    bind_inventory_api();
    bind_ui_state();
    bind_tuning();
    bind_state_and_view();
    add_signals();
    add_properties();
}

void PlayerController::bind_actions() {
    ClassDB::bind_method(D_METHOD("toggle_fly_mode"), &PlayerController::toggle_fly_mode);
    ClassDB::bind_method(D_METHOD("break_block"), &PlayerController::break_block);
    ClassDB::bind_method(D_METHOD("place_block"), &PlayerController::place_block);
    ClassDB::bind_method(D_METHOD("use_item"), &PlayerController::use_item);
    ClassDB::bind_method(D_METHOD("get_selected_block"), &PlayerController::get_selected_block);
    ClassDB::bind_method(D_METHOD("set_selected_block", "block_id"), &PlayerController::set_selected_block);
    ClassDB::bind_method(D_METHOD("get_block_edit_counter"), &PlayerController::get_block_edit_counter);
    ClassDB::bind_method(D_METHOD("get_break_state"), &PlayerController::get_break_state);
}

void PlayerController::bind_inventory_api() {
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
}

void PlayerController::bind_ui_state() {
    ClassDB::bind_method(D_METHOD("set_chat_open", "open"), &PlayerController::set_chat_open);
    ClassDB::bind_method(D_METHOD("is_chat_open"), &PlayerController::is_chat_open);
    ClassDB::bind_method(D_METHOD("set_settings_open", "open"), &PlayerController::set_settings_open);
    ClassDB::bind_method(D_METHOD("set_wand_menu_open", "open"),
                         &PlayerController::set_wand_menu_open);
    ClassDB::bind_method(D_METHOD("is_wand_held"), &PlayerController::wand_held);
    ClassDB::bind_method(D_METHOD("is_wand_menu_open"), &PlayerController::is_wand_menu_open);
    ClassDB::bind_method(D_METHOD("is_settings_open"), &PlayerController::is_settings_open);
    ClassDB::bind_method(D_METHOD("teleport_to", "pos"), &PlayerController::teleport_to);
    ClassDB::bind_method(D_METHOD("set_fly_mode", "on"), &PlayerController::set_fly_mode);
    ClassDB::bind_method(D_METHOD("get_fly_mode"), &PlayerController::get_fly_mode);
}

void PlayerController::bind_tuning() {
    ClassDB::bind_method(D_METHOD("set_sensitivity", "s"), &PlayerController::set_sensitivity);
    ClassDB::bind_method(D_METHOD("get_sensitivity"), &PlayerController::get_sensitivity);
    ClassDB::bind_method(D_METHOD("set_fly_speed", "s"), &PlayerController::set_fly_speed);
    ClassDB::bind_method(D_METHOD("get_fly_speed"), &PlayerController::get_fly_speed);
}

void PlayerController::bind_state_and_view() {
    ClassDB::bind_method(D_METHOD("get_health"), &PlayerController::get_health);
    ClassDB::bind_method(D_METHOD("set_health", "value"), &PlayerController::set_health);
    ClassDB::bind_method(D_METHOD("is_dead"), &PlayerController::is_dead);
    ClassDB::bind_method(D_METHOD("die"), &PlayerController::die);
    ClassDB::bind_method(D_METHOD("respawn"), &PlayerController::respawn);
    ClassDB::bind_method(D_METHOD("is_on_floor"), &PlayerController::is_on_floor);
    ClassDB::bind_method(D_METHOD("is_in_water"), &PlayerController::is_in_water);

    ClassDB::bind_method(D_METHOD("toggle_third_person"), &PlayerController::toggle_third_person);
    ClassDB::bind_method(D_METHOD("set_third_person", "on"), &PlayerController::set_third_person);
    ClassDB::bind_method(D_METHOD("get_third_person"), &PlayerController::get_third_person);
    ClassDB::bind_method(D_METHOD("set_third_person_view", "view"), &PlayerController::set_third_person_view);
    ClassDB::bind_method(D_METHOD("get_third_person_view"), &PlayerController::get_third_person_view);
    ClassDB::bind_method(D_METHOD("update_player_animation", "is_walking"), &PlayerController::update_player_animation);
    ClassDB::bind_method(D_METHOD("get_aim_origin"), &PlayerController::get_aim_origin);
    ClassDB::bind_method(D_METHOD("get_aim_direction"), &PlayerController::get_aim_direction);
}

void PlayerController::add_signals() {
    ADD_SIGNAL(MethodInfo("crafting_table_used"));
    ADD_SIGNAL(MethodInfo("block_placed"));
    ADD_SIGNAL(MethodInfo("died"));
    ADD_SIGNAL(MethodInfo("respawned"));
    // A held wand's clicks: middle opens its menu, right works the tool in the
    // world, left commits what the tool is showing. The wand never reaches the
    // placement path (its item id is above the block registry, so it could not
    // place a block anyway), and these are emitted instead of breaking or
    // placing so exactly one thing answers a click while it is held.
    ADD_SIGNAL(MethodInfo("wand_menu"));
    ADD_SIGNAL(MethodInfo("wand_use"));
    ADD_SIGNAL(MethodInfo("wand_confirm"));
}

void PlayerController::add_properties() {
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

bool PlayerController::is_in_water() const { return sim_.is_in_water(); }

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
    camera_ = get_node<Camera3D>("Camera3D");
    if (camera_) {
        camera_->set_position(Vector3(0, 1.62f, 0));
    }

    // The visual body (player.glb) follows the player's position; hidden in
    // first person. It lives under a ModelPivot wrapper so the body can lag
    // behind the look direction (vanilla's body-yaw) while the head
    // stays glued to the camera.
    if (model_pivot_ == nullptr) {
        model_pivot_ = memnew(godot::Node3D);
        model_pivot_->set_name("ModelPivot");
        add_child(model_pivot_);
    }
    model_ = Object::cast_to<Node3D>(find_child("PlayerModel", true, false));
    if (model_) {
        if (model_->get_parent() != model_pivot_) {
            model_->reparent(model_pivot_);
        }
        model_->set_visible(third_person_view_ > 0);
    } else {
        // Every third-person path keys off model_, so a missing node degrades to
        // "F5 shows an empty world" with no error anywhere. An editor re-save of
        // Main.tscn can drop the node without touching anything else, so say so
        // loudly instead of letting the body vanish in silence.
        WARN_PRINT("PlayerController: no \"PlayerModel\" node under the scene — "
                   "third person will show no body. Main.tscn should instance "
                   "player.glb as Player/PlayerModel with player_model.gd attached.");
    }

    // The ChunkManager is a SIBLING of this node, so its lifetime is not tied to
    // ours: it is cached as an instance ID and resolved on every use rather than
    // held as a raw pointer (see godot_bindings/cached_node.hpp).
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (cm_node) {
        chunk_manager_ = Object::cast_to<ChunkManager>(cm_node);
        if (chunk_manager_) {
            cache_object(chunk_manager_id_, chunk_manager_);
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
    // Drop every cached node pointer on the way out. Nothing here may be used
    // after teardown, and a non-null pointer is what makes the rest of the class
    // willing to dereference one.
    camera_ = nullptr;
    collision_resolver_ = nullptr;
    chunk_manager_ = nullptr;
    chunk_manager_id_ = 0;
    model_ = nullptr;
    model_pivot_ = nullptr;
}

// Re-resolve the sibling nodes this class depends on. Held as instance IDs, so a
// ChunkManager that was freed or replaced resolves to nullptr instead of being
// dereferenced, and a replacement is looked up and re-cached here. The resolver
// is owned by the chunk manager, so it is refreshed with it rather than cached
// on its own (it is not an Object and has no ID of its own).
void PlayerController::refresh_cached_nodes() {
    ChunkManager* chunks = resolve_cached<ChunkManager>(chunk_manager_id_);
    if (chunks == nullptr) {
        // Either never seen or gone since: try the scene again, which is what
        // picks up a replacement rather than leaving the game permanently
        // without a world.
        chunks = Object::cast_to<ChunkManager>(get_node_or_null(NodePath("/root/Main/ChunkManager")));
        cache_object(chunk_manager_id_, chunks);
    }
    if (chunks != chunk_manager_) {
        chunk_manager_ = chunks;
        collision_resolver_ = chunks != nullptr ? chunks->get_collision_resolver() : nullptr;
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
    // vanilla's F5 cycles: first -> behind player -> in front of player.
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
