#ifndef FARLANDS_PLAYER_LIGHT_HPP
#define FARLANDS_PLAYER_LIGHT_HPP
#include "core/chunk_data.hpp"
#include "core/chunk_map.hpp"
#include <godot_cpp/variant/vector3.hpp>
#include "lighting/light_propagation.hpp"
#include <cstdint>
#include <vector>
#include <array>
#include <functional>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Player light — manages the light source block that follows the player.
//
// update() acquires lock_all_exclusive() on the ChunkMap for the duration
// of its ChunkData reads/writes and BFS calls.  mark_dirty callbacks are
// deferred to after the lock is released so they can safely use
// auto-locking accessors.
// -------------------------------------------------------------------------
class PlayerLight {
public:
    using LightPropagateRemove = std::function<void(int32_t, int32_t, int32_t, std::vector<LightNode>&, std::vector<LightNode>&)>;
    using LightPropagateAdd = std::function<void(int32_t, int32_t, int32_t, std::vector<LightNode>&)>;
    using MarkDirtyFn = std::function<void(int32_t, int32_t, int32_t)>;

    void update(const godot::Vector3& player_position,
                double runtime_elapsed,
                double initial_loading_duration,
                ChunkMap& chunk_map,
                const LightPropagateRemove& propagate_remove,
                const LightPropagateAdd& propagate_add,
                const MarkDirtyFn& mark_dirty) {
        if (!enabled) return;
        if (runtime_elapsed < initial_loading_duration) return;

        int32_t px = static_cast<int32_t>(std::floor(player_position.x));
        int32_t py = static_cast<int32_t>(std::floor(player_position.y));
        int32_t pz = static_cast<int32_t>(std::floor(player_position.z));

        if (px == last_x && py == last_y && pz == last_z) {
            return;
        }

        int32_t old_cx = (last_x != INT32_MIN)
            ? static_cast<int32_t>(std::floor(static_cast<float>(last_x) / CHUNK_WIDTH))
            : INT32_MIN;
        int32_t old_cy = (last_y != INT32_MIN)
            ? static_cast<int32_t>(std::floor(static_cast<float>(last_y) / CHUNK_HEIGHT))
            : INT32_MIN;
        int32_t old_cz = (last_z != INT32_MIN)
            ? static_cast<int32_t>(std::floor(static_cast<float>(last_z) / CHUNK_DEPTH))
            : INT32_MIN;
        int32_t new_cx = static_cast<int32_t>(std::floor(static_cast<float>(px) / CHUNK_WIDTH));
        int32_t new_cy = static_cast<int32_t>(std::floor(static_cast<float>(py) / CHUNK_HEIGHT));
        int32_t new_cz = static_cast<int32_t>(std::floor(static_cast<float>(pz) / CHUNK_DEPTH));

        // Defer dirty-marking until after the lock is released.
        std::vector<std::array<int32_t, 3>> dirty_chunks;

        {
            // BFS from remove + add can each reach at most 1 chunk in each
            // direction (max light level 12 < chunk size 32). Lock the union
            // of 3×3×3 neighborhoods around old and new chunk positions.
            std::vector<uint64_t> keys;
            keys.reserve(54);
            if (last_x != INT32_MIN) {
                for (int dz = -1; dz <= 1; dz++)
                    for (int dy = -1; dy <= 1; dy++)
                        for (int dx = -1; dx <= 1; dx++)
                            keys.push_back(chunk_map.get_chunk_key(old_cx + dx, old_cy + dy, old_cz + dz));
            }
            for (int dz = -1; dz <= 1; dz++)
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++)
                        keys.push_back(chunk_map.get_chunk_key(new_cx + dx, new_cy + dy, new_cz + dz));
            auto lock = chunk_map.lock_keys_exclusive(keys);

            // Remove light from old position
            if (last_x != INT32_MIN) {
                int32_t old_lx = last_x - old_cx * CHUNK_WIDTH;
                int32_t old_ly = last_y - old_cy * CHUNK_HEIGHT;
                int32_t old_lz = last_z - old_cz * CHUNK_DEPTH;

                ChunkData* old_chunk = chunk_map.get_chunk_data_fast(old_cx, old_cy, old_cz);
                if (old_chunk && old_ly >= 0 && old_ly < CHUNK_HEIGHT) {
                    std::vector<LightNode> remove_queue;
                    std::vector<LightNode> add_queue;
                    remove_queue.reserve(64);
                    add_queue.reserve(64);

                    const uint8_t old_r = old_chunk->get_light_r(old_lx, old_ly, old_lz);
                    const uint8_t old_g = old_chunk->get_light_g(old_lx, old_ly, old_lz);
                    const uint8_t old_b = old_chunk->get_light_b(old_lx, old_ly, old_lz);
                    if (old_r > 0 || old_g > 0 || old_b > 0) {
                        old_chunk->set_light_rgb(old_lx, old_ly, old_lz, 0, 0, 0);
                        remove_queue.push_back({old_cx, old_cy, old_cz, static_cast<int16_t>(old_lx), static_cast<int16_t>(old_ly), static_cast<int16_t>(old_lz), old_r, old_g, old_b});
                        propagate_remove(old_cx, old_cy, old_cz, remove_queue, add_queue);
                        if (!add_queue.empty()) {
                            propagate_add(old_cx, old_cy, old_cz, add_queue);
                        }
                        dirty_chunks.push_back({old_cx, old_cy, old_cz});
                    }
                }
            }

            // Add light to new position
            ChunkData* new_chunk = chunk_map.get_chunk_data_fast(new_cx, new_cy, new_cz);
            if (new_chunk) {
                int32_t new_lx = px - new_cx * CHUNK_WIDTH;
                int32_t new_ly = py - new_cy * CHUNK_HEIGHT;
                int32_t new_lz = pz - new_cz * CHUNK_DEPTH;

                std::vector<LightNode> add_queue;
                add_queue.reserve(64);

                new_chunk->set_light_rgb(new_lx, new_ly, new_lz, level, level, level);
                add_queue.push_back({new_cx, new_cy, new_cz, static_cast<int16_t>(new_lx), static_cast<int16_t>(new_ly), static_cast<int16_t>(new_lz), level, level, level});
                propagate_add(new_cx, new_cy, new_cz, add_queue);
                dirty_chunks.push_back({new_cx, new_cy, new_cz});
            }
        }
        // Lock released — safe to call auto-locking mark_dirty.

        for (auto& [cx, cy, cz] : dirty_chunks) {
            mark_dirty(cx, cy, cz);
        }

        last_x = px;
        last_y = py;
        last_z = pz;
    }

    void reset() {
        last_x = INT32_MIN;
        last_y = INT32_MIN;
        last_z = INT32_MIN;
    }

    void set_enabled(bool e) { enabled = e; }
    bool get_enabled() const { return enabled; }
    void set_level(uint8_t l) { level = l; }
    uint8_t get_level() const { return level; }

private:
    bool enabled = true;
    uint8_t level = 12;
    int32_t last_x = INT32_MIN;
    int32_t last_y = INT32_MIN;
    int32_t last_z = INT32_MIN;
};

} // namespace VoxelEngine

#endif // FARLANDS_PLAYER_LIGHT_HPP
