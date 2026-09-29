// data/block_shapes.json: the part/AABB vocabulary, the derivation that turns a
// shape's parts into its two static box lists, and the entry point that reads the
// file. The block definitions (data/block_definitions.json) are in
// core/block_types_load.cpp.

#include "core/block_types.hpp"
#include "core/block_types_internal.hpp"
#include "core/shape_resolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
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

} // anonymous namespace

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

} // namespace VoxelEngine
