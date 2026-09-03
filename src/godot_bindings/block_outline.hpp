#ifndef FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_HPP
#define FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_HPP

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/standard_material3d.hpp>
#include <godot_cpp/classes/box_mesh.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/vector3.hpp>

class BlockOutline : public godot::Node3D {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(BlockOutline, godot::Node3D)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    BlockOutline();
    ~BlockOutline() override;

    void _ready() override;
    void _process(double delta) override;

protected:
    static void _bind_methods();

private:
    // --- Exposed properties (settings_menu.gd reads/writes via get/set) ---
    bool outline_enabled_ = true;
    godot::Color outline_color_ = godot::Color(0, 0, 0, 1);
    float outline_opacity_ = 1.0f;
    float outline_thickness_ = 0.1f;
    bool outline_pulse_enabled_ = false;
    float outline_pulse_speed_ = 1.5f;
    float outline_pulse_min_opacity_ = 0.75f;
    float outline_pulse_max_opacity_ = 1.0f;
    float reach_distance_ = 5.0f;

    bool fill_enabled_ = false;
    godot::Color fill_color_ = godot::Color(1, 1, 1, 1);
    float fill_opacity_ = 0.1f;
    bool fill_pulse_enabled_ = true;
    float fill_pulse_speed_ = 1.5f;
    float fill_pulse_min_opacity_ = 0.05f;
    float fill_pulse_max_opacity_ = 0.1f;

    // --- Internal state ---
    godot::MeshInstance3D* outline_mesh_ = nullptr;
    godot::MeshInstance3D* fill_mesh_ = nullptr;
    godot::StandardMaterial3D* outline_material_ = nullptr;
    godot::StandardMaterial3D* fill_material_ = nullptr;
    float current_thickness_ = -1.0f;
    float pulse_time_ = 0.0f;
    int current_block_id_ = -1;
    godot::Array current_boxes_;

    // Throttling
    godot::Vector3 last_camera_position_;
    godot::Vector3 last_camera_rotation_;
    int last_block_edit_counter_ = -1;
    static constexpr float POSITION_THRESHOLD = 0.01f;
    static constexpr float ROTATION_THRESHOLD = 0.001f;

    // --- Methods ---
    void rebuild_outline_mesh();
    void create_fill();
    void update_fill_for_boxes();
    void create_materials();
    void update_materials();

    // Getter/setter pairs for exposed properties
    bool get_outline_enabled() const { return outline_enabled_; }
    void set_outline_enabled(bool v) { outline_enabled_ = v; }
    godot::Color get_outline_color() const { return outline_color_; }
    void set_outline_color(const godot::Color& v) { outline_color_ = v; }
    float get_outline_opacity() const { return outline_opacity_; }
    void set_outline_opacity(float v) { outline_opacity_ = v; }
    float get_outline_thickness() const { return outline_thickness_; }
    void set_outline_thickness(float v) { outline_thickness_ = v; }
    bool get_outline_pulse_enabled() const { return outline_pulse_enabled_; }
    void set_outline_pulse_enabled(bool v) { outline_pulse_enabled_ = v; }
    float get_outline_pulse_speed() const { return outline_pulse_speed_; }
    void set_outline_pulse_speed(float v) { outline_pulse_speed_ = v; }
    float get_outline_pulse_min_opacity() const { return outline_pulse_min_opacity_; }
    void set_outline_pulse_min_opacity(float v) { outline_pulse_min_opacity_ = v; }
    float get_outline_pulse_max_opacity() const { return outline_pulse_max_opacity_; }
    void set_outline_pulse_max_opacity(float v) { outline_pulse_max_opacity_ = v; }
    float get_reach_distance() const { return reach_distance_; }
    void set_reach_distance(float v) { reach_distance_ = v; }

    bool get_fill_enabled() const { return fill_enabled_; }
    void set_fill_enabled(bool v) { fill_enabled_ = v; }
    godot::Color get_fill_color() const { return fill_color_; }
    void set_fill_color(const godot::Color& v) { fill_color_ = v; }
    float get_fill_opacity() const { return fill_opacity_; }
    void set_fill_opacity(float v) { fill_opacity_ = v; }
    bool get_fill_pulse_enabled() const { return fill_pulse_enabled_; }
    void set_fill_pulse_enabled(bool v) { fill_pulse_enabled_ = v; }
    float get_fill_pulse_speed() const { return fill_pulse_speed_; }
    void set_fill_pulse_speed(float v) { fill_pulse_speed_ = v; }
    float get_fill_pulse_min_opacity() const { return fill_pulse_min_opacity_; }
    void set_fill_pulse_min_opacity(float v) { fill_pulse_min_opacity_ = v; }
    float get_fill_pulse_max_opacity() const { return fill_pulse_max_opacity_; }
    void set_fill_pulse_max_opacity(float v) { fill_pulse_max_opacity_ = v; }
};

#endif // FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_HPP
