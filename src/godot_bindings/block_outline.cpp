#include "godot_bindings/block_outline.hpp"
#include "godot_bindings/block_outline_builder.hpp"
#include "godot_bindings/chunk_manager.hpp"
#include "godot_bindings/player_controller.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/array_mesh.hpp>
#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <cmath>

using namespace godot;

BlockOutline::BlockOutline() = default;
BlockOutline::~BlockOutline() = default;

void BlockOutline::_ready() {
    // Cache node references once instead of per-frame lookups
    player_controller_ = Object::cast_to<PlayerController>(get_node_or_null(NodePath("/root/Main/Player")));
    chunk_manager_ = Object::cast_to<VoxelEngine::ChunkManager>(get_node_or_null(NodePath("/root/Main/ChunkManager")));

    create_fill();
    create_materials();
}

void BlockOutline::_process(double delta) {
    if (!player_controller_ || !chunk_manager_) {
        if (outline_mesh_) outline_mesh_->set_visible(false);
        if (fill_mesh_) fill_mesh_->set_visible(false);
        return;
    }

    if (player_controller_->is_chat_open() || player_controller_->is_inventory_open() || player_controller_->is_settings_open()) {
        if (outline_mesh_) outline_mesh_->set_visible(false);
        if (fill_mesh_) fill_mesh_->set_visible(false);
        return;
    }

    auto* camera = Object::cast_to<Camera3D>(player_controller_->get_node_or_null(NodePath("Camera3D")));
    if (!camera) return;

    const Vector3 current_position = camera->get_global_position();
    const Vector3 current_rotation = camera->get_global_rotation();
    const int current_edit_counter = player_controller_->get_block_edit_counter();

    const bool position_changed = current_position.distance_to(last_camera_position_) > POSITION_THRESHOLD;
    const bool rotation_changed =
        std::abs(current_rotation.x - last_camera_rotation_.x) > ROTATION_THRESHOLD ||
        std::abs(current_rotation.y - last_camera_rotation_.y) > ROTATION_THRESHOLD ||
        std::abs(current_rotation.z - last_camera_rotation_.z) > ROTATION_THRESHOLD;
    const bool world_changed = current_edit_counter != last_block_edit_counter_;

    const bool needs_raycast = position_changed || rotation_changed || world_changed ||
                               last_camera_position_ == Vector3();

    if (!needs_raycast) {
        pulse_time_ += static_cast<float>(delta);
        update_materials();
        return;
    }

    last_camera_position_ = current_position;
    last_camera_rotation_ = current_rotation;
    last_block_edit_counter_ = current_edit_counter;

    Dictionary result = chunk_manager_->raycast_from_camera(reach_distance_);
    if (result.is_empty() || !result.get("success", false)) {
        if (outline_mesh_) outline_mesh_->set_visible(false);
        if (fill_mesh_) fill_mesh_->set_visible(false);
        return;
    }

    Variant block_pos_var = result.get("position", Variant());
    Variant place_pos_var = result.get("place_position", Variant());
    if (block_pos_var.get_type() == Variant::NIL || place_pos_var.get_type() == Variant::NIL) {
        if (outline_mesh_) outline_mesh_->set_visible(false);
        if (fill_mesh_) fill_mesh_->set_visible(false);
        return;
    }

    const Vector3 block_pos = block_pos_var;
    const int bx = static_cast<int>(std::floor(block_pos.x));
    const int by = static_cast<int>(std::floor(block_pos.y));
    const int bz = static_cast<int>(std::floor(block_pos.z));

    set_global_position(Vector3(bx, by, bz));

    const int block_id = static_cast<int>(result.get("block_id", 0));
    if (block_id != current_block_id_) {
        current_block_id_ = block_id;
        current_boxes_ = chunk_manager_->get_selection_boxes(block_id);
        rebuild_outline_mesh();
        update_fill_for_boxes();
    } else if (outline_thickness_ != current_thickness_) {
        rebuild_outline_mesh();
    }

    if (outline_mesh_) outline_mesh_->set_visible(outline_enabled_);
    if (fill_mesh_) fill_mesh_->set_visible(fill_enabled_);

    pulse_time_ += static_cast<float>(delta);
    update_materials();
}

void BlockOutline::rebuild_outline_mesh() {
    current_thickness_ = outline_thickness_;
    if (outline_mesh_) {
        outline_mesh_->queue_free();
        outline_mesh_ = nullptr;
    }

    if (current_boxes_.is_empty()) return;

    Dictionary data = BlockOutlineBuilder::build_outline(current_boxes_, outline_thickness_);
    PackedVector3Array verts = data.get("verts", PackedVector3Array());
    if (verts.is_empty()) return;

    Array arrays;
    arrays.resize(Mesh::ARRAY_MAX);
    arrays[Mesh::ARRAY_VERTEX] = verts;
    arrays[Mesh::ARRAY_INDEX] = data.get("indices", PackedInt32Array());

    auto* mesh = memnew(ArrayMesh);
    mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);

    outline_mesh_ = memnew(MeshInstance3D);
    outline_mesh_->set_mesh(Ref<Mesh>(mesh));
    outline_mesh_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    if (outline_material_.is_valid()) {
        outline_mesh_->set_material_override(outline_material_);
    }
    add_child(outline_mesh_);
}

void BlockOutline::create_fill() {
    fill_mesh_ = memnew(MeshInstance3D);
    auto* box_mesh = memnew(BoxMesh);
    box_mesh->set_size(Vector3(1.001, 1.001, 1.001));
    fill_mesh_->set_mesh(Ref<Mesh>(box_mesh));
    fill_mesh_->set_position(Vector3(0.5, 0.5, 0.5));
    fill_mesh_->set_cast_shadows_setting(GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
    fill_mesh_->set_visible(false);
    add_child(fill_mesh_);
}

void BlockOutline::update_fill_for_boxes() {
    if (current_boxes_.is_empty()) return;

    Dictionary data = BlockOutlineBuilder::fill_bounds(current_boxes_);
    const Vector3 center = data.get("center", Vector3());
    const Vector3 size = data.get("size", Vector3());
    const float pad = 0.001f;

    auto* box_mesh = Object::cast_to<BoxMesh>(fill_mesh_->get_mesh().ptr());
    if (box_mesh) {
        box_mesh->set_size(size + Vector3(pad, pad, pad));
    }
    fill_mesh_->set_position(center);
}

void BlockOutline::create_materials() {
    outline_material_.instantiate();
    outline_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    Color oc = outline_color_;
    oc.a = outline_opacity_;
    outline_material_->set_albedo(oc);
    outline_material_->set_cull_mode(BaseMaterial3D::CULL_DISABLED);
    outline_material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
    outline_material_->set_render_priority(10);

    fill_material_.instantiate();
    fill_material_->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
    fill_material_->set_cull_mode(BaseMaterial3D::CULL_BACK);
    fill_material_->set_transparency(BaseMaterial3D::TRANSPARENCY_ALPHA);
    Color fc = fill_color_;
    fc.a = fill_opacity_;
    fill_material_->set_albedo(fc);
    fill_material_->set_render_priority(9);

    if (fill_mesh_ && fill_mesh_->get_mesh().is_valid() && fill_material_.is_valid()) {
        fill_mesh_->get_mesh()->surface_set_material(0, fill_material_);
    }
}

void BlockOutline::update_materials() {
    const float outline_pulse_factor = (std::sin(pulse_time_ * outline_pulse_speed_) + 1.0f) / 2.0f;
    const float fill_pulse_factor = (std::sin(pulse_time_ * fill_pulse_speed_) + 1.0f) / 2.0f;

    float current_outline_opacity = outline_opacity_;
    if (outline_pulse_enabled_) {
        current_outline_opacity = outline_pulse_min_opacity_ +
            (outline_pulse_max_opacity_ - outline_pulse_min_opacity_) * outline_pulse_factor;
    }
    Color oc = outline_color_;
    oc.a = std::clamp(current_outline_opacity, 0.0f, 1.0f);
    if (outline_material_.is_valid()) {
        outline_material_->set_albedo(oc);
    }

    float current_fill_opacity = fill_opacity_;
    if (fill_pulse_enabled_) {
        current_fill_opacity = fill_pulse_min_opacity_ +
            (fill_pulse_max_opacity_ - fill_pulse_min_opacity_) * fill_pulse_factor;
    }
    Color fc = fill_color_;
    fc.a = std::clamp(current_fill_opacity, 0.0f, 1.0f);
    if (fill_material_.is_valid()) {
        fill_material_->set_albedo(fc);
    }
}

void BlockOutline::_bind_methods() {
    // Outline properties
    ClassDB::bind_method(D_METHOD("get_outline_enabled"), &BlockOutline::get_outline_enabled);
    ClassDB::bind_method(D_METHOD("set_outline_enabled", "value"), &BlockOutline::set_outline_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "outline_enabled"), "set_outline_enabled", "get_outline_enabled");

    ClassDB::bind_method(D_METHOD("get_outline_color"), &BlockOutline::get_outline_color);
    ClassDB::bind_method(D_METHOD("set_outline_color", "value"), &BlockOutline::set_outline_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "outline_color"), "set_outline_color", "get_outline_color");

    ClassDB::bind_method(D_METHOD("get_outline_opacity"), &BlockOutline::get_outline_opacity);
    ClassDB::bind_method(D_METHOD("set_outline_opacity", "value"), &BlockOutline::set_outline_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_outline_opacity", "get_outline_opacity");

    ClassDB::bind_method(D_METHOD("get_outline_thickness"), &BlockOutline::get_outline_thickness);
    ClassDB::bind_method(D_METHOD("set_outline_thickness", "value"), &BlockOutline::set_outline_thickness);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_thickness", PROPERTY_HINT_RANGE, "0.0,0.99"), "set_outline_thickness", "get_outline_thickness");

    ClassDB::bind_method(D_METHOD("get_outline_pulse_enabled"), &BlockOutline::get_outline_pulse_enabled);
    ClassDB::bind_method(D_METHOD("set_outline_pulse_enabled", "value"), &BlockOutline::set_outline_pulse_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "outline_pulse_enabled"), "set_outline_pulse_enabled", "get_outline_pulse_enabled");

    ClassDB::bind_method(D_METHOD("get_outline_pulse_speed"), &BlockOutline::get_outline_pulse_speed);
    ClassDB::bind_method(D_METHOD("set_outline_pulse_speed", "value"), &BlockOutline::set_outline_pulse_speed);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_pulse_speed", PROPERTY_HINT_RANGE, "0.5,10.0"), "set_outline_pulse_speed", "get_outline_pulse_speed");

    ClassDB::bind_method(D_METHOD("get_outline_pulse_min_opacity"), &BlockOutline::get_outline_pulse_min_opacity);
    ClassDB::bind_method(D_METHOD("set_outline_pulse_min_opacity", "value"), &BlockOutline::set_outline_pulse_min_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_pulse_min_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_outline_pulse_min_opacity", "get_outline_pulse_min_opacity");

    ClassDB::bind_method(D_METHOD("get_outline_pulse_max_opacity"), &BlockOutline::get_outline_pulse_max_opacity);
    ClassDB::bind_method(D_METHOD("set_outline_pulse_max_opacity", "value"), &BlockOutline::set_outline_pulse_max_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "outline_pulse_max_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_outline_pulse_max_opacity", "get_outline_pulse_max_opacity");

    ClassDB::bind_method(D_METHOD("get_reach_distance"), &BlockOutline::get_reach_distance);
    ClassDB::bind_method(D_METHOD("set_reach_distance", "value"), &BlockOutline::set_reach_distance);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "reach_distance", PROPERTY_HINT_RANGE, "0.0,10.0"), "set_reach_distance", "get_reach_distance");

    // Fill properties
    ClassDB::bind_method(D_METHOD("get_fill_enabled"), &BlockOutline::get_fill_enabled);
    ClassDB::bind_method(D_METHOD("set_fill_enabled", "value"), &BlockOutline::set_fill_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fill_enabled"), "set_fill_enabled", "get_fill_enabled");

    ClassDB::bind_method(D_METHOD("get_fill_color"), &BlockOutline::get_fill_color);
    ClassDB::bind_method(D_METHOD("set_fill_color", "value"), &BlockOutline::set_fill_color);
    ADD_PROPERTY(PropertyInfo(Variant::COLOR, "fill_color"), "set_fill_color", "get_fill_color");

    ClassDB::bind_method(D_METHOD("get_fill_opacity"), &BlockOutline::get_fill_opacity);
    ClassDB::bind_method(D_METHOD("set_fill_opacity", "value"), &BlockOutline::set_fill_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fill_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_fill_opacity", "get_fill_opacity");

    ClassDB::bind_method(D_METHOD("get_fill_pulse_enabled"), &BlockOutline::get_fill_pulse_enabled);
    ClassDB::bind_method(D_METHOD("set_fill_pulse_enabled", "value"), &BlockOutline::set_fill_pulse_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "fill_pulse_enabled"), "set_fill_pulse_enabled", "get_fill_pulse_enabled");

    ClassDB::bind_method(D_METHOD("get_fill_pulse_speed"), &BlockOutline::get_fill_pulse_speed);
    ClassDB::bind_method(D_METHOD("set_fill_pulse_speed", "value"), &BlockOutline::set_fill_pulse_speed);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fill_pulse_speed", PROPERTY_HINT_RANGE, "0.5,10.0"), "set_fill_pulse_speed", "get_fill_pulse_speed");

    ClassDB::bind_method(D_METHOD("get_fill_pulse_min_opacity"), &BlockOutline::get_fill_pulse_min_opacity);
    ClassDB::bind_method(D_METHOD("set_fill_pulse_min_opacity", "value"), &BlockOutline::set_fill_pulse_min_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fill_pulse_min_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_fill_pulse_min_opacity", "get_fill_pulse_min_opacity");

    ClassDB::bind_method(D_METHOD("get_fill_pulse_max_opacity"), &BlockOutline::get_fill_pulse_max_opacity);
    ClassDB::bind_method(D_METHOD("set_fill_pulse_max_opacity", "value"), &BlockOutline::set_fill_pulse_max_opacity);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fill_pulse_max_opacity", PROPERTY_HINT_RANGE, "0.0,1.0"), "set_fill_pulse_max_opacity", "get_fill_pulse_max_opacity");
}
