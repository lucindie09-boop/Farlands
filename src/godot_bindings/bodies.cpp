#include "godot_bindings/bodies.hpp"

#include <godot_cpp/core/class_db.hpp>

#include <vector>

#include "engine/entity_push.hpp"

using namespace godot;
using namespace VoxelEngine;

Dictionary Bodies::push_pair(const Vector3& feet_a, const Vector3& size_a,
                            const Vector3& feet_b, const Vector3& size_b) {
    Dictionary out;
    const PushPair pair = push_apart(PushBody{feet_a, size_a}, PushBody{feet_b, size_b});
    if (!pair.touching) return out;
    out["a"] = pair.a;
    out["b"] = pair.b;
    return out;
}

PackedVector3Array Bodies::push_pairs(const PackedVector3Array& positions,
                                      const PackedVector3Array& sizes) {
    PackedVector3Array out;
    const int64_t count = positions.size();
    if (count <= 0) return out;
    // One size is shorthand for "every body is the same box" -- the crowd's own case, and the one
    // where building a parallel array of 100 identical vectors would be pure waste.
    if (sizes.size() != count && sizes.size() != 1) return out;

    std::vector<PushBody> bodies(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) {
        bodies[static_cast<size_t>(i)] =
            PushBody{positions[i], sizes[sizes.size() == 1 ? 0 : i]};
    }
    std::vector<Vector3> deltas(static_cast<size_t>(count));
    push_apart_all(bodies.data(), bodies.size(), deltas.data(), deltas.size());

    out.resize(count);
    for (int64_t i = 0; i < count; ++i) out.set(i, deltas[static_cast<size_t>(i)]);
    return out;
}

void Bodies::_bind_methods() {
    ClassDB::bind_static_method("Bodies", D_METHOD("push_pair", "feet_a", "size_a", "feet_b", "size_b"),
                                &Bodies::push_pair);
    ClassDB::bind_static_method("Bodies", D_METHOD("push_pairs", "positions", "sizes"),
                                &Bodies::push_pairs);
}
