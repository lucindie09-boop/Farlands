#include "lighting/light_propagator.hpp"
#include "lighting/block_light_region.hpp"

#include "mesh/mesh_manager.hpp"
#include "core/chunk_data.hpp"
#include <godot_cpp/core/print_string.hpp>
#include <godot_cpp/variant/string.hpp>
#include <unordered_set>

using namespace godot;

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Public wrapper: acquire lock, call _locked, release, dirty-mark.
// -------------------------------------------------------------------------
void LightPropagator::propagate_block_light_region(int32_t cx, int32_t cy, int32_t cz) {
    ChunkData* chunk = chunk_map->get_chunk_data(cx, cy, cz);
    if (!chunk) return;
    {
        uint64_t keys[27];
        int idx = 0;
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    keys[idx++] = chunk_map->get_chunk_key(cx + dx, cy + dy, cz + dz);
        auto lock = chunk_map->lock_keys_exclusive(keys);
        propagate_block_light_region_locked(cx, cy, cz);
    }
    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(cx, cy, cz);
    }
}

void LightPropagator::propagate_from_existing_light(int32_t cx, int32_t cy, int32_t cz) {
    ChunkData* chunk = chunk_map->get_chunk_data(cx, cy, cz);
    if (!chunk) return;
    std::vector<LightNode> add_queue;
    add_queue.reserve(64);
    for (int y = 0; y < CHUNK_HEIGHT; y++) {
        for (int z = 0; z < CHUNK_DEPTH; z++) {
            for (int x = 0; x < CHUNK_WIDTH; x++) {
                uint8_t r = chunk->get_light_r(x, y, z);
                uint8_t g = chunk->get_light_g(x, y, z);
                uint8_t b = chunk->get_light_b(x, y, z);
                if (r > 0 || g > 0 || b > 0) {
                    add_queue.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), r, g, b});
                }
            }
        }
    }
    if (!add_queue.empty()) {
        light_propagate_add(cx, cy, cz, add_queue);
    }
    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(cx, cy, cz);
    }
}

void LightPropagator::light_propagate_add(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& queue) {
    {
        // BFS can reach at most 1 chunk in each direction (max light level 15,
        // chunk size 32). Collect all seed chunks' 3×3×3 neighborhoods and
        // lock only those shards.
        std::vector<uint64_t> keys;
        keys.reserve(static_cast<size_t>(27) * 4);
        bool seen[ChunkMap::kNumShards] = {};
        auto add_key = [&](uint64_t k) {
            size_t s = k % ChunkMap::kNumShards;
            if (!seen[s]) { seen[s] = true; keys.push_back(k); }
        };
        // Origin chunk + its 3×3×3
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    add_key(chunk_map->get_chunk_key(origin_cx + dx, origin_cy + dy, origin_cz + dz));
        // Each seed node's chunk + its 3×3×3
        for (auto& node : queue) {
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        add_key(chunk_map->get_chunk_key(node.cx + dx, node.cy + dy, node.cz + dz));
        }
        auto lock = chunk_map->lock_keys_exclusive(keys);
        light_propagate_add_locked(origin_cx, origin_cy, origin_cz, queue);
    }
    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(origin_cx, origin_cy, origin_cz);
    }
}

void LightPropagator::light_propagate_remove(int32_t origin_cx, int32_t origin_cy, int32_t origin_cz, std::vector<LightNode>& remove_queue, std::vector<LightNode>& add_queue) {
    {
        // Same bounded-reach reasoning as light_propagate_add.
        std::vector<uint64_t> keys;
        keys.reserve(static_cast<size_t>(27) * 4);
        bool seen[ChunkMap::kNumShards] = {};
        auto add_key = [&](uint64_t k) {
            size_t s = k % ChunkMap::kNumShards;
            if (!seen[s]) { seen[s] = true; keys.push_back(k); }
        };
        for (int dz = -1; dz <= 1; dz++)
            for (int dy = -1; dy <= 1; dy++)
                for (int dx = -1; dx <= 1; dx++)
                    add_key(chunk_map->get_chunk_key(origin_cx + dx, origin_cy + dy, origin_cz + dz));
        for (auto& node : remove_queue) {
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        add_key(chunk_map->get_chunk_key(node.cx + dx, node.cy + dy, node.cz + dz));
        }
        auto lock = chunk_map->lock_keys_exclusive(keys);
        light_propagate_remove_locked(origin_cx, origin_cy, origin_cz, remove_queue, add_queue);
    }
    if (mesh_manager) {
        mesh_manager->mark_chunks_dirty_for_light(origin_cx, origin_cy, origin_cz);
    }
}

// -------------------------------------------------------------------------
// _locked variants: caller already holds lock_all_exclusive().
// MUST NOT call mark_chunks_dirty_for_light or any auto-locking accessor.
// -------------------------------------------------------------------------

void LightPropagator::propagate_block_light_region_locked(int32_t cx, int32_t cy, int32_t cz) {
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
    light_region.collect_emissive_sources(sources);
    light_region.clear_block_light();
    light_region.propagate_additive(sources);
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

            const uint8_t cur_r = dst->get_light_r(nx, ny, nz);
            const uint8_t cur_g = dst->get_light_g(nx, ny, nz);
            const uint8_t cur_b = dst->get_light_b(nx, ny, nz);

            const uint8_t out_r = std::max(cur_r, next_r);
            const uint8_t out_g = std::max(cur_g, next_g);
            const uint8_t out_b = std::max(cur_b, next_b);
            if (out_r != cur_r || out_g != cur_g || out_b != cur_b) {
                dst->set_light_rgb(nx, ny, nz, out_r, out_g, out_b);
                queue.push_back({ncx, ncy, ncz, nx, ny, nz, out_r, out_g, out_b});
            }
        }
    }
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
        const uint8_t node_level = node.r > node.g ? (node.r > node.b ? node.r : node.b) : (node.g > node.b ? node.g : node.b);
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
            const uint8_t cur_level = cur_r > cur_g ? (cur_r > cur_b ? cur_r : cur_b) : (cur_g > cur_b ? cur_g : cur_b);

            if (cur_level < node_level) {
                dst->set_light_rgb(nx, ny, nz, 0, 0, 0);
                remove_queue.push_back({ncx, ncy, ncz, nx, ny, nz, cur_r, cur_g, cur_b});
            } else {
                const uint8_t out_r = (node.r > 0 && cur_r < node.r) ? 0 : cur_r;
                const uint8_t out_g = (node.g > 0 && cur_g < node.g) ? 0 : cur_g;
                const uint8_t out_b = (node.b > 0 && cur_b < node.b) ? 0 : cur_b;

                dst->set_light_rgb(nx, ny, nz, out_r, out_g, out_b);

                if (out_r > 0 || out_g > 0 || out_b > 0) {
                    add_queue.push_back({ncx, ncy, ncz, nx, ny, nz, out_r, out_g, out_b});
                }
                // Note: We do NOT re-add to remove_queue here. If light remains (added to add_queue),
                // it means other sources are still contributing. The add_queue will re-propagate
                // the remaining light to neighbors. Re-adding to remove_queue causes infinite loops
                // when multiple sources overlap.
            }
        }
    }
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

    std::vector<LightNode> remove_queue;
    std::vector<LightNode> add_queue;
    remove_queue.reserve(64);
    add_queue.reserve(64);

    if (old_light_level > 0 && old_light_level > target_level) {
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
        remove_queue.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), remove_r, remove_g, remove_b});
        light_propagate_remove_locked(cx, cy, cz, remove_queue, add_queue);
    } else if (old_emissive && !new_emissive) {
        // Special case: breaking an emissive block that had no stored light but still needs light removal
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
        remove_queue.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), old_type.light_r, old_type.light_g, old_type.light_b});
        light_propagate_remove_locked(cx, cy, cz, remove_queue, add_queue);
    }

    if (target_level > 0) {
        chunk->set_light_rgb(x, y, z, target_r, target_g, target_b);
        add_queue.push_back({cx, cy, cz, static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(z), target_r, target_g, target_b});
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
                add_queue.push_back({ncx, ncy, ncz, nx, ny, nz, lr, lg, lb});
            }
        }
    } else {
        chunk->set_light_rgb(x, y, z, 0, 0, 0);
    }

    if (!add_queue.empty()) {
        light_propagate_add_locked(cx, cy, cz, add_queue);
    }
}

} // namespace VoxelEngine
