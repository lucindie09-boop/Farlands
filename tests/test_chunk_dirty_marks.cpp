// What a freshly installed chunk is allowed to dirty in its neighbourhood.
//
// The fill's dirty stage used to queue every face neighbour, including chunks
// that are entirely air: an all-air chunk has no mesh to correct, so the pop that
// followed freed RIDs it never made. should_dirty_neighbor is the one decision
// the install path asks, and it ignores the face: a non-air neighbour is worth a
// rebuild from any direction, an all-air one from none.
#include "doctest.h"

#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "mesh/chunk_boundary_dirty.hpp"

#include <array>

using namespace VoxelEngine;

namespace {

constexpr std::array<FaceDirection, 6> kAllFaces = {
    FaceDirection::Top, FaceDirection::Bottom,
    FaceDirection::Right, FaceDirection::Left,
    FaceDirection::Front, FaceDirection::Back,
};

} // namespace

TEST_CASE("should_dirty_neighbor never queues an all-air neighbour") {
    ChunkData all_air;
    ChunkData solid;
    solid.set_block(15, 15, 15, BlockIDs::STONE);

    for (FaceDirection face : kAllFaces) {
        // The review's case: a solid chunk installed beside an all-air chunk. The
        // air chunk has nothing to redraw, so it must not be queued on any face.
        CHECK_FALSE(should_dirty_neighbor(&all_air, face, &solid));
        // A solid neighbour is rebuilt from every face. The old face-by-face test
        // only ever fired for an all-air neighbour anyway, so the X-only rule is
        // gone and this pins the replacement.
        CHECK(should_dirty_neighbor(&solid, face, &solid));
    }

    // A missing chunk on either side is not a neighbour to dirty.
    CHECK_FALSE(should_dirty_neighbor(nullptr, FaceDirection::Right, &solid));
    CHECK_FALSE(should_dirty_neighbor(&all_air, FaceDirection::Right, nullptr));
    CHECK_FALSE(should_dirty_neighbor(&solid, FaceDirection::Right, nullptr));

    // The installed chunk's own contents are not the question: the decision is
    // whether the NEIGHBOUR can draw, so a solid neighbour beside an air install
    // still rebuilds.
    CHECK(should_dirty_neighbor(&solid, FaceDirection::Right, &all_air));
}
