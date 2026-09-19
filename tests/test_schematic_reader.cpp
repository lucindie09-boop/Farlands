#include "doctest.h"
#include "schematic/schematic_reader.hpp"
#include "schematic_test_data.hpp"

#include <string>
#include <vector>

using schematic_test::fixture_add_block_id;
using schematic_test::fixture_block_id;
using schematic_test::fixture_data_value;
using schematic_test::kFixtureCells;
using schematic_test::kFixtureHeight;
using schematic_test::kFixtureLength;
using schematic_test::kFixtureWidth;
using schematic_test::kSchemAddHex;
using schematic_test::kSchemBadDataHex;
using schematic_test::kSchemByteHex;
using schematic_test::kSchemMissingDimsHex;
using schematic_test::kSchemNibbleHex;
using schematic_test::kSchemWrongRootHex;
using schematic_test::nbt_begin;
using schematic_test::nbt_byte_array;
using schematic_test::nbt_end;
using schematic_test::nbt_short;
using schematic_test::nbt_string;
using schematic_test::nbt_tag;
using schematic_test::unhex;
using VoxelEngine::schematic::ContainerKind;
using VoxelEngine::schematic::DataLayout;
using VoxelEngine::schematic::LegacyBlockState;
using VoxelEngine::schematic::load_schematic_bytes;
using VoxelEngine::schematic::parse_schematic_nbt;
using VoxelEngine::schematic::SchematicData;

namespace {

// Walks every cell of a decoded fixture and reports the first place the build
// disagrees with the geometry it was written from. Checking all 60 cells against
// a coordinate formula is what pins the cell order down: with x, y and z mixed
// up the ids still land in range, they just land in the wrong places.
std::string first_mismatch(const SchematicData& data, bool add_blocks) {
    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const LegacyBlockState* state = data.state_at(x, y, z);
                if (state == nullptr) return "no state at a cell inside the box";
                const uint16_t expected_id =
                    add_blocks ? fixture_add_block_id(x, y, z) : fixture_block_id(x, y, z);
                if (state->id != expected_id) {
                    return "id " + std::to_string(state->id) + " at (" + std::to_string(x) + "," +
                           std::to_string(y) + "," + std::to_string(z) + "), expected " +
                           std::to_string(expected_id);
                }
                if (state->data != fixture_data_value(x, y, z)) {
                    return "data " + std::to_string(state->data) + " at (" + std::to_string(x) + "," +
                           std::to_string(y) + "," + std::to_string(z) + "), expected " +
                           std::to_string(fixture_data_value(x, y, z));
                }
            }
        }
    }
    return std::string();
}

} // namespace

TEST_CASE("schematic: a gzipped build decodes with its dimensions and metadata") {
    const std::vector<uint8_t> file = unhex(kSchemByteHex);
    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(file.data(), file.size(), data, &error));
    CHECK(error.empty());

    CHECK(data.container == ContainerKind::Gzip);
    CHECK(data.root_name == "Schematic");
    CHECK(data.materials == "Alpha");
    CHECK(data.width == kFixtureWidth);
    CHECK(data.height == kFixtureHeight);
    CHECK(data.length == kFixtureLength);
    CHECK(data.cell_count() == kFixtureCells);
    CHECK(data.data_layout == DataLayout::BytePerCell);
    CHECK(data.blocks_bytes == kFixtureCells);
    CHECK(data.data_bytes == kFixtureCells);
    CHECK_FALSE(data.has_add_blocks);
    CHECK(data.palette.size() > 1);
    CHECK(data.non_air_cells > 0);
    CHECK(data.file_bytes == file.size());
}

TEST_CASE("schematic: cell order is x fastest, then z, then y") {
    const std::vector<uint8_t> file = unhex(kSchemByteHex);
    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(file.data(), file.size(), data, &error));
    const std::string mismatch = first_mismatch(data, false);
    if (!mismatch.empty()) std::printf("\nschematic cell order: %s\n", mismatch.c_str());
    CHECK(mismatch.empty());
}

TEST_CASE("schematic: nibble-packed Data decodes to the same build as byte-per-cell") {
    const std::vector<uint8_t> per_byte = unhex(kSchemByteHex);
    const std::vector<uint8_t> per_nibble = unhex(kSchemNibbleHex);
    SchematicData byte_data;
    SchematicData nibble_data;
    std::string error;
    CHECK(load_schematic_bytes(per_byte.data(), per_byte.size(), byte_data, &error));
    CHECK(load_schematic_bytes(per_nibble.data(), per_nibble.size(), nibble_data, &error));

    CHECK(nibble_data.data_layout == DataLayout::NibblePacked);
    CHECK(nibble_data.data_bytes == (kFixtureCells + 1) / 2);
    // The two files describe the same building, byte for byte.
    CHECK(nibble_data.cells == byte_data.cells);
    CHECK(nibble_data.palette.size() == byte_data.palette.size());
    const std::string mismatch = first_mismatch(nibble_data, false);
    if (!mismatch.empty()) std::printf("\nnibble-packed data: %s\n", mismatch.c_str());
    CHECK(mismatch.empty());
}

TEST_CASE("schematic: AddBlocks carries ids above 255") {
    const std::vector<uint8_t> file = unhex(kSchemAddHex);
    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(file.data(), file.size(), data, &error));
    CHECK(data.has_add_blocks);
    CHECK(data.add_blocks_bytes == (kFixtureCells + 1) / 2);

    uint16_t highest = 0;
    for (const LegacyBlockState& state : data.palette) {
        if (state.id > highest) highest = state.id;
    }
    CHECK(highest > 255);

    const std::string mismatch = first_mismatch(data, true);
    if (!mismatch.empty()) std::printf("\nAddBlocks ids: %s\n", mismatch.c_str());
    CHECK(mismatch.empty());
}

TEST_CASE("schematic: the palette deduplicates states and counts cells") {
    const std::vector<uint8_t> file = unhex(kSchemByteHex);
    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(file.data(), file.size(), data, &error));

    // Every cell holds a palette slot, and the slots together are the whole box.
    size_t summed = 0;
    for (size_t slot = 0; slot < data.palette.size(); ++slot) {
        size_t count = 0;
        for (const uint32_t cell : data.cells) {
            if (cell == slot) ++count;
        }
        summed += count;
    }
    CHECK(summed == data.cell_count());

    const LegacyBlockState first = data.palette.front();
    CHECK(data.count_of(first) > 0);
    CHECK(data.count_of(LegacyBlockState{9999, 0}) == 0);
}

TEST_CASE("schematic: a Data array of neither layout is refused") {
    const std::vector<uint8_t> file = unhex(kSchemBadDataHex);
    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(file.data(), file.size(), data, &error));
    // The message names the array and both layouts it could have been.
    CHECK(error.find("Data holds") != std::string::npos);
    CHECK(error.find("neither one byte per cell") != std::string::npos);
    CHECK(error.find("nibble-packed") != std::string::npos);
}

TEST_CASE("schematic: a file that is not a classic schematic is refused by name") {
    const std::vector<uint8_t> file = unhex(kSchemWrongRootHex);
    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(file.data(), file.size(), data, &error));
    CHECK(error.find("NotASchematic") != std::string::npos);
}

TEST_CASE("schematic: missing dimensions are refused") {
    const std::vector<uint8_t> file = unhex(kSchemMissingDimsHex);
    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(file.data(), file.size(), data, &error));
    CHECK(error.find("Width/Height/Length") != std::string::npos);
}

TEST_CASE("schematic: truncated files fail rather than decode half a build") {
    const std::vector<uint8_t> file = unhex(kSchemByteHex);
    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(file.data(), file.size() / 2, data, &error));
    CHECK_FALSE(error.empty());
}

TEST_CASE("schematic: an uncompressed NBT tree loads through the same entry point") {
    // The same shape of file with no container around it, built here so the raw
    // path is covered without another fixture.
    std::vector<uint8_t> nbt;
    nbt_begin(nbt, "Schematic");
    nbt_tag(nbt, 2, "Width");
    nbt_short(nbt, 2);
    nbt_tag(nbt, 2, "Height");
    nbt_short(nbt, 1);
    nbt_tag(nbt, 2, "Length");
    nbt_short(nbt, 1);
    nbt_tag(nbt, 7, "Blocks");
    nbt_byte_array(nbt, {1, 0});
    nbt_tag(nbt, 7, "Data");
    nbt_byte_array(nbt, {0, 0});
    nbt_tag(nbt, 8, "Materials");
    nbt_string(nbt, "Alpha");
    nbt_end(nbt);

    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(nbt.data(), nbt.size(), data, &error));
    CHECK(data.container == ContainerKind::RawNbt);
    CHECK(data.width == 2);
    CHECK(data.height == 1);
    CHECK(data.length == 1);
    CHECK(data.non_air_cells == 1);
    CHECK(data.palette.size() == 2);  // stone and air are different states
    const LegacyBlockState* first = data.state_at(0, 0, 0);
    const LegacyBlockState* second = data.state_at(1, 0, 0);
    CHECK(first != nullptr);
    CHECK(second != nullptr);
    if (first != nullptr) CHECK(first->id == 1);
    if (second != nullptr) CHECK(second->is_air());

    // And the parse entry point alone agrees.
    SchematicData direct;
    CHECK(parse_schematic_nbt(nbt.data(), nbt.size(), direct, &error));
    CHECK(direct.cells == data.cells);
    CHECK(direct.cell_count() == 2);
}

TEST_CASE("schematic: tile entities and entities are counted, not decoded") {
    // A file with one tile entity holding a position and a sign's text, plus one
    // entity: both are reported as counts, and neither derails the walk.
    std::vector<uint8_t> nbt;
    nbt_begin(nbt, "Schematic");
    nbt_tag(nbt, 2, "Width");
    nbt_short(nbt, 1);
    nbt_tag(nbt, 2, "Height");
    nbt_short(nbt, 1);
    nbt_tag(nbt, 2, "Length");
    nbt_short(nbt, 1);
    nbt_tag(nbt, 7, "Blocks");
    nbt_byte_array(nbt, {1});
    nbt_tag(nbt, 7, "Data");
    nbt_byte_array(nbt, {0});
    nbt_tag(nbt, 9, "TileEntities");
    nbt.push_back(10);  // element type: compound
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(1);  // one element
    nbt_tag(nbt, 3, "x");
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(3);
    nbt_tag(nbt, 3, "y");
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(4);
    nbt_tag(nbt, 3, "z");
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(5);
    nbt_tag(nbt, 8, "Text1");
    nbt_string(nbt, "a plaque");
    nbt_end(nbt);
    nbt_tag(nbt, 9, "Entities");
    nbt.push_back(10);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(0);
    nbt.push_back(2);   // two elements, never decoded
    nbt_end(nbt);       // first element: an empty compound
    nbt_end(nbt);       // second element: an empty compound
    nbt_end(nbt);       // end of the root compound

    SchematicData data;
    std::string error;
    CHECK(load_schematic_bytes(nbt.data(), nbt.size(), data, &error));
    CHECK(data.tile_entity_count == 1);
    CHECK(data.tile_entity_positions.size() == 1);
    if (data.tile_entity_positions.size() == 1) {
        CHECK(data.tile_entity_positions[0][0] == 3);
        CHECK(data.tile_entity_positions[0][1] == 4);
        CHECK(data.tile_entity_positions[0][2] == 5);
    }
    CHECK_FALSE(data.tile_entities_truncated);
    CHECK(data.entity_count == 2);
    CHECK(data.width == 1);
}
