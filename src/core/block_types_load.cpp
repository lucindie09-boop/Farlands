// data/block_definitions.json: the block reader, and the drop-chain resolution that
// runs once every block is in. The shape file's reader is in
// core/block_types_shapes.cpp; the derivation this calls is declared in
// core/block_types_internal.hpp; the family tables the reader builds afterwards are
// in core/block_types_families.cpp.
//
// load_from_json is a frame: load the shapes, open and parse the file, then hand
// each entry to the helpers below, one per group of keys, and the finished table to
// the passes in the companion file. The helpers are file-local free functions taking
// the BlockType they fill, so the only registry state they touch is the shape table
// (read) and the "hidden" flag (handed back per entry for the caller to store).
//
// Each field keeps the clamping its consumers' arithmetic needs — read the helper
// before deciding a bound is redundant.

#include "core/block_types.hpp"
#include "core/block_types_internal.hpp"
#include "core/shape_resolver.hpp"

#include <algorithm>
#include <deque>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

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

// The shape table a block's "shape" field is looked up in.
using ShapeTable = std::unordered_map<std::string, BlockShape>;

// "name": the pointer a BlockType keeps has to stay valid for the process lifetime,
// and a deque's element addresses are stable across pushes, so bt.name keeps
// pointing at valid storage (a vector would dangle every earlier pointer on
// reallocation).
const char* intern_block_name(const godot::String& name) {
    static std::deque<std::string> name_storage;
    name_storage.push_back(name.utf8().get_data());
    return name_storage.back().c_str();
}

// A list of [min_x, min_y, min_z, max_x, max_y, max_z] boxes, in block-local space.
// Selection and collision boxes are both declared this way, so they share a reader;
// a box with fewer than six numbers is skipped rather than read past its end.
void read_boxes(const godot::Array& boxes, std::vector<BlockAABB>& out) {
    out.clear();
    out.reserve(static_cast<size_t>(boxes.size()));
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
            out.push_back(aabb);
        }
    }
}

// The render keys: "properties", "visible_faces", "textures" and
// "emissive_textures". Texture *indices* are all left 0 — the layer a texture name
// resolves to is assigned later, by TextureArrayGenerator.
void parse_render_tables(const godot::Dictionary& d, BlockType& bt) {
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
}

// "light" and "light_opacity": the emitted colour and how much light the block
// removes from what crosses it, both on the 0..15 scale.
void parse_light(const godot::Dictionary& d, BlockType& bt) {
    // light [r, g, b]
    godot::Array lt = d["light"];
    if (lt.size() >= 3) {
        bt.light_r = static_cast<uint8_t>(static_cast<int64_t>(lt[0]));
        bt.light_g = static_cast<uint8_t>(static_cast<int64_t>(lt[1]));
        bt.light_b = static_cast<uint8_t>(static_cast<int64_t>(lt[2]));
        bt.light_level = (bt.light_r > 0 || bt.light_g > 0 || bt.light_b > 0) ? 15 : 0;
    }

    // light_opacity (extra light levels removed crossing this block;
    // clamped to the 0..15 light scale, 15 = stops light like opaque)
    if (d.has("light_opacity")) {
        int64_t opacity = static_cast<int64_t>(d["light_opacity"]);
        if (opacity < 0) opacity = 0;
        if (opacity > 15) opacity = 15;
        bt.light_opacity = static_cast<uint8_t>(opacity);
    }
}

// The three floats gameplay arithmetic divides by — "top_face_offset",
// "slipperiness" and "hardness" — which is why each is clamped here rather than
// trusted from the file.
void parse_physics(const godot::Dictionary& d, BlockType& bt) {
    // top_face_offset
    if (d.has("top_face_offset")) {
        float offset = static_cast<float>(static_cast<double>(d["top_face_offset"]));
        // Clamp to [0.0, 1.0] to prevent UB in vertex format conversion
        // (negative values wrap when cast to uint16_t, >1.0 exceeds chunk bounds)
        if (offset < 0.0f) offset = 0.0f;
        if (offset > 1.0f) offset = 1.0f;
        bt.top_face_offset = offset;
    }

    // slipperiness. Vanilla's range is 0.6 (most blocks) to 0.98 (ice); a
    // value outside 0.05..1.0 is clamped rather than honoured, because the
    // ground-acceleration arithmetic divides by it (pow(0.6 / s, 3)) and a
    // zero or negative would turn velocities into inf then NaN — the same
    // divisor class of bug hardness above guards.
    if (d.has("slipperiness")) {
        float slip = static_cast<float>(static_cast<double>(d["slipperiness"]));
        bt.slipperiness = std::clamp(slip, 0.05f, 1.0f);
    }

    // hardness (break time in seconds; -1.0 = unbreakable). Clamped like
    // light_opacity above, because both end up as divisors in gameplay
    // arithmetic: the break path divides by hardness, and the physics
    // divides by slipperiness twice over.
    //
    // A ZERO is not the unbreakable sentinel here - -1 is, and that is left
    // alone. Zero in this table means "breaks at once": the torch family
    // (light_torch and its four wall variants) declares it, and reading it
    // as unbreakable would leave a player unable to take down a torch they
    // can place. So zero is lifted to the same floor as any other value
    // under it, which is 0.05s: fast enough that holding the mouse is the
    // whole of the interaction, and no longer a zero divisor.
    //
    // That floor is on any sub-floor value rather than on zero alone, because
    // a hundredth of a second is not a break time a player can see the
    // difference of, and it keeps tool_speed / hardness inside the range the
    // progress accumulator was tuned for. The divisor the floor is really
    // guarding is NaN on a zero-delta frame, which never breaks at all.
    if (d.has("hardness")) {
        float hardness = static_cast<float>(static_cast<double>(d["hardness"]));
        if (hardness >= 0.0f && hardness < 0.05f) hardness = 0.05f;
        bt.hardness = hardness;
    }
}

// "preferred_tool" (tool class mined fastest against this block; "" = none) and
// "min_tier" (the tier the speed bonus needs, 0 = any).
void parse_tool(const godot::Dictionary& d, BlockType& bt) {
    if (d.has("preferred_tool")) {
        bt.preferred_tool = godot::String(d["preferred_tool"]).utf8().get_data();
    }

    if (d.has("min_tier")) {
        bt.min_tier = static_cast<int32_t>(static_cast<int64_t>(d["min_tier"]));
        if (bt.min_tier < 0) bt.min_tier = 0;
    }
}

// "fluid": which fluid state this block IS (see fluids/fluid_state_table.hpp). An
// entry the table does not know is logged and left None rather than guessed at,
// because a block that is silently not a fluid stops the simulation from ever
// touching it.
void parse_fluid_state(const godot::Dictionary& d, BlockType& bt, const godot::String& name_str) {
    if (d.has("fluid")) {
        godot::Dictionary fd = d["fluid"];
        if (fd.has("kind")) {
            const godot::String kind_name = fd["kind"];
            bt.fluid_kind = fluid_kind_from_name(kind_name.utf8().get_data());
            if (bt.fluid_kind == FluidKind::None) {
                ERR_PRINT("BlockRegistry: unknown fluid kind \"" + kind_name +
                          "\" for block \"" + name_str + "\"");
            }
        }
        if (fd.has("depth")) {
            const int64_t depth = static_cast<int64_t>(fd["depth"]);
            bt.fluid_depth = static_cast<uint8_t>(depth < 0 ? 0 : (depth > 255 ? 255 : depth));
        }
        if (fd.has("falling")) {
            bt.fluid_falling = fd.get("falling", false).booleanize();
        }
        if (bt.fluid_kind == FluidKind::None) {
            ERR_PRINT("BlockRegistry: block \"" + name_str +
                      "\" has a \"fluid\" entry but no usable \"kind\"");
        }
    }
}

// The shape the block draws with: "shape" resolved against block_shapes.json,
// the "selection_boxes" / "collision_boxes" overrides, and the two flags derived
// from them that every hot path checks. A shape name the file does not declare only
// logs — the block stays a full cube, which is the same fallback an absent "shape"
// gets.
void parse_geometry(const godot::Dictionary& d, BlockType& bt, const godot::String& name_str,
                    const ShapeTable& shapes) {
    // Resolve shape reference from block_shapes.json
    if (d.has("shape")) {
        godot::String shape_name_str = d["shape"];
        std::string shape_key = shape_name_str.utf8().get_data();
        auto it = shapes.find(shape_key);
        if (it != shapes.end()) {
            apply_shape_to_block(it->second, bt, name_str, shape_name_str);
        } else {
            ERR_PRINT("BlockRegistry: unknown shape \"" + shape_name_str + "\" for block \"" + name_str + "\"");
        }
    }

    // selection_boxes: array of [min_x, min_y, min_z, max_x, max_y, max_z]
    if (d.has("selection_boxes")) {
        godot::Array boxes = d["selection_boxes"];
        read_boxes(boxes, bt.selection_boxes);
    }

    // collision_boxes: optional override for collision only
    if (d.has("collision_boxes")) {
        godot::Array cboxes = d["collision_boxes"];
        read_boxes(cboxes, bt.collision_boxes);
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
}

// One entry of the top-level array, in field groups. `hidden` is the one field the
// registry stores out of line: it is per id, not on the BlockType.
BlockType parse_block_entry(const godot::Dictionary& d, const ShapeTable& shapes, bool& hidden) {
    BlockType bt{};

    // name
    const godot::String name_str = d["name"];
    bt.name = intern_block_name(name_str);

    // hidden (placement-only variants are kept out of the inventory /give list)
    hidden = d.get("hidden", false).booleanize();

    parse_render_tables(d, bt);
    parse_light(d, bt);
    parse_physics(d, bt);
    parse_tool(d, bt);
    parse_fluid_state(d, bt, name_str);
    parse_geometry(d, bt, name_str, shapes);

    return bt;
}

} // namespace

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
    const ShapeTable& shape_table = shapes;
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];

        bool hidden = false;
        const BlockType bt = parse_block_entry(d, shape_table, hidden);
        hidden_[i] = hidden;
        register_block(bt);
    }

    // The two passes that need every id to exist first, in the order their results
    // are asked for: the references ("drops", "crush_result") resolve against ids,
    // then the families the placement code looks blocks up by.
    resolve_block_references(blocks_arr);
    build_slab_families(blocks_arr);
    build_stair_families(blocks_arr);
    build_wall_families(blocks_arr);

    return true;
}

void BlockRegistry::resolve_pending_drops(
    const std::function<BlockID(const char*)>& item_lookup) noexcept {
    for (const auto& [id, name] : pending_drops_) {
        BlockID target = get_block_id_by_name(name.c_str());
        if (target == BlockIDs::AIR && item_lookup) {
            target = item_lookup(name.c_str());
        }
        if (target == BlockIDs::AIR) {
            ERR_PRINT("BlockRegistry: unknown drops \"" + godot::String(name.c_str())
                      + "\" for block \"" + godot::String(get_block(id).name)
                      + "\" (not a block or an item)");
            continue;
        }
        get_block_mutable(id)->drops = target;
    }
    pending_drops_.clear();
}
#endif

} // namespace VoxelEngine
