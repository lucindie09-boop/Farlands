#ifndef FARLANDS_LOD_NODE_CACHE_HPP
#define FARLANDS_LOD_NODE_CACHE_HPP

// The far field's column samples, shared by every tile that wants one
// (docs/lod-modes.md).
//
// One column sample is the mode's whole cost, and a tile asks for the same node
// its neighbour already asked for: at the outermost level a tile is ONE cell, so
// each of its four nodes is wanted by up to four tiles and three of those asks are
// repeats. Measured on the top of the reach slider, that is 185,017 columns for
// 46,656 distinct ones.
//
// The table is keyed by the node's world column alone, because a column's sample
// does not depend on the spacing a caller wants it at: the coarse levels and the
// fine ones share ONE table. That is also what pays for the occlusion ring the
// surface asks for (four more nodes per node, every one of them a neighbour's own
// node rather than a new column).
//
// Deliberately Godot-free and small enough to test on its own
// (tests/test_lod_node_cache.cpp), like the rest of src/lod/.

#include "lod/lod_surface.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>

namespace VoxelEngine {
namespace lod {

class NodeCache {
public:
    // What a miss costs: the column, sampled however the caller samples columns.
    using Sample = std::function<SurfaceSample(int32_t x, int32_t z)>;

    // Entries held before the table is emptied wholesale. A node is a world column
    // and its sample is 16 bytes, so this is a few megabytes -- and the whole
    // outermost lattice of a 27 km reach is 46,656 of them, which is the number
    // this has to be comfortable above. Over it the table is CLEARED rather than
    // aged out: the ring the player wants at any moment is one contiguous band, an
    // eviction policy would have to be told which part of it that is, and the cost
    // of being wrong twice (a clear) is re-sampling one band.
    static constexpr size_t kDefaultLimit = 1u << 17;  // 131,072 nodes

    // The node at (x, z): sampled on the first ask, answered from the table after.
    SurfaceSample get(int32_t x, int32_t z, const Sample& sample) {
        const uint64_t key = node_key(x, z);
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto found = nodes.find(key);
            if (found != nodes.end()) {
                ++hit_count;
                return found->second;
            }
            ++miss_count;
        }
        // Outside the lock on purpose: a column is milliseconds of work, and holding
        // the table across it would serialize every worker behind every other one.
        // Two workers can therefore sample the same missing node at once, and the
        // second insert wins -- a wasted column, never a wrong answer.
        const SurfaceSample fresh = sample(x, z);
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (nodes.size() >= limit) nodes.clear();
            nodes.emplace(key, fresh);
        }
        return fresh;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        nodes.clear();
    }

    void set_limit(size_t entries) {
        std::lock_guard<std::mutex> lock(mutex);
        limit = entries > 0 ? entries : 1;
    }

    [[nodiscard]] size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return nodes.size();
    }

    [[nodiscard]] int64_t hits() const {
        std::lock_guard<std::mutex> lock(mutex);
        return hit_count;
    }

    [[nodiscard]] int64_t misses() const {
        std::lock_guard<std::mutex> lock(mutex);
        return miss_count;
    }

    void reset_counters() {
        std::lock_guard<std::mutex> lock(mutex);
        hit_count = 0;
        miss_count = 0;
    }

    // The packing the tile map uses, at node granularity: a negative coordinate has
    // to keep a column of its own.
    [[nodiscard]] static uint64_t node_key(int32_t x, int32_t z) {
        return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) |
               static_cast<uint32_t>(z);
    }

private:
    mutable std::mutex mutex;
    std::unordered_map<uint64_t, SurfaceSample> nodes;
    size_t limit = kDefaultLimit;
    int64_t hit_count = 0;
    int64_t miss_count = 0;
};

} // namespace lod
} // namespace VoxelEngine

#endif // FARLANDS_LOD_NODE_CACHE_HPP
