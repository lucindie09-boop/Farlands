#ifndef FUK_MINECRAFT_MESH_QUEUE_HPP
#define FUK_MINECRAFT_MESH_QUEUE_HPP
#include "core/chunk_map.hpp"
#include "core/frustum.hpp"
#include <cstdint>
#include <queue>
#include <deque>
#include <unordered_set>
#include <vector>
#include <chrono>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Dirty mesh queue entry (priority by distance)
// -------------------------------------------------------------------------
struct DirtyChunkEntry {
    uint64_t key = 0;
    int32_t dist_sq = 0;
    bool urgent = false;
    bool in_frustum = false;
    uint32_t priority_revision = 0;
    bool operator>(const DirtyChunkEntry& other) const {
        if (urgent != other.urgent) return !urgent && other.urgent;
        if (in_frustum != other.in_frustum) return !in_frustum && other.in_frustum;
        return dist_sq > other.dist_sq;
    }
};

// -------------------------------------------------------------------------
// Mesh queue — manages the priority queue of chunks that need mesh rebuilds.
// -------------------------------------------------------------------------
class MeshQueue {
public:
    void clear() {
        while (!dirty_mesh_queue.empty()) dirty_mesh_queue.pop();
        dirty_mesh_pending.clear();
        immediate_dirty_mesh_queue.clear();
        immediate_dirty_mesh_pending.clear();
        skip_next_dirty_mesh_rebuild.clear();
        urgent_mesh_chunks.clear();
        player_chunk_x_ = INT32_MIN;
        player_chunk_y_ = INT32_MIN;
        player_chunk_z_ = INT32_MIN;
        frustum_ = nullptr;
        priority_revision_ = 0;
    }

    void queue_dirty_chunk(uint64_t key, int32_t dist_sq, bool urgent) {
        if (!dirty_mesh_pending.insert(key).second) {
            return;
        }
        DirtyChunkEntry entry;
        entry.key = key;
        entry.dist_sq = dist_sq;
        entry.urgent = urgent || is_urgent(key);
        entry.priority_revision = priority_revision_;
        if (frustum_ && player_chunk_x_ != INT32_MIN) {
            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            entry.in_frustum = frustum_->is_chunk_visible(cx, cy, cz);
        }
        dirty_mesh_queue.push(entry);
    }

    void queue_immediate_dirty_chunk(uint64_t key, bool is_already_dirty) {
        if (is_already_dirty) {
            skip_next_dirty_mesh_rebuild.insert(key);
        }
        if (!immediate_dirty_mesh_pending.insert(key).second) {
            return;
        }
        immediate_dirty_mesh_queue.push_back(key);
    }

    void mark_urgent(uint64_t key) {
        urgent_mesh_chunks.insert(key);
    }

    bool is_urgent(uint64_t key) const {
        return urgent_mesh_chunks.find(key) != urgent_mesh_chunks.end();
    }

    bool erase_urgent(uint64_t key) {
        return urgent_mesh_chunks.erase(key) > 0;
    }

    void reprioritize(int32_t player_chunk_x, int32_t player_chunk_y, int32_t player_chunk_z,
                      const Frustum* frustum = nullptr) {
        player_chunk_x_ = player_chunk_x;
        player_chunk_y_ = player_chunk_y;
        player_chunk_z_ = player_chunk_z;
        frustum_ = frustum;
        ++priority_revision_;
    }

    // Process the mesh queues. Calls the provided callback for each chunk that needs rebuilding.
    // Returns the number of rebuilds processed.
template<typename RebuildCallback>
    int32_t process(RebuildCallback&& rebuild_callback,
                    int32_t max_immediate_rebuilds,
                    int32_t max_rebuilds,
                    double budget_ms) {
        int32_t total_processed = 0;
        int32_t immediate_processed = 0;

        while (!immediate_dirty_mesh_queue.empty() &&
               immediate_processed < max_immediate_rebuilds) {
            const uint64_t key = immediate_dirty_mesh_queue.front();
            immediate_dirty_mesh_queue.pop_front();
            immediate_dirty_mesh_pending.erase(key);

            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(key, cx, cy, cz);
            rebuild_callback(cx, cy, cz);
            immediate_processed++;
        }
        total_processed += immediate_processed;

        auto start_time = std::chrono::high_resolution_clock::now();
        int32_t rebuilds_this_frame = 0;
        while (!dirty_mesh_queue.empty() && rebuilds_this_frame < max_rebuilds) {
            auto current_time = std::chrono::high_resolution_clock::now();
            double elapsed_ms = std::chrono::duration<double, std::milli>(current_time - start_time).count();
            if (elapsed_ms >= budget_ms) break;

            DirtyChunkEntry entry = dirty_mesh_queue.top();
            dirty_mesh_queue.pop();

            if (entry.priority_revision != priority_revision_ && player_chunk_x_ != INT32_MIN) {
                int32_t cx, cy, cz;
                ChunkMap::decode_chunk_key(entry.key, cx, cy, cz);
                const int32_t dx = cx - player_chunk_x_;
                const int32_t dy = cy - player_chunk_y_;
                const int32_t dz = cz - player_chunk_z_;
                entry.dist_sq = dx * dx + dy * dy + dz * dz;
                entry.urgent = entry.urgent || is_urgent(entry.key);
                entry.in_frustum = frustum_ ? frustum_->is_chunk_visible(cx, cy, cz) : false;
                entry.priority_revision = priority_revision_;
                dirty_mesh_queue.push(entry);
                continue;
            }

            dirty_mesh_pending.erase(entry.key);
            if (skip_next_dirty_mesh_rebuild.erase(entry.key) > 0) {
                continue;
            }

            int32_t cx, cy, cz;
            ChunkMap::decode_chunk_key(entry.key, cx, cy, cz);
            rebuild_callback(cx, cy, cz);
            rebuilds_this_frame++;
        }
        total_processed += rebuilds_this_frame;

        return total_processed;
    }

    [[nodiscard]] bool is_pending(uint64_t key) const {
        return dirty_mesh_pending.find(key) != dirty_mesh_pending.end();
    }

    [[nodiscard]] bool is_immediate_pending(uint64_t key) const {
        return immediate_dirty_mesh_pending.find(key) != immediate_dirty_mesh_pending.end();
    }

    [[nodiscard]] size_t size() const { return dirty_mesh_queue.size(); }
    [[nodiscard]] size_t immediate_size() const { return immediate_dirty_mesh_queue.size(); }

private:
    std::priority_queue<DirtyChunkEntry, std::vector<DirtyChunkEntry>, std::greater<DirtyChunkEntry>> dirty_mesh_queue;
    std::unordered_set<uint64_t> dirty_mesh_pending;
    std::deque<uint64_t> immediate_dirty_mesh_queue;
    std::unordered_set<uint64_t> immediate_dirty_mesh_pending;
    std::unordered_set<uint64_t> skip_next_dirty_mesh_rebuild;
    std::unordered_set<uint64_t> urgent_mesh_chunks;
    int32_t player_chunk_x_ = INT32_MIN;
    int32_t player_chunk_y_ = INT32_MIN;
    int32_t player_chunk_z_ = INT32_MIN;
    const Frustum* frustum_ = nullptr;
    uint32_t priority_revision_ = 0;
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_MESH_QUEUE_HPP
