// The palette families (Sponge v2 at the root, v3 under a compound): the state
// strings split into name plus properties, the palette compound itself, the varint
// index stream read against it, and the parser that joins them. The numeric-id
// family is in schematic_reader_classic.cpp.

#include "schematic/schematic_reader.hpp"
#include "schematic/schematic_reader_internal.hpp"

#include "schematic/nbt_reader.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {
namespace schematic_detail {

// One palette entry per distinct state; a palette far past any real build is a
// sign the indices are being read as something else.
constexpr int64_t kMaxPaletteEntries = 1 << 20;
// A varint in the cell stream is the palette index; five bytes covers any index
// a palette could hold and stops a corrupt run from shifting forever.
constexpr int kMaxVarintBytes = 5;
// ---------------------------------------------------------------------------
// Named states
// ---------------------------------------------------------------------------

// A named file spells its states "minecraft:oak_stairs[facing=north]", and
// state strings are conventionally lowercase. Names, keys and values are folded
// to lowercase so a writer that shouts still matches the table; a state whose
// case mattered would be a state this format does not have.
[[nodiscard]] std::string lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (const char raw : text) {
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(raw))));
    }
    return out;
}

[[nodiscard]] std::string trim(std::string_view text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

// Splits a palette entry into its name and its properties. The name keeps its
// namespace, and a missing one is read as the vanilla one: a palette entry
// without a namespace is either that or a mod block pretending, and the table
// can only speak about the former.
bool split_state(const std::string& raw, std::string& name,
                 std::vector<std::pair<std::string, std::string>>& properties,
                 std::string* error) {
    const size_t bracket = raw.find('[');
    const std::string head = lower(bracket == std::string::npos ? std::string_view(raw)
                                                                : std::string_view(raw).substr(0, bracket));
    name = head.find(':') == std::string::npos ? "minecraft:" + head : head;
    if (name == "minecraft:") return fail(error, "a palette entry has no block name");

    properties.clear();
    if (bracket == std::string::npos) return true;
    const size_t close = raw.find(']', bracket);
    if (close == std::string::npos) {
        return fail(error, "palette entry \"" + raw + "\" has an unclosed property list");
    }
    if (raw.find_first_not_of(" \t", close + 1) != std::string::npos) {
        return fail(error, "palette entry \"" + raw + "\" has text after its property list");
    }

    std::string_view body(raw.data() + bracket + 1, close - bracket - 1);
    while (!body.empty()) {
        const size_t comma = body.find(',');
        const std::string_view field = body.substr(0, comma);
        const size_t equals = field.find('=');
        if (equals == std::string_view::npos) {
            return fail(error, "palette entry \"" + raw + "\" has a property without a value");
        }
        properties.emplace_back(lower(trim(field.substr(0, equals))),
                                lower(trim(field.substr(equals + 1))));
        if (comma == std::string_view::npos) break;
        body.remove_prefix(comma + 1);
    }
    // Sorted by key, so the order a writer printed them in cannot make two
    // spellings of the same state look like different ones.
    std::sort(properties.begin(), properties.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    return true;
}
// ---------------------------------------------------------------------------
// Palette files
// ---------------------------------------------------------------------------

// The palette is a compound of state name -> index, so it is walked as entries
// rather than read as an array.
bool read_palette(NbtReader& reader, std::vector<std::string>& states, SchematicData& out,
                  std::string* error) {
    if (!reader.push_compound()) return fail_from_reader(error, reader);
    NbtReader::Entry entry;
    int64_t seen = 0;
    while (reader.next_entry(entry)) {
        int64_t index = 0;
        if (!reader.read_any_int(index)) return fail_from_reader(error, reader);
        if (index < 0 || index >= kMaxPaletteEntries) {
            return fail(error, "a palette index is outside 0.." + std::to_string(kMaxPaletteEntries));
        }
        if (static_cast<size_t>(index) >= states.size()) states.resize(static_cast<size_t>(index) + 1);
        states[static_cast<size_t>(index)] = entry.name;
        ++seen;
    }
    if (!reader.ok()) return fail_from_reader(error, reader);
    if (!reader.pop_compound()) return fail_from_reader(error, reader);
    out.palette_max = std::max<int32_t>(out.palette_max, static_cast<int32_t>(states.size()));
    return seen > 0 ? true : fail(error, "the palette is empty");
}

// The index stream: one base-128 varint per cell, in the same cell order as the
// classic format.
bool decode_block_data(const uint8_t* bytes, size_t length, size_t cell_count,
                       std::vector<uint32_t>& out, std::string* error) {
    out.clear();
    out.reserve(cell_count);
    uint32_t value = 0;
    int shift = 0;
    int used = 0;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t byte = bytes[i];
        value |= static_cast<uint32_t>(byte & 0x7F) << shift;
        ++used;
        if ((byte & 0x80) != 0) {
            if (used >= kMaxVarintBytes) {
                return fail(error, "the cell data holds a varint longer than " +
                                       std::to_string(kMaxVarintBytes) + " bytes");
            }
            shift += 7;
            continue;
        }
        if (out.size() >= cell_count) {
            return fail(error, "the cell data holds more cells than Width x Height x Length");
        }
        out.push_back(value);
        value = 0;
        shift = 0;
        used = 0;
    }
    if (used != 0) return fail(error, "the cell data ends in the middle of a value");
    if (out.size() != cell_count) {
        return fail(error, "the cell data holds " + std::to_string(out.size()) + " cells, expected " +
                               std::to_string(cell_count));
    }
    return true;
}

// Turns the decoded indices into the shared palette-plus-cells shape, and
// refuses a file whose cells point at palette slots the palette never defined.
bool build_palette_states(const std::vector<std::string>& names, const std::vector<uint32_t>& indices,
                          SchematicData& out, std::string* error) {
    std::unordered_map<std::string, uint32_t> slots;
    slots.reserve(names.size());
    std::vector<uint32_t> state_slot(names.size(), UINT32_MAX);
    out.palette.clear();
    out.palette.reserve(names.size());
    out.cells.assign(indices.size(), 0);

    std::vector<uint32_t> resolved;
    resolved.reserve(indices.size());
    for (size_t cell = 0; cell < indices.size(); ++cell) {
        const uint32_t index = indices[cell];
        if (index >= names.size() || names[index].empty()) {
            return fail(error, "a cell points at palette slot " + std::to_string(index) +
                                   ", which the palette does not define");
        }
        uint32_t slot = state_slot[index];
        if (slot == UINT32_MAX) {
            BlockState state;
            if (!split_state(names[index], state.name, state.properties, error)) {
                return false;
            }
            const std::string key = state.describe();
            const auto found = slots.find(key);
            if (found == slots.end()) {
                slot = static_cast<uint32_t>(out.palette.size());
                slots.emplace(key, slot);
                out.palette.push_back(std::move(state));
            } else {
                slot = found->second;
            }
            state_slot[index] = slot;
        }
        resolved.push_back(slot);
        if (!out.palette[slot].is_air()) ++out.non_air_cells;
    }
    out.cells = std::move(resolved);
    return true;
}

bool parse_palette(NbtReader& reader, SchematicData& out, std::string* error) {
    const uint8_t* block_data = nullptr;
    size_t block_data_len = 0;
    bool have_block_data = false;
    std::vector<std::string> states;
    bool have_palette = false;
    int64_t width = 0, height = 0, length = 0;
    bool have_width = false, have_height = false, have_length = false;

    // v3 nests the same three keys under "Blocks"; this reads either spelling.
    const auto read_carrier = [&](NbtReader& inner) -> bool {
        NbtReader::Entry entry;
        while (inner.next_entry(entry)) {
            if (entry.name == "Palette") {
                if (!read_palette(inner, states, out, error)) return false;
                have_palette = true;
            } else if (entry.name == "PaletteMax") {
                int64_t value = 0;
                if (!inner.read_any_int(value)) return fail_from_reader(error, inner);
                out.palette_max = static_cast<int32_t>(value);
            } else if (entry.name == "Data") {
                if (!inner.read_byte_array(block_data, block_data_len)) {
                    return fail_from_reader(error, inner);
                }
                have_block_data = true;
            } else if (entry.name == "BlockEntities") {
                if (!read_tile_entities(inner, out, error)) return false;
            } else if (!inner.skip_value()) {
                return fail_from_reader(error, inner);
            }
        }
        return inner.ok() ? true : fail_from_reader(error, inner);
    };

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
        } else if (entry.name == "Version") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            out.format_version = static_cast<int32_t>(value);
        } else if (entry.name == "DataVersion") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            out.data_version = value;
        } else if (entry.name == "Palette") {
            if (!read_palette(reader, states, out, error)) return false;
            have_palette = true;
        } else if (entry.name == "PaletteMax") {
            int64_t value = 0;
            if (!reader.read_any_int(value)) return fail_from_reader(error, reader);
            out.palette_max = static_cast<int32_t>(value);
        } else if (entry.name == "BlockData") {
            if (!reader.read_byte_array(block_data, block_data_len)) return fail_from_reader(error, reader);
            have_block_data = true;
        } else if (entry.name == "Blocks" && entry.type == NbtTag::Compound) {
            if (!reader.push_compound()) return fail_from_reader(error, reader);
            const bool ok = read_carrier(reader);
            if (!reader.pop_compound()) return fail_from_reader(error, reader);
            if (!ok) return false;
        } else if (entry.name == "BlockEntities") {
            if (!read_tile_entities(reader, out, error)) return false;
        } else if (entry.name == "Entities") {
            if (!count_entities(reader, out, error)) return false;
        } else if (entry.name == "Offset") {
            if (!read_offset(reader, out, error)) return false;
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
    if (!have_palette) return fail(error, "no Palette in a palette-format file");
    if (!have_block_data) return fail(error, "no block data in a palette-format file");

    out.block_data_bytes = block_data_len;
    std::vector<uint32_t> indices;
    if (!decode_block_data(block_data, block_data_len, cell_count, indices, error)) return false;
    return build_palette_states(states, indices, out, error);
}

} // namespace schematic_detail
} // namespace schematic
} // namespace VoxelEngine
