// The classic (MCEdit / .schematic) family: one numeric id per cell with its data
// nibbles, the legacy ids they are expanded through, and the block-entity list,
// which is where this family keeps a position. The palette families are in
// schematic_reader_palette.cpp.

#include "schematic/schematic_reader.hpp"
#include "schematic/schematic_reader_internal.hpp"

#include "schematic/nbt_reader.hpp"

#include <string>
#include <unordered_map>

namespace VoxelEngine {
namespace schematic {
namespace schematic_detail {

// ---------------------------------------------------------------------------
// Classic files
// ---------------------------------------------------------------------------

bool parse_classic(NbtReader& reader, SchematicData& out, std::string* error) {
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
            if (!count_entities(reader, out, error)) return false;
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

    size_t cell_count = 0;
    if (!read_dimensions(width, height, length, have_width, have_height, have_length, out, cell_count,
                         error)) {
        return false;
    }
    if (!have_blocks) return fail(error, "no Blocks array");
    if (blocks_len != cell_count) {
        return fail(error, "Blocks holds " + std::to_string(blocks_len) + " entries, expected " +
                               std::to_string(cell_count) + " (Width x Height x Length)");
    }

    // A nibble-packed array needs one byte per two cells. Some writers round that
    // up to a whole byte whatever the count, so both the tight size and the size
    // one longer are read as nibble-packed; the trailing half-byte is ignored.
    // Nothing else is accepted: any other length means the layout is not what
    // this reader thinks it is, and guessing would scramble every oriented block.
    const size_t packed = (cell_count + 1) / 2;
    const auto nibble_fits = [packed](size_t length) { return length == packed || length == packed + 1; };

    // The two Data layouts are told apart by length; both are in the wild, and
    // guessing wrong silently scrambles every oriented block.
    DataLayout layout = DataLayout::Absent;
    if (have_data) {
        if (data_len == cell_count) {
            layout = DataLayout::BytePerCell;
        } else if (nibble_fits(data_len)) {
            layout = DataLayout::NibblePacked;
            out.data_padded = data_len != packed;
        } else {
            return fail(error, "Data holds " + std::to_string(data_len) +
                                   " entries, which is neither one byte per cell (" +
                                   std::to_string(cell_count) + ") nor nibble-packed (" +
                                   std::to_string(packed) + ")");
        }
    }
    if (have_add && !nibble_fits(add_len)) {
        return fail(error, "AddBlocks holds " + std::to_string(add_len) +
                               " entries, expected the nibble-packed " + std::to_string(packed));
    }
    if (have_add) out.add_blocks_padded = add_len != packed;

    out.data_layout = layout;
    out.cells.assign(cell_count, 0);
    out.palette.clear();
    std::unordered_map<uint32_t, uint32_t> slots;
    slots.reserve(256);

    for (size_t i = 0; i < cell_count; ++i) {
        uint16_t id = blocks[i];
        if (have_add) {
            const uint8_t high_byte = add_blocks[i >> 1];
            id = static_cast<uint16_t>(id |
                                       (static_cast<uint16_t>((i & 1) ? (high_byte >> 4) : (high_byte & 0x0F))
                                        << 8));
        }

        uint8_t data_value = 0;
        if (layout == DataLayout::BytePerCell) {
            data_value = data_array[i];
        } else if (layout == DataLayout::NibblePacked) {
            const uint8_t raw = data_array[i >> 1];
            data_value = static_cast<uint8_t>((i & 1) ? (raw >> 4) : (raw & 0x0F));
        }

        BlockState state;
        state.id = id;
        state.data = data_value;
        const uint32_t key = state.packed();
        const auto found = slots.find(key);
        uint32_t slot = 0;
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

} // namespace schematic_detail
} // namespace schematic
} // namespace VoxelEngine
