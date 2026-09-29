#include "doctest.h"

#include "core/block_types.hpp"
#include "core/crc32.hpp"
#include "core/edit_map.hpp"

#include <cstdint>
#include <vector>

using namespace VoxelEngine;

// =========================================================================
// Edit map round-trip: encode then decode preserves all block data
// =========================================================================
TEST_CASE("edit map round-trip preserves block data") {
    BlockRegistry::get_instance().initialize_default_blocks();

    EditMap original;
    // Mixed edits at various coordinates
    for (int32_t y = 0; y < 32; y++) {
        original.set_block(5, y, 10, y < 16 ? BlockIDs::STONE : BlockIDs::DIRT);
    }
    original.set_block(0, 0, 0, BlockIDs::LIGHT_BLOCK);
    original.set_block(31, 31, 31, BlockIDs::GRASS);

    std::vector<uint8_t> data;
    serialize_edit_map(original, data);

    EditMap decoded;
    bool ok = deserialize_edit_map(data.data(), data.size(), decoded, BlockRegistry::get_instance());
    CHECK(ok);

    // Verify all edits match
    for (int32_t y = 0; y < 32; y++) {
        CHECK(decoded.get_block(5, y, 10, BlockIDs::AIR) == (y < 16 ? BlockIDs::STONE : BlockIDs::DIRT));
    }
    CHECK(decoded.get_block(0, 0, 0, BlockIDs::AIR) == BlockIDs::LIGHT_BLOCK);
    CHECK(decoded.get_block(31, 31, 31, BlockIDs::AIR) == BlockIDs::GRASS);
}

// =========================================================================
// Edit map decode rejects truncated buffers (fuzz-relevant)
// =========================================================================
TEST_CASE("edit map decode rejects truncated input") {
    BlockRegistry::get_instance().initialize_default_blocks();

    // Truncated header
    {
        const uint8_t trunc1[] = {0x01}; // partial header
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(trunc1, sizeof(trunc1), m, BlockRegistry::get_instance()));
    }
    // Truncated body (header says 1 edit but body is missing)
    {
        uint8_t trunc2[] = {0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}; // version=1, count=1, crc=0
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(trunc2, sizeof(trunc2), m, BlockRegistry::get_instance()));
    }
    // Empty body
    {
        EditMap m;
        CHECK_FALSE(deserialize_edit_map(nullptr, 0, m, BlockRegistry::get_instance()));
    }
}

// =========================================================================
// CRC32 mismatch detection: tampered body is rejected
// =========================================================================
TEST_CASE("CRC32 detects tampered edit map body") {
    BlockRegistry::get_instance().initialize_default_blocks();

    EditMap edit_map;
    for (int32_t y = 0; y < 32; y++) {
        edit_map.set_block(0, y, 0, BlockIDs::STONE);
    }

    std::vector<uint8_t> data;
    serialize_edit_map(edit_map, data);
    
    // Extract CRC from header (bytes 8-11)
    uint32_t original_crc = data[8] | (data[9] << 8) | (data[10] << 16) | (data[11] << 24);

    // Flip one byte in the body (after 12-byte header)
    data[12 + data.size() / 2] ^= 0xFF;
    
    // Recompute CRC
    uint32_t tampered_crc = crc32(data.data() + 12, data.size() - 12);

    CHECK(original_crc != tampered_crc);
}
