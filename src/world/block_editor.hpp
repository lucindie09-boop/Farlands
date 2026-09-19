#ifndef FARLANDS_BLOCK_EDITOR_HPP
#define FARLANDS_BLOCK_EDITOR_HPP
#include <godot_cpp/variant/vector3.hpp>
#include <cstdint>
#include <cmath>
#include <string>
#include <vector>

#include "core/chunk_types.hpp"
#include "schematic/paste_plan.hpp"

namespace VoxelEngine {

class ChunkWorld;
class MeshManager;
class LightPropagator;

struct RaycastResult {
    bool success = false;
    godot::Vector3 position;
    godot::Vector3 place_position;
    int block_id = 0;
    godot::Vector3 hit_normal;
    godot::Vector3 hit_point;
};

// What a bulk paste did, and what it declined. `skipped_covered` is the one the
// caller cannot predict from the plan: it counts cells the world already had a
// block in, which `PasteOptions::replace_solid = false` turns into a no-op.
struct PasteWriteResult {
    size_t written = 0;
    size_t skipped_covered = 0;
    size_t skipped_unchanged = 0;
    size_t skipped_unloaded = 0;   // cell's chunk is not resident
    size_t skipped_out_of_bounds = 0;
    size_t chunks_touched = 0;
    int32_t min_x = 0, max_x = -1, min_y = 0, max_y = -1, min_z = 0, max_z = -1;
    bool undo_available = false;
};

class BlockEditor {
public:
    BlockEditor(ChunkWorld* cw, MeshManager* mm, LightPropagator* lp);

    void place_block(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    int query_block(int32_t world_x, int32_t world_y, int32_t world_z) const;

    // Writes a whole planned paste in one pass. Per cell this is place_block's
    // write path, but the locking, the light work and the mesh/undo bookkeeping
    // are done once for the volume instead of once per block: a few thousand
    // single-block edits would take a 3×3×3 exclusive lock and a light update
    // each, which is what makes the difference between a paste and a stall.
    //
    // Unlike place_block it may REPLACE an existing block (that is the point —
    // a build dropped on terrain would otherwise lose every cell it overlaps),
    // which is why it is its own entry point rather than a loop over that one.
    // The displaced blocks are kept as the undo record; a second paste replaces
    // it, so undo is one level deep and always describes the last paste.
    PasteWriteResult apply_paste(const schematic::PastePlan& plan,
                                 const schematic::PasteOptions& options);

    // Puts the last paste back, through the same writer. Returns false when there
    // is nothing to undo. The record is cleared only if the revert wrote
    // something, so a failed revert can be retried.
    bool undo_paste(PasteWriteResult* out = nullptr);
    [[nodiscard]] bool has_paste_undo() const noexcept { return paste_undo_.valid(); }
    // Size of the volume the last paste displaced (0 when there is no undo).
    [[nodiscard]] size_t paste_undo_cells() const noexcept { return paste_undo_.cells.size(); }

    RaycastResult raycast_from_ray(const godot::Vector3& origin,
                                    const godot::Vector3& direction,
                                    double max_distance) const;

private:
    ChunkWorld* chunk_world;
    MeshManager* mesh_manager;
    LightPropagator* light_propagator;
    // One level deep, and only ever written by apply_paste/undo_paste: the
    // displaced blocks of the last paste, in the order they were written.
    schematic::PasteUndo paste_undo_;

    void set_block_variant(int32_t world_x, int32_t world_y, int32_t world_z, BlockID block_id);
    void update_mud_variants(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block);
    void post_block_change(int32_t world_x, int32_t world_y, int32_t world_z, BlockID new_block);

    bool is_local_in_bounds(int32_t local_x, int32_t local_y, int32_t local_z) const {
        return local_x >= 0 && local_x < CHUNK_WIDTH &&
               local_y >= 0 && local_y < CHUNK_HEIGHT &&
               local_z >= 0 && local_z < CHUNK_DEPTH;
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_BLOCK_EDITOR_HPP