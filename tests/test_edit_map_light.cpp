#include "doctest.h"
#include "core/edit_map.hpp"
#include "core/chunk_data.hpp"
#include "core/block_types.hpp"
#include <cstring>

using namespace VoxelEngine;

TEST_CASE("edit map applied before light propagation correctly handles emissive removal") {
    // This test simulates the bug where:
    // 1. Chunk is generated with an emissive block
    // 2. Emissive block is broken (saved to edit map)
    // 3. On load, edit map is applied but light propagation ran BEFORE edit map
    // 4. Result: emissive light persists even though block is AIR

    BlockRegistry::get_instance().initialize_default_blocks();

    // Simulate chunk generation with an emissive block
    auto chunk_data = std::make_unique<ChunkData>();
    chunk_data->clear();
    chunk_data->set_block(16, 16, 16, BlockIDs::LIGHT_RED);

    // Initial light propagation (as would happen during generation)
    chunk_data->propagate_light();

    // Verify light is present
    uint8_t r = chunk_data->get_light_r(16, 16, 16);
    CHECK(r == 15);

    // Simulate breaking the emissive block (saved to edit map)
    EditMap edit_map;
    edit_map.set_block(16, 16, 16, BlockIDs::AIR);

    // Apply edit map BEFORE light propagation (the fix)
    for (const auto& entry : edit_map.edits) {
        int32_t lx, ly, lz;
        EditMap::unpack_coord(entry.first, lx, ly, lz);
        chunk_data->set_block(lx, ly, lz, entry.second);
    }
    chunk_data->compute_section_flags();
    chunk_data->compute_fully_solid();

    // Re-propagate light with updated emissive count
    chunk_data->propagate_light();

    // Verify light is cleared
    r = chunk_data->get_light_r(16, 16, 16);
    CHECK(r == 0);

    // Verify no light anywhere in chunk
    bool any_light = false;
    for (int y = 0; y < CHUNK_HEIGHT; ++y)
        for (int z = 0; z < CHUNK_DEPTH; ++z)
            for (int x = 0; x < CHUNK_WIDTH; ++x) {
                uint8_t lr = chunk_data->get_light_r(x, y, z);
                uint8_t lg = chunk_data->get_light_g(x, y, z);
                uint8_t lb = chunk_data->get_light_b(x, y, z);
                if (lr || lg || lb) any_light = true;
            }
    CHECK(!any_light);
}

TEST_CASE("edit map applied before light propagation preserves valid emissive lights") {
    // This test verifies that when edit map is applied before light propagation,
    // new emissive blocks added via edits are correctly lit.

    BlockRegistry::get_instance().initialize_default_blocks();

    // Simulate chunk generation without emissive blocks
    auto chunk_data = std::make_unique<ChunkData>();
    chunk_data->clear();

    // Add an emissive block via edit map
    EditMap edit_map;
    edit_map.set_block(16, 16, 16, BlockIDs::LIGHT_RED);

    // Apply edit map BEFORE light propagation (the fix)
    for (const auto& entry : edit_map.edits) {
        int32_t lx, ly, lz;
        EditMap::unpack_coord(entry.first, lx, ly, lz);
        chunk_data->set_block(lx, ly, lz, entry.second);
    }
    chunk_data->compute_section_flags();
    chunk_data->compute_fully_solid();

    // Light propagation with updated emissive count
    chunk_data->propagate_light();

    // Verify light is present
    uint8_t r = chunk_data->get_light_r(16, 16, 16);
    CHECK(r == 15);

    // Verify light propagates to neighbors
    uint8_t r_neighbor = chunk_data->get_light_r(17, 16, 16);
    CHECK(r_neighbor == 14);
}
