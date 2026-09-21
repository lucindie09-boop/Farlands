#include "render/multimesh_instance_layout.hpp"

#ifdef DEBUG_ENABLED

#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/quad_mesh.hpp>
#include <godot_cpp/variant/basis.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <cstdio>
#include <cstring>

namespace VoxelEngine {
namespace render {
namespace {

// Distinguishable in every component, so a wrong order cannot coincide with the right
// one by having equal numbers in the places it swaps. Nothing is rotated: a unit basis
// is all the ghost uses, and it makes the two candidate orders (rows kept row-major vs
// columns kept column-major) agree, which is what we want — the check is about the
// POSITION of the translation and the diagonal, which is where the bug was.
constexpr float kTestX = 11.0f;
constexpr float kTestY = 22.0f;
constexpr float kTestZ = 33.0f;

constexpr int kFloatsPerInstance = 12;

void describe(const float* values, char* out, size_t size) {
    std::snprintf(out, size, "[%.0f %.0f %.0f %.0f | %.0f %.0f %.0f %.0f | %.0f %.0f %.0f %.0f]",
                  values[0], values[1], values[2], values[3], values[4], values[5], values[6],
                  values[7], values[8], values[9], values[10], values[11]);
}

}  // namespace

bool verify_multimesh_instance_layout(std::string& report) {
    report.clear();

    godot::Ref<godot::MultiMesh> probe;
    probe.instantiate();
    if (probe.is_null()) {
        // No engine to ask. Report nothing: this is not the failure being looked for.
        return true;
    }
    probe->set_transform_format(godot::MultiMesh::TRANSFORM_3D);
    // A mesh is required, not decoration: writing instance data makes the renderer
    // recompute the multimesh's bounds, and it complains (and has nothing to measure)
    // when the mesh is null. One quad is enough for the bounds and costs nothing.
    godot::Ref<godot::QuadMesh> marker;
    marker.instantiate();
    probe->set_mesh(marker);
    probe->set_instance_count(1);
    probe->set_instance_transform(
        0, godot::Transform3D(godot::Basis(), godot::Vector3(kTestX, kTestY, kTestZ)));

    const godot::PackedFloat32Array packed = probe->get_buffer();
    if (packed.size() != kFloatsPerInstance) {
        // The dummy renderer keeps no instance data (--headless): the engine has no
        // answer, so there is nothing to compare. Headless probes must stay quiet.
        return true;
    }

    float ours[kFloatsPerInstance];
    pack_unit_instance_transform(ours, kTestX, kTestY, kTestZ);

    // Compared value by value rather than with memcmp: agreeing here is a statement
    // about the numbers, and two representations of the same float that differ in
    // padding or negative zero are not a layout difference.
    const float* engine_packed = packed.ptr();
    bool same = true;
    for (int i = 0; i < kFloatsPerInstance; ++i) {
        if (engine_packed[i] != ours[i]) {
            same = false;
            break;
        }
    }
    if (same) {
        return true;
    }

    char engine_text[128];
    char ours_text[128];
    describe(engine_packed, engine_text, sizeof(engine_text));
    describe(ours, ours_text, sizeof(ours_text));

    char message[512];
    std::snprintf(message, sizeof(message),
                  "MultiMesh instance layout mismatch: the engine packs a unit transform at "
                  "(%.0f, %.0f, %.0f) as %s, and pack_unit_instance_transform writes %s. Every "
                  "instance would decode as a degenerate transform and never appear: the build "
                  "preview would show its outline and no cells. Fix the packing in "
                  "render/multimesh_instance_layout.hpp, and check the order against "
                  ".freebuff/probe_mm_layout.gd rather than against the class reference.",
                  kTestX, kTestY, kTestZ, engine_text, ours_text);
    report = message;
    return false;
}

}  // namespace render
}  // namespace VoxelEngine

#endif  // DEBUG_ENABLED
