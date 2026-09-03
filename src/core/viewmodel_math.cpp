#include "core/viewmodel_math.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kTau = 2.0 * kPi;
} // namespace

double smoothstep_01(double x) {
    const double t = std::clamp(x, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

BobResult step_walk_bob(double delta, const double player_pos[3], const double last_pos[3],
                        bool has_last_pos, bool grounded, double walk_dist, double bob) {
    BobResult r;
    r.walk_dist = walk_dist;
    r.bob = bob;
    for (int i = 0; i < 3; ++i) r.last_pos[i] = player_pos[i];

    double horiz[2] = {0.0, 0.0};
    if (has_last_pos) {
        horiz[0] = player_pos[0] - last_pos[0];
        horiz[1] = player_pos[2] - last_pos[2];
        const double dist = std::sqrt(horiz[0] * horiz[0] + horiz[1] * horiz[1]);
        r.walk_dist += dist;
        const double speed = dist / std::max(delta, 0.0001);
        double target_bob = 0.0;
        if (grounded) {
            target_bob = std::clamp(speed / 4.3, 0.0, 1.0);
        }
        r.bob = bob + (target_bob - bob) * (1.0 - std::exp(-10.0 * delta));
    }

    const double bob_radians = r.walk_dist * kPi * 0.6;
    r.offset[0] = std::sin(bob_radians) * r.bob * 0.5 * 0.1;
    r.offset[1] = -std::abs(std::cos(bob_radians) * r.bob) * 0.1;
    r.offset[2] = 0.0;

    r.rotation[0] = std::cos(bob_radians - 0.2) * r.bob * 5.0 * 0.1;
    r.rotation[1] = 0.0;
    r.rotation[2] = std::sin(bob_radians) * r.bob * 3.0 * 0.1;
    return r;
}

void step_sway(double delta, double mouse_delta_x, double mouse_delta_y,
               double item_sway[3], double out[3]) {
    const double tx = std::clamp(mouse_delta_y * 0.018, -0.27, 0.27);
    const double ty = std::clamp(mouse_delta_x * 0.0135, -0.22, 0.22);
    const double tz = std::clamp(-mouse_delta_x * 0.006, -0.10, 0.10);
    const double t = 1.0 - std::exp(-12.0 * delta);
    out[0] = item_sway[0] + (tx - item_sway[0]) * t;
    out[1] = item_sway[1] + (ty - item_sway[1]) * t;
    out[2] = item_sway[2] + (tz - item_sway[2]) * t;
}

void compute_swing_pose(double swing, double swing_place, double swing_strength,
                        double equip, bool is_swapping,
                        double rot_x, double rot_y, double rot_z,
                        double pos_x, double pos_y, double pos_z,
                        const double bob_offset[3],
                        const double peak_rot[3], const double peak_pos[3],
                        SwingPose& out) {
    const double swing_progress = std::max(swing, swing_place); // 1 -> 0

    // Equip offset.
    double equip_offset;
    if (is_swapping) {
        if (equip < 0.5) {
            const double unequip_progress = equip * 2.0;
            equip_offset = -0.25 - 0.5 * unequip_progress;
        } else {
            const double equip_progress = (equip - 0.5) * 2.0;
            equip_offset = -0.75 + 0.5 * equip_progress;
        }
    } else {
        equip_offset = -0.75 + 0.5 * equip;
    }

    out.hand_bob_pos[0] = bob_offset[0];
    out.hand_bob_pos[1] = equip_offset + bob_offset[1];
    out.hand_bob_pos[2] = bob_offset[2];

    const double s_raw = std::sin(swing_progress * kPi);
    const double s = smoothstep_01(s_raw) * swing_strength;
    const double angle = (1.0 - swing_progress) * kTau;

    out.arm_rot[0] = rot_x + (peak_rot[0] - rot_x) * s;
    out.arm_rot[1] = rot_y + (peak_rot[1] - rot_y) * s;
    out.arm_rot[2] = rot_z + (peak_rot[2] - rot_z) * s;

    // straight = rest.lerp(peak_pos, s)
    const double rest[3] = {pos_x, pos_y, pos_z};
    double straight[3] = {
        rest[0] + (peak_pos[0] - rest[0]) * s,
        rest[1] + (peak_pos[1] - rest[1]) * s,
        rest[2] + (peak_pos[2] - rest[2]) * s,
    };

    // perp = Vector3(-dir.z, 0.2, dir.x).normalized(); dir = peak_pos - rest_pos.
    double dir[3] = {peak_pos[0] - rest[0], peak_pos[1] - rest[1], peak_pos[2] - rest[2]};
    double perp[3] = {-dir[2], 0.2, dir[0]};
    {
        const double len = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
        if (len > 1e-12) {
            perp[0] /= len; perp[1] /= len; perp[2] /= len;
        }
    }
    const double arc = std::sin(angle) * 0.15;
    out.arm_pos[0] = straight[0] + perp[0] * arc;
    out.arm_pos[1] = straight[1] + perp[1] * arc;
    out.arm_pos[2] = straight[2] + perp[2] * arc;

    // Broadcast.
    out.swing_s = s;
    out.swing_angle = angle;

    // swing_node.position = rest_pos, rotation zero.
    out.swing_node_pos[0] = pos_x;
    out.swing_node_pos[1] = pos_y;
    out.swing_node_pos[2] = pos_z;
}

void compute_swing_transform(double s, double angle,
                             double rest_pos_x, double rest_pos_y, double rest_pos_z,
                             double rest_rot_x, double rest_rot_y, double rest_rot_z,
                             const double peak_pos[3], const double peak_rot[3],
                             double out_pos[3], double out_rot[3]) {
    const double rest[3] = {rest_pos_x, rest_pos_y, rest_pos_z};
    double dir[3] = {peak_pos[0] - rest[0], peak_pos[1] - rest[1], peak_pos[2] - rest[2]};
    double perp[3] = {-dir[2], 0.2, dir[0]};
    {
        const double len = std::sqrt(perp[0] * perp[0] + perp[1] * perp[1] + perp[2] * perp[2]);
        if (len > 1e-12) {
            perp[0] /= len; perp[1] /= len; perp[2] /= len;
        }
    }
    const double arc = std::sin(angle) * 0.15;
    const double straight[3] = {
        rest[0] + (peak_pos[0] - rest[0]) * s,
        rest[1] + (peak_pos[1] - rest[1]) * s,
        rest[2] + (peak_pos[2] - rest[2]) * s,
    };
    out_pos[0] = straight[0] + perp[0] * arc;
    out_pos[1] = straight[1] + perp[1] * arc;
    out_pos[2] = straight[2] + perp[2] * arc;

    out_rot[0] = rest_rot_x + (peak_rot[0] - rest_rot_x) * s;
    out_rot[1] = rest_rot_y + (peak_rot[1] - rest_rot_y) * s;
    out_rot[2] = rest_rot_z + (peak_rot[2] - rest_rot_z) * s;
}

} // namespace VoxelEngine
