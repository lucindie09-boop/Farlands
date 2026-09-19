#ifndef FARLANDS_SCHEMATIC_READER_HPP
#define FARLANDS_SCHEMATIC_READER_HPP

// -----------------------------------------------------------------------------
// Classic block-file decoding.
//
// The `.schematic` format is a gzip'd NBT tree whose root compound is named
// "Schematic" and holds the build the same way every writer since MCEdit has
// laid it out: three dimensions, an array of legacy numeric block ids, and a
// parallel array of per-block data values. Blocks are numbered oldest-first,
// which is why a build carries ids that no longer exist in any modern version —
// decoding is only half the job, and what an id means is decided later by a
// translation table (see mc_palette).
//
// This file is format only: no engine types, no Godot, no opinion about what a
// block should become. It hands back a palette of (id, data) states plus one
// palette index per cell, which is compact enough for a big build and keeps the
// translation step free to be data-driven.
//
// A note on the two layouts of `Data`. Writers disagree: some emit one byte per
// block, others pack two blocks into each byte's nibbles. They are told apart by
// the array length, never by assumption — reading a per-byte array as nibbles
// yields orientations that look plausible and are wrong, which is the single
// easiest way to paste a scrambled building.
// -----------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// A legacy block state: the numeric id plus its 4-bit data value. Together they
// are the whole identity of a block in this format.
struct LegacyBlockState {
    uint16_t id = 0;
    uint8_t data = 0;

    [[nodiscard]] bool is_air() const noexcept { return id == 0; }
    [[nodiscard]] uint32_t packed() const noexcept {
        return (static_cast<uint32_t>(id) << 8) | data;
    }
};

// How the `Data` array was packed, as deduced from its length.
enum class DataLayout : uint8_t { Absent = 0, BytePerCell, NibblePacked };

// What wrapped the NBT tree in the file.
enum class ContainerKind : uint8_t { RawNbt = 0, Gzip, Zlib };

[[nodiscard]] const char* data_layout_name(DataLayout layout) noexcept;
[[nodiscard]] const char* container_kind_name(ContainerKind kind) noexcept;

struct SchematicData {
    // File-level facts, for reporting.
    ContainerKind container = ContainerKind::RawNbt;
    size_t file_bytes = 0;
    size_t nbt_bytes = 0;
    std::string root_name;
    std::string materials;  // "Alpha", "Pocket", or empty when absent

    // The build.
    int32_t width = 0;
    int32_t height = 0;
    int32_t length = 0;
    DataLayout data_layout = DataLayout::Absent;
    size_t blocks_bytes = 0;
    size_t data_bytes = 0;
    size_t add_blocks_bytes = 0;
    bool has_add_blocks = false;

    // Unique states in first-seen order, and one index per cell into it.
    std::vector<LegacyBlockState> palette;
    std::vector<uint32_t> cells;
    size_t non_air_cells = 0;

    // Dropped content, kept as a count so the caller can say what it ignored.
    size_t tile_entity_count = 0;
    bool tile_entities_truncated = false;
    std::vector<std::array<int32_t, 3>> tile_entity_positions;
    size_t entity_count = 0;

    // Where the build was copied from, when the writer recorded it. Placement
    // ignores this; it is metadata worth showing.
    bool has_origin = false;
    std::array<int32_t, 3> origin{0, 0, 0};

    [[nodiscard]] size_t cell_count() const noexcept {
        return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(length);
    }
    [[nodiscard]] bool in_bounds(int32_t x, int32_t y, int32_t z) const noexcept {
        return x >= 0 && x < width && y >= 0 && y < height && z >= 0 && z < length;
    }
    // Cell order every classic writer uses: x fastest, then z, then y.
    [[nodiscard]] size_t index(int32_t x, int32_t y, int32_t z) const noexcept {
        return (static_cast<size_t>(y) * static_cast<size_t>(length) + static_cast<size_t>(z)) *
                   static_cast<size_t>(width) +
               static_cast<size_t>(x);
    }
    // Null when out of bounds or when the build has not been decoded.
    [[nodiscard]] const LegacyBlockState* state_at(int32_t x, int32_t y, int32_t z) const noexcept;
    [[nodiscard]] size_t count_of(const LegacyBlockState& state) const noexcept;
};

// Decodes an already-decompressed NBT tree. `out` is reset first.
bool parse_schematic_nbt(const uint8_t* data, size_t size, SchematicData& out,
                         std::string* error = nullptr);

// Decodes a whole file: sniffs the container (gzip, zlib, or bare NBT), inflates
// it if needed, then parses. `out` is reset first.
bool load_schematic_bytes(const uint8_t* data, size_t size, SchematicData& out,
                          std::string* error = nullptr);

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_SCHEMATIC_READER_HPP
