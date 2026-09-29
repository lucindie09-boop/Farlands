// Reading a schematic: what kind of file it is, the fields both families carry,
// and the public API -- the name tables, the BlockState/SchematicData methods and
// the two entry points. The numeric-id parser is in schematic_reader_classic.cpp
// and the palette families in schematic_reader_palette.cpp; what they share is in
// schematic/schematic_reader_internal.hpp.

#include "schematic/schematic_reader.hpp"
#include "schematic/schematic_reader_internal.hpp"

#include "schematic/gzip_inflate.hpp"
#include "schematic/nbt_reader.hpp"

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// The shared reads are the only part of this file another translation unit needs,
// so they are the only part that cannot stay file-local.
namespace schematic_detail {

// The root compound classic writers name.
constexpr const char* kBuildCompoundName = "Schematic";
// A build this size is not a build; the cap also keeps the dimension product
// well inside size_t.
constexpr int32_t kMaxAxis = 8192;
constexpr size_t kMaxCells = static_cast<size_t>(128) * 1024u * 1024u;
// Tile-entity positions are only reported, so a file claiming millions of them
// records the count and stops collecting.
constexpr size_t kMaxTileEntities = 4096;

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool fail_from_reader(std::string* error, const NbtReader& reader) {
    return fail(error, reader.error().empty() ? std::string("malformed nbt") : reader.error());
}

// ---------------------------------------------------------------------------
// Shared field reads
// ---------------------------------------------------------------------------

// Reads the tile-entity list, keeping positions and discarding inventories, sign
// text and everything else it carries. Both formats carry a position and spell
// it differently: the classic one as x/y/z, the palette one as a "Pos" array.
bool read_tile_entities(NbtReader& reader, SchematicData& out, std::string* error) {
    NbtTag element_type = NbtTag::End;
    int32_t count = 0;
    if (!reader.read_list_header(element_type, count)) return fail_from_reader(error, reader);
    out.tile_entity_count = static_cast<size_t>(count);

    for (int32_t i = 0; i < count; ++i) {
        if (element_type != NbtTag::Compound) {
            if (!reader.skip_value()) return fail_from_reader(error, reader);
            continue;
        }
        if (!reader.push_compound()) return fail_from_reader(error, reader);

        std::array<int32_t, 3> position{0, 0, 0};
        std::array<bool, 3> seen{false, false, false};
        NbtReader::Entry entry;
        while (reader.next_entry(entry)) {
            if (entry.name == "Pos") {
                std::vector<int32_t> values;
                if (!reader.read_int_array(values)) return fail_from_reader(error, reader);
                if (values.size() != 3) {
                    return fail(error, "a block entity position is not three numbers");
                }
                for (size_t axis = 0; axis < 3; ++axis) {
                    position[axis] = values[axis];
                    seen[axis] = true;
                }
                continue;
            }
            const int axis = entry.name == "x" ? 0 : (entry.name == "y" ? 1 : (entry.name == "z" ? 2 : -1));
            if (axis >= 0) {
                int64_t value = 0;
                if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
                position[static_cast<size_t>(axis)] = static_cast<int32_t>(value);
                seen[static_cast<size_t>(axis)] = true;
                continue;
            }
            if (!reader.skip_value()) return fail_from_reader(error, reader);
        }
        if (!reader.ok()) return fail_from_reader(error, reader);
        // Closes the element for the cursor: the loop above already read its End
        // tag, and pop_compound is what retires it from the list being walked.
        if (!reader.pop_compound()) return fail_from_reader(error, reader);

        if (seen[0] || seen[1] || seen[2]) {
            if (out.tile_entity_positions.size() < kMaxTileEntities) {
                out.tile_entity_positions.push_back(position);
            } else {
                out.tile_entities_truncated = true;
            }
        }
    }
    return true;
}

// Counts a list of compounds without reading any of them.
bool count_entities(NbtReader& reader, SchematicData& out, std::string* error) {
    NbtTag element_type = NbtTag::End;
    int32_t count = 0;
    if (!reader.read_list_header(element_type, count)) return fail_from_reader(error, reader);
    out.entity_count = static_cast<size_t>(count);
    for (int32_t i = 0; i < count; ++i) {
        if (!reader.skip_value()) return fail_from_reader(error, reader);
    }
    return true;
}

bool read_offset(NbtReader& reader, SchematicData& out, std::string* error) {
    std::vector<int32_t> values;
    if (!reader.read_int_array(values)) return fail_from_reader(error, reader);
    if (values.size() != 3) return fail(error, "the offset is not three numbers");
    for (size_t axis = 0; axis < 3; ++axis) out.offset[axis] = values[axis];
    out.has_offset = true;
    return true;
}

// The dimensions and the box they describe, checked the same way for both
// families.
bool read_dimensions(int64_t width, int64_t height, int64_t length, bool have_width, bool have_height,
                     bool have_length, SchematicData& out, size_t& cell_count, std::string* error) {
    if (!have_width || !have_height || !have_length) {
        return fail(error, "missing Width/Height/Length");
    }
    if (width <= 0 || height <= 0 || length <= 0) return fail(error, "non-positive dimensions");
    if (width > kMaxAxis || height > kMaxAxis || length > kMaxAxis) {
        return fail(error, "dimensions above the " + std::to_string(kMaxAxis) + " cap");
    }
    cell_count = static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(length);
    if (cell_count > kMaxCells) return fail(error, "volume above the cell cap");
    out.width = static_cast<int32_t>(width);
    out.height = static_cast<int32_t>(height);
    out.length = static_cast<int32_t>(length);
    return true;
}

} // namespace schematic_detail

// Those names are what the public half below calls, exactly as it did when they
// were file-local; the scan helpers stay in the anonymous namespace because only
// this translation unit classifies a file.
using namespace schematic_detail;

namespace {

[[nodiscard]] bool name_is_air(std::string_view name) noexcept {
    return name == "minecraft:air" || name == "minecraft:cave_air" || name == "minecraft:void_air";
}

[[nodiscard]] bool same_state(const BlockState& a, const BlockState& b) noexcept {
    if (a.is_legacy() != b.is_legacy()) return false;
    if (a.is_legacy()) return a.packed() == b.packed();
    if (a.name != b.name || a.properties.size() != b.properties.size()) return false;
    for (size_t i = 0; i < a.properties.size(); ++i) {
        if (a.properties[i] != b.properties[i]) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Scanning: what kind of file is this?
// ---------------------------------------------------------------------------

// The keys that tell the two families apart, and the ones that matter inside a
// wrapper compound.
struct KeyScan {
    bool blocks_array = false;      // classic: one id per cell
    bool data_array = false;        // classic: its data values
    bool add_blocks = false;        // classic: the id's high nibble
    bool palette = false;           // palette format v2: state names at the root
    bool block_data = false;        // palette format v2: the index stream
    bool blocks_compound = false;   // palette format v3: the same, nested
    bool has_version = false;
    int64_t version = 0;
    bool build_here = false;        // this compound holds a build
    bool in_child = false;          // the build is a nested compound, not this one
};

[[nodiscard]] BlockFileFormat classify(const KeyScan& keys) noexcept {
    // A byte array of one id per cell is the classic signature, and nothing in
    // the other family has it (their per-cell stream is a palette index, and it
    // lives under a compound in v3).
    if (keys.blocks_array) return BlockFileFormat::Classic;
    // The nesting decides v3, not the version number: a file written to the v3
    // shape is what has to be parsed, whatever its Version field claims.
    if (keys.blocks_compound) return BlockFileFormat::SpongeV3;
    if (keys.palette || keys.block_data) return BlockFileFormat::SpongeV2;
    return BlockFileFormat::Unknown;
}

// Walks one compound's entries recording which keys it holds. A compound child
// named "Schematic" is entered and scanned too, because a writer is free to wrap
// its build in an unnamed root, and one level of that is the whole range in the
// wild.
bool scan_keys(NbtReader& reader, KeyScan& out, int depth, std::string* error) {
    NbtReader::Entry entry;
    while (reader.next_entry(entry)) {
        if (entry.name == "Blocks") {
            if (entry.type == NbtTag::ByteArray) out.blocks_array = true;
            else if (entry.type == NbtTag::Compound) out.blocks_compound = true;
        } else if (entry.name == "Data") {
            if (entry.type == NbtTag::ByteArray) out.data_array = true;
        } else if (entry.name == "AddBlocks") {
            out.add_blocks = true;
        } else if (entry.name == "Palette") {
            out.palette = true;
        } else if (entry.name == "BlockData") {
            out.block_data = true;
        } else if (entry.name == "Version") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            out.has_version = true;
            out.version = value;
            continue;
        } else if (depth == 0 && entry.name == kBuildCompoundName && entry.type == NbtTag::Compound) {
            KeyScan child;
            if (!reader.push_compound()) return fail_from_reader(error, reader);
            if (!scan_keys(reader, child, depth + 1, error)) return false;
            if (!reader.pop_compound()) return fail_from_reader(error, reader);
            if (classify(child) != BlockFileFormat::Unknown) {
                // The wrapper is the file, from here on: its keys decide the
                // format, and the parse pass descends into the same child.
                child.in_child = true;
                out = child;
                return true;
            }
            continue;
        }
        if (!reader.skip_value()) return fail_from_reader(error, reader);
    }
    if (!reader.ok()) return fail_from_reader(error, reader);
    out.build_here = classify(out) != BlockFileFormat::Unknown;
    return true;
}

} // namespace

const char* block_file_format_name(BlockFileFormat format) noexcept {
    switch (format) {
        case BlockFileFormat::Unknown: return "unknown";
        case BlockFileFormat::Classic: return "classic schematic (numeric ids)";
        case BlockFileFormat::SpongeV2: return "palette schematic v2 (named states)";
        case BlockFileFormat::SpongeV3: return "palette schematic v3 (named states)";
    }
    return "unknown";
}

const char* data_layout_name(DataLayout layout) noexcept {
    switch (layout) {
        case DataLayout::Absent: return "none";
        case DataLayout::BytePerCell: return "one byte per cell";
        case DataLayout::NibblePacked: return "nibble-packed";
    }
    return "unknown";
}

const char* container_kind_name(ContainerKind kind) noexcept {
    switch (kind) {
        case ContainerKind::RawNbt: return "raw nbt";
        case ContainerKind::Gzip: return "gzip";
        case ContainerKind::Zlib: return "zlib";
    }
    return "unknown";
}

bool BlockState::is_air() const noexcept {
    if (is_legacy()) return id == 0;
    return name_is_air(name);
}

const std::string* BlockState::property(std::string_view key) const noexcept {
    for (const auto& entry : properties) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

std::string BlockState::describe() const {
    if (is_legacy()) return std::to_string(id) + ":" + std::to_string(data);
    std::string out = name;
    if (properties.empty()) return out;
    out.push_back('[');
    for (size_t i = 0; i < properties.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += properties[i].first;
        out.push_back('=');
        out += properties[i].second;
    }
    out.push_back(']');
    return out;
}

const BlockState* SchematicData::state_at(int32_t x, int32_t y, int32_t z) const noexcept {
    if (!in_bounds(x, y, z)) return nullptr;
    const size_t cell = index(x, y, z);
    if (cell >= cells.size()) return nullptr;
    const uint32_t slot = cells[cell];
    if (slot >= palette.size()) return nullptr;
    return &palette[slot];
}

std::vector<size_t> SchematicData::counts() const {
    std::vector<size_t> out(palette.size(), 0);
    for (const uint32_t cell : cells) {
        if (cell < out.size()) ++out[cell];
    }
    return out;
}

size_t SchematicData::count_of(const BlockState& state) const noexcept {
    size_t slot = palette.size();
    for (size_t i = 0; i < palette.size(); ++i) {
        if (same_state(palette[i], state)) {
            slot = i;
            break;
        }
    }
    if (slot == palette.size()) return 0;
    size_t total = 0;
    for (const uint32_t cell : cells) {
        if (cell == slot) ++total;
    }
    return total;
}

bool parse_schematic_nbt(const uint8_t* data, size_t size, SchematicData& out, std::string* error) {
    out = SchematicData{};
    if (data == nullptr || size == 0) return fail(error, "empty nbt buffer");

    // The cursor is forward-only, so the file is identified by a scan first and
    // then parsed with a fresh reader: the scan looks for the keys either family
    // is built from, and descends into a wrapper compound when the root has none.
    std::string root_name;
    KeyScan keys;
    bool wrapped = false;
    {
        NbtReader reader(data, size, NbtByteOrder::BigEndian);
        if (!reader.read_root(root_name)) return fail_from_reader(error, reader);
        if (!scan_keys(reader, keys, 0, error)) return false;
        // scan_keys leaves the wrapper's scan in place when the wrapper is what
        // holds the build and flags it; the reader below only has to know
        // whether to descend before parsing the same keys again.
        wrapped = keys.in_child;
    }

    const BlockFileFormat format = classify(keys);
    if (format == BlockFileFormat::Unknown) {
        return fail(error,
                    "the file holds neither a Blocks array nor a palette (root \"" + root_name +
                        "\"); a saved build from either format would have one");
    }

    // Two readers are cheap here: this one walks the compound the build is in,
    // which is the root unless a writer wrapped it.
    NbtReader reader(data, size, NbtByteOrder::BigEndian);
    std::string name;
    if (!reader.read_root(name)) return fail_from_reader(error, reader);
    if (wrapped) {
        bool descended = false;
        NbtReader::Entry entry;
        while (reader.next_entry(entry)) {
            if (entry.name == kBuildCompoundName && entry.type == NbtTag::Compound) {
                if (!reader.push_compound()) return fail_from_reader(error, reader);
                descended = true;
                break;
            }
            if (!reader.skip_value()) return fail_from_reader(error, reader);
        }
        if (!descended) return fail(error, "the wrapped build compound disappeared on the second pass");
    }

    out.root_name = root_name.empty() ? kBuildCompoundName : root_name;
    out.nbt_bytes = size;
    out.format = format;
    if (format == BlockFileFormat::Classic) {
        return parse_classic(reader, out, error);
    }
    return parse_palette(reader, out, error);
}

bool load_schematic_bytes(const uint8_t* data, size_t size, SchematicData& out, std::string* error) {
    out = SchematicData{};
    if (data == nullptr || size == 0) return fail(error, "empty file");

    // A bare NBT dump begins with its root tag; anything else is a container to
    // inflate first.
    if (data[0] == static_cast<uint8_t>(NbtTag::Compound)) {
        if (!parse_schematic_nbt(data, size, out, error)) return false;
        out.container = ContainerKind::RawNbt;
        out.file_bytes = size;
        return true;
    }

    WrapKind wrap = WrapKind::Raw;
    std::vector<uint8_t> inflated;
    if (!inflate_auto(data, size, inflated, &wrap, error)) return false;
    if (!parse_schematic_nbt(inflated.data(), inflated.size(), out, error)) return false;

    out.container = wrap == WrapKind::Gzip ? ContainerKind::Gzip
                                           : (wrap == WrapKind::Zlib ? ContainerKind::Zlib
                                                                     : ContainerKind::RawNbt);
    out.file_bytes = size;
    return true;
}

} // namespace schematic
} // namespace VoxelEngine