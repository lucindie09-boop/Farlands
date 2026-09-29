#ifndef FARLANDS_TESTS_SCHEMATIC_READER_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_SCHEMATIC_READER_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixture check shared by the schematic reader tests.
//
// Walks every cell of a decoded fixture and reports the first place the build
// disagrees with the geometry it was written from. Checking all 60 cells against a
// coordinate formula is what pins the cell order down: with x, y and z mixed up the
// ids still land in range, they just land in the wrong places.
//
// This was file-local to test_schematic_reader.cpp; the split moved it here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "schematic/schematic_reader.hpp"
#include "schematic_test_data.hpp"

#include <string>

using VoxelEngine::schematic::BlockState;
using VoxelEngine::schematic::SchematicData;
using schematic_test::fixture_add_block_id;
using schematic_test::fixture_block_id;
using schematic_test::fixture_data_value;

namespace schematic_reader_test {

inline std::string first_mismatch(const SchematicData& data, bool add_blocks) {
    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const BlockState* state = data.state_at(x, y, z);
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

} // namespace schematic_reader_test

#endif // FARLANDS_TESTS_SCHEMATIC_READER_TEST_SUPPORT_HPP
