#ifndef FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_BUILDER_HPP
#define FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_BUILDER_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

// Static helpers that hand the block-outline mesh math to C++. The outline
// node script (block_outline.gd) feeds the selection boxes from
// ChunkManager::get_selection_boxes() (Array of [min_x,min_y,min_z,max_x,max_y,max_z])
// into these and gets back ready-to-upload arrays.
class BlockOutlineBuilder : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(BlockOutlineBuilder, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // boxes -> { "verts": PackedVector3Array, "indices": PackedInt32Array }.
    static godot::Dictionary build_outline(const godot::Array& boxes, double thickness);
    // boxes -> { "center": Vector3, "size": Vector3 } (union AABB).
    static godot::Dictionary fill_bounds(const godot::Array& boxes);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_BLOCK_OUTLINE_BUILDER_HPP