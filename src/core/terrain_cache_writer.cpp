#include "core/terrain_cache_writer.hpp"

#include "core/crc32.hpp"

#include <algorithm>
#include <array>
#include <set>
#include <tuple>

namespace VoxelEngine {

namespace {

int32_t floor_div(int32_t value, int32_t divisor) noexcept {
    int32_t quotient = value / divisor;
    if (value % divisor != 0 && (value < 0) != (divisor < 0)) --quotient;
    return quotient;
}

bool chunk_before(const TerrainCacheEncodedChunk& a, const TerrainCacheEncodedChunk& b) {
    return std::tie(a.x, a.y, a.z) < std::tie(b.x, b.y, b.z);
}

using ChunkCoord = std::array<int32_t, 3>;

} // namespace

bool TerrainCacheWriter::stage(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                               std::vector<uint8_t> payload, double now_seconds) {
    if (payload.empty() || payload.size() > kTerrainCacheMaxChunkBytes) return false;

    const auto region = std::make_pair(floor_div(chunk_x, kTerrainCacheRegionChunks),
                                       floor_div(chunk_z, kTerrainCacheRegionChunks));
    RegionBuffer& buffer = regions_[region];
    for (TerrainCacheEncodedChunk& existing : buffer.chunks) {
        if (existing.x == chunk_x && existing.y == chunk_y && existing.z == chunk_z) {
            existing.payload = std::move(payload);
            buffer.last_stage = now_seconds;
            ++stats_.chunks_staged;
            return true;
        }
    }

    TerrainCacheEncodedChunk chunk;
    chunk.x = chunk_x;
    chunk.y = chunk_y;
    chunk.z = chunk_z;
    chunk.payload = std::move(payload);
    buffer.chunks.push_back(std::move(chunk));
    buffer.last_stage = now_seconds;
    ++stats_.chunks_staged;
    return true;
}

void TerrainCacheWriter::take_ready(double now_seconds, std::vector<ReadyRegion>& out,
                                    const std::unordered_set<uint64_t>* busy) {
    out.clear();
    auto is_busy = [busy](int32_t region_x, int32_t region_z) {
        return busy != nullptr &&
               busy->find(terrain_cache_region_key(region_x, region_z)) != busy->end();
    };

    for (auto it = regions_.begin(); it != regions_.end();) {
        const bool ready =
            !is_busy(it->first.first, it->first.second) &&
            (it->second.chunks.size() >= kTerrainCacheRegionFlushChunks ||
             now_seconds - it->second.last_stage >= kTerrainCacheRegionIdleSeconds);
        if (!ready) {
            ++it;
            continue;
        }
        out.push_back(ReadyRegion{it->first.first, it->first.second,
                                  std::move(it->second.chunks)});
        it = regions_.erase(it);
    }

    size_t staged_chunks = 0;
    for (const auto& entry : regions_) staged_chunks += entry.second.chunks.size();
    if (staged_chunks <= kTerrainCacheStagedChunkLimit) return;

    std::vector<std::pair<double, std::pair<int32_t, int32_t>>> oldest;
    oldest.reserve(regions_.size());
    for (const auto& entry : regions_) {
        if (is_busy(entry.first.first, entry.first.second)) continue;
        oldest.push_back({entry.second.last_stage, entry.first});
    }
    std::sort(oldest.begin(), oldest.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first < b.first;
        return a.second < b.second;
    });

    for (const auto& entry : oldest) {
        if (staged_chunks <= kTerrainCacheStagedChunkLimit) break;
        const auto it = regions_.find(entry.second);
        if (it == regions_.end()) continue;
        staged_chunks -= it->second.chunks.size();
        out.push_back(ReadyRegion{it->first.first, it->first.second,
                                  std::move(it->second.chunks)});
        regions_.erase(it);
    }
}

bool TerrainCacheWriter::build_pack(uint64_t key, const ReadyRegion& region,
                                    const std::vector<uint8_t>& existing,
                                    std::vector<uint8_t>& out, size_t& out_merged_chunks) {
    out.clear();
    out_merged_chunks = 0;

    std::vector<TerrainCacheEncodedChunk> merged = region.chunks;
    std::set<ChunkCoord> fresh;
    for (const TerrainCacheEncodedChunk& chunk : region.chunks) {
        fresh.insert(ChunkCoord{chunk.x, chunk.y, chunk.z});
    }

    if (!existing.empty()) {
        TerrainCachePackHeader header;
        std::vector<TerrainCacheChunkRecord> index;
        if (decode_terrain_cache_pack_index(existing.data(), existing.size(), key,
                                            header, index) &&
            header.region_x == region.region_x && header.region_z == region.region_z) {
            for (const TerrainCacheChunkRecord& record : index) {
                if (fresh.count(ChunkCoord{record.x, record.y, record.z}) != 0) continue;
                if (record.payload_offset > existing.size() ||
                    record.payload_size > existing.size() - record.payload_offset) {
                    continue;
                }
                const uint8_t* payload = existing.data() + record.payload_offset;
                if (crc32(payload, record.payload_size) != record.payload_crc) continue;
                TerrainCacheEncodedChunk chunk;
                chunk.x = record.x;
                chunk.y = record.y;
                chunk.z = record.z;
                chunk.payload.assign(payload, payload + record.payload_size);
                merged.push_back(std::move(chunk));
                ++out_merged_chunks;
            }
        }
    }

    std::sort(merged.begin(), merged.end(), chunk_before);
    return encode_terrain_cache_pack(key, region.region_x, region.region_z, merged, out);
}

void TerrainCacheWriter::note_pack_written(size_t fresh_chunks, size_t merged_chunks,
                                           size_t bytes, uint64_t encode_us,
                                           uint64_t write_us) {
    ++stats_.packs_written;
    stats_.chunks_written += fresh_chunks;
    stats_.chunks_merged += merged_chunks;
    stats_.bytes_written += bytes;
    stats_.encode_us += encode_us;
    stats_.write_us += write_us;
}

void TerrainCacheWriter::note_encode_time(uint64_t encode_us) {
    stats_.encode_us += encode_us;
}

void TerrainCacheWriter::note_encode_failure() { ++stats_.encode_failures; }

void TerrainCacheWriter::note_write_failure() { ++stats_.write_failures; }

TerrainCacheWriterStats TerrainCacheWriter::snapshot_stats() const {
    TerrainCacheWriterStats stats = stats_;
    stats.staged_regions = regions_.size();
    stats.staged_chunks = 0;
    for (const auto& entry : regions_) stats.staged_chunks += entry.second.chunks.size();
    return stats;
}

void TerrainCacheWriter::reset_stats() {
    const size_t staged_regions = stats_.staged_regions;
    const size_t staged_chunks = stats_.staged_chunks;
    stats_ = TerrainCacheWriterStats{};
    stats_.staged_regions = staged_regions;
    stats_.staged_chunks = staged_chunks;
}

void TerrainCacheWriter::clear() { regions_.clear(); }

} // namespace VoxelEngine
