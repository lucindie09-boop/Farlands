#include "godot_bindings/viewmodel_pose.hpp"

#include "core/viewmodel_math.hpp"

#include <godot_cpp/core/class_db.hpp>

using namespace godot;
using namespace VoxelEngine;

Dictionary ViewmodelPose::step_walk_bob(double delta, const Vector3& player_pos,
                                        const Vector3& last_pos, bool has_last_pos,
                                        bool grounded, double walk_dist, double bob) {
    const double pp[3] = {player_pos.x, player_pos.y, player_pos.z};
    const double lp[3] = {last_pos.x, last_pos.y, last_pos.z};
    BobResult r = VoxelEngine::step_walk_bob(delta, pp, lp, has_last_pos, grounded, walk_dist, bob);

    Dictionary d;
    d["walk_dist"] = r.walk_dist;
    d["bob"] = r.bob;
    d["bob_offset"] = Vector3(r.offset[0], r.offset[1], r.offset[2]);
    d["bob_rotation"] = Vector3(r.rotation[0], r.rotation[1], r.rotation[2]);
    d["last_pos"] = Vector3(r.last_pos[0], r.last_pos[1], r.last_pos[2]);
    d["has_last_pos"] = r.has_last_pos;
    return d;
}

Vector3 ViewmodelPose::step_sway(double delta, const Vector2& mouse_delta,
                                 const Vector3& item_sway) {
    double cur[3] = {item_sway.x, item_sway.y, item_sway.z};
    double out[3] = {0.0, 0.0, 0.0};
    VoxelEngine::step_sway(delta, mouse_delta.x, mouse_delta.y, cur, out);
    return Vector3(out[0], out[1], out[2]);
}

Dictionary ViewmodelPose::compute_swing_pose(double swing, double swing_place,
                                             double swing_strength, double equip,
                                             bool is_swapping,
                                             double rot_x, double rot_y, double rot_z,
                                             double pos_x, double pos_y, double pos_z,
                                             const Vector3& bob_offset, const Vector3& peak_rot,
                                             const Vector3& peak_pos) {
    const double bob[3] = {bob_offset.x, bob_offset.y, bob_offset.z};
    const double prot[3] = {peak_rot.x, peak_rot.y, peak_rot.z};
    const double ppos[3] = {peak_pos.x, peak_pos.y, peak_pos.z};
    SwingPose pose;
    VoxelEngine::compute_swing_pose(swing, swing_place, swing_strength, equip, is_swapping,
                                    rot_x, rot_y, rot_z, pos_x, pos_y, pos_z, bob, prot, ppos, pose);

    Dictionary d;
    d["hand_bob_pos"] = Vector3(pose.hand_bob_pos[0], pose.hand_bob_pos[1], pose.hand_bob_pos[2]);
    d["arm_rot"] = Vector3(pose.arm_rot[0], pose.arm_rot[1], pose.arm_rot[2]);
    d["arm_pos"] = Vector3(pose.arm_pos[0], pose.arm_pos[1], pose.arm_pos[2]);
    d["swing_s"] = pose.swing_s;
    d["swing_angle"] = pose.swing_angle;
    d["swing_node_pos"] = Vector3(pose.swing_node_pos[0], pose.swing_node_pos[1], pose.swing_node_pos[2]);
    return d;
}

Dictionary ViewmodelPose::compute_swing_transform(double s, double angle,
                                                  double rest_pos_x, double rest_pos_y,
                                                  double rest_pos_z,
                                                  double rest_rot_x, double rest_rot_y,
                                                  double rest_rot_z,
                                                  const Vector3& peak_pos,
                                                  const Vector3& peak_rot) {
    const double ppos[3] = {peak_pos.x, peak_pos.y, peak_pos.z};
    const double prot[3] = {peak_rot.x, peak_rot.y, peak_rot.z};
    double out_pos[3] = {0.0, 0.0, 0.0};
    double out_rot[3] = {0.0, 0.0, 0.0};
    VoxelEngine::compute_swing_transform(s, angle, rest_pos_x, rest_pos_y, rest_pos_z,
                                         rest_rot_x, rest_rot_y, rest_rot_z, ppos, prot, out_pos, out_rot);

    Dictionary d;
    d["position"] = Vector3(out_pos[0], out_pos[1], out_pos[2]);
    d["rotation_degrees"] = Vector3(out_rot[0], out_rot[1], out_rot[2]);
    return d;
}

double ViewmodelPose::smoothstep_01(double x) {
    return VoxelEngine::smoothstep_01(x);
}

void ViewmodelPose::_bind_methods() {
    ClassDB::bind_static_method("ViewmodelPose", D_METHOD("step_walk_bob", "delta", "player_pos", "last_pos", "has_last_pos", "grounded", "walk_dist", "bob"), &ViewmodelPose::step_walk_bob);
    ClassDB::bind_static_method("ViewmodelPose", D_METHOD("step_sway", "delta", "mouse_delta", "item_sway"), &ViewmodelPose::step_sway);
    ClassDB::bind_static_method("ViewmodelPose", D_METHOD("compute_swing_pose", "swing", "swing_place", "swing_strength", "equip", "is_swapping", "rot_x", "rot_y", "rot_z", "pos_x", "pos_y", "pos_z", "bob_offset", "peak_rot", "peak_pos"), &ViewmodelPose::compute_swing_pose);
    ClassDB::bind_static_method("ViewmodelPose", D_METHOD("compute_swing_transform", "s", "angle", "rest_pos_x", "rest_pos_y", "rest_pos_z", "rest_rot_x", "rest_rot_y", "rest_rot_z", "peak_pos", "peak_rot"), &ViewmodelPose::compute_swing_transform);
    ClassDB::bind_static_method("ViewmodelPose", D_METHOD("smoothstep_01", "x"), &ViewmodelPose::smoothstep_01);
}
