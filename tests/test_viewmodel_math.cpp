#include "core/viewmodel_math.hpp"

#include <doctest.h>
#include <cmath>

using namespace VoxelEngine;

namespace {
constexpr double kPi = 3.14159265358979323846;

bool near(double a, double b, double eps = 1e-9) {
    return std::abs(a - b) <= eps;
}
} // namespace

TEST_CASE("viewmodel_math smoothstep_01") {
    // Cubic smoothstep: 0 -> 1 with zero slope at both endpoints.
    CHECK(smoothstep_01(-1.0) == 0.0);
    CHECK(smoothstep_01(0.0) == 0.0);
    CHECK(smoothstep_01(0.5) == 0.5);
    CHECK(near(smoothstep_01(0.25), 0.15625));
    CHECK(smoothstep_01(1.0) == 1.0);
    CHECK(smoothstep_01(2.0) == 1.0);
}

TEST_CASE("viewmodel_math step_walk_bob idle") {
    // Idle: no movement -> bob stays 0, offsets zero.
    const double pos[3] = {10.0, 5.0, 20.0};
    const double last[3] = {10.0, 5.0, 20.0};
    BobResult r = step_walk_bob(0.016, pos, last, true, true, 0.0, 0.0);
    CHECK(near(r.walk_dist, 0.0));
    CHECK(near(r.bob, 0.0));
    CHECK(near(r.offset[0], 0.0));
    CHECK(near(r.offset[1], 0.0));
    CHECK(near(r.offset[2], 0.0));
    CHECK(r.last_pos[0] == 10.0);
    CHECK(r.last_pos[2] == 20.0);
    CHECK(r.has_last_pos);
}

TEST_CASE("viewmodel_math step_walk_bob accumulates distance") {
    // Move 2 blocks horizontally -> walk_dist accumulates 2, bob ramps up.
    const double pos[3] = {10.0, 5.0, 22.0};
    const double last[3] = {10.0, 5.0, 20.0};
    BobResult r = step_walk_bob(0.016, pos, last, true, true, 0.0, 0.0);
    // Horizontal distance = z delta = 2 (only x/z considered).
    CHECK(near(r.walk_dist, 2.0, 1e-6));
    // 2 / 0.016 = 125 blocks/s -> target_bob clamps to 1.0; exponential lerp ~18%.
    CHECK(r.bob > 0.0);
    CHECK(r.bob < 1.0);
    // Bob offsets are within their bounded ranges.
    CHECK(std::abs(r.offset[0]) <= 0.05);
    CHECK(r.offset[1] <= 0.0);
}

TEST_CASE("viewmodel_math step_walk_bob airborne settles") {
    // While airborne (grounded=false), target_bob = 0 so bob decays toward 0.
    const double pos[3] = {10.0, 8.0, 22.0};
    const double last[3] = {10.0, 5.0, 20.0};
    BobResult r = step_walk_bob(0.016, pos, last, true, false, 0.0, 1.0);
    CHECK(r.bob < 1.0); // decayed toward 0
    CHECK(r.bob >= 0.0);
}

TEST_CASE("viewmodel_math step_sway clamps and lerps") {
    // Large mouse delta clamps to the sway bounds.
    double sway[3] = {0.0, 0.0, 0.0};
    // delta small -> target pulled mostly back toward 0 (big t when delta large).
    // Use a large delta to approach the target closely.
    double out[3] = {0.0, 0.0, 0.0};
    step_sway(1.0, 500.0, 500.0, sway, out); // delta=1 -> t ~= 1 - e^-12 ~= 1
    // x from -delta.y clamp: y-mouse_delta 500 -> clamp(500*0.018,-0.27,0.27)=0.27
    CHECK(near(out[0], 0.27, 1e-4));
    // y from +delta.x: clamp(500*0.0135,-0.22,0.22)=0.22
    CHECK(near(out[1], 0.22, 1e-4));
    // z from -delta.x: clamp(-500*0.006,-0.10,0.10)=-0.10
    CHECK(near(out[2], -0.10, 1e-4));
}

TEST_CASE("viewmodel_math step_sway small delta stays near origin") {
    // Tiny delta -> tiny lerp factor, sway barely moves from origin.
    double sway[3] = {0.0, 0.0, 0.0};
    double out[3] = {0.0, 0.0, 0.0};
    step_sway(0.001, 100.0, 100.0, sway, out);
    // t = 1 - e^(-12*0.001) ~ 0.0119. target x = clamp(100*0.018)=0.27.
    // out[0] ~ 0.27*0.0119 ~ 0.0032.
    CHECK(out[0] < 0.01);
}

TEST_CASE("viewmodel_math compute_swing_pose at rest") {
    // swing=0, swing_place=0, equip=0, is_swapping=false.
    const double bob[3] = {0.0, 0.0, 0.0};
    const double peak_rot[3] = {0.0, 58.0, -10.0};
    const double peak_pos[3] = {0.19, 0.26, -0.75};
    SwingPose pose;
    compute_swing_pose(0.0, 0.0, 1.0, 0.0, false,
                       5.0, -13.0, 5.0, 0.67, -0.01, -0.75, bob, peak_rot, peak_pos, pose);
    // swing_progress = 0 -> s_raw = 0 -> s = 0.
    CHECK(near(pose.swing_s, 0.0));
    // Equip offset at equip=0, non-swap: -0.75 + 0 = -0.75.
    CHECK(near(pose.hand_bob_pos[1], -0.75));
    CHECK(near(pose.hand_bob_pos[0], 0.0));
    // arm_rot = rest (lerp to peak with s=0).
    CHECK(near(pose.arm_rot[0], 5.0));
    CHECK(near(pose.arm_rot[1], -13.0));
    CHECK(near(pose.arm_rot[2], 5.0));
    // arm_pos = straight (s=0 -> rest) + perp*sin(angle). At swing_progress=0,
    // angle = (1-0)*TAU = TAU -> sin(TAU)=0 -> arc=0, so arm_pos = rest pos.
    CHECK(near(pose.arm_pos[0], 0.67));
    CHECK(near(pose.arm_pos[1], -0.01));
    CHECK(near(pose.arm_pos[2], -0.75));
    // swing_node_pos = rest pos.
    CHECK(near(pose.swing_node_pos[0], 0.67));
    CHECK(near(pose.swing_node_pos[1], -0.01));
    CHECK(near(pose.swing_node_pos[2], -0.75));
}

TEST_CASE("viewmodel_math compute_swing_pose peak") {
    // swing=1 (progress 0? No: swing is 1->0, so swing=1 means just started).
    // At swing_progress=1: s_raw = sin(PI) = 0, so s=0 too. Use swing=0.5 (progress
    // 0.5) to reach the peak: s_raw = sin(0.5*PI) = 1, s = smoothstep(1)*1 = 1.
    const double bob[3] = {0.0, 0.0, 0.0};
    const double peak_rot[3] = {0.0, 58.0, -10.0};
    const double peak_pos[3] = {0.19, 0.26, -0.75};
    SwingPose pose;
    compute_swing_pose(0.5, 0.0, 1.0, 0.0, false,
                       5.0, -13.0, 5.0, 0.67, -0.01, -0.75, bob, peak_rot, peak_pos, pose);
    CHECK(near(pose.swing_s, 1.0));
    // arm_rot reaches peak at s=1.
    CHECK(near(pose.arm_rot[0], 0.0));
    CHECK(near(pose.arm_rot[1], 58.0));
    CHECK(near(pose.arm_rot[2], -10.0));
    // arm_pos: straight = peak at s=1; arc = sin(angle)*0.15 where angle=(1-0.5)*TAU=PI
    // -> sin(PI)=0, so arc=0 and arm_pos = peak_pos exactly.
    CHECK(near(pose.arm_pos[0], 0.19));
    CHECK(near(pose.arm_pos[1], 0.26));
    CHECK(near(pose.arm_pos[2], -0.75));
}

TEST_CASE("viewmodel_math compute_swing_pose swapping") {
    // is_swapping=true with equip<0.5: unequip -0.25 - 0.5*progress.
    const double bob[3] = {0.0, 0.0, 0.0};
    const double peak_rot[3] = {0.0, 58.0, -10.0};
    const double peak_pos[3] = {0.19, 0.26, -0.75};
    // equip=0.25 (<0.5): unequip_progress=0.5, offset=-0.25-0.25=-0.5.
    SwingPose pose;
    compute_swing_pose(0.0, 0.0, 1.0, 0.25, true,
                       5.0, -13.0, 5.0, 0.67, -0.01, -0.75, bob, peak_rot, peak_pos, pose);
    CHECK(near(pose.hand_bob_pos[1], -0.5));

    // equip=0.75 (>=0.5): equip_progress=0.5, offset=-0.75+0.25=-0.5.
    SwingPose pose2;
    compute_swing_pose(0.0, 0.0, 1.0, 0.75, true,
                       5.0, -13.0, 5.0, 0.67, -0.01, -0.75, bob, peak_rot, peak_pos, pose2);
    CHECK(near(pose2.hand_bob_pos[1], -0.5));
}

TEST_CASE("viewmodel_math compute_swing_transform") {
    // Item-style: rest rot = (0,-692,422), rest pos = (0,0.09,0.46), peak at s=0.5.
    const double peak_pos[3] = {-0.66, -0.03, -0.16};
    const double peak_rot[3] = {-20.0, -660.0, 372.0};
    double pos[3] = {0.0, 0.0, 0.0};
    double rot[3] = {0.0, 0.0, 0.0};
    compute_swing_transform(1.0, kPi, 0.0, 0.09, 0.46, 0.0, -692.0, 422.0, peak_pos, peak_rot, pos, rot);
    // At s=1, straight = peak; arc = sin(PI)*0.15 = 0 -> exactly peak.
    CHECK(near(pos[0], -0.66));
    CHECK(near(pos[1], -0.03));
    CHECK(near(pos[2], -0.16));
    CHECK(near(rot[0], -20.0));
    CHECK(near(rot[1], -660.0));
    CHECK(near(rot[2], 372.0));

    // At s=0 -> rest exactly.
    compute_swing_transform(0.0, 0.0, 0.0, 0.09, 0.46, 0.0, -692.0, 422.0, peak_pos, peak_rot, pos, rot);
    CHECK(near(pos[1], 0.09));
    CHECK(near(rot[1], -692.0));
}
