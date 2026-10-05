#include "world/block_editor.hpp"
#include "world/chunk_world.hpp"
#include "mesh/mesh_manager.hpp"
#include "lighting/light_propagator.hpp"
#include "core/block_types.hpp"

namespace VoxelEngine {
using namespace godot;

BlockEditor::BlockEditor(ChunkWorld* cw, MeshManager* mm, LightPropagator* lp)
    : chunk_world(cw), mesh_manager(mm), light_propagator(lp) {}

int BlockEditor::query_block(int32_t world_x, int32_t world_y, int32_t world_z) const {
    return chunk_world->get_block_world(world_x, world_y, world_z);
}

void BlockEditor::place_block(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block) {
    int32_t chunk_x, chunk_y, chunk_z, local_x, local_y, local_z;
    world_to_chunk_local(world_x, world_y, world_z, chunk_x, chunk_y, chunk_z, local_x, local_y, local_z);

    ChunkMap& cm = chunk_world->get_chunk_map();

    // Quick check: if chunk not loaded, queue pending placement and bail.
    // Uses shared lock only — safe before exclusive scope.
    {
        auto sl = cm.lock_chunk(chunk_x, chunk_y, chunk_z);
        if (!cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z)) {
            chunk_world->queue_pending_placement(world_x, world_y, world_z, static_cast<int>(new_block));
            return;
        }
    }

    // Track mud variant chunk for post-lock edit map write.
    int32_t mud_cx = 0, mud_cy = 0, mud_cz = 0;
    int32_t mud_lx = 0, mud_ly = 0, mud_lz = 0;
    BlockID mud_variant = BlockIDs::AIR;
    bool did_mud = false;

    // Calculate mud variant local coordinates once (world_to_chunk_local needs chunk coords)
    int32_t below_world_y = world_y - 1;
    int32_t bw_cx, bw_cy, bw_cz;
    world_to_chunk_local(world_x, below_world_y, world_z, bw_cx, bw_cy, bw_cz, mud_lx, mud_ly, mud_lz);

    {
        // Block change + light propagation can reach at most 1 chunk in each
        // direction (max light level 15 < chunk size 32). Lock 3×3×3 around
        // the center chunk — covers block write, light BFS, and mud variant.
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = cm.get_chunk_key(chunk_x + dx, chunk_y + dy, chunk_z + dz);
        auto lock = cm.lock_keys_exclusive(keys);
        ChunkData* chunk_data = cm.get_chunk_data_fast(chunk_x, chunk_y, chunk_z);
        if (!chunk_data) return;
        if (!is_local_in_bounds(local_x, local_y, local_z)) return;

        const BlockID old_block = chunk_data->get_block_unsafe(local_x, local_y, local_z);
        const auto& reg = VoxelEngine::BlockRegistry::get_instance();
        const auto* new_slab_fam = reg.get_slab_family(new_block);
        const auto* new_wall_fam = reg.get_wall_family(new_block);
        const auto* old_wall_fam = reg.get_wall_family(old_block);
        const bool is_merge = (new_slab_fam && new_block == new_slab_fam->full
                               && (old_block == new_slab_fam->bottom || old_block == new_slab_fam->top))
                           || (new_wall_fam && old_wall_fam == new_wall_fam
                               && new_block == new_wall_fam->full);
        if (old_block != BlockIDs::AIR && new_block != BlockIDs::AIR && !is_merge) return;
        if (old_block == new_block) return;

        const BlockRegistry& registry = BlockRegistry::get_instance();
        const BlockType& old_type = registry.get_block(old_block);
        const BlockType& new_type = registry.get_block(new_block);
        const bool old_opaque = HasProperty(old_type.properties, BlockProperty::Opaque);
        const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);

        const uint8_t old_r = chunk_data->get_light_r(local_x, local_y, local_z);
        const uint8_t old_g = chunk_data->get_light_g(local_x, local_y, local_z);
        const uint8_t old_b = chunk_data->get_light_b(local_x, local_y, local_z);

        chunk_data->set_block(local_x, local_y, local_z, new_block);

        // A change that can move the column's sky light: a block entering or leaving
        // the Opaque set, or one whose own light_opacity differs from what it
        // replaced (a leaf, a pool). The recompute re-scans the column and then
        // un-propagates what the column used to hand out sideways and re-relaxes
        // what survives, so placing one block in mid-air casts a dimple rather than
        // a shaft, and breaking one lets the light back in.
        if (old_opaque != new_opaque || old_type.light_opacity != new_type.light_opacity) {
            light_propagator->sky_light_recompute_column_locked(chunk_x, chunk_y, chunk_z, local_x, local_z);
        }

        light_propagator->update_block_light_incremental_locked(
            chunk_x, chunk_y, chunk_z, chunk_x, chunk_y, chunk_z,
            local_x, local_y, local_z,
            old_block, new_block, old_r, old_g, old_b);

        ChunkRenderData* render_data = cm.get_chunk_render_data_fast(chunk_x, chunk_y, chunk_z);
        if (render_data) {
            render_data->is_mesh_dirty = true;
            render_data->mesh_version++;
            render_data->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(local_x, local_y, local_z));
            render_data->mark_block_dirty(local_x, local_y, local_z);
        }

        // Mud variant: kept inline rather than factored out, because the cell it
        // touches has to be read and written under the band this lock already
        // holds (a standalone version re-acquires the shard and deadlocks).
        if (world_y > 0) {
            ChunkData* below_chunk = cm.get_chunk_data_fast(bw_cx, bw_cy, bw_cz);
            if (below_chunk && is_local_in_bounds(mud_lx, mud_ly, mud_lz)) {
                const BlockID below = below_chunk->get_block_unsafe(mud_lx, mud_ly, mud_lz);
                BlockID variant = BlockIDs::AIR;
                if (new_block != BlockIDs::AIR) {
                    if (below == BlockIDs::MUD) variant = BlockIDs::MUD_FULL;
                    else if (below == BlockIDs::WET_SAND) variant = BlockIDs::WET_SAND_FULL;
                } else {
                    if (below == BlockIDs::MUD_FULL) variant = BlockIDs::MUD;
                    else if (below == BlockIDs::WET_SAND_FULL) variant = BlockIDs::WET_SAND;
                }
                if (variant != BlockIDs::AIR && below != variant) {
                    below_chunk->set_block(mud_lx, mud_ly, mud_lz, variant);
                    ChunkRenderData* bw_rd = cm.get_chunk_render_data_fast(bw_cx, bw_cy, bw_cz);
                    if (bw_rd) {
                        bw_rd->is_mesh_dirty = true;
                        bw_rd->mesh_version++;
                        bw_rd->dirty_subchunks |= static_cast<uint8_t>(1 << subchunk_index(mud_lx, mud_ly, mud_lz));
                        bw_rd->mark_block_dirty(mud_lx, mud_ly, mud_lz);
                    }
                    mud_cx = bw_cx;
                    mud_cy = bw_cy;
                    mud_cz = bw_cz;
                    mud_variant = variant;
                    did_mud = true;
                }
            }
        }
    }
    // Lock released — auto-locking accessors are safe now.

    // Persist the edit in the edit map instead of marking the whole chunk dirty
    chunk_world->add_block_edit(chunk_x, chunk_y, chunk_z, local_x, local_y, local_z, new_block);
    // A block change (and any light removal) can touch every chunk in the
    // 3×3×3 neighborhood — including diagonals when a light block near a chunk
    // corner spills light across two boundaries at once. Bump mesh_version on
    // the whole neighborhood so rebuild_rendering_server_mesh's version gate
    // lets every affected chunk remesh (the old queue_player_edit_chunk_refresh
    // only set is_mesh_dirty, which the gate silently discarded).
    mesh_manager->mark_chunks_dirty_for_light(chunk_x, chunk_y, chunk_z);
    if (did_mud) {
        // Persist the mud variant edit as well
        chunk_world->add_block_edit(mud_cx, mud_cy, mud_cz, mud_lx, mud_ly, mud_lz, mud_variant);
        mesh_manager->queue_dirty_chunk(mud_cx, mud_cy, mud_cz);
    }
}

} // namespace VoxelEngine
