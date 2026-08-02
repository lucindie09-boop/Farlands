#ifndef FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP
#define FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>

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
    
    // Inventory API
    int get_hotbar_slot_count(int slot) const;
    int get_hotbar_slot_block_id(int slot) const;
    int get_selected_hotbar_slot() const;
    void select_hotbar_slot(int slot);
    void set_hotbar_slot(int slot, int block_id, int count);
    int get_inventory_slot_count(int slot) const;
    int get_inventory_slot_block_id(int slot) const;
    void set_inventory_slot(int slot, int block_id, int count);
    void save_inventory();
    bool load_inventory();
    void set_inventory_open(bool open);
    bool is_inventory_open() const;
    
    void set_sensitivity(float s);
    float get_sensitivity() const;
    void set_fly_speed(float s);
    float get_fly_speed() const;

protected:
    static void _bind_methods();

private:
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
    bool inventory_saved_ = false;
    float rendered_eye_height_ = 1.62f;
};

#endif // FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP
