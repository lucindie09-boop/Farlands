#include "doctest.h"

#include "core/block_types.hpp"
#include "core/terrain_cache_format.hpp"
#include "core/terrain_cache_writer.hpp"

#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

using namespace VoxelEngine;

namespace {

std::vector<uint8_t> make_payload(BlockID block) {
    ChunkData chunk;
    chunk.set_block(0, 0, 0, block);
    std::vector<uint8_t> payload;
    CHECK(encode_terrain_cache_chunk(chunk, {}, payload));
    return payload;
}

BlockID decode_first_block(const std::vector<uint8_t>& payload) {
    ChunkData chunk;
    std::vector<TerrainCacheSpill> spill;
    CHECK(decode_terrain_cache_chunk(payload.data(), payload.size(),
                                     BlockRegistry::get_instance(), chunk, spill));
    return chunk.get_block(0, 0, 0);
}

int32_t block_at(const std::vector<uint8_t>& pack, int32_t x, int32_t y, int32_t z) {
    TerrainCachePackHeader header;
    std::vector<TerrainCacheChunkRecord> index;
    CHECK(decode_terrain_cache_pack_index(pack.data(), pack.size(), 0xABC, header, index));
    for (const TerrainCacheChunkRecord& record : index) {
        if (record.x != x || record.y != y || record.z != z) continue;
        ChunkData chunk;
        std::vector<TerrainCacheSpill> spill;
        CHECK(decode_terrain_cache_pack_chunk(pack.data(), pack.size(), record,
                                              BlockRegistry::get_instance(), chunk, spill));
        return chunk.get_block(0, 0, 0);
    }
    return -1;
}

} // namespace

TEST_CASE("terrain cache writer flushes a region only after it goes idle") {
    TerrainCacheWriter writer;
    CHECK(writer.stage(0, 0, 0, make_payload(BlockIDs::STONE), 10.0));
    CHECK(writer.staged_region_count() == 1);
    CHECK(writer.snapshot_stats().staged_chunks == 1);

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    writer.take_ready(11.0, ready);
    CHECK(ready.empty());
    CHECK(writer.staged_region_count() == 1);

    writer.take_ready(10.0 + kTerrainCacheRegionIdleSeconds, ready);
    CHECK(ready.size() == 1);
    CHECK(ready[0].region_x == 0);
    CHECK(ready[0].region_z == 0);
    CHECK(ready[0].chunks.size() == 1);
    CHECK(writer.staged_region_count() == 0);
}

TEST_CASE("terrain cache writer keeps regions apart and replaces a restaged chunk") {
    TerrainCacheWriter writer;
    CHECK(writer.stage(0, 0, 0, make_payload(BlockIDs::STONE), 0.0));
    CHECK(writer.stage(8, 0, 0, make_payload(BlockIDs::DIRT), 0.0));
    CHECK(writer.stage(0, 0, 0, make_payload(BlockIDs::GRASS), 0.0));

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    writer.take_ready(100.0, ready);
    CHECK(ready.size() == 2);

    bool saw_origin = false;
    bool saw_neighbour = false;
    for (const TerrainCacheWriter::ReadyRegion& region : ready) {
        if (region.region_x == 0 && region.region_z == 0) {
            saw_origin = true;
            CHECK(region.chunks.size() == 1);
            if (region.chunks.size() == 1) {
                CHECK(decode_first_block(region.chunks[0].payload) == BlockIDs::GRASS);
            }
        } else if (region.region_x == 1 && region.region_z == 0) {
            saw_neighbour = true;
            CHECK(region.chunks.size() == 1);
            if (region.chunks.size() == 1) {
                CHECK(decode_first_block(region.chunks[0].payload) == BlockIDs::DIRT);
            }
        }
    }
    CHECK(saw_origin);
    CHECK(saw_neighbour);
}

TEST_CASE("terrain cache writer flushes a full region without waiting for idle") {
    TerrainCacheWriter writer;
    for (size_t i = 0; i < kTerrainCacheRegionFlushChunks; ++i) {
        const int32_t x = static_cast<int32_t>(i % 8);
        const int32_t z = static_cast<int32_t>((i / 8) % 8);
        const int32_t y = static_cast<int32_t>(i / 64);
        CHECK(writer.stage(x, y, z, make_payload(BlockIDs::STONE), 0.0));
    }

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    writer.take_ready(0.0, ready);
    CHECK(ready.size() == 1);
    CHECK(ready[0].chunks.size() == kTerrainCacheRegionFlushChunks);
    CHECK(writer.snapshot_stats().staged_chunks == 0);
}

TEST_CASE("terrain cache writer bounds staging when every region is touched") {
    TerrainCacheWriter writer;
    constexpr int kRegions = 24;
    constexpr int kPerRegion = 180;
    for (int region = 0; region < kRegions; ++region) {
        for (int i = 0; i < kPerRegion; ++i) {
            const int32_t x = region * 8 + (i % 8);
            const int32_t z = (i / 8) % 8;
            const int32_t y = i / 64;
            CHECK(writer.stage(x, y, z, make_payload(BlockIDs::STONE), 0.0));
        }
    }

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    writer.take_ready(0.0, ready);
    CHECK_FALSE(ready.empty());
    CHECK(writer.snapshot_stats().staged_chunks <= kTerrainCacheStagedChunkLimit);

    size_t flushed = 0;
    for (const TerrainCacheWriter::ReadyRegion& region : ready) {
        flushed += region.chunks.size();
    }
    CHECK(kRegions * kPerRegion - flushed <= kTerrainCacheStagedChunkLimit);
}

TEST_CASE("terrain cache writer merges a fresh region into its existing pack") {
    BlockRegistry::get_instance().initialize_default_blocks();
    constexpr uint64_t key = 0xABC;

    std::vector<TerrainCacheEncodedChunk> existing_chunks;
    existing_chunks.push_back({0, 0, 0, make_payload(BlockIDs::STONE)});
    existing_chunks.push_back({0, 1, 0, make_payload(BlockIDs::DIRT)});
    std::vector<uint8_t> existing;
    CHECK(encode_terrain_cache_pack(key, 0, 0, existing_chunks, existing));

    TerrainCacheWriter::ReadyRegion region;
    region.region_x = 0;
    region.region_z = 0;
    region.chunks.push_back({0, 1, 0, make_payload(BlockIDs::GRASS)});
    region.chunks.push_back({0, 2, 0, make_payload(BlockIDs::DIRT)});

    std::vector<uint8_t> pack;
    size_t merged = 0;
    CHECK(TerrainCacheWriter::build_pack(key, region, existing, pack, merged));
    CHECK(merged == 1);
    CHECK(block_at(pack, 0, 0, 0) == static_cast<int32_t>(BlockIDs::STONE));
    CHECK(block_at(pack, 0, 1, 0) == static_cast<int32_t>(BlockIDs::GRASS));
    CHECK(block_at(pack, 0, 2, 0) == static_cast<int32_t>(BlockIDs::DIRT));
}

TEST_CASE("terrain cache writer drops a corrupt or stale existing pack") {
    BlockRegistry::get_instance().initialize_default_blocks();
    constexpr uint64_t key = 0xABC;

    std::vector<TerrainCacheEncodedChunk> existing_chunks;
    existing_chunks.push_back({0, 0, 0, make_payload(BlockIDs::STONE)});
    std::vector<uint8_t> existing;
    CHECK(encode_terrain_cache_pack(key, 0, 0, existing_chunks, existing));

    TerrainCacheWriter::ReadyRegion region;
    region.region_x = 0;
    region.region_z = 0;
    region.chunks.push_back({0, 2, 0, make_payload(BlockIDs::GRASS)});

    // A corrupted index is not a reason to keep the old pack: the fresh chunks
    // are written alone, which replaces the region's file with a valid one.
    std::vector<uint8_t> corrupt = existing;
    corrupt.back() ^= 0xFF;
    std::vector<uint8_t> pack;
    size_t merged = 0;
    CHECK(TerrainCacheWriter::build_pack(key, region, corrupt, pack, merged));
    CHECK(merged == 0);
    CHECK(block_at(pack, 0, 2, 0) == static_cast<int32_t>(BlockIDs::GRASS));
    CHECK(block_at(pack, 0, 0, 0) == -1);

    // Same for a pack produced under a different key.
    CHECK(TerrainCacheWriter::build_pack(key + 1, region, existing, pack, merged));
    CHECK(merged == 0);
}

TEST_CASE("terrain cache writer stages from several threads while draining") {
    BlockRegistry::get_instance().initialize_default_blocks();
    TerrainCacheWriter writer;
    std::mutex mutex;
    std::atomic<bool> done{false};

    std::vector<TerrainCacheEncodedChunk> collected;
    std::thread collector([&] {
        std::vector<TerrainCacheWriter::ReadyRegion> ready;
        while (!done.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lock(mutex);
            writer.take_ready(1.0e9, ready);
            for (TerrainCacheWriter::ReadyRegion& region : ready) {
                for (TerrainCacheEncodedChunk& chunk : region.chunks) {
                    collected.push_back(std::move(chunk));
                }
            }
        }
    });

    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    std::vector<std::thread> stagers;
    stagers.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
        stagers.emplace_back([&, t] {
            for (int i = 0; i < kPerThread; ++i) {
                const int32_t x = t * 16 + (i % 8);
                const int32_t z = (i / 8) % 8;
                const int32_t y = i / 64;
                std::lock_guard<std::mutex> lock(mutex);
                writer.stage(x, y, z, make_payload(BlockIDs::STONE), 0.0);
            }
        });
    }
    for (std::thread& stager : stagers) stager.join();
    done.store(true, std::memory_order_relaxed);
    collector.join();

    std::vector<TerrainCacheWriter::ReadyRegion> ready;
    {
        std::lock_guard<std::mutex> lock(mutex);
        writer.take_ready(1.0e9, ready);
    }
    for (TerrainCacheWriter::ReadyRegion& region : ready) {
        for (TerrainCacheEncodedChunk& chunk : region.chunks) {
            collected.push_back(std::move(chunk));
        }
    }

    CHECK(collected.size() == kThreads * kPerThread);
    std::set<std::tuple<int32_t, int32_t, int32_t>> coordinates;
    for (const TerrainCacheEncodedChunk& chunk : collected) {
        coordinates.insert({chunk.x, chunk.y, chunk.z});
    }
    CHECK(coordinates.size() == collected.size());
}
