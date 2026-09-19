#include "schematic/schematic_reader.hpp"

#include "schematic/gzip_inflate.hpp"
#include "schematic/nbt_reader.hpp"

#include <unordered_map>

namespace VoxelEngine {
namespace schematic {
namespace {

// The root compound every classic writer names.
constexpr const char* kRootName = "Schematic";
// A build this size is not a build; the cap also keeps the dimension product
// well inside size_t.
constexpr int32_t kMaxAxis = 8192;
constexpr size_t kMaxCells = 128u * 1024u * 1024u;
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

// Reads the tile-entity list, keeping positions and discarding inventories, sign
// text and everything else it carries.
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

} // namespace

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

const LegacyBlockState* SchematicData::state_at(int32_t x, int32_t y, int32_t z) const noexcept {
    if (!in_bounds(x, y, z)) return nullptr;
    const size_t cell = index(x, y, z);
    if (cell >= cells.size()) return nullptr;
    const uint32_t slot = cells[cell];
    if (slot >= palette.size()) return nullptr;
    return &palette[slot];
}

size_t SchematicData::count_of(const LegacyBlockState& state) const noexcept {
    const uint32_t key = state.packed();
    size_t slot = palette.size();
    for (size_t i = 0; i < palette.size(); ++i) {
        if (palette[i].packed() == key) {
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

    NbtReader reader(data, size, NbtByteOrder::BigEndian);
    std::string root_name;
    if (!reader.read_root(root_name)) return fail_from_reader(error, reader);
    out.root_name = root_name;
    if (root_name != kRootName) {
        return fail(error, "root compound is \"" + root_name + "\", expected \"" + kRootName +
                               "\" (a classic schematic)");
    }
    out.nbt_bytes = size;

    const uint8_t* blocks = nullptr;
    size_t blocks_len = 0;
    bool have_blocks = false;
    const uint8_t* data_array = nullptr;
    size_t data_len = 0;
    bool have_data = false;
    const uint8_t* add_blocks = nullptr;
    size_t add_len = 0;
    bool have_add = false;
    int64_t width = 0;
    int64_t height = 0;
    int64_t length = 0;
    bool have_width = false, have_height = false, have_length = false;

    NbtReader::Entry entry;
    while (reader.next_entry(entry)) {
        if (entry.name == "Width" || entry.name == "Height" || entry.name == "Length") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            if (entry.name == "Width") {
                width = value;
                have_width = true;
            } else if (entry.name == "Height") {
                height = value;
                have_height = true;
            } else {
                length = value;
                have_length = true;
            }
        } else if (entry.name == "Blocks") {
            if (!reader.read_byte_array(blocks, blocks_len)) return fail_from_reader(error, reader);
            have_blocks = true;
            out.blocks_bytes = blocks_len;
        } else if (entry.name == "Data") {
            if (!reader.read_byte_array(data_array, data_len)) return fail_from_reader(error, reader);
            have_data = true;
            out.data_bytes = data_len;
        } else if (entry.name == "AddBlocks") {
            if (!reader.read_byte_array(add_blocks, add_len)) return fail_from_reader(error, reader);
            have_add = true;
            out.has_add_blocks = true;
            out.add_blocks_bytes = add_len;
        } else if (entry.name == "Materials") {
            if (!reader.read_string(out.materials)) return fail_from_reader(error, reader);
        } else if (entry.name == "TileEntities") {
            if (!read_tile_entities(reader, out, error)) return false;
        } else if (entry.name == "Entities") {
            // Only the count matters: entities are not blocks and cannot be
            // pasted, but a report that silently ignores them would be lying.
            NbtTag element_type = NbtTag::End;
            int32_t count = 0;
            if (!reader.read_list_header(element_type, count)) return fail_from_reader(error, reader);
            out.entity_count = static_cast<size_t>(count);
            for (int32_t i = 0; i < count; ++i) {
                if (!reader.skip_value()) return fail_from_reader(error, reader);
            }
        } else if (entry.name == "WEOriginX" || entry.name == "WEOriginY" || entry.name == "WEOriginZ") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            const size_t axis = entry.name == "WEOriginX" ? 0 : (entry.name == "WEOriginY" ? 1 : 2);
            out.origin[axis] = static_cast<int32_t>(value);
            out.has_origin = true;
        } else if (!reader.skip_value()) {
            return fail_from_reader(error, reader);
        }
    }
    if (!reader.ok()) return fail_from_reader(error, reader);

    if (!have_width || !have_height || !have_length) {
        return fail(error, "missing Width/Height/Length");
    }
    if (width <= 0 || height <= 0 || length <= 0) return fail(error, "non-positive dimensions");
    if (width > kMaxAxis || height > kMaxAxis || length > kMaxAxis) {
        return fail(error, "dimensions above the " + std::to_string(kMaxAxis) + " cap");
    }
    const size_t cell_count = static_cast<size_t>(width) * static_cast<size_t>(height) *
                              static_cast<size_t>(length);
    if (cell_count > kMaxCells) return fail(error, "volume above the cell cap");
    if (!have_blocks) return fail(error, "no Blocks array");
    if (blocks_len != cell_count) {
        return fail(error, "Blocks holds " + std::to_string(blocks_len) + " entries, expected " +
                               std::to_string(cell_count) + " (Width x Height x Length)");
    }

    // The two Data layouts are told apart by length; both are in the wild, and
    // guessing wrong silently scrambles every oriented block.
    DataLayout layout = DataLayout::Absent;
    if (have_data) {
        if (data_len == cell_count) {
            layout = DataLayout::BytePerCell;
        } else if (data_len == (cell_count + 1) / 2) {
            layout = DataLayout::NibblePacked;
        } else {
            return fail(error, "Data holds " + std::to_string(data_len) +
                                   " entries, which is neither one byte per cell (" +
                                   std::to_string(cell_count) + ") nor nibble-packed (" +
                                   std::to_string((cell_count + 1) / 2) + ")");
        }
    }
    if (have_add && add_len != (cell_count + 1) / 2) {
        return fail(error, "AddBlocks holds " + std::to_string(add_len) +
                               " entries, expected the nibble-packed " + std::to_string((cell_count + 1) / 2));
    }

    out.width = static_cast<int32_t>(width);
    out.height = static_cast<int32_t>(height);
    out.length = static_cast<int32_t>(length);
    out.data_layout = layout;
    out.cells.assign(cell_count, 0);
    out.palette.clear();
    std::unordered_map<uint32_t, uint32_t> slots;
    slots.reserve(256);

    for (size_t i = 0; i < cell_count; ++i) {
        uint16_t id = blocks[i];
        if (have_add) {
            const uint8_t packed = add_blocks[i >> 1];
            const uint16_t high = static_cast<uint16_t>((i & 1) ? (packed >> 4) : (packed & 0x0F));
            id = static_cast<uint16_t>(id | static_cast<uint16_t>(high << 8));
        }

        uint8_t data_value = 0;
        if (layout == DataLayout::BytePerCell) {
            data_value = data_array[i];
        } else if (layout == DataLayout::NibblePacked) {
            const uint8_t packed = data_array[i >> 1];
            data_value = static_cast<uint8_t>((i & 1) ? (packed >> 4) : (packed & 0x0F));
        }

        const LegacyBlockState state{id, data_value};
        const uint32_t key = state.packed();
        uint32_t slot = 0;
        const auto found = slots.find(key);
        if (found == slots.end()) {
            slot = static_cast<uint32_t>(out.palette.size());
            slots.emplace(key, slot);
            out.palette.push_back(state);
        } else {
            slot = found->second;
        }
        out.cells[i] = slot;
        if (!state.is_air()) ++out.non_air_cells;
    }

    return true;
}

bool load_schematic_bytes(const uint8_t* data, size_t size, SchematicData& out, std::string* error) {
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
