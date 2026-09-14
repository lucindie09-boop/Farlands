// The renderer's water invariant, in pieces:
//
//   * ChunkData::liquid_count — an exact O(1) answer to "does this chunk hold a
//     cell the liquid surface pass has to draw", maintained on every write path.
//   * BlockType::draws_fluid_surface — the same rule the mesher uses, for every
//     registered block, so counting and drawing cannot disagree.
//   * Liquids reach the water buffer at every LOD stride and in both emitter
//     modes. At stride 1 the fluid pass draws them; at stride > 1 the generic
//     emitters do, as boxes — and they used to skip liquids there, which left LOD
//     chunks with no water geometry at all.
//   * mesh_content_hash covers both surfaces, so water that changes while the
//     opaque mesh does not still uploads (see mesh/mesh_content_hash.hpp).
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "mesh/mesh_builder.hpp"
#include "fluids/fluid_state_table.hpp"
#include "mesh/mesh_content_hash.hpp"
#include "mesh/mesh_fluid.hpp"

#include <vector>

using namespace VoxelEngine;

namespace {

bool uses_fluid_surface(BlockID id, const BlockRegistry& registry) {
    return mesh_fluid::family_of(id, registry.get_block(id)) != FluidKind::None;
}

} // namespace

TEST_CASE("liquid: the counting rule and the mesher's rule are the same rule") {
    BlockRegistry& registry = BlockRegistry::get_instance();
    registry.initialize_default_blocks();
    int checked = 0;
    for (uint32_t id = 0; id < registry.get_count(); ++id) {
        const BlockType& type = registry.get_block(id);
        CHECK(type.draws_fluid_surface() == uses_fluid_surface(id, registry));
        ++checked;
    }
    CHECK(checked > 0);
}

TEST_CASE("liquid: liquid_count tracks every write path") {
    BlockRegistry::get_instance().initialize_default_blocks();
    ChunkData chunk;
    CHECK(chunk.liquid_count() == 0);

    chunk.fill_blocks(BlockIDs::SURFACE_WATER);
    CHECK(chunk.liquid_count() == CHUNK_VOLUME);

    chunk.fill_blocks(BlockIDs::STONE);
    CHECK(chunk.liquid_count() == 0);

    chunk.set_block(4, 5, 6, BlockIDs::SURFACE_WATER);
    CHECK(chunk.liquid_count() == 1);
    chunk.set_block(4, 5, 6, BlockIDs::WATER);
    CHECK(chunk.liquid_count() == 1);            // one liquid replaced by another
    chunk.set_block(4, 5, 6, BlockIDs::STONE);
    CHECK(chunk.liquid_count() == 0);
    chunk.set_block(4, 5, 6, BlockIDs::STONE);
    CHECK(chunk.liquid_count() == 0);            // a no-op write stays a no-op

    chunk.set_block(0, 0, 0, BlockIDs::STONE);
    CHECK(chunk.liquid_count() == 0);

    ChunkData copy = chunk;
    copy.set_block(1, 1, 1, BlockIDs::SURFACE_WATER);
    CHECK(copy.liquid_count() == 1);
    CHECK(chunk.liquid_count() == 0);

    copy.clear();
    CHECK(copy.liquid_count() == 0);
}

TEST_CASE("liquid: liquid_count comes out of a dense set_data load") {
    BlockRegistry::get_instance().initialize_default_blocks();
    std::vector<BlockID> dense(static_cast<size_t>(CHUNK_WIDTH) * CHUNK_HEIGHT * CHUNK_DEPTH,
                              BlockIDs::STONE);
    auto at = [](int32_t x, int32_t y, int32_t z) {
        return static_cast<size_t>(x) + static_cast<size_t>(y) * CHUNK_WIDTH +
               static_cast<size_t>(z) * CHUNK_WIDTH * CHUNK_HEIGHT;
    };
    for (int32_t y = 0; y < 8; ++y) {
        for (int32_t x = 0; x < 4; ++x) {
            for (int32_t z = 0; z < 3; ++z) dense[at(x, y, z)] = BlockIDs::SURFACE_WATER;
        }
    }
    ChunkData chunk;
    chunk.set_data(dense.data(), CHUNK_VOLUME);
    CHECK(chunk.liquid_count() == 8 * 4 * 3);
}

TEST_CASE("liquid: at LOD stride, the generic emitters draw running water into the water buffer") {
    BlockRegistry& registry = BlockRegistry::get_instance();
    registry.initialize_default_blocks();

    // Runoff and falling cells, not the two natural-water ids: at LOD stride > 1
    // the generic emitters are what draws liquids, and they used to route only
    // `water` and `surface_water` into the water buffer — every flowing state
    // ended up in the opaque buffer, drawn with the terrain material.
    fluids::FluidStateTable table;
    // CHECK, not REQUIRE: this build compiles doctest with exceptions off.
    CHECK(table.build_from(registry) > 0);
    const BlockID runoff = table.block_for(fluids::FluidCell{FluidKind::Water, 3, false});
    const BlockID falling = table.block_for(fluids::FluidCell{FluidKind::Water, 0, true});
    CHECK(runoff != BlockIDs::AIR);
    CHECK(falling != BlockIDs::AIR);

    for (bool greedy : {true, false}) {
        for (float detail : {0.5f, 1.0f}) {
            ChunkData chunk;
            chunk.fill_blocks(BlockIDs::AIR);
            for (int32_t x = 8; x < 20; ++x) {
                for (int32_t z = 8; z < 20; ++z) {
                    chunk.set_block(x, 10, z, runoff);
                }
            }
            chunk.set_block(9, 11, 9, falling);
            chunk.set_block(10, 11, 9, falling);
            chunk.compute_section_flags();

            MeshBuilder mb;
            mb.set_greedy_enabled(greedy);
            mb.set_detail_level(detail);
            mb.build_mesh(chunk);

            INFO("greedy=" << greedy << " detail=" << detail);
            CHECK(mb.get_water_vertices().size() > 0);
            CHECK(mb.get_water_indices().size() > 0);
        }
    }
}

TEST_CASE("liquid: content hash covers the water surface, not just the opaque one") {
    std::vector<uint8_t> solid_a{1, 2, 3, 4};
    std::vector<uint8_t> solid_b{1, 2, 3, 5};
    std::vector<uint32_t> index_a{0, 1, 2};
    std::vector<uint32_t> index_b{0, 1, 3};
    const std::vector<uint8_t> no_vertices;
    const std::vector<uint32_t> no_indices;

    // Nothing to upload.
    CHECK(mesh_content_hash(no_vertices, no_indices, no_vertices, no_indices) == 0);

    const uint64_t opaque_only = mesh_content_hash(solid_a, index_a, no_vertices, no_indices);
    const uint64_t water_only = mesh_content_hash(no_vertices, no_indices, solid_a, index_a);
    CHECK(opaque_only != 0);
    CHECK(water_only != 0);
    CHECK(opaque_only != water_only);

    // The bug this guards: same blocks, same light, only the water changed.
    const uint64_t with_water_a = mesh_content_hash(solid_a, index_a, solid_a, index_a);
    const uint64_t with_water_b = mesh_content_hash(solid_a, index_a, solid_b, index_b);
    CHECK(with_water_a != with_water_b);

    // ... and only the opaque changed, with the water held constant.
    CHECK(mesh_content_hash(solid_a, index_a, solid_b, index_b) !=
          mesh_content_hash(solid_b, index_b, solid_b, index_b));

    // The two surfaces cannot be swapped for one another.
    CHECK(mesh_content_hash(solid_a, index_a, solid_b, index_b) !=
          mesh_content_hash(solid_b, index_b, solid_a, index_a));

    // Deterministic, and equal inputs agree.
    CHECK(mesh_content_hash(solid_a, index_a, solid_a, index_a) == with_water_a);
}
