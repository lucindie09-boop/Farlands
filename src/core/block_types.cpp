#include "core/block_types.hpp"

#include <vector>
#include <string>
#include <cstring>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

BlockID BlockRegistry::get_block_id_by_name(const char* name) const noexcept {
    if (name == nullptr) {
        return BlockIDs::AIR;
    }
    for (size_t i = 0; i < count; ++i) {
        const BlockType& bt = block_types[i];
        if (bt.name != nullptr && std::strcmp(bt.name, name) == 0) {
            return bt.id;
        }
    }
    return BlockIDs::AIR;
}

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
bool BlockRegistry::load_from_json(const godot::String& json_path) noexcept {
    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(json_path, godot::FileAccess::READ);
    if (!file.is_valid()) {
        ERR_PRINT("BlockRegistry: failed to open " + json_path);
        return false;
    }

    godot::String text = file->get_as_text();
    file->close();

    godot::Variant parsed = godot::JSON::parse_string(text);
    if (parsed.get_type() != godot::Variant::ARRAY) {
        ERR_PRINT("BlockRegistry: failed to parse " + json_path);
        return false;
    }

    godot::Array blocks_arr = parsed;
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];

        BlockType bt{};

        // name
        godot::String name_str = d["name"];
        static std::vector<std::string> name_storage;
        name_storage.push_back(name_str.utf8().get_data());
        bt.name = name_storage.back().c_str();

        // properties
        godot::Array props = d["properties"];
        for (int p = 0; p < static_cast<int>(props.size()); ++p) {
            godot::String flag = props[p];
            if (flag == "Solid")          bt.properties = bt.properties | BlockProperty::Solid;
            else if (flag == "Transparent") bt.properties = bt.properties | BlockProperty::Transparent;
            else if (flag == "Opaque")      bt.properties = bt.properties | BlockProperty::Opaque;
            else if (flag == "Liquid")      bt.properties = bt.properties | BlockProperty::Liquid;
            else if (flag == "RenderAllFaces") bt.properties = bt.properties | BlockProperty::RenderAllFaces;
            else if (flag == "NoOcclusion") bt.properties = bt.properties | BlockProperty::NoOcclusion;
            else if (flag == "Emissive")    bt.properties = bt.properties | BlockProperty::Emissive;
        }

        // visible_faces
        godot::Array vf = d["visible_faces"];
        for (int f = 0; f < 6 && f < vf.size(); ++f) {
            bt.visible_faces[f] = vf[f].booleanize();
        }

        // textures
        godot::Array tx = d["textures"];
        for (int f = 0; f < 6 && f < tx.size(); ++f) {
            godot::String tex_name = tx[f];
            bt.texture_names[f] = tex_name.utf8().get_data();
            bt.texture_indices[f] = 0;  // resolved later by TextureArrayGenerator
        }

        // emissive_textures
        if (d.has("emissive_textures")) {
            godot::Array etx = d["emissive_textures"];
            for (int f = 0; f < 6 && f < etx.size(); ++f) {
                godot::String tex_name = etx[f];
                bt.emissive_texture_names[f] = tex_name.utf8().get_data();
                bt.emissive_texture_indices[f] = 0;  // resolved later by TextureArrayGenerator
            }
        }

        // light [r, g, b]
        godot::Array lt = d["light"];
        if (lt.size() >= 3) {
            bt.light_r = static_cast<uint8_t>(static_cast<int64_t>(lt[0]));
            bt.light_g = static_cast<uint8_t>(static_cast<int64_t>(lt[1]));
            bt.light_b = static_cast<uint8_t>(static_cast<int64_t>(lt[2]));
            bt.light_level = (bt.light_r > 0 || bt.light_g > 0 || bt.light_b > 0) ? 15 : 0;
        }

        // top_face_offset
        if (d.has("top_face_offset")) {
            float offset = static_cast<float>(static_cast<double>(d["top_face_offset"]));
            // Clamp to [0.0, 1.0] to prevent UB in vertex format conversion
            // (negative values wrap when cast to uint16_t, >1.0 exceeds chunk bounds)
            if (offset < 0.0f) offset = 0.0f;
            if (offset > 1.0f) offset = 1.0f;
            bt.top_face_offset = offset;
        }

        // slipperiness
        if (d.has("slipperiness")) {
            bt.slipperiness = static_cast<float>(static_cast<double>(d["slipperiness"]));
        }

        // selection_boxes: array of [min_x, min_y, min_z, max_x, max_y, max_z]
        if (d.has("selection_boxes")) {
            godot::Array boxes = d["selection_boxes"];
            bt.selection_boxes.clear();
            bt.selection_boxes.reserve(static_cast<size_t>(boxes.size()));
            for (int b = 0; b < static_cast<int>(boxes.size()); ++b) {
                godot::Array box = boxes[b];
                if (box.size() >= 6) {
                    BlockAABB aabb;
                    aabb.min[0] = static_cast<float>(static_cast<double>(box[0]));
                    aabb.min[1] = static_cast<float>(static_cast<double>(box[1]));
                    aabb.min[2] = static_cast<float>(static_cast<double>(box[2]));
                    aabb.max[0] = static_cast<float>(static_cast<double>(box[3]));
                    aabb.max[1] = static_cast<float>(static_cast<double>(box[4]));
                    aabb.max[2] = static_cast<float>(static_cast<double>(box[5]));
                    bt.selection_boxes.push_back(aabb);
                }
            }
        }

        // collision_boxes: optional override for collision only
        if (d.has("collision_boxes")) {
            godot::Array cboxes = d["collision_boxes"];
            bt.collision_boxes.clear();
            bt.collision_boxes.reserve(static_cast<size_t>(cboxes.size()));
            for (int b = 0; b < static_cast<int>(cboxes.size()); ++b) {
                godot::Array box = cboxes[b];
                if (box.size() >= 6) {
                    BlockAABB aabb;
                    aabb.min[0] = static_cast<float>(static_cast<double>(box[0]));
                    aabb.min[1] = static_cast<float>(static_cast<double>(box[1]));
                    aabb.min[2] = static_cast<float>(static_cast<double>(box[2]));
                    aabb.max[0] = static_cast<float>(static_cast<double>(box[3]));
                    aabb.max[1] = static_cast<float>(static_cast<double>(box[4]));
                    aabb.max[2] = static_cast<float>(static_cast<double>(box[5]));
                    bt.collision_boxes.push_back(aabb);
                }
            }
        }

        // Compute cached full_cube_ flag
        bt.full_cube_ = (bt.selection_boxes.empty()) ||
            (bt.selection_boxes.size() == 1 &&
             bt.selection_boxes[0].min[0] == 0.0f && bt.selection_boxes[0].min[1] == 0.0f && bt.selection_boxes[0].min[2] == 0.0f &&
             bt.selection_boxes[0].max[0] == 1.0f && bt.selection_boxes[0].max[1] == 1.0f && bt.selection_boxes[0].max[2] == 1.0f);

        register_block(bt);
    }

    return true;
}
#endif

void BlockRegistry::initialize_default_blocks() noexcept {
    // Helper for solid, opaque, AO-generating blocks with all 6 faces visible.
    const auto solid = [&](const char* name) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    };

    // 0: Air
    {
        BlockType bt{};
        bt.name = "air";
        bt.properties = BlockProperty::Transparent | BlockProperty::NoOcclusion;
        bt.visible_faces = {false, false, false, false, false, false};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 1-4: Basic solids
    solid("stone");
    solid("dirt");

    // 3: Grass (bottom face hidden by dirt underneath)
    {
        BlockType bt{};
        bt.name = "grass";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, false, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    solid("sand");

    // 5: Surface water (lowered top face)
    {
        BlockType bt{};
        bt.name = "surface_water";
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, false, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 6: Water (lowered top face)
    {
        BlockType bt{};
        bt.name = "water";
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 7-8: Wood & Leaves
    solid("wood");

    // 8: Leaves
    {
        BlockType bt{};
        bt.name = "leaves";
        bt.properties = BlockProperty::Solid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 9: Bedrock
    solid("bedrock");

    // 10: Mud (lowered top face)
    {
        BlockType bt{};
        bt.name = "mud";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.0625f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 11: Wet sand (lowered top face)
    {
        BlockType bt{};
        bt.name = "wet_sand";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.0625f;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 12-13: Full variants (no offset)
    solid("mud_full");
    solid("wet_sand_full");

    // 14-17: Light blocks (emissive)
    {
        BlockType bt{};
        bt.name = "light_block";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 15; bt.light_g = 15; bt.light_b = 15;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_red";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 15; bt.light_g = 0; bt.light_b = 0;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_green";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 0; bt.light_g = 15; bt.light_b = 0;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    {
        BlockType bt{};
        bt.name = "light_blue";
        bt.properties = BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_level = 15;
        bt.light_r = 0; bt.light_g = 0; bt.light_b = 15;
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.slipperiness = 0.6f;
        bt.full_cube_ = true;
        register_block(bt);
    }

    // 18-20: Snow, Gravel, Cactus
    solid("snow");
    solid("gravel");
    solid("cactus");
}

} // namespace VoxelEngine
