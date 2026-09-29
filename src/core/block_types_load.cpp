// data/block_definitions.json: the block reader, and the drop-chain resolution that
// runs once every block is in. The shape file's reader is in
// core/block_types_shapes.cpp; the derivation this calls is declared in
// core/block_types_internal.hpp.

#include "core/block_types.hpp"
#include "core/block_types_internal.hpp"
#include "core/shape_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <deque>
#include <string>
#include <string_view>
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
        // slipperiness. Vanilla's range is 0.6 (most blocks) to 0.98 (ice); a
        // value outside 0.05..1.0 is clamped rather than honoured, because the
        // ground-acceleration arithmetic divides by it (pow(0.6 / s, 3)) and a
        // zero or negative would turn velocities into inf then NaN — the same
        // divisor class of bug hardness above guards.
        if (d.has("slipperiness")) {
            float slip = static_cast<float>(static_cast<double>(d["slipperiness"]));
            bt.slipperiness = std::clamp(slip, 0.05f, 1.0f);
        }

        // light_opacity (extra light levels removed crossing this block;
        // clamped to the 0..15 light scale, 15 = stops light like opaque)
        if (d.has("light_opacity")) {
            int64_t opacity = static_cast<int64_t>(d["light_opacity"]);
            if (opacity < 0) opacity = 0;
            if (opacity > 15) opacity = 15;
            bt.light_opacity = static_cast<uint8_t>(opacity);
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
        // "drops" is the one reference that may name an ITEM (the torch block drops
        // the torch item, so breaking a torch hands back the thing you can place),
        // and items load AFTER blocks. A name that is not a block yet is therefore
        // not an error: it waits for resolve_pending_drops(), which the controller
        // calls once items exist. "crush_result" is always a block and still errors
        // here.
        if (d.has("drops")) {
            const godot::String target_name = d["drops"];
            const BlockID target = get_block_id_by_name(target_name.utf8().get_data());
            if (target != 0) {
                bt->drops = target;
            } else {
                pending_drops_.emplace_back(static_cast<BlockID>(i),
                                            target_name.utf8().get_data());
            }
        }
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
