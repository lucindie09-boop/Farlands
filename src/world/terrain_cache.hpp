#ifndef FARLANDS_TERRAIN_CACHE_HPP
#define FARLANDS_TERRAIN_CACHE_HPP

#include "core/terrain_cache_reader.hpp"
#include "core/terrain_cache_writer.hpp"
#include "core/terrain_params.hpp"

#include <memory>
#include <string>

namespace VoxelEngine {

class ThreadPool;
struct TerrainCacheWriterState;

// Godot-facing adapter for the pure cache reader and writer: it owns the key and
// the cache root, discovers the region packs, gives each worker thread its own
// FileAccess handle for reads, and stages encoded generator output for the
// writer. `ensure_open`, `tick` and `close` are main-thread; `read_chunk` and
// `stage_chunk` may run on any generation worker once the cache is open.
class TerrainCache {
public:
    TerrainCache() = default;
    TerrainCache(const TerrainCache&) = delete;
    TerrainCache& operator=(const TerrainCache&) = delete;

    // Reads the immutable key inputs and the manifest/indexes when the switch is
    // on and the parameters are new. A missing cache or any I/O/format problem
    // leaves the reader closed; that is a miss, never an error. The writer half
    // still opens, because the first boot is exactly when it has work to do.
    bool ensure_open(const TerrainParams& params);
    void close();

    [[nodiscard]] bool is_open() const noexcept { return reader_.is_open(); }
    [[nodiscard]] bool environment_enabled() const noexcept { return env_enabled_; }
    [[nodiscard]] uint64_t key() const noexcept { return key_; }
    [[nodiscard]] const std::string& cache_root() const noexcept { return root_; }

    bool read_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                    const BlockRegistry& registry, ChunkData& out_chunk,
                    std::vector<TerrainCacheSpill>& out_spill) {
        return reader_.read_chunk(chunk_x, chunk_y, chunk_z, registry, out_chunk, out_spill);
    }

    // Encodes one generator answer and stages it for its region pack. Called
    // from the generation worker at the capture boundary; a disabled or closed
    // cache drops it, and a failure is a silent miss like every other cache
    // problem.
    void stage_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                     const ChunkData& chunk, const std::vector<TerrainCacheSpill>& spill);

    // Main-thread pump: hands idle/full regions to the pool. Cheap when nothing
    // is staged (one atomic read), so it runs on every completion drain.
    void tick(ThreadPool* pool);

    [[nodiscard]] TerrainCacheReaderStats stats() const { return reader_.snapshot_stats(); }
    [[nodiscard]] TerrainCacheWriterStats writer_stats() const;
    void reset_stats();

private:
    TerrainCacheReader reader_;
    std::shared_ptr<TerrainCacheWriterState> writer_;
    uint64_t key_ = 0;
    std::string root_;
    uint64_t attempted_params_key_ = 0;
    bool attempted_ = false;
    bool env_enabled_ = false;
};

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_HPP
