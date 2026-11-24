// Breaking and using: the hold-to-break progress and its crack state, the punch
// cooldown's mining half, item use, and the held light. Kept apart from
// godot_bindings/player_controller.cpp so a change to the break curve is one file.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/chunk_manager.hpp"
#include "godot_bindings/player_controller_internal.hpp"
#include "core/item_registry.hpp"
#include "core/mining.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <algorithm>
#include <cmath>

using namespace godot;
using namespace VoxelEngine;
using namespace player_detail;
void PlayerController::break_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (result.get("success", false)) {
        Vector3 pos = result["position"];
        int bx = static_cast<int>(std::floor(pos.x));
        int by = static_cast<int>(std::floor(pos.y));
        int bz = static_cast<int>(std::floor(pos.z));
        
        // Get the block type before breaking
        int block_type = cm->get_block(bx, by, bz);

        // What this break yields: variant collapse plus the hammer's crush.
        // resolve_block_drop is also what the hold-to-break gate above checks,
        // so the gate and the collect cannot disagree about the drop.
        const VoxelEngine::BlockDrop drop = VoxelEngine::resolve_block_drop(
            static_cast<BlockID>(block_type), inventory_.get_selected_block());

        // Only break if we can add it to inventory (and it's not air)
        if (block_type != 0 && inventory_.can_add_block(drop.id, drop.count)) {
            // Break the block
            cm->set_block(bx, by, bz, 0);

            // Add to inventory
            inventory_.add_block(drop.id, drop.count);

            // Increment edit counter to invalidate block outline
            block_edit_counter_++;

            // The chips have a listener of their own (scripts/block_break_particles.gd);
            // the id is the block that went in, not the drop it yielded, so the
            // debris wears the texture of the block the player was looking at.
            emit_signal("block_broken", block_type, Vector3i(bx, by, bz));
        }
    }
}

bool PlayerController::wand_held() const {
    const VoxelEngine::ItemUseAction* use =
        VoxelEngine::ItemRegistry::get_instance().get_item_use(inventory_.get_selected_block());
    return use != nullptr && use->is_wand();
}

void PlayerController::update_break_progress(float delta) {
    if (!chunk_manager_) {
        break_target_valid_ = false;
        break_progress_ = 0.0f;
        return;
    }

    // A wand is not a pickaxe: nothing accumulates while one is held, so
    // right-clicking a build file into place cannot chew a hole in whatever is
    // behind it at the same time.
    if (wand_held()) {
        break_target_valid_ = false;
        break_progress_ = 0.0f;
        return;
    }

    Input* input = Input::get_singleton();
    const bool mouse_captured = input && input->get_mouse_mode() == Input::MOUSE_MODE_CAPTURED;
    const bool held = input && input->is_action_pressed("mouse_click_left");
    const bool ui_blocked = inventory_open_ || table_menu_open_ || chat_open_ || settings_open_ || wand_menu_open_;

    // When the dummy is under the crosshair (closer than the block), LMB keeps
    // punching it at the vanilla swing interval instead of mining (vanilla: the
    // entity absorbs the click). Also gates the re-aim below so the crack
    // overlay and break_block never target the block behind the dummy.
    const bool punch_targeted =
        held && mouse_captured && !ui_blocked && dummy_blocks_break_aim();
    if (punch_targeted) {
        punch_cooldown_ -= delta;
        if (punch_cooldown_ <= 0.0f) {
            try_punch_dummy();
            punch_cooldown_ = kPunchInterval;
        }
    } else {
        punch_cooldown_ = 0.0f;
    }

    // Re-aim while LMB is held with the mouse captured and no UI open.
    bool aiming = false;
    Vector3i target;
    float hardness = -1.0f;
    BlockID collect_id = 0;
    int collect_count = 1;
    int block_type = 0;
    if (held && mouse_captured && !ui_blocked && !punch_targeted) {
        Dictionary result = chunk_manager_->raycast_from_camera(10.0);
        if (result.get("success", false)) {
            Vector3 pos = result["position"];
            target = Vector3i(static_cast<int>(std::floor(pos.x)),
                              static_cast<int>(std::floor(pos.y)),
                              static_cast<int>(std::floor(pos.z)));
            block_type = chunk_manager_->get_block(target.x, target.y, target.z);
            if (block_type != 0) {
                aiming = true;
                const VoxelEngine::BlockRegistry& reg = VoxelEngine::BlockRegistry::get_instance();
                hardness = reg.get_block(static_cast<BlockID>(block_type)).hardness;
                // Same drop rule as break_block, crush included, so the gate
                // checks room for exactly what the break will hand over.
                const VoxelEngine::BlockDrop drop = VoxelEngine::resolve_block_drop(
                    static_cast<BlockID>(block_type), inventory_.get_selected_block());
                collect_id = drop.id;
                collect_count = drop.count;
            }
        }
    }

    // A different target (or first aim) restarts progress; aiming the same block again resumes it.
    if (aiming && (!break_target_valid_ || target != break_target_)) {
        break_target_ = target;
        break_progress_ = 0.0f;
        break_target_valid_ = true;
        break_block_id_ = block_type;
    }

    // Releasing LMB (or losing the breakable target) drops mining progress; the
    // crack vanishes and the swing stops. Progress only builds while actively aimed.
    if (!aiming || !break_target_valid_) {
        break_progress_ = 0.0f;
        break_target_valid_ = false;
        return;
    }

    // Inventory-full gate: no progress (matches break_block's insta-collect rule).
    if (!inventory_.can_add_block(collect_id, collect_count)) return;

    // Unbreakable blocks never crack or progress. The guard is <= 0, not
    // merely < 0: hardness is also a divisor two lines down, and a zero would
    // send the progress to +inf (or NaN on a zero-delta frame, which then never
    // breaks). The registry clamps at load, but this path defends itself.
    if (hardness <= 0.0f) return;

    // Tool speed: the selected hotbar tool multiplies break rate when its class
    // matches this block's preferred tool (see mining_speed_multiplier).
    const float tool_speed = mining_speed_multiplier(
        static_cast<BlockID>(break_block_id_), inventory_.get_selected_block());
    break_progress_ += delta * tool_speed / hardness;
    if (break_progress_ >= 1.0f) {
        break_progress_ = 0.0f;
        break_target_valid_ = false;
        break_block();
    }
}

godot::Dictionary PlayerController::get_break_state() {
    Dictionary state;

    // Invalidate the crack if the memoized block changed or vanished in the world.
    if (break_target_valid_ && chunk_manager_) {
        if (chunk_manager_->get_block(break_target_.x, break_target_.y, break_target_.z) != break_block_id_) {
            break_target_valid_ = false;
            break_progress_ = 0.0f;
        }
    }

    // World-space overlay — stays visible behind menus (progress simply pauses
    // while a UI is open thanks to the ui_blocked gate in update_break_progress).
    const bool visible = break_target_valid_ && break_progress_ > 0.0f && !dead_;
    state["active"] = visible;
    if (visible) {
        state["x"] = break_target_.x;
        state["y"] = break_target_.y;
        state["z"] = break_target_.z;
        // Map progress [0,1) onto 10 crack stages [0,9]; stage 9 is nearly broken.
        state["stage"] = static_cast<int>(std::floor(std::min(break_progress_ * 10.0f, 9.999f)));
    }
    return state;
}

void PlayerController::use_item() {
    // A held item with a use action does that; everything else is a placement.
    // Items stay out of voxels either way (their ids live above the block
    // registry, so place_block() refuses them).
    const VoxelEngine::BlockID held = inventory_.get_selected_block();
    const VoxelEngine::ItemUseAction* use =
        VoxelEngine::ItemRegistry::get_instance().get_item_use(held);
    if (use != nullptr && use->has_use()) {
        // A wand's use is a signal, not a world edit: it is answered by the
        // layer that knows which function and which build file are selected.
        if (use->is_wand()) {
            emit_signal("wand_use");
            return;
        }
        // A spawn egg is a signal too: the thing it makes is an entity built in
        // GDScript (pose_clone_debug.gd, which owns the model and the physics).
        if (use->is_dummy()) {
            emit_signal("dummy_spawn");
            return;
        }
        if (use->is_pour()) {
            pour_fluid_at_aim(use->block);
        } else if (use->is_fill()) {
            fill_bucket_at_aim();
        } else if (use->is_swap()) {
            swap_held_item(use->item);
        }
        return;
    }
    place_block();
}

bool PlayerController::swap_held_item(BlockID partner) {
    // A rod and its thrown version are two items, so a cast is a swap of what is
    // held rather than a state flag on one item. Nothing in the world is
    // consulted: a swap has no target, so this works aiming at the sky.
    const ItemRegistry& items = ItemRegistry::get_instance();
    if (!items.is_item(partner)) return false;
    const BlockID held = inventory_.get_selected_block();
    const int count = inventory_.get_selected_count();
    if (held == partner || !items.is_item(held)) return false;
    const int slot = inventory_.get_selected_slot();
    inventory_.set_hotbar_slot(slot, partner, count > 0 ? count : 1);
    // Its own signal, not `block_placed`: a throw is the FULL punch, while a
    // placement (and the bucket fill that borrows it) plays the weaker stroke.
    // The SLOT travels with it: a listener that answers the id change has to know
    // which slot it is answering, or a second slot changing in the same frame
    // takes the answer instead (the HUD's own arrival pop and name).
    emit_signal("item_swapped", slot);
    return true;
}

void PlayerController::update_held_light() {
    if (!chunk_manager_) return;

    // get_item_light returns nullptr for a non-item and for an item that lights
    // nothing, which is the whole test: only a light item takes the light over.
    const VoxelEngine::BlockID held = inventory_.get_selected_block();
    const VoxelEngine::ItemLight* light =
        VoxelEngine::ItemRegistry::get_instance().get_item_light(held);

    if (light != nullptr) {
        if (light_item_ != held) {
            // A light item took the light over: remember what the scene's own
            // toggle was before answering for it, so it can be handed back.
            if (light_item_ == VoxelEngine::BlockIDs::AIR) {
                light_manual_enabled_ = chunk_manager_->get_player_light_enabled();
            }
            light_item_ = held;
            chunk_manager_->set_player_light_level(light->level);
            chunk_manager_->set_player_light_color(
                Color(light->r, light->g, light->b));
        }
        if (!chunk_manager_->get_player_light_enabled()) {
            chunk_manager_->set_player_light_enabled(true);
        }
        return;
    }

    if (light_item_ != VoxelEngine::BlockIDs::AIR) {
        // Put away. The item's light goes out — both the setting it forced and
        // the level/colour it wrote — and the toggle that was live before it is
        // what governs again.
        light_item_ = VoxelEngine::BlockIDs::AIR;
        chunk_manager_->set_player_light_enabled(light_manual_enabled_);
        chunk_manager_->set_player_light_level(VoxelEngine::PlayerLight::DEFAULT_LEVEL);
        chunk_manager_->set_player_light_color(VoxelEngine::PlayerLight::default_color());
    }
}

// --- Entity punching ---------------------------------------------------------
// The K-key dummy and every goblin the spawn tool makes are one thing to the
// player's click: a body carrying dummy.gd, its own vanilla 0.6 x 1.8 x 0.6 box
// standing on its feet, in the `pose_clone` group. Only the pick lives here.

Node* PlayerController::nearest_punch_target(float& out_t) const {
    out_t = -1.0f;
    SceneTree* tree = get_tree();
    if (!tree) return nullptr;
    const TypedArray<Node> bodies = tree->get_nodes_in_group("pose_clone");
    const Vector3 eye = get_aim_origin();
    const Vector3 dir = get_aim_direction();
    Node* best = nullptr;
    for (int i = 0; i < bodies.size(); ++i) {
        Node* node = Object::cast_to<Node>(bodies[i]);
        Node3D* body = Object::cast_to<Node3D>(node);
        // A body that cannot take the hit is not a target: the group can outlive a
        // node freed this frame, and only a physics body answers `apply_knockback`.
        if (body == nullptr || !body->has_method("apply_knockback")) continue;
        const Vector3 pos = body->get_global_position();
        const Vector3 box_min = pos - Vector3(kDummyHalfWidth, 0.0f, kDummyHalfWidth);
        const Vector3 box_max = pos + Vector3(kDummyHalfWidth, kDummyHeight, kDummyHalfWidth);
        const float t = ray_aabb_hit(eye, dir, box_min, box_max);
        if (t < 0.0f || t > kPunchReach) continue;
        if (best == nullptr || t < out_t) {
            best = node;
            out_t = t;
        }
    }
    return best;
}

float PlayerController::dummy_aim_hit_t() const {
    float t = -1.0f;
    return nearest_punch_target(t) != nullptr ? t : -1.0f;
}

bool PlayerController::dummy_blocks_break_aim() const {
    float t = -1.0f;
    if (nearest_punch_target(t) == nullptr) return false;
    // Vanilla entity precedence: the nearer of the entity and the block along the
    // aim ray wins the click. No block hit (or a block farther away than the body)
    // means the body absorbs it.
    if (!chunk_manager_) return true;
    const Dictionary result = chunk_manager_->raycast_from_camera(10.0);
    if (!result.get("success", false)) return true;
    const Vector3 hit_point = result["hit_point"];
    const float block_dist = (hit_point - get_aim_origin()).length();
    return t <= block_dist;
}

bool PlayerController::try_punch_dummy() {
    if (!dummy_blocks_break_aim()) return false;
    float t = -1.0f;
    // The pick again, now for the node itself: the nearest body is the one the
    // swing lands on, whichever order the group lists them in.
    Node* target = nearest_punch_target(t);
    if (target == nullptr) return false;
    // First-person arm swing (viewmodel.gd punch()).
    Node* viewmodel = get_node_or_null(NodePath("Camera3D/Viewmodel"));
    if (viewmodel) viewmodel->call("punch");
    // Base vanilla 1.8.8 knockback — the body computes the direction and velocity
    // from its own and the attacker's positions.
    // Sprint bonus (vanilla 1.8.8): a sprinting attacker adds facing * 0.5 (+0.1 up)
    // on top — the victim inherits the attacker's forward momentum. Uses the sim's
    // sprint state (sprint key + forward + grounded).
    Vector3 extra;
    if (sim_.get_state() == MoveState::SPRINTING) {
        Vector3 fwd = get_aim_direction();
        fwd.y = 0.0f;
        if (fwd.length_squared() > 0.001f) fwd = fwd.normalized();
        extra = fwd * 0.5f + Vector3(0.0f, 0.1f, 0.0f);
    }
    target->call("apply_knockback", get_global_position(), extra);
    return true;
}
