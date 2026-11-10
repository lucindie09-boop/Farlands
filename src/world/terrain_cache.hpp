#ifndef FARLANDS_TERRAIN_CACHE_HPP
#define FARLANDS_TERRAIN_CACHE_HPP

#include "core/terrain_cache_reader.hpp"
#include "core/terrain_params.hpp"

namespace VoxelEngine {

// Godot-facing adapter for the pure reader: it owns the key and the cache root,
// discovers the region packs, and gives each worker thread its own FileAccess
// handle. `ensure_open` is main-thread; `read_chunk` may run on any generation
// worker once it has returned.
class TerrainCache {
public:
    TerrainCache() = default;
    TerrainCache(const TerrainCache&) = delete;
    TerrainCache& operator=(const TerrainCache&) = delete;

    // Reads the immutable key inputs and the manifest/indexes when the switch is
    // on and the parameters are new. A missing cache or any I/O/format problem
    // leaves this closed; that is a miss, never an error.
    bool ensure_open(const TerrainParams& params);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return reader_.is_open(); }
    [[nodiscard]] bool environment_enabled() const noexcept { return env_enabled_; }
    [[nodiscard]] uint64_t key() const noexcept { return reader_.key(); }
    [[nodiscard]] const std::string& cache_root() const noexcept { return reader_.root(); }

    bool read_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                    const BlockRegistry& registry, ChunkData& out_chunk,
                    std::vector<TerrainCacheSpill>& out_spill) {
        return reader_.read_chunk(chunk_x, chunk_y, chunk_z, registry, out_chunk, out_spill);
    }

    [[nodiscard]] TerrainCacheReaderStats stats() const { return reader_.snapshot_stats(); }
    void reset_stats() { reader_.reset_stats(); }

private:
    TerrainCacheReader reader_;
    uint64_t attempted_params_key_ = 0;
    bool attempted_ = false;
    bool env_enabled_ = false;
};

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_HPP
