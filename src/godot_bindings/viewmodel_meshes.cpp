#include "godot_bindings/viewmodel_meshes.hpp"

#include "core/viewmodel_meshes.hpp"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector3.hpp>

#include <vector>

using namespace godot;
using namespace VoxelEngine;

namespace {
constexpr const char* kVerts = "verts";
constexpr const char* kUvs = "uvs";
constexpr const char* kNormals = "normals";
constexpr const char* kIndices = "indices";

Dictionary pack_mesh(const MeshGeometry& g) {
    Dictionary result;
    PackedVector3Array verts;
    PackedVector2Array uvs;
    PackedVector3Array normals;
    PackedInt32Array indices;
    verts.resize(static_cast<int64_t>(g.verts.size() / 3));
    uvs.resize(static_cast<int64_t>(g.uvs.size() / 2));
    normals.resize(static_cast<int64_t>(g.normals.size() / 3));
    indices.resize(static_cast<int64_t>(g.indices.size()));
    for (size_t i = 0; i < g.verts.size() / 3; ++i) {
        verts.set(static_cast<int64_t>(i), Vector3(g.verts[i * 3], g.verts[i * 3 + 1], g.verts[i * 3 + 2]));
    }
    for (size_t i = 0; i < g.uvs.size() / 2; ++i) {
        uvs.set(static_cast<int64_t>(i), Vector2(g.uvs[i * 2], g.uvs[i * 2 + 1]));
    }
    for (size_t i = 0; i < g.normals.size() / 3; ++i) {
        normals.set(static_cast<int64_t>(i), Vector3(g.normals[i * 3], g.normals[i * 3 + 1], g.normals[i * 3 + 2]));
    }
    for (size_t i = 0; i < g.indices.size(); ++i) {
        indices.set(static_cast<int64_t>(i), g.indices[i]);
    }
    result[kVerts] = verts;
    result[kUvs] = uvs;
    result[kNormals] = normals;
    result[kIndices] = indices;
    return result;
}
} // namespace

Dictionary ViewmodelMeshes::build_cube_mesh() {
    return pack_mesh(build_unit_cube_mesh());
}

Dictionary ViewmodelMeshes::build_shaped_mesh(const Array& boxes) {
    std::vector<float> flattened;
    for (int64_t i = 0; i < boxes.size(); ++i) {
        const PackedFloat32Array box = boxes[i];
        for (int64_t c = 0; c < box.size() && c < 6; ++c) {
            flattened.push_back(box[c]);
        }
    }
    return pack_mesh(build_box_mesh(flattened));
}

Dictionary ViewmodelMeshes::build_item_mesh(const Ref<Texture2D>& texture) {
    if (texture.is_null()) {
        return Dictionary();
    }
    Ref<Image> image = texture->get_image();
    if (image.is_null() || image->is_empty()) {
        return Dictionary();
    }
    if (image->get_format() != Image::FORMAT_RGBA8) {
        Ref<Image> converted;
        converted.instantiate();
        converted->copy_from(image);
        converted->convert(Image::FORMAT_RGBA8);
        image = converted;
    }
    const int32_t width = image->get_width();
    const int32_t height = image->get_height();
    const PackedByteArray bytes = image->get_data();
    const int64_t total = static_cast<int64_t>(width) * height * 4;
    if (bytes.size() < total) {
        return Dictionary();
    }
    return pack_mesh(build_sprite_mesh(bytes.ptr(), static_cast<int>(width), static_cast<int>(height)));
}

void ViewmodelMeshes::_bind_methods() {
    ClassDB::bind_static_method("ViewmodelMeshes", D_METHOD("build_cube_mesh"), &ViewmodelMeshes::build_cube_mesh);
    ClassDB::bind_static_method("ViewmodelMeshes", D_METHOD("build_shaped_mesh", "boxes"), &ViewmodelMeshes::build_shaped_mesh);
    ClassDB::bind_static_method("ViewmodelMeshes", D_METHOD("build_item_mesh", "texture"), &ViewmodelMeshes::build_item_mesh);
}