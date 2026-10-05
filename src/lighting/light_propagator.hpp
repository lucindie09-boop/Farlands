#ifndef FARLANDS_LIGHT_PROPAGATOR_HPP
#define FARLANDS_LIGHT_PROPAGATOR_HPP
#include "lighting/light_propagation.hpp"
#include "core/chunk_map.hpp"
#include "core/block_types.hpp"
#include <vector>
#include <unordered_set>
#include <mutex>

namespace VoxelEngine {

class MeshManager;

class LightPropagator {
public:
    void set_chunk_map(ChunkMap* cm) { chunk_map = cm; }
    void set_mesh_manager(MeshManager* mm) { mesh_manager = mm; }

    // Public wrappers: acquire exclusive lock, call _locked, release lock, then dirty-mark.
    // The pipeline reaches light through exactly three entries — the region pass,
    // the incremental block change, and try_fixup_chunk. The BFS primitives below
    // (_locked variants) are internal: a caller that took the BFS wrappers directly
    // would have to reproduce their lock-set arithmetic, and the one former public
    // entry that scanned a chunk's lights without holding the shard's lock
    // (propagate_from_existing_light) was removed rather than kept as a trap.
    void propagate_block_light_region(int32_t cx, int32_t cy, int32_t cz);
    void update_block_light_incremental(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b);

    // _locked BFS variants: caller MUST already hold lock_all_exclusive().
    // Uses _fast accessors only. MUST NOT call mark_chunks_dirty_for_light.
    void light_propagate_add_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& queue);
    void light_propagate_remove_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& remove_queue, std::vector<LightNode>& add_queue);
    void update_block_light_incremental_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b);

    void try_fixup_chunk(uint64_t key, int32_t cx, int32_t cy, int32_t cz);

    // --- Sky light -----------------------------------------------------------------
    // Sky light is a scalar with the same one-step rules as the block-light BFS
    // (a step into a cell costs 1 plus that cell's light_opacity, opaque cells
    // take nothing), so it has the same pair of primitives. What makes it different
    // is where the light comes from: the per-column scan is its emitter, and a
    // cell's own column result is a source even at a low level (a pool's surface
    // cell is a legitimate source for the water beside it).
    //
    // Both walks are bounded to the 3x3x3 band their caller holds: a chain from a
    // seed cannot leave that band because the level is at most 15 and a chunk is
    // 32 wide, and a write outside it would be a write under someone else's lock.

    // Relax `queue` outward, raising cells as it goes. When `modified_out` is
    // given it receives a 27-bit mask of the chunks this pass actually WROTE
    // (BlockLightRegion::slot_bit layout, relative to region_cx/cy/cz), so a
    // caller can mesh exactly what changed and nothing else.
    void sky_propagate_add_locked(std::vector<SkyNode>& queue,
                                  int32_t region_cx, int32_t region_cy, int32_t region_cz,
                                  uint32_t* modified_out = nullptr);

    // Clear the light `remove_queue` explains -- each node carries the level its
    // cell held while it was the source -- and fill `add_queue` with the cells
    // whose own light survives, to be relaxed afterwards by the add walk. No mask:
    // the one caller of the pair is the block-edit path, which dirties the whole
    // 3x3x3 neighborhood afterwards regardless.
    void sky_propagate_remove_locked(std::vector<SkyNode>& remove_queue,
                                     std::vector<SkyNode>& add_queue,
                                     int32_t region_cx, int32_t region_cy, int32_t region_cz);

    // Re-scans one column's sky light after a block changed and applies what that
    // means for the volume around it: a placed block casts its dimple, a removed
    // one lets the column's light back out through the sides. Caller MUST already
    // hold the exclusive band around (cx, cy, cz).
    void sky_light_recompute_column_locked(int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t z);

    // The same for many columns of one chunk at once, each packed as (x << 16) | z --
    // the packing the paste path already keeps. A pasted roof shades the volume
    // under every one of its columns, so the changes go in as one batch and the
    // volume is walked once instead of once per column. Returns the 27-bit mask of
    // the chunks it wrote (BlockLightRegion::slot_bit layout), so a caller that
    // holds the band can dirty those meshes once it has released it.
    uint32_t sky_light_recompute_columns_locked(int32_t cx, int32_t cy, int32_t cz,
                                                const std::vector<uint32_t>& columns);

    // Additive sky pass for a chunk that has just been installed: light spreads
    // into shade around the new chunk and out of it. No wipe, no region rebuild --
    // the arriving chunk's own column scan is the source of truth for its cells,
    // and a neighbour's light only ever IMPROVES them. Returns the same 27-bit
    // modified mask, or 0 when there was nothing to fill.
    //
    // The public form takes the band and marks the meshes of what it changed, the
    // way propagate_block_light_region does. The `_locked` form is for the install
    // worker, which is already inside one and must not re-acquire it.
    uint32_t scatter_sky_light_region(int32_t cx, int32_t cy, int32_t cz);
    uint32_t scatter_sky_light_region_locked(int32_t cx, int32_t cy, int32_t cz);

private:
    // `modified_out`, when given, receives a 27-bit mask of the chunks the pass
    // actually wrote light to (see BlockLightRegion::slot_bit). The public wrapper
    // uses it so a region pass marks only the chunks whose light changed.
    void propagate_block_light_region_locked(int32_t cx, int32_t cy, int32_t cz,
                                            uint32_t* modified_out = nullptr);

    ChunkMap* chunk_map = nullptr;
    MeshManager* mesh_manager = nullptr;
    mutable std::mutex pending_light_removals_mutex_;
    std::unordered_set<uint64_t> pending_light_removals_;

    // Thread-local buffers for light propagation queues (avoid repeated allocation)
    static thread_local std::vector<LightNode> add_queue_buffer;
    static thread_local std::vector<LightNode> remove_queue_buffer;
    static thread_local std::vector<SkyNode> sky_add_buffer;
    static thread_local std::vector<SkyNode> sky_remove_buffer;
};

} // namespace VoxelEngine

#endif // FARLANDS_LIGHT_PROPAGATOR_HPP
