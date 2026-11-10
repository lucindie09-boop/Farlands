#ifndef FARLANDS_TERRAIN_CACHE_INTERNAL_HPP
#define FARLANDS_TERRAIN_CACHE_INTERNAL_HPP

#include "core/terrain_cache_writer.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace VoxelEngine {

class ThreadPool;

// Shared by terrain_cache.cpp (open/close) and terrain_cache_write.cpp (stage,
// tick, pack and manifest writes). A pool task holds a shared_ptr to this, so a
// world reset can drop the TerrainCache's reference while a pack is still being
// written without the task touching a destroyed adapter; `stop` makes it leave
// the cache alone from then on.
struct TerrainCacheWriterState {
    mutable std::mutex mutex;
    std::mutex manifest_mutex;
    TerrainCacheWriter writer;
    std::unordered_set<uint64_t> in_flight;
    std::unordered_set<uint64_t> packs;
    std::unordered_map<uint64_t, uint64_t> pack_bytes;
    std::string root;
    uint64_t key = 0;
    uint64_t created_unix = 0;
    std::atomic<bool> stop{false};
    std::atomic<size_t> staged_regions{0};
};

// Writes cache.meta from the state's pack set. Locking is internal, and
// publishers are serialised so the manifest left on disk is the most complete
// one that was built.
void terrain_cache_publish_manifest(const std::shared_ptr<TerrainCacheWriterState>& state);

// Pool task body: merges the region with its existing pack, replaces the pack
// file atomically, then publishes the manifest.
void terrain_cache_write_region(const std::shared_ptr<TerrainCacheWriterState>& state,
                                const TerrainCacheWriter::ReadyRegion& region);

// Replaces a pack, working around Windows file sharing. FileAccess reads do not
// share delete access, so a rename over an open pack fails there; this closes the
// shared handle first and leaves it closed after a successful replace because the
// reader's in-RAM offsets still describe the old pack. The next launch reloads
// the new index. A failed replace reopens the old pack; POSIX needs no close
// because the open descriptor keeps the old inode alive.
bool terrain_cache_replace_pack_file(const std::string& path,
                                     const std::function<bool()>& replace);

} // namespace VoxelEngine

#endif // FARLANDS_TERRAIN_CACHE_INTERNAL_HPP
