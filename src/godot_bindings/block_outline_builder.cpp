#include "godot_bindings/block_outline_builder.hpp"

#include "render/block_outline_mesh.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/vector3.hpp>

using namespace godot;
using namespace VoxelEngine;

Dictionary BlockOutlineBuilder::build_outline(const Array& boxes, double thickness) {
    std::vector<OutlineBox> out_boxes;
    out_boxes.reserve(static_cast<size_t>(boxes.size()));
    for (int64_t i = 0; i < boxes.size(); ++i) {
        const PackedFloat32Array box = boxes[i];
        if (box.size() < 6) {
            continue;
        }
        OutlineBox ob;
        for (int c = 0; c < 3; ++c) {
            ob.min[c] = box[c];
            ob.max[c] = box[3 + c];
        }
        out_boxes.push_back(ob);
    }

    const OutlineMeshData data = build_outline_mesh(out_boxes, static_cast<float>(thickness));

    Dictionary result;
    PackedVector3Array verts;
    PackedInt32Array indices;
    const size_t vert_count = data.verts.size() / 3;
    verts.resize(static_cast<int64_t>(vert_count));
    for (size_t i = 0; i < vert_count; ++i) {
        verts.set(static_cast<int64_t>(i), Vector3(data.verts[i * 3], data.verts[i * 3 + 1], data.verts[i * 3 + 2]));
    }
    indices.resize(static_cast<int64_t>(data.indices.size()));
    for (size_t i = 0; i < data.indices.size(); ++i) {
        indices.set(static_cast<int64_t>(i), static_cast<int32_t>(data.indices[i]));
    }
    result["verts"] = verts;
    result["indices"] = indices;
    return result;
}

Dictionary BlockOutlineBuilder::fill_bounds(const Array& boxes) {
    std::vector<OutlineBox> out_boxes;
    out_boxes.reserve(static_cast<size_t>(boxes.size()));
    for (int64_t i = 0; i < boxes.size(); ++i) {
        const PackedFloat32Array box = boxes[i];
        if (box.size() < 6) {
            continue;
        }
        OutlineBox ob;
        for (int c = 0; c < 3; ++c) {
            ob.min[c] = box[c];
            ob.max[c] = box[3 + c];
        }
        out_boxes.push_back(ob);
    }

    float center[3];
    float size[3];
    outline_fill_bounds(out_boxes, center, size);

    Dictionary result;
    result["center"] = Vector3(center[0], center[1], center[2]);
    result["size"] = Vector3(size[0], size[1], size[2]);
    return result;
}

void BlockOutlineBuilder::_bind_methods() {
    ClassDB::bind_static_method("BlockOutlineBuilder", D_METHOD("build_outline", "boxes", "thickness"), &BlockOutlineBuilder::build_outline);
    ClassDB::bind_static_method("BlockOutlineBuilder", D_METHOD("fill_bounds", "boxes"), &BlockOutlineBuilder::fill_bounds);
}