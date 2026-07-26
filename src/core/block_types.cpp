#include "core/block_types.hpp"

#include <vector>
#include <string>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

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
            bt.top_face_offset = static_cast<float>(static_cast<double>(d["top_face_offset"]));
        }

        // slipperiness
        if (d.has("slipperiness")) {
            bt.slipperiness = static_cast<float>(static_cast<double>(d["slipperiness"]));
        }

        register_block(bt);
    }

    return true;
}
#endif

void BlockRegistry::initialize_default_blocks() noexcept {
    // Helper for solid, opaque, AO-generating blocks with all 6 faces visible.
    const auto solid = [&](const char* name) {
        register_block({
            0, name,
            BlockProperty::Solid | BlockProperty::Opaque,
             {true, true, true, true, true, true},
            {0, 0, 0, 0, 0, 0},
            0,
            0, 0, 0,
            LightEmissionPattern::Diamond
        });
    };

    // 0: Air
    register_block({
        0, "air",
        BlockProperty::Transparent | BlockProperty::NoOcclusion,
        {false, false, false, false, false, false},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0
    });

    // 1-4: Basic solids
    solid("stone");
    solid("dirt");

    // 3: Grass (bottom face hidden by dirt underneath)
    register_block({
        0, "grass",
        BlockProperty::Solid | BlockProperty::Opaque,
        {true, true, true, false, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0
    });

    solid("sand");

    // 5: Surface water (lowered top face)
    register_block({
        0, "surface_water",
        BlockProperty::Liquid | BlockProperty::Transparent,
        {true, true, true, false, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0,
        LightEmissionPattern::Diamond,
        0.12f
    });

    // 6: Water (lowered top face)
    register_block({
        0, "water",
        BlockProperty::Liquid | BlockProperty::Transparent,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0,
        LightEmissionPattern::Diamond,
        0.12f
    });

    // 7-8: Wood & Leaves
    solid("wood");

    register_block({
        0, "leaves",
        BlockProperty::Solid | BlockProperty::Transparent,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0
    });

    // 9: Bedrock
    solid("bedrock");

    // 10: Mud (lowered top face)
    register_block({
        0, "mud",
        BlockProperty::Solid | BlockProperty::Opaque,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0,
        LightEmissionPattern::Diamond,
        0.0625f
    });

    // 11: Wet sand (lowered top face)
    register_block({
        0, "wet_sand",
        BlockProperty::Solid | BlockProperty::Opaque,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        0,
        0, 0, 0,
        LightEmissionPattern::Diamond,
        0.0625f
    });

    // 12-13: Full variants (no offset)
    solid("mud_full");
    solid("wet_sand_full");

    // 14-17: Light blocks (emissive)
    register_block({
        0, "light_block",
        BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        15,
        15, 15, 15,
        LightEmissionPattern::Diamond
    });

    register_block({
        0, "light_red",
        BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        15,
        15, 0, 0,
        LightEmissionPattern::Diamond
    });

    register_block({
        0, "light_green",
        BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        15,
        0, 15, 0,
        LightEmissionPattern::Diamond
    });

    register_block({
        0, "light_blue",
        BlockProperty::Solid | BlockProperty::Opaque | BlockProperty::Emissive,
        {true, true, true, true, true, true},
        {0, 0, 0, 0, 0, 0},
        15,
        0, 0, 15,
        LightEmissionPattern::Diamond
    });

    // 18-20: Snow, Gravel, Cactus
    solid("snow");
    solid("gravel");
    solid("cactus");
}

} // namespace VoxelEngine
