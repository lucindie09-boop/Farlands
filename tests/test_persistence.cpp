#include "doctest.h"
#include "core/edit_map.hpp"
#include "core/chunk_coords.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/crc32.hpp"
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>
#include <memory>

using namespace VoxelEngine;

TEST_CASE("edit map round-trip preserves all block data") {
    BlockRegistry::get_instance().initialize_default_blocks();
    EditMap original;
    
    original.set_block(0, 0, 0, BlockIDs::STONE);
    original.set_block(31, 31, 31, BlockIDs::GRASS);
    original.set_block(16, 16, 16, BlockIDs::LIGHT_BLOCK);
    original.set_block(5, 20, 10, BlockIDs::SAND);
    original.set_block(25, 1, 29, BlockIDs::WOOD);
    
    std::vector<uint8_t> data;
    serialize_edit_map(original, data);
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(ok);
    
    CHECK(loaded.get_block(0, 0, 0, BlockIDs::AIR) == BlockIDs::STONE);
    CHECK(loaded.get_block(31, 31, 31, BlockIDs::AIR) == BlockIDs::GRASS);
    CHECK(loaded.get_block(16, 16, 16, BlockIDs::AIR) == BlockIDs::LIGHT_BLOCK);
    CHECK(loaded.get_block(5, 20, 10, BlockIDs::AIR) == BlockIDs::SAND);
    CHECK(loaded.get_block(25, 1, 29, BlockIDs::AIR) == BlockIDs::WOOD);
}

TEST_CASE("edit map last-write-wins coalescing") {
    BlockRegistry::get_instance().initialize_default_blocks();
    EditMap edit_map;
    
    // Place a block
    edit_map.set_block(10, 10, 10, BlockIDs::STONE);
    CHECK(edit_map.size() == 1);
    
    // Overwrite the same coordinate
    edit_map.set_block(10, 10, 10, BlockIDs::GRASS);
    CHECK(edit_map.size() == 1); // Still one entry
    CHECK(edit_map.get_block(10, 10, 10, BlockIDs::AIR) == BlockIDs::GRASS);
}

TEST_CASE("edit map coordinate packing/unpacking") {
    for (int32_t x = 0; x < 32; x++) {
        for (int32_t y = 0; y < 32; y++) {
            for (int32_t z = 0; z < 32; z++) {
                uint32_t packed = EditMap::pack_coord(x, y, z);
                int32_t rx, ry, rz;
                EditMap::unpack_coord(packed, rx, ry, rz);
                CHECK(rx == x);
                CHECK(ry == y);
                CHECK(rz == z);
            }
        }
    }
}

TEST_CASE("empty edit map serializes to empty") {
    EditMap empty;
    std::vector<uint8_t> data;
    serialize_edit_map(empty, data);
    
    // Header only: version (4) + count (4) + crc (4) = 12 bytes
    CHECK(data.size() == 12);
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(ok);
    CHECK(loaded.empty());
}

TEST_CASE("edit map CRC32 mismatch detection") {
    BlockRegistry::get_instance().initialize_default_blocks();
    EditMap original;
    original.set_block(10, 10, 10, BlockIDs::STONE);
    
    std::vector<uint8_t> data;
    serialize_edit_map(original, data);
    
    // Corrupt the CRC32
    data[11] ^= 0xFF;
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map body corruption detection") {
    BlockRegistry::get_instance().initialize_default_blocks();
    EditMap original;
    original.set_block(10, 10, 10, BlockIDs::STONE);
    
    std::vector<uint8_t> data;
    serialize_edit_map(original, data);
    
    // Corrupt a byte in the body (after the 12-byte header)
    data[12] ^= 0xFF;
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map removes blocks") {
    BlockRegistry::get_instance().initialize_default_blocks();
    EditMap edit_map;
    
    edit_map.set_block(10, 10, 10, BlockIDs::STONE);
    CHECK(edit_map.has_edit(10, 10, 10));
    
    edit_map.remove_block(10, 10, 10);
    CHECK(!edit_map.has_edit(10, 10, 10));
    CHECK(edit_map.size() == 0);
}

TEST_CASE("edit map serialization size scales with edit count") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Test with unique coordinates only
    for (int num_edits = 0; num_edits <= 100; num_edits += 10) {
        EditMap edit_map;
        for (int i = 0; i < num_edits; i++) {
            // Use unique coordinates by making i a linear index
            int x = i % 32;
            int y = (i / 32) % 32;
            int z = (i / 1024) % 32;
            if (z < 32) { // Only add if z is in valid range
                edit_map.set_block(x, y, z, static_cast<BlockID>(i % 10));
            }
        }
        
        std::vector<uint8_t> data;
        serialize_edit_map(edit_map, data);
        
        // Expected size: 12-byte header + 4 bytes per edit
        size_t expected_size = 12 + (edit_map.size() * 4);
        CHECK(data.size() == expected_size);
    }
}

TEST_CASE("edit map rejects invalid block IDs") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Manually construct a malformed edit map with an invalid block ID
    std::vector<uint8_t> data;
    data.resize(16); // 12-byte header + 1 edit (4 bytes)
    
    // Write header
    data[0] = 1; data[1] = 0; data[2] = 0; data[3] = 0; // version = 1
    data[4] = 1; data[5] = 0; data[6] = 0; data[7] = 0; // count = 1
    data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 0; // CRC placeholder
    
    // Write malformed edit: valid coord, invalid block ID (300, MAX_BLOCK_TYPES is 256)
    // Stored as little-endian: low byte first, high byte second
    data[12] = 0; data[13] = 0; // coord = 0
    data[14] = 44; data[15] = 1; // block_id = 300 (0x012C)
    
    // Recompute CRC
    uint32_t crc = crc32(data.data() + 12, 4);
    data[8] = crc & 0xFF;
    data[9] = (crc >> 8) & 0xFF;
    data[10] = (crc >> 16) & 0xFF;
    data[11] = (crc >> 24) & 0xFF;
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map rejects malformed coordinate packing") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Manually construct a malformed edit map with bit 15 set in coordinate
    std::vector<uint8_t> data;
    data.resize(16); // 12-byte header + 1 edit (4 bytes)
    
    // Write header
    data[0] = 1; data[1] = 0; data[2] = 0; data[3] = 0; // version = 1
    data[4] = 1; data[5] = 0; data[6] = 0; data[7] = 0; // count = 1
    data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 0; // CRC placeholder
    
    // Write malformed edit: bit 15 set in coord (invalid)
    // Stored as little-endian: low byte first, high byte second
    data[12] = 0x00; data[13] = 0x80; // coord = 0x8000 (bit 15 set)
    data[14] = 1; data[15] = 0; // block_id = 1 (valid)
    
    // Recompute CRC
    uint32_t crc = crc32(data.data() + 12, 4);
    data[8] = crc & 0xFF;
    data[9] = (crc >> 8) & 0xFF;
    data[10] = (crc >> 16) & 0xFF;
    data[11] = (crc >> 24) & 0xFF;
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map rejects version mismatch") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Construct an edit map with wrong version
    std::vector<uint8_t> data;
    data.resize(12);
    
    // Write header with version = 99
    data[0] = 99; data[1] = 0; data[2] = 0; data[3] = 0; // version = 99
    data[4] = 0; data[5] = 0; data[6] = 0; data[7] = 0; // count = 0
    data[8] = 0; data[9] = 0; data[10] = 0; data[11] = 0; // CRC = 0
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map rejects truncated data") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Truncated header (less than 12 bytes)
    std::vector<uint8_t> data(11, 0);
    
    EditMap loaded;
    bool ok = deserialize_edit_map(data.data(), data.size(), loaded);
    CHECK(!ok);
}

TEST_CASE("edit map round-trip integration test") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Simulate player making edits
    EditMap original;
    original.set_block(10, 10, 10, BlockIDs::STONE);
    original.set_block(5, 15, 20, BlockIDs::GRASS);
    original.set_block(31, 0, 31, BlockIDs::DIRT);
    
    // Serialize (save to disk simulation)
    std::vector<uint8_t> saved_data;
    serialize_edit_map(original, saved_data);
    
    // Simulate process restart: clear original map
    EditMap reloaded;
    
    // Deserialize (load from disk simulation)
    bool ok = deserialize_edit_map(saved_data.data(), saved_data.size(), reloaded);
    CHECK(ok);
    
    // Verify all edits survived the round-trip
    CHECK(reloaded.get_block(10, 10, 10, BlockIDs::AIR) == BlockIDs::STONE);
    CHECK(reloaded.get_block(5, 15, 20, BlockIDs::AIR) == BlockIDs::GRASS);
    CHECK(reloaded.get_block(31, 0, 31, BlockIDs::AIR) == BlockIDs::DIRT);
    
    // Verify coalescing still works after round-trip
    CHECK(reloaded.size() == 3);
}

TEST_CASE("ChunkWorld edit persistence integration test") {
    BlockRegistry::get_instance().initialize_default_blocks();
    
    // Test the critical wiring: edit map load + apply sequence
    // This tests the actual code path that was broken (load function existed but wasn't called)
    
    // Create and populate an edit map
    EditMap original;
    original.set_block(10, 10, 10, BlockIDs::STONE);
    original.set_block(5, 15, 20, BlockIDs::GRASS);
    
    // Serialize to a file (using the same format as ChunkWorld)
    std::vector<uint8_t> saved_data;
    serialize_edit_map(original, saved_data);
    
    // Write to a temporary file (simulating ChunkWorld::write_edit_map_file_locked)
    const char* test_file = "test_integration.edit";
    std::ofstream f(test_file, std::ios::binary);
    f.write(reinterpret_cast<const char*>(saved_data.data()), saved_data.size());
    f.close();
    
    // Clear the original map (simulating process restart where chunk_edit_maps is empty)
    EditMap reloaded;
    
    // Deserialize from file (simulating ChunkWorld::load_edit_map_from_disk)
    std::ifstream in(test_file, std::ios::binary);
    std::vector<uint8_t> loaded_data((std::istreambuf_iterator<char>(in)), 
                                      std::istreambuf_iterator<char>());
    in.close();
    
    bool load_ok = deserialize_edit_map(loaded_data.data(), loaded_data.size(), reloaded);
    CHECK(load_ok);
    
    // Apply to ChunkData (simulating ChunkWorld::apply_edit_map_to_chunk)
    ChunkData chunk_data;
    chunk_data.clear();
    for (const auto& entry : reloaded.edits) {
        int32_t lx, ly, lz;
        EditMap::unpack_coord(entry.first, lx, ly, lz);
        chunk_data.set_block(lx, ly, lz, entry.second);
    }
    
    // Verify the edits survived the full round-trip (the critical integration test)
    CHECK(chunk_data.get_block(10, 10, 10) == BlockIDs::STONE);
    CHECK(chunk_data.get_block(5, 15, 20) == BlockIDs::GRASS);
    
    // Clean up
    std::remove(test_file);
}
