#ifndef FARLANDS_SCHEMATIC_READER_HPP
#define FARLANDS_SCHEMATIC_READER_HPP

// -----------------------------------------------------------------------------
// Block-file decoding, for both shapes a saved build comes in.
//
// There are two families in the wild and they store the same thing very
// differently:
//
//   * the *classic* file (`.schematic`, and `.nbt` in the same style) is a
//     gzip'd NBT tree whose root is named "Schematic": three dimensions, one
//     legacy numeric id per cell, and a parallel array of 4-bit data values.
//     Blocks are numbered oldest-first, so the numbers only mean something
//     against a translation table.
//
//   * the *palette* file (`.schem`, the newer format) keeps its states as
//     names with properties — "minecraft:oak_stairs[facing=west,half=top]" —
//     and stores one index per cell into that palette, as base-128 varints.
//     A named state does not need a translation table to be *understood*, only
//     to be landed on a block this game has.
//
// Both are decoded here into one shape: a palette of states plus one palette
// index per cell. That is compact for a big build, and it keeps the question of
// what a state should *become* entirely out of this file — that is the
// translation table's job (see mc_palette).
//
// This file is format only: no engine types, no Godot.
//
// Two writer quirks are handled rather than assumed away, because both produce
// a plausible-looking file that decodes into silently wrong blocks:
//
//   * the nibble-packed arrays (`Data`, `AddBlocks`) are sometimes written one
//     byte longer than the packed size — a rounding slip that is common enough
//     to be the norm in some tools. The exact length and that length are both
//     accepted; the slack is reported. Reading a per-cell array as nibbles, or
//     the reverse, yields orientations that look fine and are wrong, so the
//     layout is decided by array length and never guessed.
//   * a file may wrap its build compound in an unnamed root. The build is
//     located by shape (a Blocks array, or a palette) rather than by the root's
//     name alone, and a compound child named "Schematic" is entered when the
//     root itself holds no build.
// -----------------------------------------------------------------------------

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// Which family of file this is, as decided from its own contents.
enum class BlockFileFormat : uint8_t {
    Unknown = 0,
    Classic,   // numeric ids + data values
    SpongeV2,  // flat palette, one varint index per cell
    SpongeV3,  // the same, nested under a "Blocks" compound
};

[[nodiscard]] const char* block_file_format_name(BlockFileFormat format) noexcept;

// One state a file can hold.
//
// A classic file fills in `id`/`data` and leaves `name` empty; a palette file
// does the opposite. `properties` is kept sorted by key, so two palette entries
// that differ only in the order their writer happened to print them are the
// same state — which is what makes the palette collapse as far as it should.
struct BlockState {
    uint16_t id = 0;
    uint8_t data = 0;
    std::string name;
    std::vector<std::pair<std::string, std::string>> properties;

    [[nodiscard]] bool is_legacy() const noexcept { return name.empty(); }
    // Classic air is id 0. A named file says "minecraft:air", and the two
    // variants that are air without being seen (cave/void) are dropped with it.
    [[nodiscard]] bool is_air() const noexcept;
    // The classic key: id in the high bits, data in the low. Only meaningful for
    // a legacy state, and only used as a lookup key.
    [[nodiscard]] uint32_t packed() const noexcept {
        return (static_cast<uint32_t>(id) << 8) | data;
    }
    // The value of `key`, or null when the state does not carry it.
    [[nodiscard]] const std::string* property(std::string_view key) const noexcept;
    // "53:2" for a legacy state, "minecraft:oak_stairs[...]" for a named one.
    [[nodiscard]] std::string describe() const;
};

// How the classic `Data` array was packed, as deduced from its length.
enum class DataLayout : uint8_t { Absent = 0, BytePerCell, NibblePacked };

// What wrapped the NBT tree in the file.
enum class ContainerKind : uint8_t { RawNbt = 0, Gzip, Zlib };

[[nodiscard]] const char* data_layout_name(DataLayout layout) noexcept;
[[nodiscard]] const char* container_kind_name(ContainerKind kind) noexcept;

struct SchematicData {
    // File-level facts, for reporting.
    BlockFileFormat format = BlockFileFormat::Unknown;
    ContainerKind container = ContainerKind::RawNbt;
    size_t file_bytes = 0;
    size_t nbt_bytes = 0;
    std::string root_name;
    // The palette format's own version (2 or 3), and the game version it was
    // written from. Both are 0 for a classic file.
    int32_t format_version = 0;
    int64_t data_version = 0;

    // The build.
    int32_t width = 0;
    int32_t height = 0;
    int32_t length = 0;
    DataLayout data_layout = DataLayout::Absent;
    size_t blocks_bytes = 0;
    size_t data_bytes = 0;
    size_t add_blocks_bytes = 0;
    bool has_add_blocks = false;
    // The writer's arrays were longer than the packed size requires. Harmless,
    // and worth saying out loud: it is the difference between "this file is odd"
    // and "this file decoded under protest".
    bool data_padded = false;
    bool add_blocks_padded = false;
    // Palette files only: the size the palette declares, and the cell data's
    // byte length (the values themselves are a varint stream).
    int32_t palette_max = 0;
    size_t block_data_bytes = 0;

    // Classic files record what the block ids are (always "Alpha" in practice).
    std::string materials;

    // Unique states in first-seen order, and one index per cell into it.
    std::vector<BlockState> palette;
    std::vector<uint32_t> cells;
    size_t non_air_cells = 0;

    // Dropped content, kept as a count so the caller can say what it ignored.
    size_t tile_entity_count = 0;
    bool tile_entities_truncated = false;
    std::vector<std::array<int32_t, 3>> tile_entity_positions;
    size_t entity_count = 0;

    // Where the build sat, when the writer recorded it. Placement ignores this;
    // it is metadata worth showing. `origin` is the classic convention (where it
    // was copied from), `offset` the palette format's (where it sits in its
    // region); both are three numbers and are reported the same way.
    bool has_origin = false;
    std::array<int32_t, 3> origin{0, 0, 0};
    bool has_offset = false;
    std::array<int32_t, 3> offset{0, 0, 0};

    [[nodiscard]] size_t cell_count() const noexcept {
        return static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(length);
    }
    [[nodiscard]] bool in_bounds(int32_t x, int32_t y, int32_t z) const noexcept {
        return x >= 0 && x < width && y >= 0 && y < height && z >= 0 && z < length;
    }
    // Cell order every writer uses: x fastest, then z, then y.
    [[nodiscard]] size_t index(int32_t x, int32_t y, int32_t z) const noexcept {
        return (static_cast<size_t>(y) * static_cast<size_t>(length) + static_cast<size_t>(z)) *
                   static_cast<size_t>(width) +
               static_cast<size_t>(x);
    }
    // Null when out of bounds or when the build has not been decoded.
    [[nodiscard]] const BlockState* state_at(int32_t x, int32_t y, int32_t z) const noexcept;
    // How many cells each palette slot covers, in one pass. The per-state
    // alternative is a scan of every cell for every state, which is quadratic in
    // a way that shows on a build with a million cells.
    [[nodiscard]] std::vector<size_t> counts() const;
    // Convenience for one state: O(cells), so it is for reporting, not for a
    // per-state loop.
    [[nodiscard]] size_t count_of(const BlockState& state) const noexcept;
};

// Decodes an already-decompressed NBT tree, whichever family it is. `out` is
// reset first.
bool parse_schematic_nbt(const uint8_t* data, size_t size, SchematicData& out,
                         std::string* error = nullptr);

// Decodes a whole file: sniffs the container (gzip, zlib, or bare NBT), inflates
// it if needed, then parses. `out` is reset first.
bool load_schematic_bytes(const uint8_t* data, size_t size, SchematicData& out,
                          std::string* error = nullptr);

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_SCHEMATIC_READER_HPP
