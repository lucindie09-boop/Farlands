#ifndef FARLANDS_GODOT_BINDINGS_BODIES_HPP
#define FARLANDS_GODOT_BINDINGS_BODIES_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

// Body against body, for GDScript: `Bodies.push_pair(feet_a, size_a, feet_b, size_b)` answers
// the two velocity deltas that separate a pair standing in one place. Static methods only, like
// BlockTextures and LootTables, so a script calls the class and nothing is instantiated.
//
// The arithmetic is src/engine/entity_push.cpp, which has no scene in it and is pinned by
// `tests/test_entity_push.cpp`; this class is the Dictionary translation, because a GDScript
// caller wants both halves at once (each one pushed by its own share) and a Vector3 pair is
// cheapest as a two-key Dictionary.
class Bodies : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(Bodies, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // {"a": Vector3, "b": Vector3} — the horizontal velocity to add to each body — or an empty
    // Dictionary when the two boxes do not intersect. A pair sharing a centre is `touching` and
    // both deltas are zero, which is the reference's own answer. Kept as the reference the
    // batched form is checked against (`probes/probe_mob_push.gd`).
    static godot::Dictionary push_pair(const godot::Vector3& feet_a, const godot::Vector3& size_a,
                                       const godot::Vector3& feet_b, const godot::Vector3& size_b);

    // The whole crowd at once: one summed delta per entry, in the same order. `sizes` is either one
    // box for every body (a crowd of goblins) or one per entry. The player is an entry like any
    // other — a caller puts them last — so their delta is the sum of their own pairs, pushed once.
    // Empty when the two arrays cannot be paired up.
    static godot::PackedVector3Array push_pairs(const godot::PackedVector3Array& positions,
                                                const godot::PackedVector3Array& sizes);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_BODIES_HPP
