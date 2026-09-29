#include "lighting/light_propagator.hpp"
#include "lighting/block_light_region.hpp"

#include "mesh/mesh_manager.hpp"
#include "core/chunk_data.hpp"
#include <godot_cpp/core/print_string.hpp>
#include <godot_cpp/variant/string.hpp>
#include <algorithm>
#include <unordered_set>

namespace {

// A light BFS's thread-local queue is kept allocated between calls so the
// steady case (a torch placed or broken) never allocates. A pathological call
// — clearing a whole built-up region at once — can push it far past that, and
// the memory would then sit pinned in the worker thread forever. Past this
// cap the buffer is handed back: the next call re-reserves the small steady
// size and the spike is paid for once, by the frame that caused it.
constexpr size_t kQueueBufferTrimCapacity = 4096;

} // namespace

using namespace godot;

namespace VoxelEngine {

namespace {

// Shrinks `buffer` when its capacity ran past the cap. Returns it unchanged
// (the common case: capacity under the cap, nothing to do).
void trim_queue_buffer(std::vector<LightNode>& buffer) {
    if (buffer.capacity() > kQueueBufferTrimCapacity) {
        std::vector<LightNode> trimmed;
        trimmed.reserve(64);
        buffer.swap(trimmed);
    }
}

} // namespace

// Thread-local buffers for light propagation queues
thread_local std::vector<LightNode> LightPropagator::add_queue_buffer;
thread_local std::vector<LightNode> LightPropagator::remove_queue_buffer;

// -------------------------------------------------------------------------
// Public wrapper: acquire lock, call _locked, release, dirty-mark.
// -------------------------------------------------------------------------
void LightPropagator::propagate_block_light_region(int32_t cx, int32_t cy, int32_t cz) {
    ChunkData* chunk = chunk_map->get_chunk_data(cx, cy, cz);
    if (!chunk) return;
    uint32_t modified = 0;
    {
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = chunk_map->get_chunk_key(cx + dx, cy + dy, cz + dz);
        auto lock = chunk_map->lock_keys_exclusive(keys);
        propagate_block_light_region_locked(cx, cy, cz, &modified);
    }
    if (mesh_manager) {
        // Only the chunks the pass actually wrote. Marking all 27 unconditionally
        // queued 27 remeshes per chunk a paste touched (and per chunk that arrived
        // during streaming) for light that, in the common case, had not moved at
        // all: a region of daylight terrain holds no block light and no sources, so
        // this whole pass now leaves an empty mask and queues nothing.
        for (int dz = -1; dz <= 1; ++dz) {
            for (int dy = -1; dy <= 1; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if ((modified & BlockLightRegion::slot_bit(dx, dy, dz)) != 0) {
                        mesh_manager->mark_chunk_dirty_for_light(cx + dx, cy + dy, cz + dz);
                    }
                }
            }
        }
    }
}

// -------------------------------------------------------------------------
// _locked variants: caller already holds lock_all_exclusive().
// MUST NOT call mark_chunks_dirty_for_light or any auto-locking accessor.
// The public lock-then-call wrappers around these two BFS primitives were
// removed as dead code: the pipeline reaches light through the region pass,
// the incremental block change and try_fixup_chunk, and a caller that took
// the primitives directly (the player light, below in environment_controller)
// manages its own lock lifetime.
// -------------------------------------------------------------------------

void LightPropagator::propagate_block_light_region_locked(int32_t cx, int32_t cy, int32_t cz,
                                                        uint32_t* modified_out) {
    ChunkData* region_grid[3][3][3] = {};
    for (int dz = -1; dz <= 1; dz++) {
        for (int dy = -1; dy <= 1; dy++) {
            for (int dx = -1; dx <= 1; dx++) {
                region_grid[dx + 1][dy + 1][dz + 1] = chunk_map->get_chunk_data_fast(cx + dx, cy + dy, cz + dz);
            }
        }
    }
    BlockLightRegion light_region(region_grid);
    std::vector<EmissiveSource> sources;
    // Only the part a change in the CENTRE chunk can reach is wiped: that chunk and
    // its six faces, because a chunk is 32 wide and block light travels 15, so light
    // added or removed by the centre's own cells cannot touch an edge or corner slot of
    // the 3x3x3. That is this function's whole contract (cx, cy, cz is the chunk whose
    // blocks changed), and it is where the pass stops being unaffordable: the other 20
    // slots were being wiped and rebuilt identically. Measured with one emitter a
    // chunk: 11.3 -> 5.3 ms, and 29.6 -> 10.3 ms with four.
    //
    // Cleared once here, then told so: the region has to be cleared even when there
    // are no sources (that is what makes it dark), but propagate_additive clearing it a
    // second time cost half of this whole function — 2.2 ms of its 4.4 ms on a 3x3x3 of
    // sky-lit chunks. The collect runs AFTER the clear because it asks the region which
    // slots the wipe took (see collect_emissive_sources).
    light_region.clear_block_light_affected();
    light_region.collect_emissive_sources(sources, /*only_cleared=*/true);
    light_region.propagate_additive(sources, /*already_cleared=*/true);
    if (modified_out != nullptr) {
        *modified_out = light_region.modified_mask();
    }
}

void LightPropagator::light_propagate_add_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& queue) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    static constexpr int16_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1}
    };

    queue.reserve(512);
    size_t idx = 0;
    while (idx < queue.size()) {
        const LightNode node = queue[idx++];
        if (node.r <= 1 && node.g <= 1 && node.b <= 1) continue;
        const uint8_t next_r = node.r > 1 ? static_cast<uint8_t>(node.r - 1) : 0;
        const uint8_t next_g = node.g > 1 ? static_cast<uint8_t>(node.g - 1) : 0;
        const uint8_t next_b = node.b > 1 ? static_cast<uint8_t>(node.b - 1) : 0;

        for (int i = 0; i < 6; i++) {
            int32_t ncx = node.cx;
            int32_t ncy = node.cy;
            int32_t ncz = node.cz;
            int16_t nx = static_cast<int16_t>(node.x + offsets[i][0]);
            int16_t ny = static_cast<int16_t>(node.y + offsets[i][1]);
            int16_t nz = static_cast<int16_t>(node.z + offsets[i][2]);

            wrap_local_to_world(nx, ny, nz, ncx, ncy, ncz);

            ChunkData* dst = chunk_map->get_chunk_data_fast(ncx, ncy, ncz);
            if (!dst) {
                std::lock_guard<std::mutex> guard(pending_light_removals_mutex_);
                pending_light_removals_.insert(chunk_map->get_chunk_key(ncx, ncy, ncz));
                continue;
            }

            const BlockID neighbor_block = dst->get_block_unsafe(nx, ny, nz);
            const BlockType& neighbor_type = registry.get_block(neighbor_block);
            if (HasProperty(neighbor_type.properties, BlockProperty::Opaque)) continue;

            // Light crossing a translucent block pays its opacity (water/acid 3,
            // lava 15): a torch two cells into a pool is dimmer than one beside
            // it in air, and lava swallows block light outright.
            const int extra = neighbor_type.light_opacity;
            const uint8_t cur_r = dst->get_light_r(nx, ny, nz);
            const uint8_t cur_g = dst->get_light_g(nx, ny, nz);
            const uint8_t cur_b = dst->get_light_b(nx, ny, nz);

            const uint8_t out_r = std::max(cur_r, extra > 0 ? static_cast<uint8_t>(std::max(0, next_r - extra)) : next_r);
            const uint8_t out_g = std::max(cur_g, extra > 0 ? static_cast<uint8_t>(std::max(0, next_g - extra)) : next_g);
            const uint8_t out_b = std::max(cur_b, extra > 0 ? static_cast<uint8_t>(std::max(0, next_b - extra)) : next_b);
            if (out_r != cur_r || out_g != cur_g || out_b != cur_b) {
                dst->set_light_rgb(nx, ny, nz, out_r, out_g, out_b);
                queue.push_back({ncx, ncy, ncz, nx, ny, nz, out_r, out_g, out_b});
            }
        }
    }
    // `queue` is usually one of the thread-local buffers (see the caller); hand
    // back the memory a pathological BFS grew it to.
    trim_queue_buffer(queue);
}

void LightPropagator::light_propagate_remove_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& remove_queue, std::vector<LightNode>& add_queue) {
    static constexpr int16_t offsets[6][3] = {
        {1, 0, 0}, {-1, 0, 0},
        {0, 1, 0}, {0, -1, 0},
        {0, 0, 1}, {0, 0, -1}
    };

    remove_queue.reserve(512);
    add_queue.reserve(512);
    size_t idx = 0;
    while (idx < remove_queue.size()) {
        const LightNode node = remove_queue[idx++];
        for (int i = 0; i < 6; i++) {
            int32_t ncx = node.cx;
            int32_t ncy = node.cy;
            int32_t ncz = node.cz;
            int16_t nx = static_cast<int16_t>(node.x + offsets[i][0]);
            int16_t ny = static_cast<int16_t>(node.y + offsets[i][1]);
            int16_t nz = static_cast<int16_t>(node.z + offsets[i][2]);

            wrap_local_to_world(nx, ny, nz, ncx, ncy, ncz);

            ChunkData* dst = chunk_map->get_chunk_data_fast(ncx, ncy, ncz);
            if (!dst) {
                std::lock_guard<std::mutex> guard(pending_light_removals_mutex_);
                pending_light_removals_.insert(chunk_map->get_chunk_key(ncx, ncy, ncz));
                continue;
            }

            const uint8_t cur_r = dst->get_light_r(nx, ny, nz);
            const uint8_t cur_g = dst->get_light_g(nx, ny, nz);
            const uint8_t cur_b = dst->get_light_b(nx, ny, nz);
            if (cur_r == 0 && cur_g == 0 && cur_b == 0) continue;

            // Per-channel removal. A channel is cleared only if the removed
            // source emitted that channel AND this cell's level is strictly
            // below the removed source's level here — i.e. the light could have
            // come from the source being removed. Channels supplied by other
            // (still present) sources are kept so they can refill the region.
            const bool remove_r = node.r > 0 && cur_r > 0 && cur_r < node.r;
            const bool remove_g = node.g > 0 && cur_g > 0 && cur_g < node.g;
            const bool remove_b = node.b > 0 && cur_b > 0 && cur_b < node.b;

            if (!remove_r && !remove_g && !remove_b) {
                // This cell's light comes from another source at the same or a
                // higher level. Re-add it so the surviving sources refill the
                // region that the removal BFS is about to clear.
                add_queue.push_back({ncx, ncy, ncz, nx, ny, nz, cur_r, cur_g, cur_b});
                continue;
            }

            const uint8_t out_r = remove_r ? 0 : cur_r;
            const uint8_t out_g = remove_g ? 0 : cur_g;
            const uint8_t out_b = remove_b ? 0 : cur_b;
            dst->set_light_rgb(nx, ny, nz, out_r, out_g, out_b);

            // Continue the removal BFS carrying the cell's previous levels for
            // the removed channels, so the cleared region follows the removed
            // source's light falloff. Each (cell, channel) is cleared at most
            // once, so the BFS terminates even with many overlapping sources.
            remove_queue.push_back({ncx, ncy, ncz, nx, ny, nz,
                                    remove_r ? cur_r : static_cast<uint8_t>(0),
                                    remove_g ? cur_g : static_cast<uint8_t>(0),
                                    remove_b ? cur_b : static_cast<uint8_t>(0)});

            // Re-add the channels that survive here so they refill this cell
            // and everything cleared behind it.
            if (out_r > 0 || out_g > 0 || out_b > 0) {
                add_queue.push_back({ncx, ncy, ncz, nx, ny, nz, out_r, out_g, out_b});
            }
        }
    }
    // Only `remove_queue` is trimmed here. It is an INPUT to this function and
    // the loop above has walked its cursor to the end of it, so every node in
    // it is already processed and only the memory is left to hand back.
    //
    // `add_queue` is deliberately NOT trimmed. It is an OUTPUT: this loop only
    // fills it with the seeds that refill what the removal cleared (the
    // surviving channels it found at line ~230 and ~251), and both callers run
    // a separate add pass over it afterwards. Trimming it would swap the seeds
    // away for a large enough removal, that pass's `!empty()` check would then
    // read false, and the cells this pass just cleared would never be refilled -
    // a dark region left where the surviving light should be, which is exactly
    // the artifact the per-channel removal logic exists to avoid.
    trim_queue_buffer(remove_queue);
}

void LightPropagator::try_fixup_chunk(uint64_t key, int32_t cx, int32_t cy, int32_t cz) {
    {
        std::lock_guard<std::mutex> guard(pending_light_removals_mutex_);
        auto it = pending_light_removals_.find(key);
        if (it == pending_light_removals_.end()) return;
        pending_light_removals_.erase(it);
    }
    propagate_block_light_region(cx, cy, cz);
    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(cx, cy, cz);
    }
}

// -------------------------------------------------------------------------
// update_block_light_incremental: public wrapper
// -------------------------------------------------------------------------
void LightPropagator::update_block_light_incremental(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b) {
    ChunkData* chunk = chunk_map->get_chunk_data(cx, cy, cz);
    if (!chunk) return;

    {
        // BFS from a single block change can reach at most 1 chunk in each
        // direction (max light level 15 < chunk size 32). Lock 3×3×3 around
        // both the origin and center chunk positions.
        uint64_t keys[27 * 2];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++) {
                    keys[idx++] = chunk_map->get_chunk_key(origin_cx + dx, origin_cy + dy, origin_cz + dz);
                    keys[idx++] = chunk_map->get_chunk_key(cx + dx, cy + dy, cz + dz);
                }
        auto lock = chunk_map->lock_keys_exclusive(keys);
        update_block_light_incremental_locked(origin_cx, origin_cy, origin_cz, cx, cy, cz, x, y, z, old_block, new_block, old_cell_r, old_cell_g, old_cell_b);
    }

    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(origin_cx, origin_cy, origin_cz);
    }
}

// -------------------------------------------------------------------------
// _locked variant: caller already holds lock_all_exclusive().
// Uses _fast accessors only. MUST NOT call mark_chunks_dirty_for_light.
// -------------------------------------------------------------------------
void LightPropagator::update_block_light_incremental_locked(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, int32_t cx, int32_t cy, int32_t cz, int32_t x, int32_t y, int32_t z, BlockID old_block, BlockID new_block, uint8_t old_cell_r, uint8_t old_cell_g, uint8_t old_cell_b) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    const BlockType& old_type = registry.get_block(old_block);
    const BlockType& new_type = registry.get_block(new_block);
    const bool old_emissive = HasProperty(old_type.properties, BlockProperty::Emissive);
    const bool new_emissive = HasProperty(new_type.properties, BlockProperty::Emissive);
    const bool new_opaque = HasProperty(new_type.properties, BlockProperty::Opaque);

    const uint8_t old_cell_level = old_cell_r > old_cell_g ? (old_cell_r > old_cell_b ? old_cell_r : old_cell_b) : (old_cell_g > old_cell_b ? old_cell_g : old_cell_b);
    const uint8_t old_emission_level = old_emissive ? (old_type.light_r > old_type.light_g ? (old_type.light_r > old_type.light_b ? old_type.light_r : old_type.light_b) : (old_type.light_g > old_type.light_b ? old_type.light_g : old_type.light_b)) : 0;
    const uint8_t old_light_level = std::max(old_cell_level, old_emission_level);
    const uint8_t target_r = new_emissive ? new_type.light_r : 0;
    const uint8_t target_g = new_emissive ? new_type.light_g : 0;
    const uint8_t target_b = new_emissive ? new_type.light_b : 0;
    const uint8_t target_level = target_r > target_g ? (target_r > target_b ? target_r : target_b) : (target_g > target_b ? target_g : target_b);

    uint8_t remove_r = old_cell_r;
    uint8_t remove_g = old_cell_g;
    uint8_t remove_b = old_cell_b;
    if (old_cell_r == 0 && old_cell_g == 0 && old_cell_b == 0 && old_emissive) {
        remove_r = old_type.light_r;
        remove_g = old_type.light_g;
        remove_b = old_type.light_b;
    }

    ChunkData* chunk = chunk_map->get_chunk_data_fast(cx, cy, cz);
    if (!chunk) return;

    // Use thread-local buffers instead of local allocation
    remove_queue_buffer.clear();
    add_queue_buffer.clear();
    remove_queue_buffer.reserve(64);
    add_queue_buffer.reserve(64);

    if (old_light_level > 0 && old_light_level > target_level) {
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
        remove_queue_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), remove_r, remove_g, remove_b});
        light_propagate_remove_locked(cx, cy, cz, remove_queue_buffer, add_queue_buffer);
    } else if (old_emissive && !new_emissive) {
        // Special case: breaking an emissive block that had no stored light but still needs light removal
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
        remove_queue_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), old_type.light_r, old_type.light_g, old_type.light_b});
        light_propagate_remove_locked(cx, cy, cz, remove_queue_buffer, add_queue_buffer);
    }

    if (target_level > 0) {
        chunk->set_light_rgb(x, y, z, target_r, target_g, target_b);
        add_queue_buffer.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), target_r, target_g, target_b});
    } else if (!new_opaque) {
        static constexpr int16_t offsets[6][3] = {
            {1, 0, 0}, {-1, 0, 0},
            {0, 1, 0}, {0, -1, 0},
            {0, 0, 1}, {0, 0, -1}
        };
        for (int i = 0; i < 6; i++) {
            int32_t ncx = cx;
            int32_t ncy = cy;
            int32_t ncz = cz;
            int16_t nx = static_cast<int16_t>(x + offsets[i][0]);
            int16_t ny = static_cast<int16_t>(y + offsets[i][1]);
            int16_t nz = static_cast<int16_t>(z + offsets[i][2]);

            wrap_local_to_world(nx, ny, nz, ncx, ncy, ncz);

            ChunkData* src = chunk_map->get_chunk_data_fast(ncx, ncy, ncz);
            if (!src) continue;

            const uint8_t lr = src->get_light_r(nx, ny, nz);
            const uint8_t lg = src->get_light_g(nx, ny, nz);
            const uint8_t lb = src->get_light_b(nx, ny, nz);
            if (lr > 0 || lg > 0 || lb > 0) {
                add_queue_buffer.push_back({ncx, ncy, ncz, nx, ny, nz, lr, lg, lb});
            }
        }
    } else {
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
    }

    if (!add_queue_buffer.empty()) {
        light_propagate_add_locked(cx, cy, cz, add_queue_buffer);
    }
}

} // namespace VoxelEngine
