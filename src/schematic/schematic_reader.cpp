#include "schematic/schematic_reader.hpp"

#include "schematic/gzip_inflate.hpp"
#include "schematic/nbt_reader.hpp"

#include <algorithm>
#include <cctype>
#include <unordered_map>

namespace VoxelEngine {
namespace schematic {
namespace {

// The root compound classic writers name.
constexpr const char* kBuildCompoundName = "Schematic";
// A build this size is not a build; the cap also keeps the dimension product
// well inside size_t.
constexpr int32_t kMaxAxis = 8192;
constexpr size_t kMaxCells = static_cast<size_t>(128) * 1024u * 1024u;
// Tile-entity positions are only reported, so a file claiming millions of them
// records the count and stops collecting.
constexpr size_t kMaxTileEntities = 4096;
// One palette entry per distinct state; a palette far past any real build is a
// sign the indices are being read as something else.
constexpr int64_t kMaxPaletteEntries = 1 << 20;
// A varint in the cell stream is the palette index; five bytes covers any index
// a palette could hold and stops a corrupt run from shifting forever.
constexpr int kMaxVarintBytes = 5;

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool fail_from_reader(std::string* error, const NbtReader& reader) {
    return fail(error, reader.error().empty() ? std::string("malformed nbt") : reader.error());
}

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
