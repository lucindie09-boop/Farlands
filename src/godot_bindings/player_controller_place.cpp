// Putting things into the world: the single-cell place path (its aim test, the
// merge rules and the mud variant) and the bucket's pour/fill at the aim point.
// Kept apart from godot_bindings/player_controller_interact.cpp, which takes things
// out again.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/chunk_manager.hpp"
#include "core/item_registry.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "world/chunk_world.hpp"

#include <godot_cpp/classes/engine.hpp>

#include <cmath>

using namespace godot;
using namespace VoxelEngine;

namespace {

// Whether a cell is inside the player's body, which is the one write a placement
// and a pour both refuse — otherwise aiming at your own feet walls you in.
bool cell_hits_player_body(const Vector3& player_pos, int bx, int by, int bz) {
    const int px = static_cast<int>(std::floor(player_pos.x));
    const int py = static_cast<int>(std::floor(player_pos.y));
    const int pz = static_cast<int>(std::floor(player_pos.z));
    return bx == px && bz == pz && (by == py || by == py + 1);
}

// What the hand is offering: a block is placed as itself, and an ITEM normally is
// not placeable at all — its id lives in a separate space above the block registry
// and must stay out of voxels — but one may declare the block it puts in the world
// (the torch item places a torch), which is the only bridge. Returns AIR when
// nothing placeable is held, and points `item_place` at the declaration when the
// placement came from an item rather than a block.
BlockID resolve_held_block(BlockID held, const ItemRegistry& items, const ItemPlace*& item_place) {
    item_place = nullptr;
    if (held == 0) return 0;  // Nothing selected
    if (!items.is_item(held)) return held;
    item_place = items.get_item_place(held);
    if (item_place == nullptr || item_place->block == 0) return 0;
    return item_place->block;
}

// Slab auto-detection: merge halves into double slab, or pick top/bottom
// orientation. Works per family (plank slabs, log stumps) so the two never
// cross-merge. May move the target cell (bx/by/bz) onto the half it merges with.
BlockID merge_slab_placement(const BlockRegistry& registry, ChunkManager& cm,
                             BlockID block_to_place, const Dictionary& result,
                             int& bx, int& by, int& bz) {
    const auto* slab_fam = registry.get_slab_family(block_to_place);
    if (slab_fam == nullptr || block_to_place == slab_fam->full) return block_to_place;

    BlockID final_block = block_to_place;
    Vector3 hit_normal = Vector3(result["hit_normal"]);
    BlockID target_block = static_cast<BlockID>(cm.get_block(bx, by, bz));

    if (target_block == slab_fam->bottom || target_block == slab_fam->top) {
        // Target cell already has a half of this family — merge
        final_block = slab_fam->full;
    } else if (std::abs(hit_normal.y) > 0.5) {
        // Horizontal face: check if clicking on an existing half to merge
        Vector3 hit_pos = result["position"];
        int pos_x = static_cast<int>(std::floor(hit_pos.x));
        int pos_y = static_cast<int>(std::floor(hit_pos.y));
        int pos_z = static_cast<int>(std::floor(hit_pos.z));
        BlockID look_block = static_cast<BlockID>(cm.get_block(pos_x, pos_y, pos_z));

        if (look_block == slab_fam->bottom || look_block == slab_fam->top) {
            // Only merge when the face completes the full block:
            // bottom half's top face (+Y) or top half's bottom face (-Y)
            bool merge = (look_block == slab_fam->bottom && hit_normal.y > 0)
                      || (look_block == slab_fam->top && hit_normal.y < 0);
            if (merge) {
                final_block = slab_fam->full;
                bx = pos_x; by = pos_y; bz = pos_z;
            } else {
                final_block = (hit_normal.y > 0) ? slab_fam->bottom : slab_fam->top;
            }
        } else {
            // +Y face = bottom half sits on surface, -Y face = top half hugs ceiling
            final_block = (hit_normal.y > 0) ? slab_fam->bottom : slab_fam->top;
        }
    } else {
        // Side face: upper half of face → top half, lower half → bottom half
        Vector3 hit_point = result["hit_point"];
        double frac_y = hit_point.y - by;
        final_block = (frac_y < 0.5) ? slab_fam->bottom : slab_fam->top;
    }
    return final_block;
}

// Stair auto-detection: pick rotation from face hit and player direction.
BlockID rotate_stair_placement(const BlockRegistry& registry, BlockID block_to_place,
                               const Dictionary& result, int bx, int by, int bz,
                               const Vector3& player_pos) {
    const auto* stair_fam = registry.get_stair_family(block_to_place);
    if (stair_fam == nullptr) return block_to_place;

    BlockID final_block = block_to_place;
    Vector3 hit_normal = Vector3(result["hit_normal"]);

    if (std::abs(hit_normal.y) > 0.5) {
        // Horizontal face: compute player-to-target direction for rotation
        double dx = player_pos.x - (bx + 0.5);
        double dz = player_pos.z - (bz + 0.5);
        bool up = hit_normal.y < 0; // bottom face = upside-down

        if (std::abs(dx) > std::abs(dz)) {
            final_block = (dx > 0)
                ? (up ? stair_fam->w_up : stair_fam->w)
                : (up ? stair_fam->e_up : stair_fam->e);
        } else {
            final_block = (dz > 0)
                ? (up ? stair_fam->n_up : stair_fam->base)
                : (up ? stair_fam->s_up : stair_fam->s);
        }
    } else {
        // Side face: step goes against the clicked face; upper portion = upside-down
        Vector3 hit_point = result["hit_point"];
        double frac_y = hit_point.y - by;
        bool up = (frac_y > 0.75);
        double nx = hit_normal.x, nz = hit_normal.z;
        if (std::abs(nx) > std::abs(nz)) {
            final_block = (nx > 0)
                ? (up ? stair_fam->w_up : stair_fam->w)
                : (up ? stair_fam->e_up : stair_fam->e);
        } else {
            final_block = (nz > 0)
                ? (up ? stair_fam->n_up : stair_fam->base)
                : (up ? stair_fam->s_up : stair_fam->s);
        }
    }
    return final_block;
}

// Wall auto-detection: there is nothing here to orient. All four per-material
// orientations carry the same connected shape, so which way a wall runs is a
// property of the cells around it rather than of the id you placed -- the
// placement is the block you were holding, and the old "which edge of the cell
// does this panel hug" question no longer has an answer to compute. What is left is
// the merge: a wall already standing in the target cell becomes the family's solid
// block instead of being overwritten or refused, which is the one case where two
// wall ids meet in a single cell.
BlockID merge_wall_placement(const BlockRegistry& registry, ChunkManager& cm,
                             BlockID block_to_place, int bx, int by, int bz) {
    const auto* wall_fam = registry.get_wall_family(block_to_place);
    if (wall_fam == nullptr) return block_to_place;

    const BlockID target_block = static_cast<BlockID>(cm.get_block(bx, by, bz));
    const auto* target_wall = registry.get_wall_family(target_block);
    if (target_wall == wall_fam && wall_fam->full != 0 && target_block != wall_fam->full) {
        return wall_fam->full;
    }
    return block_to_place;
}

} // namespace

void PlayerController::place_block() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return;

    // Vanilla container behavior: right-clicking a crafting table uses it
    // (opens its 3x3 menu via the crafting_table_used signal) instead of
    // placing a block against it.
    Dictionary hit = cm->raycast_from_camera(10.0);
    if (hit.get("success", false)) {
        Vector3 hit_pos = hit["position"];
        const int target = cm->get_block(static_cast<int>(std::floor(hit_pos.x)),
                                         static_cast<int>(std::floor(hit_pos.y)),
                                         static_cast<int>(std::floor(hit_pos.z)));
        if (target == static_cast<int>(VoxelEngine::BlockRegistry::get_instance().get_block_id_by_name("crafting_table"))) {
            emit_signal("crafting_table_used");
            return;
        }
    }

    const BlockID held = inventory_.get_selected_block();
    const ItemPlace* item_place = nullptr;
    BlockID block_to_place = resolve_held_block(held, ItemRegistry::get_instance(), item_place);
    if (block_to_place == 0) return; // nothing selected, or nothing placeable in hand

    // Check if we have enough
    if (inventory_.get_selected_count() <= 0) return;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (!result.get("success", false)) return;

    Vector3 place_pos = result["place_position"];
    int bx = static_cast<int>(std::floor(place_pos.x));
    int by = static_cast<int>(std::floor(place_pos.y));
    int bz = static_cast<int>(std::floor(place_pos.z));

    // An item that places a block picks its variant from the clicked face: a
    // torch stands on a top face and hugs the wall on a side face. A face the
    // shape does not offer resolves to AIR, which refuses the placement.
    if (item_place != nullptr) {
        const Vector3 hit_normal = Vector3(result["hit_normal"]);
        block_to_place = item_place->resolve(hit_normal.x, hit_normal.y, hit_normal.z);
        if (block_to_place == 0) return;
    }

    // The shape families: each merge rule reads the cell it would write into (and
    // the clicked half of it) and answers with the id that actually lands.
    const BlockRegistry& registry = VoxelEngine::BlockRegistry::get_instance();
    BlockID final_block = block_to_place;
    if (const auto* slab_fam = registry.get_slab_family(block_to_place);
        slab_fam != nullptr && block_to_place != slab_fam->full) {
        final_block = merge_slab_placement(registry, *cm, block_to_place, result, bx, by, bz);
    }
    if (registry.get_stair_family(block_to_place) != nullptr) {
        final_block = rotate_stair_placement(registry, block_to_place, result, bx, by, bz,
                                             get_global_position());
    }
    if (registry.get_wall_family(block_to_place) != nullptr) {
        final_block = merge_wall_placement(registry, *cm, block_to_place, bx, by, bz);
    }

    Vector3 ppos = get_global_position();
    if (cell_hits_player_body(ppos, bx, by, bz)) return;

    // Don't overwrite an occupied voxel unless a merge changed the block type
    BlockID existing = static_cast<BlockID>(cm->get_block(bx, by, bz));
    if (existing != 0 && final_block == block_to_place) return;

    // Place the block, then verify it actually landed before consuming
    cm->set_block(bx, by, bz, final_block);
    BlockID placed = static_cast<BlockID>(cm->get_block(bx, by, bz));
    if (placed != final_block) return;

    // Consume what was actually held: a placed item consumes the ITEM, not the
    // block it put in the world.
    inventory_.consume_block(item_place != nullptr ? held : block_to_place, 1);

    // Notify that a block actually landed (drives the place swing animation).
    emit_signal("block_placed");

    // Increment edit counter to invalidate block outline
    block_edit_counter_++;
}


bool PlayerController::pour_fluid_at_aim(VoxelEngine::BlockID fluid_block) {
    // A pour target of AIR means items.json named a block that does not exist;
    // that already printed a warning at load, so this is just the guard.
    if (fluid_block == VoxelEngine::BlockIDs::AIR) return false;

    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return false;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return false;

    // Aim at the cell a block would be placed in, so a pour and a placement
    // always agree on where the crosshair is pointing.
    Dictionary result = cm->raycast_from_camera(10.0);
    if (!result.get("success", false)) return false;
    Vector3 place_pos = result["place_position"];
    int bx = static_cast<int>(std::floor(place_pos.x));
    int by = static_cast<int>(std::floor(place_pos.y));
    int bz = static_cast<int>(std::floor(place_pos.z));

    // Same two guards the block path uses: never write into the body, and only
    // into a free cell (a pour has no shape-merging cases, so an occupied cell is
    // simply refused).
    if (cell_hits_player_body(get_global_position(), bx, by, bz)) return false;
    if (cm->get_block(bx, by, bz) != 0) return false;

    cm->set_block(bx, by, bz, fluid_block);
    if (static_cast<VoxelEngine::BlockID>(cm->get_block(bx, by, bz)) != fluid_block) return false;

    // Same feedback as a placement (drives the place swing), and the outline's
    // edit counter so the crosshair target refreshes.
    emit_signal("block_placed");
    block_edit_counter_++;
    return true;
}

bool PlayerController::fill_bucket_at_aim() {
    Node* cm_node = get_node_or_null(NodePath("/root/Main/ChunkManager"));
    if (!cm_node) return false;
    ChunkManager* cm = Object::cast_to<ChunkManager>(cm_node);
    if (!cm) return false;

    Dictionary result = cm->raycast_from_camera(10.0);
    if (!result.get("success", false)) return false;

    const Vector3 hit_pos = result["position"];
    const Vector3 front_pos = result["place_position"];
    const int32_t hit_x = static_cast<int32_t>(std::floor(hit_pos.x));
    const int32_t hit_y = static_cast<int32_t>(std::floor(hit_pos.y));
    const int32_t hit_z = static_cast<int32_t>(std::floor(hit_pos.z));
    const int32_t front_x = static_cast<int32_t>(std::floor(front_pos.x));
    const int32_t front_y = static_cast<int32_t>(std::floor(front_pos.y));
    const int32_t front_z = static_cast<int32_t>(std::floor(front_pos.z));

    const VoxelEngine::BlockRegistry& registry = VoxelEngine::BlockRegistry::get_instance();
    // A source a bucket can take: a declared fluid state at depth 0 that is not
    // the falling column (a fall is full strength but it is not a source, and
    // taking one would only leave a hole the column refills). Generated ocean
    // water is refused because it is not a fluid state at all — the simulation
    // never ticks it, so the hole would be permanent.
    const auto pickable_at = [&](int32_t x, int32_t y, int32_t z,
                                 VoxelEngine::BlockID& out_source,
                                 VoxelEngine::FluidKind& out_kind) {
        const int raw = cm->get_block(x, y, z);
        if (raw <= 0) return false;
        const VoxelEngine::BlockType& bt =
            registry.get_block_fast(static_cast<VoxelEngine::BlockID>(raw));
        if (!bt.is_fluid_state() || bt.fluid_depth != 0 || bt.fluid_falling) return false;
        out_source = static_cast<VoxelEngine::BlockID>(raw);
        out_kind = bt.fluid_kind;
        return true;
    };

    // Two cells can be the one under the crosshair, and both are needed. A
    // fluid is a short block, so looking steeply down lands on the fluid itself;
    // looking flat across a pool passes over its surface and hits the floor
    // under it, where the source is the free cell in front of that floor's face.
    VoxelEngine::BlockID source = VoxelEngine::BlockIDs::AIR;
    VoxelEngine::FluidKind kind = VoxelEngine::FluidKind::None;
    int32_t bx = hit_x, by = hit_y, bz = hit_z;
    if (!pickable_at(hit_x, hit_y, hit_z, source, kind)) {
        if (!pickable_at(front_x, front_y, front_z, source, kind)) return false;
        bx = front_x;
        by = front_y;
        bz = front_z;
    }

    // What the empty container becomes, by name, so the mapping stays in data:
    // water -> water_bucket. No matching item means there is nothing to hand
    // back, so the pickup is refused rather than spilling the fluid.
    std::string filled_name(VoxelEngine::fluid_kind_name(kind));
    filled_name += "_bucket";
    const VoxelEngine::BlockID filled =
        VoxelEngine::ItemRegistry::get_instance().get_item_id_by_name(filled_name.c_str());
    if (filled == VoxelEngine::BlockIDs::AIR) {
        WARN_PRINT("bucket pickup: no item named \"" + String(filled_name.c_str())
                   + "\" for that fluid, so nothing was taken");
        return false;
    }

    // Room for the swap first: emptying the cell and only then discovering the
    // filled bucket does not fit would lose the fluid.
    const VoxelEngine::BlockID held = inventory_.get_selected_block();
    const int slot = inventory_.get_selected_slot();
    const int held_count = inventory_.get_hotbar_slot(slot).count;
    if (held_count > 1 && !inventory_.can_add_block(filled, 1)) return false;

    // Empty the cell. The edit notifies the fluid simulation (see
    // ChunkWorld::add_block_edit), so the neighbours wake and settle: a gap in a
    // stream is filled back in by the water around it.
    cm->set_block(bx, by, bz, 0);
    if (cm->get_block(bx, by, bz) != 0) return false;

    if (held_count > 1) {
        inventory_.set_hotbar_slot(slot, held, held_count - 1);
        inventory_.add_block(filled, 1);
    } else {
        inventory_.set_hotbar_slot(slot, filled, 1);
    }

    // Same feedback as a placement (drives the place swing), and the outline's
    // edit counter so the crosshair target refreshes.
    emit_signal("block_placed");
    block_edit_counter_++;
    return true;
}
