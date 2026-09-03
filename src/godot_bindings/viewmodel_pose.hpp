#ifndef FARLANDS_GODOT_BINDINGS_VIEWMODEL_POSE_HPP
#define FARLANDS_GODOT_BINDINGS_VIEWMODEL_POSE_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

// Static helpers that push viewmodel.gd's per-frame animation math (walk bob,
// mouse sway, punch/place swing curves, equip animation, held item/block swing
// transform) into C++. Each method is a pure function of its inputs and returns
// the pose fields the GDScript glue applies to its node tree, so the swing/bob
// curves can't drift from the original.
//
// Peak poses (PEAK_ROT/PEAK_POS, PEAK_ROT_ITEM/PEAK_POS_ITEM,
// PEAK_ROT_BLOCK/PEAK_POS_BLOCK) are passed in as Vector3 pairs so the script
// keeps the F12-HUD-tuned constants as its single source of truth.
class ViewmodelPose : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(ViewmodelPose, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Walk bob. Returns {walk_dist, bob, bob_offset, bob_rotation, last_pos, has_last_pos}.
    static godot::Dictionary step_walk_bob(double delta, const godot::Vector3& player_pos,
                                           const godot::Vector3& last_pos, bool has_last_pos,
                                           bool grounded, double walk_dist, double bob);

    // Mouse-look sway lag. Returns the new smoothed sway offset Vector3.
    static godot::Vector3 step_sway(double delta, const godot::Vector2& mouse_delta,
                                    const godot::Vector3& item_sway);

    // Equip/swing pose. Returns {hand_bob_pos, arm_rot, arm_pos, swing_s, swing_angle, swing_node_pos}.
    static godot::Dictionary compute_swing_pose(double swing, double swing_place,
                                                double swing_strength, double equip,
                                                bool is_swapping,
                                                double rot_x, double rot_y, double rot_z,
                                                double pos_x, double pos_y, double pos_z,
                                                const godot::Vector3& bob_offset,
                                                const godot::Vector3& peak_rot,
                                                const godot::Vector3& peak_pos);

    // Held item/block swing transform. Returns {position, rotation_degrees}.
    static godot::Dictionary compute_swing_transform(double s, double angle,
                                                     double rest_pos_x, double rest_pos_y,
                                                     double rest_pos_z,
                                                     double rest_rot_x, double rest_rot_y,
                                                     double rest_rot_z,
                                                     const godot::Vector3& peak_pos,
                                                     const godot::Vector3& peak_rot);

    // Cubic smoothstep 0->1.
    static double smoothstep_01(double x);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_VIEWMODEL_POSE_HPP
