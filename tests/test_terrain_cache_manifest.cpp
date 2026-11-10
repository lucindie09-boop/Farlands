#include "doctest.h"

#include "core/terrain_cache_manifest.hpp"

#include <cstdint>
#include <vector>

using namespace VoxelEngine;

TEST_CASE("terrain cache manifest round-trips its fields") {
    TerrainCacheManifest manifest;
    manifest.key = 0xDEADBEEF12345678ULL;
    manifest.pack_count = 51;
    manifest.total_bytes = 110000000;
    manifest.created_unix = 1700000000;

    std::vector<uint8_t> bytes;
    CHECK(encode_terrain_cache_manifest(manifest, bytes));
    CHECK(bytes.size() == kTerrainCacheManifestSize);

    TerrainCacheManifest decoded;
    CHECK(decode_terrain_cache_manifest(bytes.data(), bytes.size(), decoded));
    CHECK(decoded.format_version == kTerrainCacheManifestVersion);
    CHECK(decoded.key == manifest.key);
    CHECK(decoded.pack_count == manifest.pack_count);
    CHECK(decoded.total_bytes == manifest.total_bytes);
    CHECK(decoded.created_unix == manifest.created_unix);
}

TEST_CASE("terrain cache manifest rejects truncation, bad magic and a huge pack count") {
    TerrainCacheManifest manifest;
    manifest.pack_count = 1;
    std::vector<uint8_t> bytes;
    CHECK(encode_terrain_cache_manifest(manifest, bytes));

    TerrainCacheManifest decoded;
    CHECK_FALSE(decode_terrain_cache_manifest(bytes.data(), bytes.size() - 1, decoded));

    std::vector<uint8_t> bad_magic = bytes;
    bad_magic[0] ^= 0xFF;
    CHECK_FALSE(decode_terrain_cache_manifest(bad_magic.data(), bad_magic.size(), decoded));

    std::vector<uint8_t> bad_count = bytes;
    bad_count[16] = 0xFF;
    bad_count[17] = 0xFF;
    bad_count[18] = 0xFF;
    bad_count[19] = 0xFF;
    CHECK_FALSE(decode_terrain_cache_manifest(bad_count.data(), bad_count.size(), decoded));
}

TEST_CASE("terrain cache pack filenames round-trip signed regions") {
    CHECK(terrain_cache_pack_filename(0, 0) == "r_0_0.ftc");
    CHECK(terrain_cache_pack_filename(-2, 7) == "r_-2_7.ftc");

    for (const auto& pair : {std::pair<int32_t, int32_t>{0, 0},
                             std::pair<int32_t, int32_t>{-2, 7},
                             std::pair<int32_t, int32_t>{12, -4}}) {
        const std::string name = terrain_cache_pack_filename(pair.first, pair.second);
        int32_t x = 0;
        int32_t z = 0;
        CHECK(parse_terrain_cache_pack_filename(name, x, z));
        CHECK(x == pair.first);
        CHECK(z == pair.second);
    }

    int32_t x = 0;
    int32_t z = 0;
    CHECK_FALSE(parse_terrain_cache_pack_filename("r_0.ftc", x, z));
    CHECK_FALSE(parse_terrain_cache_pack_filename("r_0_0.bin", x, z));
    CHECK_FALSE(parse_terrain_cache_pack_filename("x_0_0.ftc", x, z));
    CHECK_FALSE(parse_terrain_cache_pack_filename("r_0_0_0.ftc", x, z));
}
