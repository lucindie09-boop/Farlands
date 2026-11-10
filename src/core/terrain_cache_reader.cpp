#include "core/terrain_cache_reader.hpp"

#include <chrono>
#include <set>
#include <utility>

namespace VoxelEngine {

namespace {

uint64_t cache_chunk_key(int32_t x, int32_t y, int32_t z) noexcept {
    constexpr uint32_t kOffset = 1u << 20;
    constexpr uint32_t kMask = 0x1FFFFF;
    const uint64_t ux = static_cast<uint64_t>((static_cast<uint32_t>(x) + kOffset) & kMask);
    const uint64_t uy = static_cast<uint64_t>((static_cast<uint32_t>(y) + kOffset) & kMask);
    const uint64_t uz = static_cast<uint64_t>((static_cast<uint32_t>(z) + kOffset) & kMask);
    return (ux << 42) | (uy << 21) | uz;
}

bool read_exact(const TerrainCacheReader::ReadCallback& callback, const std::string& path,
                uint64_t offset, size_t size, std::vector<uint8_t>& out) {
    out.clear();
    if (!callback) return false;
    if (!callback(path, offset, size, out)) return false;
    return out.size() == size;
}

} // namespace

void TerrainCacheReader::close() {
    open_ = false;
    root_.clear();
    key_ = 0;
    packs_.clear();
    chunks_.clear();
    read_ = {};
}

bool TerrainCacheReader::open(const std::string& cache_root, uint64_t key,
                              const std::vector<std::string>& pack_paths,
                              ReadCallback read_callback) {
    close();
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    corrupt_.store(0, std::memory_order_relaxed);
    read_errors_.store(0, std::memory_order_relaxed);
    payload_bytes_.store(0, std::memory_order_relaxed);
    read_us_.store(0, std::memory_order_relaxed);
    decode_us_.store(0, std::memory_order_relaxed);
    index_bytes_.store(0, std::memory_order_relaxed);
    manifest_bytes_.store(0, std::memory_order_relaxed);
    open_failures_.store(0, std::memory_order_relaxed);

    if (!read_callback) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    read_ = std::move(read_callback);
    root_ = cache_root;
    key_ = key;

    std::vector<uint8_t> manifest_bytes;
    if (!read_exact(read_, terrain_cache_manifest_path(cache_root), 0,
                    kTerrainCacheManifestSize, manifest_bytes)) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    manifest_bytes_.store(manifest_bytes.size(), std::memory_order_relaxed);

    TerrainCacheManifest manifest;
    if (!decode_terrain_cache_manifest(manifest_bytes.data(), manifest_bytes.size(), manifest)) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (manifest.key != key || manifest.pack_count != pack_paths.size()) {
        open_failures_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    std::vector<PackFile> packs;
    packs.reserve(pack_paths.size());
    std::unordered_map<uint64_t, PackLocation> chunks;
    std::set<std::pair<int32_t, int32_t>> regions;
    uint64_t index_bytes = 0;

    for (const std::string& path : pack_paths) {
        if (path.empty()) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::vector<uint8_t> header_bytes;
        if (!read_exact(read_, path, 0, kTerrainCacheHeaderSize, header_bytes)) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        TerrainCachePackHeader header;
        if (!decode_terrain_cache_pack_header(header_bytes.data(), header_bytes.size(), header) ||
            header.key != key) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        if (!regions.insert({header.region_x, header.region_z}).second) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        std::vector<uint8_t> index_data;
        if (!read_exact(read_, path, header.index_offset, header.index_size, index_data)) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        std::vector<TerrainCacheChunkRecord> records;
        if (!decode_terrain_cache_pack_index_bytes(index_data.data(), index_data.size(),
                                                   header, records)) {
            open_failures_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        const size_t pack_index = packs.size();
        for (const TerrainCacheChunkRecord& record : records) {
            const uint64_t chunk_key = cache_chunk_key(record.x, record.y, record.z);
            if (!chunks.emplace(chunk_key, PackLocation{pack_index, record}).second) {
                open_failures_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }
        index_bytes += index_data.size();
        packs.push_back(PackFile{path, header});
    }

    packs_ = std::move(packs);
    chunks_ = std::move(chunks);
    index_bytes_.store(index_bytes, std::memory_order_relaxed);
    open_ = true;
    return true;
}

bool TerrainCacheReader::read_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                                    const BlockRegistry& registry, ChunkData& out_chunk,
                                    std::vector<TerrainCacheSpill>& out_spill) {
    if (!open_) return false;

    const auto found = chunks_.find(cache_chunk_key(chunk_x, chunk_y, chunk_z));
    if (found == chunks_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    const PackLocation& location = found->second;
    const PackFile& pack = packs_[location.pack_index];
    std::vector<uint8_t> payload;
    const auto read_start = std::chrono::steady_clock::now();
    const bool read_ok = read_exact(read_, pack.path, location.record.payload_offset,
                                    location.record.payload_size, payload);
    const auto read_end = std::chrono::steady_clock::now();
    read_us_.fetch_add(static_cast<uint64_t>(
                           std::chrono::duration_cast<std::chrono::microseconds>(
                               read_end - read_start).count()),
                       std::memory_order_relaxed);
    if (!read_ok) {
        read_errors_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    payload_bytes_.fetch_add(payload.size(), std::memory_order_relaxed);

    const auto decode_start = std::chrono::steady_clock::now();
    const bool decoded = decode_terrain_cache_payload(payload.data(), payload.size(),
                                                      location.record, registry,
                                                      out_chunk, out_spill);
    const auto decode_end = std::chrono::steady_clock::now();
    decode_us_.fetch_add(static_cast<uint64_t>(
                             std::chrono::duration_cast<std::chrono::microseconds>(
                                 decode_end - decode_start).count()),
                         std::memory_order_relaxed);
    if (!decoded) {
        corrupt_.fetch_add(1, std::memory_order_relaxed);
        misses_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    hits_.fetch_add(1, std::memory_order_relaxed);
    return true;
}

TerrainCacheReaderStats TerrainCacheReader::snapshot_stats() const {
    TerrainCacheReaderStats stats;
    stats.hits = hits_.load(std::memory_order_relaxed);
    stats.misses = misses_.load(std::memory_order_relaxed);
    stats.corrupt = corrupt_.load(std::memory_order_relaxed);
    stats.read_errors = read_errors_.load(std::memory_order_relaxed);
    stats.payload_bytes = payload_bytes_.load(std::memory_order_relaxed);
    stats.read_us = read_us_.load(std::memory_order_relaxed);
    stats.decode_us = decode_us_.load(std::memory_order_relaxed);
    stats.pack_count = packs_.size();
    stats.index_entries = chunks_.size();
    stats.index_bytes = index_bytes_.load(std::memory_order_relaxed);
    stats.manifest_bytes = manifest_bytes_.load(std::memory_order_relaxed);
    stats.open_failures = open_failures_.load(std::memory_order_relaxed);
    return stats;
}

void TerrainCacheReader::reset_stats() {
    hits_.store(0, std::memory_order_relaxed);
    misses_.store(0, std::memory_order_relaxed);
    corrupt_.store(0, std::memory_order_relaxed);
    read_errors_.store(0, std::memory_order_relaxed);
    payload_bytes_.store(0, std::memory_order_relaxed);
    read_us_.store(0, std::memory_order_relaxed);
    decode_us_.store(0, std::memory_order_relaxed);
    open_failures_.store(0, std::memory_order_relaxed);
}

} // namespace VoxelEngine
