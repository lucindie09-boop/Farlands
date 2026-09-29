#include "doctest.h"

#include "core/block_types.hpp"
#include "core/shape_resolver.hpp"
#include "core/chunk_data.hpp"
#include "mesh/mesh_builder.hpp"
#include "shape_resolver_test_support.hpp"

#include <cstring>
#include <vector>

using namespace VoxelEngine;
using namespace shape_resolver_test;

TEST_CASE("a shape with more boxes than the resolver can carry reports it") {
    BlockRegistry::get_instance().initialize_default_blocks();
    BlockType bt{};
    bt.name = "test_overflow";
    bt.properties = BlockProperty::NoOcclusion;
    bt.full_cube_ = false;

    ShapePart part;
    part.boxes.reserve(ShapeBoxes::kCapacity + 1);
    for (int i = 0; i <= ShapeBoxes::kCapacity; ++i) {
        part.boxes.push_back(box(0.0f, 0.0f, 0.0f, 1.0f, 0.0625f * (i % 16 + 1), 1.0f));
    }
    part.faces = shape_box_faces(part.boxes);
    bt.parts.push_back(std::move(part));

    NeighborTable none;
    const ShapeBoxes boxes = resolve_with(bt, none, ShapeBoxKind::Selection);
    CHECK(boxes.overflowed);
    CHECK(boxes.count() == ShapeBoxes::kCapacity);
}

// The property that replaces a neighbour-update pass: the geometry of a cell is a
// pure function of the cell and its six neighbours, so a chunk meshed while the
// neighbour was already there and one remeshed after the neighbour arrived have to
// be identical. The variant-per-state design this engine deliberately does not have
// would fail this the moment an edit landed beside an existing block, unless an
// update pass ran � which is exactly the machinery that does not exist here.
TEST_CASE("a chunk meshed after the neighbour arrives matches one meshed with it already there") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    const BlockID fence_id = reg.register_block(make_fence("test_fence_mesh"));
    if (fence_id == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const int32_t y = 15;
    const int32_t z = 15;

    ChunkData incremental;
    incremental.fill_blocks(BlockIDs::AIR);
    incremental.set_block(15, y, z, fence_id);
    incremental.compute_section_flags();

    MeshBuilder first;
    first.set_greedy_enabled(false);
    first.build_mesh(incremental);
    const size_t lonely_vertices = first.get_vertices().size();

    // The neighbour arrives and the chunk is remeshed, exactly as an edit does.
    incremental.set_block(16, y, z, fence_id);
    incremental.compute_section_flags();
    MeshBuilder after_edit;
    after_edit.set_greedy_enabled(false);
    after_edit.build_mesh(incremental);

    // The same two cells, from a chunk that never saw them apart.
    ChunkData at_load;
    at_load.fill_blocks(BlockIDs::AIR);
    at_load.set_block(15, y, z, fence_id);
    at_load.set_block(16, y, z, fence_id);
    at_load.compute_section_flags();
    MeshBuilder loaded;
    loaded.set_greedy_enabled(false);
    loaded.build_mesh(at_load);

    CHECK(lonely_vertices > 0);
    // The arm is geometry: the pair draws more than the post alone.
    CHECK(after_edit.get_vertices().size() > lonely_vertices);

    CHECK(after_edit.get_vertices().size() == loaded.get_vertices().size());
    CHECK(after_edit.get_indices().size() == loaded.get_indices().size());
    if (after_edit.get_vertices().size() == loaded.get_vertices().size() &&
        after_edit.get_indices().size() == loaded.get_indices().size()) {
        CHECK(std::memcmp(after_edit.get_vertices().data(), loaded.get_vertices().data(),
                          after_edit.get_vertices().size() * sizeof(Vertex)) == 0);
        CHECK(std::memcmp(after_edit.get_indices().data(), loaded.get_indices().data(),
                          after_edit.get_indices().size() * sizeof(uint32_t)) == 0);
    }

    // And the reverse direction is the same world too: removing the neighbour takes
    // the arm away again rather than leaving it behind.
    incremental.set_block(16, y, z, BlockIDs::AIR);
    incremental.compute_section_flags();
    MeshBuilder after_removal;
    after_removal.set_greedy_enabled(false);
    after_removal.build_mesh(incremental);
    CHECK(after_removal.get_vertices().size() == lonely_vertices);
}

// Same property as the fence pair above, for the shapes whose claims are read on
// faces their own boxes do not reach: load-time, paste-time and post-edit meshes of
// the same world agree to the byte.
TEST_CASE("a chunk meshed after a stair is cut beside it matches one meshed with it there") {
    BlockRegistry& reg = BlockRegistry::get_instance();
    reg.initialize_default_blocks();
    // The stair under test steps toward +X, so the cell to its east stands in front
    // of the step: a stair turned across it there cuts the step back.
    const BlockID stepper = reg.register_block(make_stair("test_stair_mesh_e", ShapeFace::Right));
    const BlockID crossing = reg.register_block(make_stair("test_stair_mesh_n", ShapeFace::Back));
    if (stepper == BlockIDs::AIR || crossing == BlockIDs::AIR) {
        CHECK(false);
        return;
    }

    const int32_t y = 15;
    const int32_t z = 15;

    ChunkData incremental;
    incremental.fill_blocks(BlockIDs::AIR);
    incremental.set_block(15, y, z, stepper);
    incremental.compute_section_flags();
    MeshBuilder whole;
    whole.set_greedy_enabled(false);
    whole.build_mesh(incremental);
    const std::vector<Vertex> whole_vertices = whole.get_vertices();

    incremental.set_block(16, y, z, crossing);
    incremental.compute_section_flags();
    MeshBuilder after_edit;
    after_edit.set_greedy_enabled(false);
    after_edit.build_mesh(incremental);
    const std::vector<Vertex>& cut_vertices = after_edit.get_vertices();
    CHECK(!whole_vertices.empty());
    // The cut is geometry: the step lost its other half, whatever the vertex count
    // does about face culling.
    const bool changed = cut_vertices.size() != whole_vertices.size() ||
                         (cut_vertices.size() == whole_vertices.size() &&
                          std::memcmp(cut_vertices.data(), whole_vertices.data(),
                                      cut_vertices.size() * sizeof(Vertex)) != 0);
    CHECK(changed);

    ChunkData at_load;
    at_load.fill_blocks(BlockIDs::AIR);
    at_load.set_block(15, y, z, stepper);
    at_load.set_block(16, y, z, crossing);
    at_load.compute_section_flags();
    MeshBuilder loaded;
    loaded.set_greedy_enabled(false);
    loaded.build_mesh(at_load);

    CHECK(after_edit.get_vertices().size() == loaded.get_vertices().size());
    CHECK(after_edit.get_indices().size() == loaded.get_indices().size());
    if (after_edit.get_vertices().size() == loaded.get_vertices().size() &&
        after_edit.get_indices().size() == loaded.get_indices().size()) {
        CHECK(std::memcmp(after_edit.get_vertices().data(), loaded.get_vertices().data(),
                          after_edit.get_vertices().size() * sizeof(Vertex)) == 0);
        CHECK(std::memcmp(after_edit.get_indices().data(), loaded.get_indices().data(),
                          after_edit.get_indices().size() * sizeof(uint32_t)) == 0);
    }

    // And taking the neighbour away puts the whole step back.
    incremental.set_block(16, y, z, BlockIDs::AIR);
    incremental.compute_section_flags();
    MeshBuilder after_removal;
    after_removal.set_greedy_enabled(false);
    after_removal.build_mesh(incremental);
    CHECK(after_removal.get_vertices().size() == whole_vertices.size());
}
