#ifndef FARLANDS_RENDER_MULTIMESH_INSTANCE_LAYOUT_HPP
#define FARLANDS_RENDER_MULTIMESH_INSTANCE_LAYOUT_HPP

// The float order of a MultiMesh instance buffer, in one place, because getting it
// wrong is SILENT: an instance packed in the wrong order does not error. It decodes as
// a degenerate transform, so the object is drawn mangled and off screen and simply
// never appears — no message, nothing in the log, and no clue in the scene.
//
// The order is three ROWS OF FOUR: each basis row followed by that row's origin
// component, so a unit transform has its diagonal at 0/5/10 and its translation at
// 3/7/11. Measured from the engine rather than read off the docs, because the docs
// describe the UNPACKED `transform_array` as "x, y, z, and origin", which is a
// different order:
//
//     set_instance_transform(0, Transform3D(Basis(), Vector3(11, 22, 33)))
//   packs to
//     1, 0, 0, 11,   0, 1, 0, 22,   0, 0, 1, 33
//
// `verify_multimesh_instance_layout` holds this against the engine's own packing, so a
// future change to the layout fails loudly instead of making the ghost vanish again.

#include <string>

namespace VoxelEngine {
namespace render {

// The twelve floats of a unit transform whose origin is (ox, oy, oz): one cell of a
// build preview, placed at the cell's centre.
inline void pack_unit_instance_transform(float* out, float ox, float oy, float oz) noexcept {
    out[0] = 1.0f;
    out[1] = 0.0f;
    out[2] = 0.0f;
    out[3] = ox;
    out[4] = 0.0f;
    out[5] = 1.0f;
    out[6] = 0.0f;
    out[7] = oy;
    out[8] = 0.0f;
    out[9] = 0.0f;
    out[10] = 1.0f;
    out[11] = oz;
}

#ifdef DEBUG_ENABLED

// Packs a known transform through the engine AND through the function above, and
// answers false with both packings in `report` when they disagree.
//
// Answers true — having proved nothing — when the renderer keeps no instance data:
// under --headless the dummy renderer drops a multimesh's data, so the engine has no
// answer to compare against and a "failure" would be an artefact of the harness.
//
// Debug builds only. Called once at startup (ChunkManager::_ready) and bound for
// probes.
bool verify_multimesh_instance_layout(std::string& report);

#endif  // DEBUG_ENABLED

}  // namespace render
}  // namespace VoxelEngine

#endif  // FARLANDS_RENDER_MULTIMESH_INSTANCE_LAYOUT_HPP
