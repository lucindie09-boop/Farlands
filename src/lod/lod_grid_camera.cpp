// The far mode's whole view into the engine: the scenario its instances live in and
// the camera whose far plane has to reach the horizon it draws.
//
// Split out of lod_grid.cpp, which keeps the tiles, the build/upload pipeline and
// the per-frame update.
#include "lod/lod_grid.hpp"

#include "lod/lod_tile_policy.hpp"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/rid.hpp>

namespace VoxelEngine {

using namespace godot;

RID LodGrid::scenario() const {
    if (!owner) return RID();
    Node3D* node = Object::cast_to<Node3D>(owner);
    if (!node) return RID();
    Ref<World3D> world = node->get_world_3d();
    return world.is_valid() ? world->get_scenario() : RID();
}

Camera3D* LodGrid::find_camera() const {
    if (!owner) return nullptr;
    Viewport* viewport = owner->get_viewport();
    return viewport ? viewport->get_camera_3d() : nullptr;
}

void LodGrid::push_camera_far() {
    Camera3D* camera = find_camera();
    if (!camera) return;
    // The horizon plus one tile: the outermost ring's tiles are 256 blocks wide and
    // their far corners are the furthest geometry the mode draws, so a plane on the
    // horizon itself would clip the last ring's far half away.
    const float wanted =
        static_cast<float>(get_outer_radius_blocks() + lod::kTileBlocks);
    if (!camera_far_raised) {
        const float current = static_cast<float>(camera->get_far());
        if (wanted <= current) return;  // godot's own plane already reaches it
        camera_far_original = current;
        camera_far_raised = true;
    }
    if (wanted > static_cast<float>(camera->get_far())) camera->set_far(wanted);
}

void LodGrid::restore_camera_far() {
    if (!camera_far_raised) return;
    camera_far_raised = false;
    Camera3D* camera = find_camera();
    if (camera) camera->set_far(camera_far_original);
}

} // namespace VoxelEngine
