// The item half of the Godot-facing physics API: the dropped items' own rigid
// bodies, against the world and each other. One call per substep, whatever the
// pile's size -- see engine/item_body_solver.hpp and engine/item_pair_solver.hpp
// for the maths and why it is not GDScript.
//
// The state is the caller's, passed as one dictionary of parallel packed arrays
// and written back into the same keys, so a substep costs no dictionary per body
// and no Variant per contact. The shapes of the bodies travel with it too (the
// same table every substep): a dropped block's boxes and points do not move, so
// they are not re-sent per body per substep.
//
// Kept out of chunk_manager_world_api.cpp (which is the world's own surface)
// and chunk_manager.cpp (the node's lifecycle), like the other split TUs.

#include "godot_bindings/chunk_manager.hpp"

#include "engine/item_body_solver.hpp"
#include "engine/item_pair_solver.hpp"
#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector4_array.hpp>

#include <algorithm>
#include <cstdint>

using namespace godot;
using namespace VoxelEngine;

namespace {

// The shape table of one call: the caller's own flattened shapes, held here so
// the pointers the solver reads through stay valid for the length of the call.
struct ShapeTableArrays {
    PackedVector3Array box_offsets;
    PackedVector3Array box_halves;
    PackedVector3Array points;
    PackedInt32Array box_start;
    PackedInt32Array box_count;
    PackedInt32Array point_start;
    PackedInt32Array point_count;

    // Whether every row points inside its own flattened boxes and points. A
    // caller that gathered a half-updated table would otherwise be read past its
    // end; a pass that does not add up is skipped, and the bodies simply do not
    // collide that substep.
    bool fits() const {
        const int64_t shapes = box_count.size();
        if (point_count.size() != shapes || box_start.size() != shapes
            || point_start.size() != shapes) {
            return false;
        }
        for (int64_t s = 0; s < shapes; ++s) {
            const int64_t boxes_start = box_start[s];
            const int64_t boxes = box_count[s];
            if (boxes_start < 0 || boxes < 0 || boxes_start + boxes > box_offsets.size()
                || boxes_start + boxes > box_halves.size()) {
                return false;
            }
            const int64_t points_start = point_start[s];
            const int64_t shape_points = point_count[s];
            if (points_start < 0 || shape_points < 0
                || points_start + shape_points > points.size()) {
                return false;
            }
        }
        return true;
    }

    ItemShapeTable pointers() const {
        ItemShapeTable table;
        table.box_offsets = box_offsets.ptr();
        table.box_halves = box_halves.ptr();
        table.box_start = box_start.ptr();
        table.box_count = box_count.ptr();
        table.points = points.ptr();
        table.point_start = point_start.ptr();
        table.point_count = point_count.ptr();
        table.shape_count = static_cast<int32_t>(box_count.size());
        return table;
    }
};

ShapeTableArrays take_shapes(const Dictionary& state) {
    ShapeTableArrays shapes;
    shapes.box_offsets = state.get("box_offsets", PackedVector3Array());
    shapes.box_halves = state.get("box_halves", PackedVector3Array());
    shapes.points = state.get("points", PackedVector3Array());
    shapes.box_start = state.get("box_start", PackedInt32Array());
    shapes.box_count = state.get("box_count", PackedInt32Array());
    shapes.point_start = state.get("point_start", PackedInt32Array());
    shapes.point_count = state.get("point_count", PackedInt32Array());
    return shapes;
}

// The bodies of one call, in the same terms: the caller's parallel arrays, and
// the pointers the solver writes through. `world` is the world step's half --
// the rest timer and the grounded output -- which the pair solve does not need.
struct BodyArrays {
    PackedVector3Array positions;
    PackedVector4Array rotations;
    PackedVector3Array velocities;
    PackedVector3Array spins;
    PackedVector3Array inertia;
    PackedFloat32Array reach;
    PackedInt32Array shape_rows;
    PackedByteArray asleep;
    PackedFloat32Array rest;
    PackedByteArray touched;
    PackedByteArray woken;
    PackedByteArray grounded;
    int64_t count = 0;

    ItemBodies pointers(bool world) {
        touched.resize(count);
        woken.resize(count);
        ItemBodies bodies;
        bodies.positions = positions.ptrw();
        bodies.rotations = rotations.ptrw();
        bodies.velocities = velocities.ptrw();
        bodies.spins = spins.ptrw();
        bodies.inertia = inertia.ptr();
        bodies.reach = reach.ptr();
        bodies.shapes = shape_rows.ptr();
        bodies.asleep = asleep.ptrw();
        bodies.touched = touched.ptrw();
        bodies.woken = woken.ptrw();
        bodies.count = static_cast<int32_t>(count);
        if (world) {
            bodies.rest = rest.ptrw();
            bodies.grounded = grounded.ptrw();
        }
        return bodies;
    }
};

// Reads the body arrays. The count is what EVERY per-body array can answer for:
// a caller that gathered one array shorter than the others gets its shortest
// body set solved rather than a read past an end.
BodyArrays take_bodies(const Dictionary& state, bool world) {
    BodyArrays bodies;
    bodies.positions = state.get("positions", PackedVector3Array());
    bodies.rotations = state.get("rotations", PackedVector4Array());
    bodies.velocities = state.get("velocities", PackedVector3Array());
    bodies.spins = state.get("spins", PackedVector3Array());
    bodies.inertia = state.get("inertia", PackedVector3Array());
    bodies.reach = state.get("reach", PackedFloat32Array());
    bodies.shape_rows = state.get("shapes", PackedInt32Array());
    bodies.asleep = state.get("asleep", PackedByteArray());
    bodies.count = bodies.positions.size();
    bodies.count = std::min(bodies.count, bodies.rotations.size());
    bodies.count = std::min(bodies.count, bodies.velocities.size());
    bodies.count = std::min(bodies.count, bodies.spins.size());
    bodies.count = std::min(bodies.count, bodies.inertia.size());
    bodies.count = std::min(bodies.count, bodies.reach.size());
    bodies.count = std::min(bodies.count, bodies.shape_rows.size());
    bodies.count = std::min(bodies.count, bodies.asleep.size());
    if (world) {
        bodies.rest = state.get("rest", PackedFloat32Array());
        bodies.grounded.resize(bodies.count);
        bodies.count = std::min(bodies.count, bodies.rest.size());
    }
    return bodies;
}

void store_bodies(Dictionary& state, const BodyArrays& bodies, bool world) {
    state["positions"] = bodies.positions;
    state["rotations"] = bodies.rotations;
    state["velocities"] = bodies.velocities;
    state["spins"] = bodies.spins;
    state["asleep"] = bodies.asleep;
    if (world) {
        state["rest"] = bodies.rest;
        state["grounded"] = bodies.grounded;
    }
}

} // namespace

void ChunkManager::solve_item_pairs(Dictionary state, float delta) {
    // `state` is the caller's own dictionary (Godot dictionaries are shared, not
    // copied), so the solved arrays are handed back by writing the same keys.
    ShapeTableArrays shapes = take_shapes(state);
    if (!shapes.fits()) {
        return;
    }
    BodyArrays bodies = take_bodies(state, false);
    if (bodies.count < 2) {
        return;  // nothing to pair
    }
    if (item_pair_solver == nullptr) {
        item_pair_solver = std::make_unique<ItemPairSolver>();
    }
    const ItemShapeTable table = shapes.pointers();
    ItemBodies raw = bodies.pointers(false);
    item_pair_solver->solve(table, raw, delta);
    store_bodies(state, bodies, false);
    state["touched"] = bodies.touched;
    state["woken"] = bodies.woken;
}

void ChunkManager::solve_item_bodies(Dictionary state, float delta) {
    ShapeTableArrays shapes = take_shapes(state);
    if (!shapes.fits()) {
        return;
    }
    BodyArrays bodies = take_bodies(state, true);
    if (bodies.count < 1) {
        return;
    }
    if (item_body_solver == nullptr) {
        item_body_solver = std::make_unique<ItemBodySolver>(
            &controller->get_collision_resolver());
    }
    const ItemShapeTable table = shapes.pointers();
    ItemBodies raw = bodies.pointers(true);
    item_body_solver->step(table, raw, delta);
    store_bodies(state, bodies, true);
    state["touched"] = bodies.touched;
    state["woken"] = bodies.woken;
}
