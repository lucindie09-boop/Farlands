#pragma once

// The shape of a schematic file, and the errors it reports, shared by the three
// translation units that read one: schematic_reader.cpp owns the format
// classification and the public API, schematic_reader_classic.cpp parses the
// numeric-id family and schematic_reader_palette.cpp the palette families.
//
// Everything here lived in schematic_reader.cpp's anonymous namespace. A
// file-local name cannot be shared, so the definitions move into
// `schematic_detail` and are declared here instead -- the two error helpers, the
// field reads both families make, and the two parsers the dispatcher calls. What
// only the owner uses (the key scan and the format classification) stayed in its
// anonymous namespace.

#include "schematic/nbt_reader.hpp"
#include "schematic/schematic_reader.hpp"

namespace VoxelEngine {
namespace schematic {
namespace schematic_detail {

// Reports a parse failure, keeping the reader's own message when it has one.
bool fail(std::string* error, const std::string& message);
bool fail_from_reader(std::string* error, const NbtReader& reader);

// The fields both families carry: the tile-entity list, its untouched count, the
// offset, and Width/Height/Length with the caps checked.
bool read_tile_entities(NbtReader& reader, SchematicData& out, std::string* error);
bool count_entities(NbtReader& reader, SchematicData& out, std::string* error);
bool read_offset(NbtReader& reader, SchematicData& out, std::string* error);
bool read_dimensions(int64_t width, int64_t height, int64_t length, bool have_width,
                     bool have_height, bool have_length, SchematicData& out, size_t& cell_count,
                     std::string* error);

// The two families, dispatched from load_schematic_bytes by the key scan.
bool parse_classic(NbtReader& reader, SchematicData& out, std::string* error);
bool parse_palette(NbtReader& reader, SchematicData& out, std::string* error);

} // namespace schematic_detail
} // namespace schematic
} // namespace VoxelEngine
