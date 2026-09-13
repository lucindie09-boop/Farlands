#ifndef FARLANDS_NAV_CHUNK_SOURCE_HPP
#define FARLANDS_NAV_CHUNK_SOURCE_HPP

// -----------------------------------------------------------------------------
// Planner cell source over the live chunk map.
//
// Every lookup goes through ChunkMap's thread-safe accessors (a per-call shared
// shard lock), so a search can run on a worker while generation, meshing and
// editing continue. Residency is cached per instance: a chunk that is not
// resident reports Unknown — never AIR — so a route can never be plotted
// through ungenerated space. A chunk that loads mid-search stays unknown for
// that search, which is the conservative direction.
// -----------------------------------------------------------------------------

#include "core/chunk_coords.hpp"
#include "core/chunk_map.hpp"
#include "pathfinding/block_class.hpp"

#include <cstdint>
#include <unordered_map>

namespace VoxelEngine {
namespace nav {

class ChunkMapNavSource {
public:
    explicit ChunkMapNavSource(const ChunkMap& map) : map_(map) {}

    [[nodiscard]] Cell sample(int32_t x, int32_t y, int32_t z) {
        if (y < 0 || y >= WORLD_HEIGHT_Y) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        int32_t cx, cy, cz, lx, ly, lz;
        world_to_chunk_local(x, y, z, cx, cy, cz, lx, ly, lz);
        if (!resident(cx, cy, cz)) return Cell{CellClass::Unknown, 0.0f, 0.0f};
        return classify_block(static_cast<BlockID>(map_.get_block_world(x, y, z)));
    }

    [[nodiscard]] size_t residency_queries() const noexcept { return residency_queries_; }

private:
    [[nodiscard]] bool resident(int32_t cx, int32_t cy, int32_t cz) {
        const uint64_t key = map_.get_chunk_key(cx, cy, cz);
        const auto it = residency_.find(key);
        if (it != residency_.end()) return it->second != 0;
        ++residency_queries_;
        const bool loaded = map_.has_loaded_chunk(cx, cy, cz);
        residency_.emplace(key, loaded ? 1 : 0);
        return loaded;
    }

    const ChunkMap& map_;
    std::unordered_map<uint64_t, uint8_t> residency_;
    size_t residency_queries_ = 0;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_CHUNK_SOURCE_HPP
