#include "core/edit_map.hpp"
#include "core/crc32.hpp"
#include "core/block_types.hpp"
#include <cstring>

namespace VoxelEngine {

void serialize_edit_map(const EditMap& edit_map, std::vector<uint8_t>& out) {
    out.clear();
    
    // Calculate body size: 4 bytes per edit (2 bytes coord + 2 bytes block_id)
    size_t body_size = edit_map.size() * 4;
    
    // Reserve space for header (12 bytes) + body
    out.reserve(12 + body_size);
    
    // Write header placeholder (we'll fill in CRC after writing body)
    out.resize(12);
    
    // Write body
    for (const auto& entry : edit_map.edits) {
        uint32_t packed_coord = entry.first;
        uint16_t block_id = static_cast<uint16_t>(entry.second);
        
        // Packed coordinate (lower 16 bits, only 15 used)
        out.push_back(packed_coord & 0xFF);
        out.push_back((packed_coord >> 8) & 0xFF);
        
        // Block ID
        out.push_back(block_id & 0xFF);
        out.push_back((block_id >> 8) & 0xFF);
    }
    
    // Calculate CRC32 of the body
    uint32_t crc = crc32(out.data() + 12, body_size);
    
    // Fill in header
    size_t pos = 0;
    // Version
    out[pos++] = EDIT_MAP_VERSION & 0xFF;
    out[pos++] = (EDIT_MAP_VERSION >> 8) & 0xFF;
    out[pos++] = (EDIT_MAP_VERSION >> 16) & 0xFF;
    out[pos++] = (EDIT_MAP_VERSION >> 24) & 0xFF;
    
    // Count
    uint32_t count = static_cast<uint32_t>(edit_map.size());
    out[pos++] = count & 0xFF;
    out[pos++] = (count >> 8) & 0xFF;
    out[pos++] = (count >> 16) & 0xFF;
    out[pos++] = (count >> 24) & 0xFF;
    
    // CRC32
    out[pos++] = crc & 0xFF;
    out[pos++] = (crc >> 8) & 0xFF;
    out[pos++] = (crc >> 16) & 0xFF;
    out[pos++] = (crc >> 24) & 0xFF;
}

bool deserialize_edit_map(const uint8_t* data, size_t size, EditMap& out_edit_map, const BlockRegistry& registry) {
    out_edit_map.clear();

    // Minimum size: header (12 bytes)
    if (size < 12) return false;

    // Read header
    size_t pos = 0;
    uint32_t version = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    if (version != EDIT_MAP_VERSION) return false;

    uint32_t count = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    uint32_t expected_crc = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | (data[pos + 3] << 24);
    pos += 4;

    // Validate body size
    size_t expected_body_size = static_cast<size_t>(count) * 4;
    if (size != 12 + expected_body_size) return false;

    // Validate CRC32
    uint32_t actual_crc = crc32(data + 12, expected_body_size);
    if (actual_crc != expected_crc) return false;

    // Get the actual registered block count for validation
    size_t registered_count = registry.get_count();

    // Read body
    for (uint32_t i = 0; i < count; i++) {
        if (pos + 4 > size) return false;

        uint16_t packed_coord = data[pos] | (data[pos + 1] << 8);
        pos += 2;

        uint16_t block_id = data[pos] | (data[pos + 1] << 8);
        pos += 2;

        // Validate packed coordinate (should only use 15 bits)
        if (packed_coord & 0x8000) return false; // Bit 15 should be 0

        // Validate block ID against actual registered count (cross-version save safety)
        // Rejects IDs from newer builds that have more block types than current build
        if (block_id >= registered_count) return false;

        out_edit_map.edits[packed_coord] = static_cast<BlockID>(block_id);
    }

    return true;
}

} // namespace VoxelEngine
