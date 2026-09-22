#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"

#include <cmath>
#include <vector>
#include <deque>
#include <string>
#include <string_view>
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

// Fluid kind names, as block_definitions.json spells them. Deliberately a
// string table in one place: the loader, the state table and any diagnostic all
// have to agree, and a new fluid is one line here plus its traits in
// src/fluids/fluid_rules.cpp.
const char* fluid_kind_name(FluidKind kind) noexcept {
    switch (kind) {
        case FluidKind::Water: return "water";
        case FluidKind::Lava:  return "lava";
        case FluidKind::Acid:  return "acid";
        case FluidKind::None:  break;
    }
    return "none";
}

FluidKind fluid_kind_from_name(const char* name) noexcept {
    if (name == nullptr) return FluidKind::None;
    const std::string_view s(name);
    if (s == "water") return FluidKind::Water;
    if (s == "lava")  return FluidKind::Lava;
    if (s == "acid")  return FluidKind::Acid;
    return FluidKind::None;
}

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

// Two box lists are the same shape when every box matches within 1/16 of a
// voxel: the JSON carries exact 16ths and the derived list is a copy of parsed
// values, so this only has to absorb the double -> float round trip.
bool box_lists_equal(const std::vector<BlockAABB>& a, const std::vector<BlockAABB>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            if (std::fabs(a[i].min[k] - b[i].min[k]) > 1e-4f) return false;
            if (std::fabs(a[i].max[k] - b[i].max[k]) > 1e-4f) return false;
        }
    }
    return true;
}

void parse_parts_array(const godot::Array& arr, std::vector<ShapePart>& out,
                       const godot::String& owner) {
    out.reserve(static_cast<size_t>(arr.size()));
    for (int i = 0; i < static_cast<int>(arr.size()); ++i) {
        if (arr[i].get_type() != godot::Variant::DICTIONARY) continue;
        godot::Dictionary pd = arr[i];
        ShapePart part;
        if (pd.has("boxes")) parse_aabb_array(pd["boxes"], part.boxes);
        if (pd.has("collision_boxes")) parse_aabb_array(pd["collision_boxes"], part.collision_boxes);
        if (pd.has("rule")) {
            godot::String rule_str = pd["rule"];
            const std::string rule_name = rule_str.utf8().get_data();
            part.rule = shape_rule_from_name(rule_name);
            if (part.rule == ShapeRule::None) {
                ERR_PRINT("BlockRegistry: unknown shape rule \"" + rule_str + "\" in " + owner +
                          " part " + godot::String::num_int64(i) +
                          " — the part stays unconditional");
            }
        }
        // The claim follows the geometry: which cell faces the boxes reach is read
        // off the boxes, so a rule can never be authored against a face the part
        // does not actually meet. A rule that asks about something other than the
        // geometry it reaches supplies its own faces instead (a stair's step face and
        // its guard side are not where its corner boxes are) — that happens in
        // apply_shape_to_block, which is where the block's own orientation is known.
        part.faces = shape_box_faces(part.boxes);
        if (pd.has("faces")) {
            godot::Array names = pd["faces"];
            uint8_t mask = 0;
            for (int f = 0; f < static_cast<int>(names.size()); ++f) {
                const std::string face_name = static_cast<godot::String>(names[f]).utf8().get_data();
                const uint8_t bit = shape_face_from_name(face_name);
                if (bit == 0xFF) {
                    ERR_PRINT("BlockRegistry: unknown face name \"" +
                              static_cast<godot::String>(names[f]) + "\" in " + owner +
                              " part " + godot::String::num_int64(i) +
                              " — expected n, s, e, w, up or down");
                    continue;
                }
                mask |= static_cast<uint8_t>(1u << bit);
            }
            if (mask != 0) {
                part.faces = mask;
                part.faces_declared = true;
            }
        }
        out.push_back(std::move(part));
    }
}

void parse_shape_dict(const godot::Dictionary& sd, BlockShape& shape,
                      const godot::String& owner) {
    if (sd.has("selection_boxes")) {
        parse_aabb_array(sd["selection_boxes"], shape.selection_boxes);
    }
    if (sd.has("collision_boxes")) {
        parse_aabb_array(sd["collision_boxes"], shape.collision_boxes);
    }
    if (sd.has("parts")) {
        parse_parts_array(sd["parts"], shape.parts, owner);
    }
}

// Copy a shape into a block, then DERIVE the two static lists from the parts.
//
// Deriving rather than trusting the file matters because three readers see a
// shape: the mesher and outline (through the resolver, so always current), and
// the GDScript icon/viewmodel code (which reads data/block_shapes.json directly
// and has no world to resolve against). Those two views have to agree, so the
// declared list is only kept if the parts reproduce it — otherwise the file has
// drifted and the icon in the hand would show a different model from the world.
void apply_shape_to_block(const BlockShape& shape, BlockType& bt, const godot::String& block_name,
                          const godot::String& shape_name) {
    bt.selection_boxes = shape.selection_boxes;
    bt.collision_boxes = shape.collision_boxes;
    bt.parts = shape.parts;

    // A stair's step face is read off its variant name, the same winding the shape
    // file uses: stair/n has its step against the cell's -Z face. It lives on the
    // block so a neighbouring stair's corner rule needs nothing but that block.
    const std::string shape_key = shape_name.utf8().get_data();
    if (shape_key.rfind("stair/", 0) == 0) {
        std::string variant = shape_key.substr(6);
        // A hanging stair is the same five rules on mirrored geometry, so `_up` is
        // the whole of the difference: it names the way up, and the face is read
        // from what is left of the name exactly as for an upright one.
        bool hanging = false;
        if (variant.size() > 3 && variant.compare(variant.size() - 3, 3, "_up") == 0) {
            hanging = true;
            variant.erase(variant.size() - 3);
        }
        if (variant == "n")      bt.stair_step_face = static_cast<uint8_t>(ShapeFace::Back);
        else if (variant == "s") bt.stair_step_face = static_cast<uint8_t>(ShapeFace::Front);
        else if (variant == "e") bt.stair_step_face = static_cast<uint8_t>(ShapeFace::Right);
        else if (variant == "w") bt.stair_step_face = static_cast<uint8_t>(ShapeFace::Left);
        bt.stair_hanging = hanging && bt.stair_step_face != kNoStairFace;
    }

    if (bt.parts.empty()) return;

    // A rule that asks about something other than the faces its boxes reach supplies
    // those faces itself, now that the block's orientation is known. Loading them from
    // the data file instead would mean spelling out, for every variant, which of the
    // six faces a claim is read on — the same fact the rule already encodes.
    for (ShapePart& part : bt.parts) {
        const uint8_t from_rule = shape_rule_faces_for(part.rule, bt);
        if (from_rule == 0) continue;
        if (part.faces_declared && part.faces != from_rule) {
            WARN_PRINT("BlockRegistry: shape \"" + shape_name +
                       "\" declares faces that are not the ones rule \"" +
                       godot::String(shape_rule_name(part.rule)) +
                       "\" reads; the rule wins, because a claim answered from the wrong "
                       "neighbour draws the part in the wrong place rather than not at all");
        }
        part.faces = from_rule;
    }

    bool any_collision = false;
    for (const ShapePart& part : bt.parts) {
        if (part.rule != ShapeRule::None) {
            // Only connector rules answer "is that neighbour the same kind of thing as
            // me", which is the one property a shape can carry. A stair's step, its cut
            // remnants and its corners are four different rules on purpose.
            if (shape_rule_is_connector(part.rule)) {
                if (bt.connector == ShapeRule::None) {
                    bt.connector = part.rule;
                } else if (bt.connector != part.rule) {
                    ERR_PRINT("BlockRegistry: shape \"" + shape_name + "\" mixes rules \"" +
                              godot::String(shape_rule_name(bt.connector)) + "\" and \"" +
                              godot::String(shape_rule_name(part.rule)) +
                              "\"; a neighbour asking \"are you the same kind of thing\" gets "
                              "one answer, so the first rule wins");
                }
            }
            if (!shape_rule_self_decided(part.rule) && part.faces == 0) {
                ERR_PRINT("BlockRegistry: shape \"" + shape_name +
                          "\" puts a rule on a part that reaches no cell face, so nothing can "
                          "ever claim it; it is drawn unconditionally");
            } else if (!part.faces_declared && shape_rule_faces_for(part.rule, bt) == 0 &&
                       (part.faces & ~shape_box_faces(part.boxes)) != 0) {
                // A claim on a face the part does not even touch is always false,
                // which would silently delete the part, so it is a load error rather
                // than a quiet no. Only geometry-driven claims are checked this way: a
                // rule-supplied claim deliberately asks about faces the part's boxes do
                // not reach, and a declared list is the author's own statement.
                ERR_PRINT("BlockRegistry: shape \"" + shape_name +
                          "\" claims a face its part does not reach; the part would never be "
                          "drawn");
            }
        }
        if (!part.collision_boxes.empty()) any_collision = true;
    }

    ShapeBoxes canonical;
    resolve_canonical_boxes(bt, ShapeBoxKind::Selection, canonical);
    if (canonical.overflowed) {
        ERR_PRINT("BlockRegistry: shape \"" + shape_name + "\" for block \"" + block_name +
                  "\" resolves to more than " + godot::String::num_int64(ShapeBoxes::kCapacity) +
                  " boxes, which the resolver cannot carry; raise ShapeBoxes::kCapacity");
    }
    std::vector<BlockAABB> derived(canonical.begin(), canonical.end());
    if (!box_lists_equal(derived, shape.selection_boxes)) {
        WARN_PRINT("BlockRegistry: shape \"" + shape_name +
                   "\" is part-based but its \"selection_boxes\" is not the canonical "
                   "flattening of those parts; block_shapes.json needs updating (the parts win "
                   "for the world, the file is what the inventory icon draws)");
    }
    bt.selection_boxes = std::move(derived);

    if (any_collision) {
        ShapeBoxes canonical_collision;
        resolve_canonical_boxes(bt, ShapeBoxKind::Collision, canonical_collision);
        std::vector<BlockAABB> derived_collision(canonical_collision.begin(),
                                                canonical_collision.end());
        if (!box_lists_equal(derived_collision, shape.collision_boxes)) {
            WARN_PRINT("BlockRegistry: shape \"" + shape_name +
                       "\" is part-based but its \"collision_boxes\" is not the canonical "
                       "flattening of those parts; block_shapes.json needs updating");
        }
        bt.collision_boxes = std::move(derived_collision);
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
            if (group.has("selection_boxes") || group.has("parts")) {
                // Leaf shape (e.g. "pole")
                BlockShape shape;
                parse_shape_dict(group, shape, godot::String(prefix.c_str()));
                shapes[prefix] = std::move(shape);
            } else {
                // Variant group (e.g. "stair" -> "n", "s", ...)
                godot::Array sub_keys = group.keys();
                for (int j = 0; j < static_cast<int>(sub_keys.size()); ++j) {
                    godot::String sub_key = sub_keys[j];
                    godot::Dictionary sub_val = group[sub_key];
                    std::string full_name = prefix + "/" + sub_key.utf8().get_data();

                    BlockShape shape;
                    parse_shape_dict(sub_val, shape, godot::String(full_name.c_str()));
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

        // light_opacity (extra light levels removed crossing this block;
        // clamped to the 0..15 light scale, 15 = stops light like opaque)
        if (d.has("light_opacity")) {
            int64_t opacity = static_cast<int64_t>(d["light_opacity"]);
            if (opacity < 0) opacity = 0;
            if (opacity > 15) opacity = 15;
            bt.light_opacity = static_cast<uint8_t>(opacity);
        }

        // hardness (break time in seconds; -1.0 = unbreakable)
        if (d.has("hardness")) {
            bt.hardness = static_cast<float>(static_cast<double>(d["hardness"]));
        }

        // preferred_tool (tool class mined fastest against this block; "" = none)
        if (d.has("preferred_tool")) {
            bt.preferred_tool = godot::String(d["preferred_tool"]).utf8().get_data();
        }

        // min_tier (tool tier required for the preferred-tool speed bonus)
        if (d.has("min_tier")) {
            bt.min_tier = static_cast<int32_t>(static_cast<int64_t>(d["min_tier"]));
            if (bt.min_tier < 0) bt.min_tier = 0;
        }

        // fluid (which fluid state this block IS; see fluids/fluid_state_table.hpp)
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

    // Resolve the cross-block references ("drops", "crush_result") to ids. This
    // runs as a pass of its own (like the families below) because a target is
    // normally defined later in the file than the block naming it, so its id
    // does not exist yet while that block's own entry is being parsed.
    for (int i = 0; i < static_cast<int>(blocks_arr.size()); ++i) {
        godot::Dictionary d = blocks_arr[i];
        BlockType* bt = get_block_mutable(static_cast<BlockID>(i));
        if (bt == nullptr) continue;
        // The message is built inside and printed outside: ERR_PRINT expands to
        // __FUNCTION__, which inside a lambda names its call operator, so the log
        // would say "operator()" rather than where this actually went wrong.
        const auto resolve_ref = [&](const char* field, BlockID& out) -> godot::String {
            if (!d.has(field)) return godot::String();
            const godot::String target_name = d[field];
            const BlockID target = get_block_id_by_name(target_name.utf8().get_data());
            if (target == 0) {
                return "BlockRegistry: unknown " + godot::String(field) + " \"" + target_name +
                       "\" for block \"" + godot::String(d["name"]) + "\"";
            }
            out = target;
            return godot::String();
        };
        const godot::String drops_error = resolve_ref("drops", bt->drops);
        if (!drops_error.is_empty()) ERR_PRINT(drops_error);
        const godot::String crush_error = resolve_ref("crush_result", bt->crush_result);
        if (!crush_error.is_empty()) ERR_PRINT(crush_error);
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
        // Every body entry carries the same connected shape, so a family is not
        // four orientations any more and the id a wall was placed as says nothing
        // about what it draws. Two questions the shapes cannot answer are what is
        // left: which id a mined wall drops -- the body the inventory offers, which
        // is declared first for every material -- and which id a wall thickens into,
        // which is the entry with no shape at all. The retired `wall/{n,s,e,w}`
        // names stay recognised so an older shape file still builds a family rather
        // than silently losing the drop.
        const bool body = shape == "wall/all" || shape == "wall/n" || shape == "wall/s" ||
                          shape == "wall/e" || shape == "wall/w";
        if (body) {
            if (fam.base == 0) fam.base = id;
        } else {
            fam.full = id;
        }
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
        // Water's light cost (see light_opacity). The ocean is the big one: a
        // generated seabed is dark, not a glow-lit floor.
        bt.light_opacity = 3;
        // Deliberately NOT a fluid state. The flowing simulation is the dynamic
        // water a player pours, and generated ocean is not part of it: left out
        // of the state table, the ocean never ticks, so it neither spills runoff
        // along its shores when something nearby is edited nor drains through a
        // channel. It is still a wall to the flow (see blocks_fluid), so a poured
        // bucket pools against the sea instead of writing ocean cells away.
        // Giving this a `fluid` state is what would make oceans live — one line
        // in block_definitions.json — and the spill along every flat shore at sea
        // level is the cost of it.
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
        // A source: this is the block a poured bucket places, and the block a
        // pool settles into.
        bt.fluid_kind = FluidKind::Water;
        bt.light_opacity = 3;
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

    // Fluid states. APPENDED, never inserted: ids are positional, so a new
    // entry here would re-point every id after it. Nothing in the fluid system
    // depends on these particular values either — it resolves states by name
    // (fluids/fluid_state_table.hpp) — which is why the test registry and the
    // game registry can number them differently and still agree on behaviour.
    // The surface of a flowing cell drops with its depth — the reference's
    // liquid height, offset = (depth + 1) / 9 rounded to hundredths — which is
    // what makes a stream slope down instead of sitting at one flat height.
    // Depth 0 is not in this table: a source keeps the established 0.12 of the
    // `water` block above, and a falling cell is FULL height (offset 0), because
    // water in a column is only ever full strength.
    static constexpr float kRunoffOffset[8] = { 0.0f, 0.22f, 0.33f, 0.44f, 0.56f, 0.67f, 0.78f, 0.89f };
    const auto fluid_state = [&](const char* name, FluidKind kind, uint8_t depth, bool falling, uint8_t opacity) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset =
            (falling || depth >= 8) ? 0.0f : kRunoffOffset[depth];
        bt.slipperiness = 0.6f;
        bt.hardness = -1.0f;  // unbreakable, like water
        bt.full_cube_ = true;
        bt.fluid_kind = kind;
        bt.fluid_depth = depth;
        bt.fluid_falling = falling;
        bt.light_opacity = opacity;
        register_block(bt);
    };
    fluid_state("water_runoff_1", FluidKind::Water, 1, false, 3);
    fluid_state("water_runoff_2", FluidKind::Water, 2, false, 3);
    fluid_state("water_runoff_3", FluidKind::Water, 3, false, 3);
    fluid_state("water_runoff_4", FluidKind::Water, 4, false, 3);
    fluid_state("water_runoff_5", FluidKind::Water, 5, false, 3);
    fluid_state("water_runoff_6", FluidKind::Water, 6, false, 3);
    fluid_state("water_runoff_7", FluidKind::Water, 7, false, 3);
    fluid_state("water_fallen", FluidKind::Water, 0, true, 3);

    // The other two fluids, appended for the same reason as everything else
    // here. Their sources are registered next to their runoff because both
    // halves are the same block with a fluid state on it, and the state table
    // (fluids/fluid_state_table.hpp) finds every one of them by scanning this
    // registry — the same scan the game's JSON goes through.
    const auto fluid_source = [&](const char* name, FluidKind kind, uint8_t opacity) {
        BlockType bt{};
        bt.name = name;
        bt.properties = BlockProperty::Liquid | BlockProperty::Transparent;
        bt.visible_faces = {true, true, true, true, true, true};
        bt.light_pattern = LightEmissionPattern::Diamond;
        bt.top_face_offset = 0.12f;
        bt.slipperiness = 0.6f;
        bt.hardness = -1.0f;
        bt.full_cube_ = true;
        bt.fluid_kind = kind;
        bt.light_opacity = opacity;
        register_block(bt);
    };
    // Lava stops at depth three (see fluid_rules.cpp), so it has no states
    // deeper than that to store — a depth nothing can reach needs no block.
    fluid_source("lava", FluidKind::Lava, 15);
    fluid_state("lava_runoff_1", FluidKind::Lava, 1, false, 15);
    fluid_state("lava_runoff_2", FluidKind::Lava, 2, false, 15);
    fluid_state("lava_runoff_3", FluidKind::Lava, 3, false, 15);
    fluid_state("lava_fallen", FluidKind::Lava, 0, true, 15);

    fluid_source("acid", FluidKind::Acid, 3);
    fluid_state("acid_runoff_1", FluidKind::Acid, 1, false, 3);
    fluid_state("acid_runoff_2", FluidKind::Acid, 2, false, 3);
    fluid_state("acid_runoff_3", FluidKind::Acid, 3, false, 3);
    fluid_state("acid_runoff_4", FluidKind::Acid, 4, false, 3);
    fluid_state("acid_runoff_5", FluidKind::Acid, 5, false, 3);
    fluid_state("acid_runoff_6", FluidKind::Acid, 6, false, 3);
    fluid_state("acid_runoff_7", FluidKind::Acid, 7, false, 3);
    fluid_state("acid_fallen", FluidKind::Acid, 0, true, 3);
}

} // namespace VoxelEngine
