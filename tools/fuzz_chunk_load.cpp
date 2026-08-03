#include "core/edit_map.hpp"
#include "core/block_types.hpp"
#include "core/crc32.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>

using namespace VoxelEngine;

// Fuzz the edit map loading path: validate header, verify CRC32, then
// exercise the edit map deserializer on the body bytes.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 12) return 0;

    BlockRegistry::get_instance().initialize_default_blocks();

    // Parse edit map header: [version:u32][count:u32][crc32:u32]
    uint32_t version, count, stored_crc;
    std::memcpy(&version, data, 4);
    std::memcpy(&count, data + 4, 4);
    std::memcpy(&stored_crc, data + 8, 4);

    // Only fuzz the current edit map version (1)
    if (version != 1) return 0;

    // Sanity check: count should be reasonable (reject obviously malicious inputs)
    // Maximum reasonable count: 32^3 = 32768 blocks, each 4 bytes = 131KB body
    // We cap at 10000 edits to avoid excessive memory allocation during fuzzing
    if (count > 10000) return 0;

    const uint8_t* body = data + 12;
    size_t body_size = size - 12;

    // Expected body size: 4 bytes per edit
    if (body_size != count * 4) {
        // Size mismatch - still attempt decode to test error handling
        EditMap edit_map;
        deserialize_edit_map(data, size, edit_map);
        return 0;
    }

    // Exercise CRC32 on the raw body bytes
    uint32_t actual_crc = crc32(body, body_size);

    // Only decode if CRC matches (fuzzer may find intentional mismatches)
    if (actual_crc == stored_crc) {
        EditMap edit_map1;
        if (deserialize_edit_map(data, size, edit_map1)) {
            // Round-trip check: decode → encode → decode, then compare.
            // This catches real encode/decode bugs while avoiding false positives
            // on non-canonical-but-valid inputs (which would fail CRC comparison).
            std::vector<uint8_t> re_encoded;
            serialize_edit_map(edit_map1, re_encoded);

            EditMap edit_map2;
            if (deserialize_edit_map(re_encoded.data(), re_encoded.size(), edit_map2)) {
                // Verify all edits match between the two decode pipelines
                if (edit_map1.size() != edit_map2.size()) {
                    abort();
                }
                for (const auto& entry : edit_map1.edits) {
                    auto it = edit_map2.edits.find(entry.first);
                    if (it == edit_map2.edits.end() || it->second != entry.second) {
                        abort();
                    }
                }
            }
        }
    } else {
        // CRC mismatch: still attempt decode to test error handling.
        // The decoder should reject malformed input without crashing.
        EditMap edit_map;
        deserialize_edit_map(data, size, edit_map);
    }

    return 0;
}
