#ifndef FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP
#define FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/input_event.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "engine/player_controller.hpp"

namespace VoxelEngine {
class CollisionResolver;
}

class PlayerController : public godot::Node3D {
    GDCLASS(PlayerController, godot::Node3D)

public:
    PlayerController();
    ~PlayerController() override;

    void _ready() override;
    void _process(double delta) override;
    void _input(const godot::Ref<godot::InputEvent> &p_event) override;

    // Bound API
    void toggle_fly_mode();
    void break_block();
    void place_block();
    int get_selected_block() const;
    void set_selected_block(int block_id);

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

    float pitch_ = 0.0f;
    float sensitivity_ = 0.003f;
    float fly_speed_ = 10.0f;
    bool fly_mode_ = false;
    int selected_block_type_ = 3; // GRASS
};

#endif // FUK_MINECRAFT_PLAYER_CONTROLLER_NODE_HPP
