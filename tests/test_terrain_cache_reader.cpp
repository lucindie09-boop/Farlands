#include "doctest.h"

#include "core/block_types.hpp"
#include "core/terrain_cache_format.hpp"
#include "core/terrain_cache_manifest.hpp"
#include "core/terrain_cache_reader.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace VoxelEngine;

namespace {

using Files = std::unordered_map<std::string, std::vector<uint8_t>>;

Files make_cache(uint64_t key, std::string& root) {
    root = "mem";
    Files files;

    auto add_pack = [&](int32_t region_x, int32_t region_z,
                        const std::vector<TerrainCacheEncodedChunk>& chunks) {
        std::vector<uint8_t> pack;
        CHECK(encode_terrain_cache_pack(key, region_x, region_z, chunks, pack));
        files[root + "/" + terrain_cache_pack_filename(region_x, region_z)] = std::move(pack);
    };

    ChunkData first;
    first.set_block(0, 0, 0, BlockIDs::STONE);
    first.set_block(3, 4, 5, BlockIDs::DIRT);
    std::vector<uint8_t> first_payload;
    CHECK(encode_terrain_cache_chunk(first, {{32, 0, 0, BlockIDs::STONE}}, first_payload));
    add_pack(0, 0, {{0, 0, 0, first_payload}});

    ChunkData second;
    second.set_block(0, 0, 0, BlockIDs::GRASS);
    std::vector<uint8_t> second_payload;
    CHECK(encode_terrain_cache_chunk(second, {}, second_payload));
    add_pack(1, 0, {{8, 0, 0, second_payload}});

    TerrainCacheManifest manifest;
    manifest.key = key;
    manifest.pack_count = 2;
    manifest.total_bytes = files[root + "/r_0_0.ftc"].size() +
                           files[root + "/r_1_0.ftc"].size();
    std::vector<uint8_t> manifest_bytes;
    CHECK(encode_terrain_cache_manifest(manifest, manifest_bytes));
    files[terrain_cache_manifest_path(root)] = std::move(manifest_bytes);
    return files;
}

TerrainCacheReader::ReadCallback read_from(const Files& files,
                                           std::vector<std::pair<uint64_t, size_t>>* reads = nullptr) {
    return [&files, reads](const std::string& path, uint64_t offset, size_t size,
                           std::vector<uint8_t>& out) {
        const auto it = files.find(path);
        if (it == files.end()) return false;
        if (offset > it->second.size() || size > it->second.size() - offset) return false;
        if (reads) reads->push_back({offset, size});
        out.assign(it->second.begin() + static_cast<size_t>(offset),
                   it->second.begin() + static_cast<size_t>(offset) + size);
        return true;
    };
}

} // namespace

TEST_CASE("terrain cache reader loads indexes without payload reads") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const uint64_t key = 0x123456789ABCDEF0ULL;
    std::string root;
    Files files = make_cache(key, root);
    const std::vector<std::string> paths = {
        root + "/r_0_0.ftc",
        root + "/r_1_0.ftc",
    };

    std::vector<std::pair<uint64_t, size_t>> reads;
    TerrainCacheReader reader;
    CHECK(reader.open(root, key, paths, read_from(files, &reads)));
    CHECK(reader.is_open());
    CHECK(reader.key() == key);

    // One manifest, then a 40-byte header and the trailing index for each pack.
    CHECK(reads.size() == 5);
    CHECK(reads[0].second == kTerrainCacheManifestSize);
    CHECK(reads[1].second == kTerrainCacheHeaderSize);
    CHECK(reads[3].second == kTerrainCacheHeaderSize);
    CHECK(reads[2].second != reads[1].second);
    CHECK(reads[4].second != reads[3].second);

    TerrainCacheReaderStats stats = reader.snapshot_stats();
    CHECK(stats.pack_count == 2);
    CHECK(stats.index_entries == 2);
    CHECK(stats.index_bytes == reads[2].second + reads[4].second);
    CHECK(stats.manifest_bytes == kTerrainCacheManifestSize);
    CHECK(stats.hits == 0);
    CHECK(stats.misses == 0);
}

TEST_CASE("terrain cache reader reads a payload by offset and replays spill") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const uint64_t key = 42;
    std::string root;
    Files files = make_cache(key, root);
    const std::vector<std::string> paths = {
        root + "/r_0_0.ftc",
        root + "/r_1_0.ftc",
    };
    TerrainCacheReader reader;
    CHECK(reader.open(root, key, paths, read_from(files)));

    ChunkData chunk;
    std::vector<TerrainCacheSpill> spill;
    CHECK(reader.read_chunk(0, 0, 0, BlockRegistry::get_instance(), chunk, spill));
    CHECK(chunk.get_block(0, 0, 0) == BlockIDs::STONE);
    CHECK(chunk.get_block(3, 4, 5) == BlockIDs::DIRT);
    CHECK(spill.size() == 1);
    if (spill.size() == 1) {
        CHECK(spill[0].x == 32);
        CHECK(spill[0].block == BlockIDs::STONE);
    }
    CHECK(reader.read_chunk(8, 0, 0, BlockRegistry::get_instance(), chunk, spill));
    CHECK(chunk.get_block(0, 0, 0) == BlockIDs::GRASS);

    TerrainCacheReaderStats stats = reader.snapshot_stats();
    CHECK(stats.hits == 2);
    CHECK(stats.misses == 0);
    CHECK(stats.payload_bytes > 0);
}

TEST_CASE("terrain cache reader treats absent, corrupt and stale data as misses") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const uint64_t key = 7;
    std::string root;
    Files files = make_cache(key, root);
    const std::vector<std::string> paths = {
        root + "/r_0_0.ftc",
        root + "/r_1_0.ftc",
    };

    TerrainCacheReader reader;
    CHECK(reader.open(root, key, paths, read_from(files)));
    ChunkData chunk;
    std::vector<TerrainCacheSpill> spill;
    CHECK_FALSE(reader.read_chunk(4, 0, 0, BlockRegistry::get_instance(), chunk, spill));
    CHECK(reader.snapshot_stats().misses == 1);

    TerrainCachePackHeader header;
    std::vector<TerrainCacheChunkRecord> index;
    CHECK(decode_terrain_cache_pack_index(files[paths[0]].data(), files[paths[0]].size(),
                                          key, header, index));
    CHECK(index.size() == 1);
    if (index.size() == 1) {
        files[paths[0]][index[0].payload_offset] ^= 0xFF;
        CHECK_FALSE(reader.read_chunk(0, 0, 0, BlockRegistry::get_instance(), chunk, spill));
        CHECK(reader.snapshot_stats().corrupt == 1);
    }

    files.erase(paths[1]);
    CHECK_FALSE(reader.read_chunk(8, 0, 0, BlockRegistry::get_instance(), chunk, spill));
    CHECK(reader.snapshot_stats().read_errors == 1);

    TerrainCacheReader wrong_key;
    CHECK_FALSE(wrong_key.open(root, key + 1, paths, read_from(files)));
    CHECK_FALSE(wrong_key.is_open());
    CHECK(wrong_key.snapshot_stats().open_failures == 1);
}

TEST_CASE("terrain cache reader serves one chunk from several workers") {
    BlockRegistry::get_instance().initialize_default_blocks();
    const uint64_t key = 99;
    std::string root;
    Files files = make_cache(key, root);
    const std::vector<std::string> paths = {
        root + "/r_0_0.ftc",
        root + "/r_1_0.ftc",
    };
    TerrainCacheReader reader;
    CHECK(reader.open(root, key, paths, read_from(files)));

    std::atomic<int> succeeded{0};
    std::vector<std::thread> workers;
    workers.reserve(4);
    for (int t = 0; t < 4; ++t) {
        workers.emplace_back([&] {
            for (int i = 0; i < 32; ++i) {
                ChunkData chunk;
                std::vector<TerrainCacheSpill> spill;
                if (reader.read_chunk(0, 0, 0, BlockRegistry::get_instance(), chunk, spill) &&
                    chunk.get_block(0, 0, 0) == BlockIDs::STONE) {
                    succeeded.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (std::thread& worker : workers) worker.join();
    CHECK(succeeded.load() == 4 * 32);
    CHECK(reader.snapshot_stats().hits == 4 * 32);
}
