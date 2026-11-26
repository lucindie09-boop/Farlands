#include "godot_bindings/bodies.hpp"

#include <godot_cpp/core/class_db.hpp>

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

void Bodies::_bind_methods() {
    ClassDB::bind_static_method("Bodies", D_METHOD("push_pair", "feet_a", "size_a", "feet_b", "size_b"),
                                &Bodies::push_pair);
}
