#ifndef FARLANDS_PLAYER_CONTROLLER_NODE_HPP
#define FARLANDS_PLAYER_CONTROLLER_NODE_HPP

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>

#include "engine/player_controller.hpp"
#include "core/inventory.hpp"
#include "engine/voxel_engine_controller.hpp"

namespace VoxelEngine {
class CollisionResolver;
class ChunkManager;
}

class PlayerController : public godot::Node3D {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(PlayerController, godot::Node3D)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    PlayerController();
    ~PlayerController() override;

    void _ready() override;
    void _exit_tree() override;
    void _process(double delta) override;
    void _input(const godot::Ref<godot::InputEvent> &p_event) override;

    // Bound API
    void toggle_fly_mode();
    void break_block();
    void place_block();
    int get_selected_block() const;
    void set_selected_block(int block_id);
    int get_block_edit_counter() const;
    
    // Inventory API
    int get_hotbar_slot_count(int slot) const;
    int get_hotbar_slot_block_id(int slot) const;
    int get_selected_hotbar_slot() const;
    void select_hotbar_slot(int slot);
    void set_hotbar_slot(int slot, int block_id, int count);
    int get_inventory_slot_count(int slot) const;
    int get_inventory_slot_block_id(int slot) const;
    void set_inventory_slot(int slot, int block_id, int count);
    bool give_block(int block_id, int count);
    void clear_inventory();
    // Crafting API: grid is 2x2 or 3x3 row-major block ids (0 = empty) with
    // parallel counts. match_recipe previews the result but only reports a
    // match when the grid actually holds every ingredient; craft_recipe
    // additionally returns the deducted grid plus the crafted output.
    godot::Dictionary match_recipe(const godot::PackedInt32Array& grid_ids,
                                   const godot::PackedInt32Array& grid_counts);
    godot::Dictionary craft_recipe(const godot::PackedInt32Array& grid_ids,
                                   const godot::PackedInt32Array& grid_counts);
    void save_inventory();
    bool load_inventory();
    bool set_active_texture_pack(const godot::String& pack_name);
    godot::PackedStringArray get_installed_pack_names() const;
    godot::String resolve_texture_path(const godot::String& texture_name) const;
    void set_inventory_open(bool open);
    bool is_inventory_open() const;

    // Crafting table 3x3 menu API
    void set_table_menu_open(bool open);
    bool is_table_menu_open() const;

    // Chat API
    void set_chat_open(bool open);
    bool is_chat_open() const;
    
    // Settings menu API
    void set_settings_open(bool open);
    bool is_settings_open() const;
    
    void teleport_to(const godot::Vector3& pos);
    void set_fly_mode(bool on);
    bool get_fly_mode() const;

    // Health API (half-hearts, vanilla scale: 20 max)
    int get_health() const;
    void set_health(int value);
    bool is_dead() const;
    void die();
    void respawn();
    
    void set_sensitivity(float s);
    float get_sensitivity() const;
    void set_fly_speed(float s);
    float get_fly_speed() const;

    // Third-person camera API (F5 toggles it)
    void toggle_third_person();
    void set_third_person(bool on);
    bool get_third_person() const;

protected:
    static void _bind_methods();

private:
    void update_mouse_mode();
    VoxelEngine::PlayerSim sim_;
    godot::Camera3D* camera_ = nullptr;
    VoxelEngine::CollisionResolver* collision_resolver_ = nullptr;
    VoxelEngine::ChunkManager* chunk_manager_ = nullptr;
    VoxelEngine::Inventory inventory_;

    float pitch_ = 0.0f;
    float sensitivity_ = 0.003f;
    float fly_speed_ = 10.0f;
    bool fly_mode_ = false;
    bool inventory_open_ = false;
    bool table_menu_open_ = false;
    bool chat_open_ = false;
    bool settings_open_ = false;
    bool inventory_saved_ = false;
    float rendered_eye_height_ = 1.62f;
    int block_edit_counter_ = 0;
    static constexpr int MAX_HEALTH = 20;
    int health_ = MAX_HEALTH;
    bool dead_ = false;
    bool needs_spawn_calc_ = true;
    bool third_person_ = false;
    godot::Node3D* model_ = nullptr;
    godot::Vector3 spawn_point_;
};

#endif // FARLANDS_PLAYER_CONTROLLER_NODE_HPP
