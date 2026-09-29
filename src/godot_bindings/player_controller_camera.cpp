// What the player is looking at: the aim ray, the pose-clone dummy's hit test and
// punch (vanilla's entity-over-block precedence), the third-person camera transform
// with its clearance walk, the walk/idle animation switch, and teleport_to. Kept
// apart from godot_bindings/player_controller.cpp, whose lifecycle does none of it.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/chunk_manager.hpp"
#include "godot_bindings/player_controller_internal.hpp"
#include "engine/collision_resolver.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/classes/scene_tree.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;
using namespace player_detail;
godot::Vector3 PlayerController::get_aim_origin() const {
    return get_global_position() + godot::Vector3(0.0f, sim_.get_eye_height(), 0.0f);
}

godot::Vector3 PlayerController::get_aim_direction() const {
    // Look direction = player yaw (on this node's basis) applied to the
    // pitch-rotated forward; matches the first-person camera's orientation.
    const godot::Vector3 local =
        godot::Vector3(0, 0, -1).rotated(godot::Vector3(1, 0, 0), pitch_);
    return get_global_transform().basis.xform(local).normalized();
}

float PlayerController::dummy_aim_hit_t() const {
    SceneTree* tree = get_tree();
    if (!tree) return -1.0f;
    Node* dummy = tree->get_first_node_in_group("pose_clone");
    if (!dummy) return -1.0f;
    Node3D* dummy3d = Object::cast_to<Node3D>(dummy);
    if (!dummy3d) return -1.0f;
    const Vector3 pos = dummy3d->get_global_position();
    const Vector3 box_min = pos - Vector3(kDummyHalfWidth, 0.0f, kDummyHalfWidth);
    const Vector3 box_max = pos + Vector3(kDummyHalfWidth, kDummyHeight, kDummyHalfWidth);
    const float t = ray_aabb_hit(get_aim_origin(), get_aim_direction(), box_min, box_max);
    return (t >= 0.0f && t <= kPunchReach) ? t : -1.0f;
}

bool PlayerController::dummy_blocks_break_aim() const {
    const float t = dummy_aim_hit_t();
    if (t < 0.0f) return false;
    // Vanilla entity precedence: the nearer of the entity and the block along
    // the aim ray wins the click. No block hit (or a block farther away than
    // the dummy) means the dummy absorbs it.
    if (!chunk_manager_) return true;
    const Dictionary result = chunk_manager_->raycast_from_camera(10.0);
    if (!result.get("success", false)) return true;
    const Vector3 hit_point = result["hit_point"];
    const float block_dist = (hit_point - get_aim_origin()).length();
    return t <= block_dist;
}

bool PlayerController::try_punch_dummy() {
    if (!dummy_blocks_break_aim()) return false;
    SceneTree* tree = get_tree();
    if (!tree) return false;
    Node* dummy = tree->get_first_node_in_group("pose_clone");
    if (!dummy) return false;
    // First-person arm swing (viewmodel.gd punch()).
    Node* viewmodel = get_node_or_null(NodePath("Camera3D/Viewmodel"));
    if (viewmodel) viewmodel->call("punch");
    // Base vanilla 1.8.8 knockback — the dummy computes the direction and
    // velocity from its own and the attacker's positions.
    // Sprint bonus (vanilla 1.8.8): a sprinting attacker adds facing * 0.5
    // (+0.1 up) on top — the victim inherits the attacker's forward momentum.
    // Uses the sim's sprint state (sprint key + forward + grounded).
    Vector3 extra;
    if (sim_.get_state() == MoveState::SPRINTING) {
        Vector3 fwd = get_aim_direction();
        fwd.y = 0.0f;
        if (fwd.length_squared() > 0.001f) fwd = fwd.normalized();
        extra = fwd * 0.5f + Vector3(0.0f, 0.1f, 0.0f);
    }
    dummy->call("apply_knockback", get_global_position(), extra);
    return true;
}

void PlayerController::update_camera_transform(float eye_height, float delta) {
    if (!camera_) return;
    const Vector3 local_forward = Vector3(0, 0, -1).rotated(Vector3(1, 0, 0), pitch_);

    if (third_person_view_ == 0) {
        // First person: steady eye-height blend, no walk bob.
        const float target = eye_height;
        rendered_eye_height_ += (target - rendered_eye_height_)
            * static_cast<float>(1.0 - std::pow(0.0001, delta));
        camera_->set_position(Vector3(0, rendered_eye_height_, 0));
        camera_->set_rotation(Vector3(pitch_, 0.0f, 0.0f));
        return;
    }

    // Third person (back or front view): the camera sits kThirdPersonOffset
    // along the look direction from the eye, pulled in before any solid block
    // so it never clips through terrain. Back view looks forward past the
    // player's back; front view mirrors the offset (in front of the player,
    // looking back at them), matching vanilla's thirdPersonView 1 and 2.
    const float dir_sign = (third_person_view_ == 1) ? -1.0f : 1.0f;
    const Transform3D g = get_global_transform();
    const Vector3 eye_world = g.xform(Vector3(0.0f, eye_height, 0.0f));
    const Vector3 dir_world = g.basis.xform(local_forward).normalized() * dir_sign;
    const float dist = camera_clear_distance(eye_world, dir_world, kThirdPersonOffset);
    camera_->set_global_position(eye_world + dir_world * dist);

    // vanilla's third-person camera uses the player's look rotation directly
    // (the front view adds 180 to the pitch), NOT an aim-at-player direction:
    // the camera sits exactly on the look ray, so its forward is parallel to
    // the eye-ray that block targeting uses and the crosshair/outline always
    // agree with first person. Aim-derived rotations were also the cause of
    // the near-vertical "lock": looking straight down/up the horizontal
    // component of the aim direction vanishes, so the camera's yaw got driven
    // by the body's lagging yaw instead of the mouse.
    if (third_person_view_ == 1) {
        camera_->set_rotation(Vector3(pitch_, 0.0f, 0.0f));
    } else {
        camera_->set_rotation(Vector3(-pitch_, kPi, 0.0f));
    }
}

float PlayerController::camera_clear_distance(const Vector3& eye_world, const Vector3& dir,
                                              float max_dist) const {
    if (!collision_resolver_) return max_dist;
    float t = kCameraMinDist;
    while (t < max_dist) {
        const Vector3 p = eye_world + dir * t;
        if (collision_resolver_->is_solid_at(static_cast<int32_t>(std::floor(p.x)),
                                             static_cast<int32_t>(std::floor(p.y)),
                                             static_cast<int32_t>(std::floor(p.z)))) {
            return std::max(t - kCameraStep, kCameraMinDist);
        }
        t += kCameraStep;
    }
    return max_dist;
}

void PlayerController::update_player_animation(bool is_walking) {
    if (model_) {
        // Call the GDScript method on the PlayerModel node
        model_->call("set_animation_state", is_walking);
    }
}

void PlayerController::teleport_to(const Vector3& pos) {
    set_global_position(pos);
    sim_.reset(pos);
    rendered_eye_height_ = 1.62f;
}
