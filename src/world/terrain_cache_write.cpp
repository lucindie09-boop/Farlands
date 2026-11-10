#include "world/terrain_cache.hpp"

#include "world/terrain_cache_internal.hpp"

#include "core/terrain_cache_manifest.hpp"
#include "core/thread_pool.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/string.hpp>

#include <chrono>
#include <utility>
#include <vector>

namespace VoxelEngine {

namespace {

using godot::DirAccess;
using godot::FileAccess;
using godot::String;

double monotonic_seconds() {
    return std::chrono::duration<double>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t elapsed_us(std::chrono::steady_clock::time_point start) {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - start).count());
}

bool write_whole_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    const godot::Ref<FileAccess> file =
        FileAccess::open(String(path.c_str()), FileAccess::WRITE);
    if (!file.is_valid()) return false;
    if (!bytes.empty()) {
        file->store_buffer(bytes.data(), static_cast<int64_t>(bytes.size()));
    }
    file->close();
    return true;
}

bool rename_in_root(const std::string& root, const std::string& from, const std::string& to) {
    const godot::Ref<DirAccess> dir = DirAccess::open(String(root.c_str()));
    if (!dir.is_valid()) return false;
    return dir->rename(String(from.c_str()), String(to.c_str())) == godot::Error::OK;
}

} // namespace

void TerrainCache::stage_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                               const ChunkData& chunk,
                               const std::vector<TerrainCacheSpill>& spill) {
    const std::shared_ptr<TerrainCacheWriterState> state = writer_;
    if (!state || state->stop.load(std::memory_order_acquire)) return;

    // Encoding is the worker's own copy of the generator answer; the staging
    // lock only covers the map insert, so several generation workers do not
    // serialise on the bytes they just spent their time producing.
    std::vector<uint8_t> payload;
    const auto encode_start = std::chrono::steady_clock::now();
    if (!encode_terrain_cache_chunk(chunk, spill, payload)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->writer.note_encode_failure();
        return;
    }
    const uint64_t encode_us = elapsed_us(encode_start);

    std::lock_guard<std::mutex> lock(state->mutex);
    if (state->stop.load(std::memory_order_relaxed)) return;
    state->writer.note_encode_time(encode_us);
    state->writer.stage(chunk_x, chunk_y, chunk_z, std::move(payload), monotonic_seconds());
    state->staged_regions.store(state->writer.staged_region_count(),
                                std::memory_order_relaxed);
}

void TerrainCache::tick(ThreadPool* pool) {
    const std::shared_ptr<TerrainCacheWriterState> state = writer_;
    if (!state || pool == nullptr || state->stop.load(std::memory_order_relaxed)) return;
    if (state->staged_regions.load(std::memory_order_relaxed) == 0) return;

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->stop.load(std::memory_order_relaxed)) return;
        state->writer.take_ready(monotonic_seconds(), ready, &state->in_flight);
        for (const TerrainCacheWriter::ReadyRegion& region : ready) {
            state->in_flight.insert(
                terrain_cache_region_key(region.region_x, region.region_z));
        }
        state->staged_regions.store(state->writer.staged_region_count(),
                                    std::memory_order_relaxed);
    }

    for (auto& region : ready) {
        // One task per region: a pack is the write unit, and keeping the tasks
        // small is what lets the pool interleave them with generation.
        pool->fire_and_forget(
            [state, region = std::move(region)]() mutable {
                terrain_cache_write_region(state, region);
            },
            PoolTaskClass::Save);
    }
}

TerrainCacheWriterStats TerrainCache::writer_stats() const {
    const std::shared_ptr<TerrainCacheWriterState> state = writer_;
    if (!state) return {};
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->writer.snapshot_stats();
}

void TerrainCache::reset_stats() {
    reader_.reset_stats();
    const std::shared_ptr<TerrainCacheWriterState> state = writer_;
    if (!state) return;
    std::lock_guard<std::mutex> lock(state->mutex);
    state->writer.reset_stats();
}

void terrain_cache_publish_manifest(const std::shared_ptr<TerrainCacheWriterState>& state) {
    if (!state || state->stop.load(std::memory_order_acquire)) return;

    std::vector<uint8_t> bytes;
    std::string root;
    {
        // The manifest mutex spans the snapshot and the rename, so two writers
        // finishing together cannot leave an older pack set as the last word.
        std::lock_guard<std::mutex> publish_lock(state->manifest_mutex);
        TerrainCacheManifest manifest;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            manifest.key = state->key;
            manifest.pack_count = static_cast<uint32_t>(state->packs.size());
            manifest.total_bytes = 0;
            for (const auto& entry : state->pack_bytes) {
                manifest.total_bytes += entry.second;
            }
            manifest.created_unix = state->created_unix;
            root = state->root;
        }
        if (!encode_terrain_cache_manifest(manifest, bytes)) return;

        const std::string path = terrain_cache_manifest_path(root);
        const std::string temp = path + ".tmp";
        if (!write_whole_file(temp, bytes)) return;
        rename_in_root(root, temp, path);
    }
}

void terrain_cache_write_region(const std::shared_ptr<TerrainCacheWriterState>& state,
                                const TerrainCacheWriter::ReadyRegion& region) {
    if (!state || state->stop.load(std::memory_order_acquire)) return;

    const uint64_t region_key = terrain_cache_region_key(region.region_x, region.region_z);
    const std::string filename = terrain_cache_pack_filename(region.region_x, region.region_z);
    const std::string root = state->root;

    // The existing pack is read outside the staging lock: it can be megabytes,
    // and holding the lock across it would stall every generation worker.
    std::string existing_path;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->packs.count(region_key) != 0) existing_path = root + "/" + filename;
    }
    std::vector<uint8_t> existing;
    if (!existing_path.empty()) {
        const godot::Ref<FileAccess> file =
            FileAccess::open(String(existing_path.c_str()), FileAccess::READ);
        if (file.is_valid()) {
            const int64_t length = file->get_length();
            if (length > 0) {
                existing.resize(static_cast<size_t>(length));
                if (file->get_buffer(existing.data(), static_cast<uint64_t>(length)) !=
                    static_cast<uint64_t>(length)) {
                    existing.clear();
                }
            }
        }
    }

    std::vector<uint8_t> pack;
    size_t merged_chunks = 0;
    const size_t fresh_chunks = region.chunks.size();
    const auto encode_start = std::chrono::steady_clock::now();
    const bool built = TerrainCacheWriter::build_pack(state->key, region, existing, pack,
                                                      merged_chunks);
    const uint64_t encode_us = elapsed_us(encode_start);
    if (!built) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->writer.note_encode_failure();
        state->in_flight.erase(region_key);
        return;
    }
    if (state->stop.load(std::memory_order_acquire)) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->in_flight.erase(region_key);
        return;
    }

    const std::string final_path = root + "/" + filename;
    const std::string temp_path = final_path + ".tmp";
    const auto write_start = std::chrono::steady_clock::now();
    bool written = write_whole_file(temp_path, pack);
    if (written) {
        written = terrain_cache_replace_pack_file(
            final_path, [&]() { return rename_in_root(root, temp_path, final_path); });
    }
    const uint64_t write_us = elapsed_us(write_start);

    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->in_flight.erase(region_key);
        if (written) {
            state->packs.insert(region_key);
            state->pack_bytes[region_key] = pack.size();
            state->writer.note_pack_written(fresh_chunks, merged_chunks, pack.size(),
                                            encode_us, write_us);
        } else {
            state->writer.note_write_failure();
        }
    }
    if (written) terrain_cache_publish_manifest(state);
}

} // namespace VoxelEngine
