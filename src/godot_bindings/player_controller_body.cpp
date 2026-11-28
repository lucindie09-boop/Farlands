// Building the player's visual body when the scene has lost it. Main.tscn
// instances player.glb as Player/PlayerModel; that node and its two ext_resources
// went missing on save three times (2 Oct, 24 Nov, 27 Nov) and every third-person
// path keys off model_, so a missing one is not a cosmetic problem.
//
// This file exists so player_controller.cpp stays inside the 500-line cap: the
// node lives there, standing a body back up from the model file lives here.

#include "godot_bindings/player_controller.hpp"

#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/packed_scene.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/script.hpp>
#include <godot_cpp/variant/basis.hpp>

using namespace godot;

namespace {

// The scene node's own numbers (Main.tscn: scale 0.05625, +0.0844 z). The model
// is authored at 17.8 units to the block and faces -z, which is what the negative
// x and z in the basis are for.
constexpr const char* kBodyScene = "res://assets/models/player.glb";
constexpr const char* kBodyScript = "res://scripts/player_model.gd";
constexpr float kBodyScale = 0.05625f;
constexpr float kBodyOffsetZ = 0.0844f;

}  // namespace

Node3D* PlayerController::build_body_from_model_file() {
    Ref<PackedScene> packed = Object::cast_to<PackedScene>(
        ResourceLoader::get_singleton()->load(kBodyScene).ptr());
    if (packed.is_null()) {
        return nullptr;
    }
    Node* body = packed->instantiate();
    Node3D* body_3d = Object::cast_to<Node3D>(body);
    if (body_3d == nullptr) {
        if (body != nullptr) {
            body->queue_free();
        }
        return nullptr;
    }
    body_3d->set_name("PlayerModel");
    body_3d->set_transform(Transform3D(Basis(Vector3(-kBodyScale, 0, 0),
                                             Vector3(0, kBodyScale, 0),
                                             Vector3(0, 0, -kBodyScale)),
                                       Vector3(0, 0, kBodyOffsetZ)));
    // Hidden like the scene's node is: the F5 cycle reveals it.
    body_3d->set_visible(false);
    Ref<Script> skin = Object::cast_to<Script>(
        ResourceLoader::get_singleton()->load(kBodyScript).ptr());
    if (skin.is_valid()) {
        body_3d->set_script(skin);
    }
    add_child(body_3d);
    return body_3d;
}
