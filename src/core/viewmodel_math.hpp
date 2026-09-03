#ifndef FARLANDS_CORE_VIEWMODEL_MATH_HPP
#define FARLANDS_CORE_VIEWMODEL_MATH_HPP

// Pure viewmodel per-frame math that used to live in viewmodel.gd's _process /
// _update_swing_hooks / _update_arm_animation / _update_item_transform /
// _update_block_transform. Everything here is a function of its inputs; the
// GDScript node glue feeds per-frame state in and applies the returned pose to
// its node tree. This keeps the swing/bob/sway curves byte-identical to the
// GDScript while moving the actual computation into tested C++.
namespace VoxelEngine {

// Walk-bob (vanilla bobView). Given the previous walk-tracking state and the
// player's current position, returns the next walk_dist/bob envelope plus the
// hand offset and rotation. Mirrors the GDScript _process bob block.
struct BobResult {
    double walk_dist = 0.0;
    double bob = 0.0;
    // Hand container offset/rotation expressed as x/y/z triples.
    double offset[3] = {0.0, 0.0, 0.0};
    double rotation[3] = {0.0, 0.0, 0.0};
    double last_pos[3] = {0.0, 0.0, 0.0};
    bool has_last_pos = true;
};

BobResult step_walk_bob(double delta,
                        const double player_pos[3],
                        const double last_pos[3],
                        bool has_last_pos,
                        bool grounded,
                        double walk_dist,
                        double bob);

// Mouse-look sway lag. Returns the new smoothed sway offset (x/y/z).
// out[3] receives the result.
void step_sway(double delta, double mouse_delta_x, double mouse_delta_y,
               double item_sway[3], double out[3]);

// The full swing/equip pose computation (previously _update_swing_hooks +
// _update_arm_animation). Produces the hand-bob container position, the arm
// rotation degrees and shoulder position, and the broadcast swing s/angle used
// to drive the held item/block swing.
struct SwingPose {
    // hand_bob.position = (bob_offset.x, equip_offset + bob_offset.y, bob_offset.z).
    double hand_bob_pos[3] = {0.0, 0.0, 0.0};
    // Arm rest rotation in degrees -> current (swang) rotation degrees.
    double arm_rot[3] = {0.0, 0.0, 0.0};
    // Shoulder position (current_pos = straight + arc_offset).
    double arm_pos[3] = {0.0, 0.0, 0.0};
    // Broadcast s (0..1) and angle (0..TAU) for item/block swing.
    double swing_s = 0.0;
    double swing_angle = 0.0;
    // swing_node position = rest_pos, rotation_degrees = zero.
    double swing_node_pos[3] = {0.0, 0.0, 0.0};
};

void compute_swing_pose(double swing, double swing_place, double swing_strength,
                        double equip, bool is_swapping,
                        double rot_x, double rot_y, double rot_z,
                        double pos_x, double pos_y, double pos_z,
                        const double bob_offset[3],
                        const double peak_rot[3], const double peak_pos[3],
                        SwingPose& out);

// Held item / block swing transform (previously _update_item_transform and
// _update_block_transform). Interpolates rest -> peak on the 's' curve plus the
// two-sided perpendicular arc. out receives position/rotation degrees/scale.
struct SwingTransform {
    double position[3] = {0.0, 0.0, 0.0};
    double rotation_deg[3] = {0.0, 0.0, 0.0};
};

void compute_swing_transform(double s, double angle,
                             double rest_pos_x, double rest_pos_y, double rest_pos_z,
                             double rest_rot_x, double rest_rot_y, double rest_rot_z,
                             const double peak_pos[3], const double peak_rot[3],
                             double out_pos[3], double out_rot[3]);

// cubic smoothstep 0->1 (previously smoothstep_01 in GDScript).
double smoothstep_01(double x);

} // namespace VoxelEngine

#endif // FARLANDS_CORE_VIEWMODEL_MATH_HPP
