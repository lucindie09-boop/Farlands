#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/crc32.hpp"
#include "core/terrain_cache_format.hpp"
#include "core/terrain_params.hpp"

#include <array>
#include <cstring>
#include <vector>

using namespace VoxelEngine;

namespace {

void init_registry() {
    BlockRegistry::get_instance().initialize_default_blocks();
}

ChunkData make_mixed_chunk() {
    ChunkData chunk;
    chunk.set_block(0, 0, 0, BlockIDs::GRASS);
    chunk.set_block(15, 15, 15, BlockIDs::STONE);
    chunk.set_block(16, 16, 16, BlockIDs::DIRT);
    chunk.set_block(31, 31, 31, BlockIDs::SURFACE_WATER);
    chunk.set_block(20, 4, 7, BlockIDs::SURFACE_WATER);
    return chunk;
}

void check_same_chunk(const ChunkData& expected, const ChunkData& actual) {
    CHECK(actual.get_block_count() == expected.get_block_count());
    CHECK(actual.get_emissive_count() == expected.get_emissive_count());
    CHECK(actual.liquid_count() == expected.liquid_count());
    CHECK(actual.is_all_air() == expected.is_all_air());
    CHECK(actual.fully_solid() == expected.fully_solid());
    CHECK(actual.uniform_block_sections() == expected.uniform_block_sections());

    for (int32_t y = 0; y < CHUNK_HEIGHT; ++y) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                if (actual.get_block(x, y, z) != expected.get_block(x, y, z)) {
                    CHECK_MESSAGE(false, "block differs at " << x << "," << y << "," << z);
                    return;
                }
            }
        }
    }
}

bool decode_valid_chunk(const std::vector<uint8_t>& payload,
                        const BlockRegistry& registry,
                        ChunkData& chunk,
                        std::vector<TerrainCacheSpill>& spill) {
    return decode_terrain_cache_chunk(payload.data(), payload.size(), registry, chunk, spill);
}

std::vector<uint8_t> bytes_of(const char* text) {
    return std::vector<uint8_t>(text, text + std::strlen(text));
}

TerrainParams base_params() {
    TerrainParams params;
    params.seed = 12345;
    params.sea_level = 200.0f;
    params.bedrock_height = 5;
    params.cave_threshold = 0.4f;
    params.cave_scale = 0.05f;
    params.subsurface_cover_depth = 4;
    params.climate_temp_scale = 0.000125f;
    params.climate_humidity_scale = 0.000125f;
    params.biome_size = 1.0f;
    return params;
}

uint64_t key_for(const TerrainParams& params,
                 const std::vector<uint8_t>& terrain = bytes_of("terrain"),
                 const std::vector<uint8_t>& biomes = bytes_of("biomes"),
                 const std::vector<uint8_t>& vegetation = bytes_of("vegetation"),
                 const std::vector<uint8_t>& blocks = bytes_of("blocks")) {
    return terrain_cache_key(params, terrain, biomes, vegetation, blocks);
}

} // namespace

TEST_CASE("terrain cache chunk round-trips air, solid and mixed sections") {
    init_registry();
    const BlockRegistry& registry = BlockRegistry::get_instance();

    ChunkData air;
    std::vector<uint8_t> air_payload;
    std::vector<TerrainCacheSpill> no_spill;
    CHECK(encode_terrain_cache_chunk(air, no_spill, air_payload));
    CHECK_FALSE(air_payload.empty());

    ChunkData air_decoded;
    std::vector<TerrainCacheSpill> air_spill;
    CHECK(decode_valid_chunk(air_payload, registry, air_decoded, air_spill));
    check_same_chunk(air, air_decoded);
    CHECK(air_spill.empty());

    ChunkData solid;
    solid.fill_blocks(BlockIDs::STONE);
    std::vector<uint8_t> solid_payload;
    CHECK(encode_terrain_cache_chunk(solid, no_spill, solid_payload));
    ChunkData solid_decoded;
    std::vector<TerrainCacheSpill> solid_spill;
    CHECK(decode_valid_chunk(solid_payload, registry, solid_decoded, solid_spill));
    check_same_chunk(solid, solid_decoded);
    CHECK(solid_decoded.fully_solid());

    ChunkData mixed = make_mixed_chunk();
    std::vector<TerrainCacheSpill> one_spill = {{32, 4, 7, BlockIDs::STONE}};
    std::vector<uint8_t> one_payload;
    CHECK(encode_terrain_cache_chunk(mixed, one_spill, one_payload));
    ChunkData one_decoded;
    std::vector<TerrainCacheSpill> one_decoded_spill;
    CHECK(decode_valid_chunk(one_payload, registry, one_decoded, one_decoded_spill));
    CHECK(one_decoded_spill.size() == 1);
    if (one_decoded_spill.size() == 1) {
        CHECK(one_decoded_spill[0].x == 32);
        CHECK(one_decoded_spill[0].y == 4);
        CHECK(one_decoded_spill[0].z == 7);
        CHECK(one_decoded_spill[0].block == BlockIDs::STONE);
    }

    std::vector<TerrainCacheSpill> spill = {
        {0, 0, 0, BlockIDs::GRASS},
        {32, 5, 4, BlockIDs::STONE},
        {-1, 31, 31, BlockIDs::SURFACE_WATER},
    };
    std::vector<uint8_t> mixed_payload;
    CHECK(encode_terrain_cache_chunk(mixed, spill, mixed_payload));
    ChunkData mixed_decoded;
    std::vector<TerrainCacheSpill> mixed_spill;
    CHECK(decode_valid_chunk(mixed_payload, registry, mixed_decoded, mixed_spill));
    check_same_chunk(mixed, mixed_decoded);
    if (mixed_spill.size() == spill.size()) {
        for (size_t i = 0; i < spill.size(); ++i) {
            CHECK(mixed_spill[i].x == spill[i].x);
            CHECK(mixed_spill[i].y == spill[i].y);
            CHECK(mixed_spill[i].z == spill[i].z);
            CHECK(mixed_spill[i].block == spill[i].block);
        }
    } else {
        CHECK_MESSAGE(false, "spill count changed");
    }

    CHECK(mixed_decoded.get_emissive_count() == mixed.get_emissive_count());
    CHECK(mixed_decoded.liquid_count() == 2);
}

TEST_CASE("terrain cache section codec preserves 8- and 16-bit index widths") {
    init_registry();
    const BlockRegistry& registry = BlockRegistry::get_instance();

    std::array<PalSection, ChunkData::NUM_BLOCK_SECTIONS> sections;
    sections[0].palette = {BlockIDs::AIR, BlockIDs::STONE, BlockIDs::DIRT};
    sections[0].bpi = 8;
    sections[0].indices.assign(PaletteStorage::SEC_VOLUME, 0);
    sections[0].indices[0] = 1; // (0,0,0) stone
    sections[0].indices[1] = 2; // (1,0,0) dirt

    sections[1].palette = {BlockIDs::AIR, BlockIDs::GRASS};
    sections[1].bpi = 16;
    sections[1].indices.assign(PaletteStorage::SEC_VOLUME * 2, 0);
    sections[1].indices[0] = 1; // (16,0,0) grass, little-endian low byte
    sections[1].indices[2] = 1; // (17,0,0) grass, second u16

    ChunkData chunk;
    chunk.set_block_sections(sections.data());
    CHECK(chunk.get_block(0, 0, 0) == BlockIDs::STONE);
    CHECK(chunk.get_block(1, 0, 0) == BlockIDs::DIRT);
    CHECK(chunk.get_block(16, 0, 0) == BlockIDs::GRASS);
    CHECK(chunk.get_block(17, 0, 0) == BlockIDs::GRASS);

    std::vector<uint8_t> payload;
    std::vector<TerrainCacheSpill> no_spill;
    CHECK(encode_terrain_cache_chunk(chunk, no_spill, payload));

    ChunkData decoded;
    std::vector<TerrainCacheSpill> decoded_spill;
    CHECK(decode_valid_chunk(payload, registry, decoded, decoded_spill));
    check_same_chunk(chunk, decoded);
    CHECK(decoded.block_section(0).bpi == 8);
    CHECK(decoded.block_section(1).bpi == 16);
}

TEST_CASE("terrain cache chunk rejects malformed payloads") {
    init_registry();
    const BlockRegistry& registry = BlockRegistry::get_instance();
    ChunkData chunk;
    std::vector<uint8_t> payload;
    std::vector<TerrainCacheSpill> spill;
    CHECK(encode_terrain_cache_chunk(chunk, spill, payload));

    ChunkData decoded;
    std::vector<TerrainCacheSpill> decoded_spill;

    std::vector<uint8_t> bad_kind = payload;
    bad_kind[0] = 2;
    CHECK_FALSE(decode_valid_chunk(bad_kind, registry, decoded, decoded_spill));

    std::vector<uint8_t> unknown_block = payload;
    unknown_block[1] = 250;
    CHECK(registry.get_count() <= 250);
    CHECK_FALSE(decode_valid_chunk(unknown_block, registry, decoded, decoded_spill));

    std::vector<uint8_t> truncated = payload;
    truncated.pop_back();
    CHECK_FALSE(decode_valid_chunk(truncated, registry, decoded, decoded_spill));

    CHECK_FALSE(decode_terrain_cache_chunk(nullptr, 4, registry, decoded, decoded_spill));
    CHECK_FALSE(decode_terrain_cache_chunk(payload.data(), 0, registry, decoded, decoded_spill));
}

TEST_CASE("terrain cache pack index and payloads round-trip") {
    init_registry();
    const BlockRegistry& registry = BlockRegistry::get_instance();
    ChunkData first = make_mixed_chunk();
    ChunkData second;
    second.fill_blocks(BlockIDs::DIRT);

    std::vector<TerrainCacheSpill> no_spill;
    std::vector<TerrainCacheEncodedChunk> chunks(2);
    chunks[0].x = 0;
    chunks[0].y = 0;
    chunks[0].z = 0;
    CHECK(encode_terrain_cache_chunk(first, no_spill, chunks[0].payload));
    chunks[1].x = 7;
    chunks[1].y = 1;
    chunks[1].z = 7;
    CHECK(encode_terrain_cache_chunk(second, no_spill, chunks[1].payload));

    constexpr uint64_t key = 0x123456789ABCDEF0ULL;
    std::vector<uint8_t> pack;
    CHECK(encode_terrain_cache_pack(key, 0, 0, chunks, pack));
    CHECK(pack.size() >= kTerrainCacheHeaderSize + 2 * kTerrainCacheIndexEntrySize);

    TerrainCachePackHeader header;
    std::vector<TerrainCacheChunkRecord> index;
    CHECK(decode_terrain_cache_pack_index(pack.data(), pack.size(), key, header, index));
    CHECK(header.key == key);
    CHECK(header.chunk_count == 2);
    CHECK(index.size() == 2);
    if (index.size() != 2) return;
    CHECK(index[0].x == 0);
    CHECK(index[0].y == 0);
    CHECK(index[0].z == 0);
    CHECK(index[1].x == 7);
    CHECK(index[1].y == 1);
    CHECK(index[1].z == 7);

    ChunkData decoded;
    std::vector<TerrainCacheSpill> decoded_spill;
    CHECK(decode_terrain_cache_pack_chunk(pack.data(), pack.size(), index[0],
                                          registry, decoded, decoded_spill));
    check_same_chunk(first, decoded);
    CHECK(decoded_spill.empty());

    CHECK(decode_terrain_cache_pack_chunk(pack.data(), pack.size(), index[1],
                                          registry, decoded, decoded_spill));
    check_same_chunk(second, decoded);
}

TEST_CASE("terrain cache pack rejects bad keys, CRCs and boundaries") {
    init_registry();
    const BlockRegistry& registry = BlockRegistry::get_instance();
    ChunkData chunk;
    chunk.set_block(1, 2, 3, BlockIDs::STONE);
    std::vector<uint8_t> payload;
    std::vector<TerrainCacheSpill> no_spill;
    CHECK(encode_terrain_cache_chunk(chunk, no_spill, payload));

    TerrainCacheEncodedChunk encoded;
    encoded.x = 0;
    encoded.y = 0;
    encoded.z = 0;
    encoded.payload = payload;
    constexpr uint64_t key = 0xABCDEF0123456789ULL;
    std::vector<uint8_t> pack;
    CHECK(encode_terrain_cache_pack(key, 0, 0, {encoded}, pack));

    TerrainCachePackHeader header;
    std::vector<TerrainCacheChunkRecord> index;
    CHECK(decode_terrain_cache_pack_index(pack.data(), pack.size(), key, header, index));

    CHECK_FALSE(decode_terrain_cache_pack_index(pack.data(), pack.size(), key + 1, header, index));

    std::vector<uint8_t> bad_payload = pack;
    bad_payload[kTerrainCacheHeaderSize] ^= 0x01;
    ChunkData decoded;
    std::vector<TerrainCacheSpill> decoded_spill;
    CHECK_FALSE(decode_terrain_cache_pack_chunk(bad_payload.data(), bad_payload.size(),
                                                index[0], registry, decoded, decoded_spill));

    std::vector<uint8_t> bad_index = pack;
    bad_index[header.index_offset] ^= 0x01;
    CHECK_FALSE(decode_terrain_cache_pack_index(bad_index.data(), bad_index.size(),
                                                key, header, index));

    std::vector<uint8_t> truncated = pack;
    truncated.pop_back();
    CHECK_FALSE(decode_terrain_cache_pack_index(truncated.data(), truncated.size(),
                                                key, header, index));

    std::vector<uint8_t> bad_count = pack;
    bad_count[24] = 0xFF;
    bad_count[25] = 0xFF;
    CHECK_FALSE(decode_terrain_cache_pack_index(bad_count.data(), bad_count.size(),
                                                key, header, index));

    std::vector<uint8_t> bad_offset = pack;
    bad_offset[28] = 0xFF;
    bad_offset[29] = 0xFF;
    CHECK_FALSE(decode_terrain_cache_pack_index(bad_offset.data(), bad_offset.size(),
                                                key, header, index));
}

TEST_CASE("terrain cache pack rejects duplicate and out-of-region chunks") {
    init_registry();
    ChunkData chunk;
    std::vector<uint8_t> payload;
    std::vector<TerrainCacheSpill> no_spill;
    CHECK(encode_terrain_cache_chunk(chunk, no_spill, payload));

    TerrainCacheEncodedChunk encoded;
    encoded.y = 0;
    encoded.payload = payload;

    encoded.x = 0;
    encoded.z = 0;
    std::vector<uint8_t> pack;
    CHECK(encode_terrain_cache_pack(1, 0, 0, {encoded, encoded}, pack) == false);
    CHECK(encode_terrain_cache_pack(1, 0, 0, {encoded}, pack));

    encoded.x = kTerrainCacheRegionChunks;
    CHECK_FALSE(encode_terrain_cache_pack(1, 0, 0, {encoded}, pack));
    encoded.x = 0;
    encoded.z = -1;
    CHECK_FALSE(encode_terrain_cache_pack(1, 0, 0, {encoded}, pack));
}

TEST_CASE("terrain cache key is stable and covers every persisted input and data file") {
    init_registry();
    const TerrainParams base = base_params();
    const uint64_t expected = key_for(base, bytes_of("terrain"), bytes_of("biomes"),
                                      bytes_of("vegetation"), bytes_of("blocks"));
    CHECK(key_for(base) == expected);

    TerrainParams changed = base;
    changed.seed += 1;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.sea_level += 1.0f;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.bedrock_height += 1;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.cave_threshold += 0.01f;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.cave_scale += 0.001f;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.subsurface_cover_depth += 1;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.climate_temp_scale += 0.000001f;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.climate_humidity_scale += 0.000001f;
    CHECK(key_for(changed) != expected);
    changed = base;
    changed.biome_size += 0.1f;
    CHECK(key_for(changed) != expected);

    CHECK(key_for(base, bytes_of("terrain2")) != expected);
    CHECK(key_for(base, bytes_of("terrain"), bytes_of("biomes2")) != expected);
    CHECK(key_for(base, bytes_of("terrain"), bytes_of("biomes"),
                  bytes_of("vegetation2")) != expected);
    CHECK(key_for(base, bytes_of("terrain"), bytes_of("biomes"),
                  bytes_of("vegetation"), bytes_of("blocks2")) != expected);
}
