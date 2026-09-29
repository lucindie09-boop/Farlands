// The passes that run once every entry of data/block_definitions.json has been
// registered: the cross-block references ("drops", "crush_result") resolve against
// ids, and the slab/stair/wall family tables are built by name. They used to be the
// tail of core/block_types_load.cpp, which owns the reader that calls them; they are
// members of the registry because each one writes a private table.
//
// All four read the block array a second time rather than hanging their work off the
// parse, because a family groups entries declared apart from each other: a reference
// normally names a block defined LATER in the file, so its id does not exist yet
// while its own entry is being parsed.

#include "core/block_types.hpp"

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>

#include <string>

namespace VoxelEngine {

void BlockRegistry::resolve_block_references(const godot::Array& blocks_arr) noexcept {
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
}

void BlockRegistry::build_slab_families(const godot::Array& blocks_arr) noexcept {
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
}

void BlockRegistry::build_stair_families(const godot::Array& blocks_arr) noexcept {
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
}

void BlockRegistry::build_wall_families(const godot::Array& blocks_arr) noexcept {
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
}

} // namespace VoxelEngine
#endif  // FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
