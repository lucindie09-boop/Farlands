#ifndef FARLANDS_EDIT_MAP_HPP
#define FARLANDS_EDIT_MAP_HPP

#include "core/chunk_coords.hpp"
#include "core/block_types.hpp"
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {

// Forward declaration
class BlockRegistry;

// Edit map format: sparse per-chunk block edits
// Key: packed local coordinate (15 bits: 5 for x, 5 for y, 5 for z)
// Value: block ID (uint16_t)
// Each entry serializes to 4 bytes total

struct EditMap {
    std::unordered_map<uint32_t, BlockID> edits;

    // Pack local coordinates into a 15-bit key
    static inline uint32_t pack_coord(int32_t local_x, int32_t local_y, int32_t local_z) {
        return (static_cast<uint32_t>(local_x) & 0x1F) |
               ((static_cast<uint32_t>(local_y) & 0x1F) << 5) |
               ((static_cast<uint32_t>(local_z) & 0x1F) << 10);
    }

    // Unpack local coordinates from a 15-bit key
    static inline void unpack_coord(uint32_t key, int32_t& local_x, int32_t& local_y, int32_t& local_z) {
        local_x = static_cast<int32_t>(key & 0x1F);
        local_y = static_cast<int32_t>((key >> 5) & 0x1F);
        local_z = static_cast<int32_t>((key >> 10) & 0x1F);
    }

    // Add or update an edit at the given local coordinates (last-write-wins)
    inline void set_block(int32_t local_x, int32_t local_y, int32_t local_z, BlockID block_id) {
        uint32_t key = pack_coord(local_x, local_y, local_z);
        edits[key] = block_id;
    }

    // Remove an edit (revert to generated terrain)
    inline void remove_block(int32_t local_x, int32_t local_y, int32_t local_z) {
        uint32_t key = pack_coord(local_x, local_y, local_z);
        edits.erase(key);
    }

    // Check if there's an edit at the given coordinates
    inline bool has_edit(int32_t local_x, int32_t local_y, int32_t local_z) const {
        uint32_t key = pack_coord(local_x, local_y, local_z);
        return edits.find(key) != edits.end();
    }

    // Get the edited block ID, or default if not edited
    inline BlockID get_block(int32_t local_x, int32_t local_y, int32_t local_z, BlockID default_id) const {
        uint32_t key = pack_coord(local_x, local_y, local_z);
        auto it = edits.find(key);
        if (it != edits.end()) {
            return it->second;
        }
        return default_id;
    }

    // Check if the edit map is empty
    inline bool empty() const {
        return edits.empty();
    }

    // Clear all edits
    inline void clear() {
        edits.clear();
    }

    // Get the number of edits
    inline size_t size() const {
        return edits.size();
    }
};

// Serialization format for edit maps:
// Header: [version:u32][count:u32][crc32:u32]
// Body: [coord_packed:u16][block_id:u16] repeated count times

constexpr uint32_t EDIT_MAP_VERSION = 1;

// Serialize an edit map to a byte buffer
void serialize_edit_map(const EditMap& edit_map, std::vector<uint8_t>& out);

// Deserialize an edit map from a byte buffer
// Returns true on success, false if the buffer is malformed, CRC mismatch, or contains
// block IDs not registered in the current session (cross-version save safety)
bool deserialize_edit_map(const uint8_t* data, size_t size, EditMap& out_edit_map, const BlockRegistry& registry);

class ChunkData;

// Apply every entry in an edit map to a chunk (last-write-wins over generated
// terrain) and recompute section flags / fully-solid masks. Pure shared logic
// so ChunkWorld::apply_edit_map_to_chunk (which owns the map lookup + mutex)
// and tests apply edits identically.
void apply_edit_map_to_chunk(const EditMap& edit_map, ChunkData& chunk_data);

// Outcome of recovering an edit map from the atomic on-disk layout
// (.edit primary + .bak backup produced by the .tmp -> .bak -> target write).
struct EditMapRecovery {
    bool recovered = false;  // true if any candidate deserialized
    bool used_backup = false; // true if the .edit was missing/corrupt and .bak won
};

// Decode the best available edit map given the raw .edit (primary) and .bak
// (backup) bytes. An empty buffer means "file missing". The primary is tried
// first (newest data); if it is missing or rejects (bad header, CRC mismatch,
// truncated, unknown block IDs), the backup is tried — this recoverable
// snapshot is what survives a crash between the two renames in the write
// pattern (see ChunkWorld::write_edit_map_file_locked). On failure out_edit_map
// is left empty. Pure shared logic: ChunkWorld::load_edit_map_from_disk and
// the crash-recovery tests call this.
EditMapRecovery recover_edit_map(const std::vector<uint8_t>& primary,
                                 const std::vector<uint8_t>& backup,
                                 EditMap& out_edit_map,
                                 const BlockRegistry& registry);

} // namespace VoxelEngine

#endif // FARLANDS_EDIT_MAP_HPP
