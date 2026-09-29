#include "doctest.h"

#include "schematic_reader_test_support.hpp"

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
using schematic_test::kSchemAddPaddedHex;
using schematic_test::kSchemBadDataHex;
using schematic_test::kSchemByteHex;
using schematic_test::kSchemMissingDimsHex;
using schematic_test::kSchemNibbleHex;
using schematic_test::kSchemWrongRootHex;
using schematic_test::kSchemWrappedHex;
using schematic_test::kSchemFloatFieldsHex;
using schematic_test::kSchemNibblePaddedHex;
using schematic_test::kSchemSpongeV2Hex;
using schematic_test::kSchemSpongeV3Hex;
using schematic_test::kSchemSpongeVarintHex;
using schematic_test::kSchemSpongeShortHex;
using schematic_test::kSpongeHeight;
using schematic_test::kSpongeLength;
using schematic_test::kSpongeWidth;
using schematic_test::nbt_int;
using schematic_test::sponge_v2_state_index;
using schematic_test::sponge_v3_state_index;
using schematic_test::nbt_begin;
using schematic_test::nbt_byte_array;
using schematic_test::nbt_end;
using schematic_test::nbt_short;
using schematic_test::nbt_string;
using schematic_test::nbt_tag;
using schematic_test::unhex;
using VoxelEngine::schematic::BlockFileFormat;
using VoxelEngine::schematic::ContainerKind;
using VoxelEngine::schematic::DataLayout;
using VoxelEngine::schematic::BlockState;
using VoxelEngine::schematic::load_schematic_bytes;
using VoxelEngine::schematic::parse_schematic_nbt;
using VoxelEngine::schematic::SchematicData;

using namespace schematic_reader_test;

TEST_CASE("schematic: the palette format names its states and indexes them per cell") {
    // `.schem` v2: a palette of state names at the root with one varint palette
    // index per cell, wrapped in an unnamed root like the real files are.
    const std::vector<uint8_t> bytes = unhex(kSchemSpongeV2Hex);
    SchematicData data;
    std::string error;
    CHECK_MESSAGE(load_schematic_bytes(bytes.data(), bytes.size(), data, &error), error);
    CHECK(data.format == BlockFileFormat::SpongeV2);
    CHECK(data.format_version == 2);
    CHECK(data.data_version == 3465);
    CHECK(data.width == kSpongeWidth);
    CHECK(data.height == kSpongeHeight);
    CHECK(data.length == kSpongeLength);
    CHECK(data.cell_count() == 12);
    CHECK(data.palette.size() == 4);
    CHECK(data.has_offset);
    CHECK(data.offset[0] == -1);
    CHECK(data.offset[1] == 0);
    CHECK(data.offset[2] == 2);

    // Three of the twelve cells are air, wherever the index formula puts index 0.
    CHECK(data.non_air_cells == 9);

    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const BlockState* state = data.state_at(x, y, z);
                CHECK(state != nullptr);
                if (state == nullptr) continue;
                const std::string where = " at (" + std::to_string(x) + "," + std::to_string(y) +
                                          "," + std::to_string(z) + ")";
                switch (sponge_v2_state_index(x, y, z)) {
                    case 0:
                        CHECK_MESSAGE(state->is_air(), "expected air" << where);
                        break;
                    case 1:
                        CHECK_MESSAGE(state->name == "minecraft:stone", "expected stone" << where);
                        CHECK(state->properties.empty());
                        break;
                    case 2:
                        CHECK_MESSAGE(state->name == "minecraft:oak_stairs", "stairs" << where);
                        CHECK(state->property("facing") != nullptr);
                        CHECK(state->property("half") != nullptr);
                        if (state->property("facing") != nullptr) {
                            CHECK(*state->property("facing") == "east");
                        }
                        if (state->property("half") != nullptr) {
                            CHECK(*state->property("half") == "top");
                        }
                        CHECK_FALSE(state->is_air());
                        // Properties are kept sorted by key, so a state prints the
                        // same way however its writer ordered them.
                        CHECK(state->describe() ==
                              "minecraft:oak_stairs[facing=east,half=top,waterlogged=false]");
                        break;
                    default:
                        CHECK_MESSAGE(state->name == "minecraft:spruce_slab", "slab" << where);
                        CHECK(state->property("type") != nullptr);
                        if (state->property("type") != nullptr) {
                            CHECK(*state->property("type") == "top");
                        }
                        break;
                }
            }
        }
    }
}

TEST_CASE("schematic: the palette format v3 nests its palette and skips what it does not need") {
    const std::vector<uint8_t> bytes = unhex(kSchemSpongeV3Hex);
    SchematicData data;
    std::string error;
    CHECK_MESSAGE(load_schematic_bytes(bytes.data(), bytes.size(), data, &error), error);
    CHECK(data.format == BlockFileFormat::SpongeV3);
    CHECK(data.format_version == 3);
    CHECK(data.data_version == 4556);
    CHECK(data.root_name == "Schematic");
    CHECK(data.cell_count() == 12);
    CHECK(data.palette.size() == 3);
    CHECK(data.has_offset);
    CHECK(data.offset[0] == 4);
    CHECK(data.offset[2] == 7);

    // Four cells are air, and the palette was written out of index order, so this
    // also proves the indices are what key the palette.
    CHECK(data.non_air_cells == 8);
    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const BlockState* state = data.state_at(x, y, z);
                CHECK(state != nullptr);
                if (state == nullptr) continue;
                const uint32_t index = sponge_v3_state_index(x, y, z);
                if (index == 0) {
                    CHECK(state->is_air());
                } else if (index == 1) {
                    CHECK(state->name == "minecraft:birch_planks");
                } else {
                    CHECK(state->name == "minecraft:sandstone");
                }
            }
        }
    }

    // The block-entity list lives inside the nested compound and is still read.
    CHECK(data.tile_entity_count == 1);
    CHECK(data.tile_entity_positions.size() == 1);
    if (data.tile_entity_positions.size() == 1) {
        CHECK(data.tile_entity_positions[0][0] == 1);
        CHECK(data.tile_entity_positions[0][1] == 0);
        CHECK(data.tile_entity_positions[0][2] == 1);
    }
    CHECK(data.entity_count == 0);
}

TEST_CASE("schematic: a sparse palette needs multi-byte varint indices") {
    // An index above 127 is two bytes, so this pins the varint decoder and the
    // palette's ability to hold a gap below its first entry.
    const std::vector<uint8_t> bytes = unhex(kSchemSpongeVarintHex);
    SchematicData data;
    std::string error;
    CHECK_MESSAGE(load_schematic_bytes(bytes.data(), bytes.size(), data, &error), error);
    CHECK(data.format == BlockFileFormat::SpongeV2);
    CHECK(data.cell_count() == 2);
    CHECK(data.block_data_bytes == 4);  // two indices, two bytes each
    CHECK(data.palette.size() == 1);
    CHECK(data.non_air_cells == 2);
    if (data.palette.size() == 1) CHECK(data.palette[0].name == "minecraft:stone");
    for (int32_t x = 0; x < data.width; ++x) {
        const BlockState* state = data.state_at(x, 0, 0);
        CHECK(state != nullptr);
        if (state != nullptr) CHECK(state->name == "minecraft:stone");
    }
}

TEST_CASE("schematic: cell data that stops short is refused") {
    // A short stream would leave the rest of the build silently air, which is the
    // kind of quiet half-paste worth refusing outright.
    const std::vector<uint8_t> bytes = unhex(kSchemSpongeShortHex);
    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(bytes.data(), bytes.size(), data, &error));
    CHECK(error.find("cell") != std::string::npos);
}

TEST_CASE("schematic: a file that is neither format is refused with the reason") {
    // A structure file has no palette and no id array, so it is not silently read
    // as one of ours.
    std::vector<uint8_t> nbt;
    nbt_begin(nbt, "");
    nbt_tag(nbt, 3, "DataVersion");
    nbt_int(nbt, 1);
    nbt_tag(nbt, 3, "size");
    nbt_int(nbt, 1);
    nbt_end(nbt);
    nbt_end(nbt);

    SchematicData data;
    std::string error;
    CHECK_FALSE(load_schematic_bytes(nbt.data(), nbt.size(), data, &error));
    CHECK(error.find("neither a Blocks array nor a palette") != std::string::npos);
}

TEST_CASE("schematic: a skipped field is skipped at its own width") {
    // Real files carry block entities and entities, which are dense with floats
    // (Health, ItemDropChance) and doubles (Yaw, Motion). A float skipped as one
    // byte instead of four does not fail where it happens: it desynchronises the
    // cursor, and the file dies much later as an impossible tag type with the
    // whole build already lost. This fixture puts those fields first, inside
    // sections that are skipped whole, so getting the width wrong loses the build.
    const std::vector<uint8_t> bytes = unhex(kSchemFloatFieldsHex);
    SchematicData data;
    std::string error;
    CHECK_MESSAGE(load_schematic_bytes(bytes.data(), bytes.size(), data, &error), error);
    CHECK(data.format == BlockFileFormat::Classic);
    CHECK(data.root_name == "Schematic");
    CHECK(data.width == kFixtureWidth);
    CHECK(data.height == kFixtureHeight);
    CHECK(data.length == kFixtureLength);
    CHECK(data.materials == "Alpha");
    CHECK(first_mismatch(data, false).empty());

    // The skipped sections are still understood: the block entity's position is
    // read out of it, and the entity list is counted.
    CHECK(data.tile_entity_count == 1);
    CHECK(data.tile_entity_positions.size() == 1);
    if (data.tile_entity_positions.size() == 1) {
        CHECK(data.tile_entity_positions[0][0] == 1);
        CHECK(data.tile_entity_positions[0][1] == 0);
        CHECK(data.tile_entity_positions[0][2] == 1);
    }
    CHECK(data.entity_count == 1);
    CHECK(data.tile_entities_truncated == false);
}
