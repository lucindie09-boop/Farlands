#include "core/block_types.hpp"

#include <vector>
#include <deque>
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

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
namespace {

void parse_aabb_array(const godot::Array& arr, std::vector<BlockAABB>& out) {
    out.reserve(static_cast<size_t>(arr.size()));
    for (int b = 0; b < static_cast<int>(arr.size()); ++b) {
        godot::Array box = arr[b];
        if (box.size() >= 6) {
            BlockAABB aabb;
            aabb.min[0] = static_cast<float>(static_cast<double>(box[0]));
            aabb.min[1] = static_cast<float>(static_cast<double>(box[1]));
            aabb.min[2] = static_cast<float>(static_cast<double>(box[2]));
            aabb.max[0] = static_cast<float>(static_cast<double>(box[3]));
            aabb.max[1] = static_cast<float>(static_cast<double>(box[4]));
            aabb.max[2] = static_cast<float>(static_cast<double>(box[5]));
            out.push_back(aabb);
        }
    }
}

void parse_shape_dict(const godot::Dictionary& sd, BlockShape& shape) {
    if (sd.has("selection_boxes")) {
        parse_aabb_array(sd["selection_boxes"], shape.selection_boxes);
    }
    if (sd.has("collision_boxes")) {
        parse_aabb_array(sd["collision_boxes"], shape.collision_boxes);
    }
}

} // anonymous namespace

bool BlockRegistry::load_shapes_from_json(const godot::String& json_path) noexcept {
    if (!shapes.empty()) return true;  // already loaded

    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(json_path, godot::FileAccess::READ);
    if (!file.is_valid()) {
        ERR_PRINT("BlockRegistry: failed to open " + json_path);
        return false;
    }

    godot::String text = file->get_as_text();
    file->close();

    godot::Variant parsed = godot::JSON::parse_string(text);
    if (parsed.get_type() != godot::Variant::DICTIONARY) {
        ERR_PRINT("BlockRegistry: failed to parse " + json_path);
        return false;
    }

    godot::Dictionary shapes_dict = parsed;
    godot::Array keys = shapes_dict.keys();
    for (int i = 0; i < static_cast<int>(keys.size()); ++i) {
        godot::String key = keys[i];
        godot::Variant val = shapes_dict[key];
        std::string prefix = key.utf8().get_data();

        if (val.get_type() == godot::Variant::DICTIONARY) {
            godot::Dictionary group = val;
            // Grouped: check if it's a leaf shape (has "selection_boxes") or a variant group
            if (group.has("selection_boxes")) {
                // Leaf shape (e.g. "pole")
                BlockShape shape;
                parse_shape_dict(group, shape);
                shapes[prefix] = std::move(shape);
            } else {
                // Variant group (e.g. "stair" -> "n", "s", ...)
                godot::Array sub_keys = group.keys();
                for (int j = 0; j < static_cast<int>(sub_keys.size()); ++j) {
                    godot::String sub_key = sub_keys[j];
                    godot::Dictionary sub_val = group[sub_key];
                    std::string full_name = prefix + "/" + sub_key.utf8().get_data();

                    BlockShape shape;
                    parse_shape_dict(sub_val, shape);
                    shapes[full_name] = std::move(shape);
                }
            }
        }
    }

    return true;
}
#endif

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
    // Load shared shapes from block_shapes.json (same directory as block_definitions.json)
    if (shapes.empty()) {
        godot::String shapes_path = json_path.left(json_path.rfind("/") + 1) + "block_shapes.json";
        load_shapes_from_json(shapes_path);
    }

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
        // deque: element addresses are stable across pushes, so bt.name keeps
        // pointing at valid storage for the process lifetime (a vector would
        // dangle every earlier pointer on reallocation).
        static std::deque<std::string> name_storage;
        name_storage.push_back(name_str.utf8().get_data());
        bt.name = name_storage.back().c_str();

        // hidden (placement-only variants are kept out of the inventory /give list)
        hidden_[i] = d.get("hidden", false).booleanize();

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

        // hardness (break time in seconds; -1.0 = unbreakable)
        if (d.has("hardness")) {
            bt.hardness = static_cast<float>(static_cast<double>(d["hardness"]));
        }

        // Resolve shape reference from block_shapes.json
        if (d.has("shape")) {
            godot::String shape_name_str = d["shape"];
            std::string shape_key = shape_name_str.utf8().get_data();
            auto it = shapes.find(shape_key);
            if (it != shapes.end()) {
                bt.selection_boxes = it->second.selection_boxes;
                bt.collision_boxes = it->second.collision_boxes;
            } else {
                ERR_PRINT("BlockRegistry: unknown shape \"" + shape_name_str + "\" for block \"" + name_str + "\"");
            }
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

        // Compute greedy_mergeable: full cubes, or bottom-anchored full-XZ columns
        // whose height is exactly 1 - top_face_offset — the only non-full geometry
        // the greedy flush can emit (it lowers tops via top_face_offset).
        // Slabs/stairs/walls/poles don't qualify and use per-AABB emission.
        bt.greedy_mergeable = bt.full_cube_ ||
            (bt.top_face_offset > 0.0f &&
             bt.selection_boxes.size() == 1 &&
             bt.selection_boxes[0].min[0] == 0.0f && bt.selection_boxes[0].max[0] == 1.0f &&
             bt.selection_boxes[0].min[1] == 0.0f &&
             bt.selection_boxes[0].max[1] == 1.0f - bt.top_face_offset &&
             bt.selection_boxes[0].min[2] == 0.0f && bt.selection_boxes[0].max[2] == 1.0f);

        register_block(bt);
    }

    // Build slab families from "slab_family" fields.  Each family name
    // collects three ids (bottom/top/full) so the placement code can work
    // generically without hardcoding any block ids.
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];
        if (!d.has("slab_family")) continue;
        godot::String fam_str = d["slab_family"];
        std::string fam_name = fam_str.utf8().get_data();
        size_t fi;
        auto it = family_index_.find(fam_name);
        if (it == family_index_.end()) {
            fi = families_.size();
            families_.emplace_back();
            family_names_.push_back(fam_name);
            family_index_[fam_name] = fi;
        } else {
            fi = it->second;
        }
        SlabFamily& fam = families_[fi];
        const BlockID id = static_cast<BlockID>(i);
        std::string shape;
        if (d.has("shape")) {
            godot::String shape_str = d["shape"];
            shape = shape_str.utf8().get_data();
        }
        if (shape == "slab/bottom")      fam.bottom = id;
        else if (shape == "slab/top")     fam.top    = id;
        else                              fam.full   = id;
        slab_family_map_[id] = static_cast<BlockID>(fi + 1);
    }

    // Build stair families from "stair_family" fields.
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];
        if (!d.has("stair_family")) continue;
        godot::String fam_str = d["stair_family"];
        std::string fam_name = fam_str.utf8().get_data();
        size_t fi;
        auto it = stair_family_index_.find(fam_name);
        if (it == stair_family_index_.end()) {
            fi = stair_families_.size();
            stair_families_.emplace_back();
            stair_family_names_.push_back(fam_name);
            stair_family_index_[fam_name] = fi;
        } else {
            fi = it->second;
        }
        StairFamily& fam = stair_families_[fi];
        const BlockID id = static_cast<BlockID>(i);
        std::string shape;
        if (d.has("shape")) {
            godot::String shape_str = d["shape"];
            shape = shape_str.utf8().get_data();
        }
        if      (shape == "stair/n")     fam.base  = id;
        else if (shape == "stair/s")     fam.s     = id;
        else if (shape == "stair/e")     fam.e     = id;
        else if (shape == "stair/w")     fam.w     = id;
        else if (shape == "stair/n_up")  fam.n_up  = id;
        else if (shape == "stair/s_up")  fam.s_up  = id;
        else if (shape == "stair/e_up")  fam.e_up  = id;
        else if (shape == "stair/w_up")  fam.w_up  = id;
        stair_family_map_[id] = static_cast<BlockID>(fi + 1);
    }

    // Build wall families from "wall_family" fields.
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];
        if (!d.has("wall_family")) continue;
        godot::String fam_str = d["wall_family"];
        std::string fam_name = fam_str.utf8().get_data();
        size_t fi;
        auto it = wall_family_index_.find(fam_name);
        if (it == wall_family_index_.end()) {
            fi = wall_families_.size();
            wall_families_.emplace_back();
            wall_family_names_.push_back(fam_name);
            wall_family_index_[fam_name] = fi;
        } else {
            fi = it->second;
        }
        WallFamily& fam = wall_families_[fi];
        const BlockID id = static_cast<BlockID>(i);
        std::string shape;
        if (d.has("shape")) {
            godot::String shape_str = d["shape"];
            shape = shape_str.utf8().get_data();
        }
        if      (shape == "wall/n")      fam.base = id;
        else if (shape == "wall/s")      fam.s    = id;
        else if (shape == "wall/e")      fam.e    = id;
        else if (shape == "wall/w")      fam.w    = id;
        else                             fam.full = id;
        wall_family_map_[id] = static_cast<BlockID>(fi + 1);
    }

    return true;
}
#endif

const BlockRegistry::SlabFamily* BlockRegistry::get_slab_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = slab_family_map_[id];
    if (fi == 0) return nullptr;
    return &families_[static_cast<size_t>(fi - 1)];
}

const BlockRegistry::StairFamily* BlockRegistry::get_stair_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = stair_family_map_[id];
    if (fi == 0) return nullptr;
    return &stair_families_[static_cast<size_t>(fi - 1)];
}

const BlockRegistry::WallFamily* BlockRegistry::get_wall_family(BlockID id) const noexcept {
    if (id >= MAX_BLOCK_TYPES) return nullptr;
    const BlockID fi = wall_family_map_[id];
    if (fi == 0) return nullptr;
    return &wall_families_[static_cast<size_t>(fi - 1)];
}

void BlockRegistry::initialize_default_blocks() noexcept {
    // Idempotent: repeated calls (e.g. once per test case) must not append
    // duplicate defaults, which would eventually overflow MAX_BLOCK_TYPES.
    static bool initialized = false;
    if (initialized) return;
    initialized = true;

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
