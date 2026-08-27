#include "core/edit_map.hpp"
#include "core/block_types.hpp"
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>

using namespace VoxelEngine;

// Fuzz the crash-recovery decision path (recover_edit_map): arbitrary primary
// (.edit) and backup (.bak) bytes. Feed layout:
//   [primary_len:u32][primary bytes...][backup bytes...]
// The harness asserts no crash, no partial map on failure, a fully-decodable
// primary never loses to the backup, and every recovered map round-trips
// consistently through serialize + deserialize.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 4) return 0;

    BlockRegistry::get_instance().initialize_default_blocks();

    uint32_t primary_len;
    std::memcpy(&primary_len, data, 4);
    if (primary_len > size - 4) {
        primary_len = static_cast<uint32_t>(size - 4);
    }

    const uint8_t* primary_bytes = data + 4;
    const uint8_t* backup_bytes = data + 4 + primary_len;
    size_t backup_len = size - 4 - primary_len;

    std::vector<uint8_t> primary(primary_bytes, primary_bytes + primary_len);
    std::vector<uint8_t> backup(backup_bytes, backup_bytes + backup_len);

    EditMap map;
    EditMapRecovery rec = recover_edit_map(primary, backup, map, BlockRegistry::get_instance());

    if (rec.recovered) {
        // The recovered map must round-trip consistently.
        std::vector<uint8_t> re_encoded;
        serialize_edit_map(map, re_encoded);
        EditMap again;
        if (!deserialize_edit_map(re_encoded.data(), re_encoded.size(), again,
                                  BlockRegistry::get_instance())) {
            abort();
        }
        if (again.size() != map.size()) {
            abort();
        }
        for (const auto& entry : map.edits) {
            auto it = again.edits.find(entry.first);
            if (it == again.edits.end() || it->second != entry.second) {
                abort();
            }
        }
    } else {
        // Failed recovery must never leave partial data behind.
        if (map.size() != 0) {
            abort();
        }
    }

    // A standalone-decodable primary must never be silently sacrificed for the
    // backup (the backup only exists to cover missing/corrupt primaries).
    EditMap primary_only;
    if (primary_len >= 12 &&
        deserialize_edit_map(primary.data(), primary_len, primary_only,
                             BlockRegistry::get_instance())) {
        if (rec.used_backup) {
            abort();
        }
    }

    return 0;
}