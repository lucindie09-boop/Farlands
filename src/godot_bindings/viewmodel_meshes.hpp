#ifndef FARLANDS_GODOT_BINDINGS_VIEWMODEL_MESHES_HPP
#define FARLANDS_GODOT_BINDINGS_VIEWMODEL_MESHES_HPP

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>

// Static helpers that push viewmodel.gd's mesh construction into C++. Returns
// surface arrays in a Dictionary (`verts`, `uvs`, `normals`, `indices`); the
// GDScript glue only has to sprinkle them into an ArrayMesh — no geometry math
// left for the script to drift.
class ViewmodelMeshes : public godot::RefCounted {
    // NOLINTBEGIN(bugprone-unhandled-self-assignment) — GDCLASS macro generates operator=
    GDCLASS(ViewmodelMeshes, godot::RefCounted)
    // NOLINTEND(bugprone-unhandled-self-assignment)

public:
    // Unit cube with texture-top on every face (the held-block mesh).
    static godot::Dictionary build_cube_mesh();

    // Selection boxes (each a PackedFloat32Array of six 0..1 floats) extruded
    // to whole-block cubes, centred on the origin.
    static godot::Dictionary build_shaped_mesh(const godot::Array& boxes);

    // Extruded sprite: the item texture's alpha field becomes a front/back
    // face with silhouette rims. Any texture format is snapped to RGBA8 first.
    static godot::Dictionary build_item_mesh(const godot::Ref<godot::Texture2D>& texture);

protected:
    static void _bind_methods();
};

#endif // FARLANDS_GODOT_BINDINGS_VIEWMODEL_MESHES_HPP