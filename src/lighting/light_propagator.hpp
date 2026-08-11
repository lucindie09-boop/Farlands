#ifndef FUK_MINECRAFT_LIGHT_PROPAGATOR_HPP
#define FUK_MINECRAFT_LIGHT_PROPAGATOR_HPP
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
    void propagate_block_light_region(int32_t cx, int32_t cy, int32_t cz);
    void propagate_from_existing_light(int32_t cx, int32_t cy, int32_t cz);
    void light_propagate_add(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& queue);
    void light_propagate_remove(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& remove_queue, std::vector<LightNode>& add_queue);
    void update_block_light_incremental(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b);

    // _locked BFS variants: caller MUST already hold lock_all_exclusive().
    // Uses _fast accessors only. MUST NOT call mark_chunks_dirty_for_light.
    void light_propagate_add_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& queue);
    void light_propagate_remove_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& remove_queue, std::vector<LightNode>& add_queue);
    void update_block_light_incremental_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b);

    void try_fixup_chunk(uint64_t key, int32_t cx, int32_t cy, int32_t cz);

private:
    void propagate_block_light_region_locked(int32_t cx, int32_t cy, int32_t cz);

    ChunkMap* chunk_map = nullptr;
    MeshManager* mesh_manager = nullptr;
    mutable std::mutex pending_light_removals_mutex_;
    std::unordered_set<uint64_t> pending_light_removals_;

    // Thread-local buffers for light propagation queues (avoid repeated allocation)
    static thread_local std::vector<LightNode> add_queue_buffer;
    static thread_local std::vector<LightNode> remove_queue_buffer;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_LIGHT_PROPAGATOR_HPP
