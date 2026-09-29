#ifndef FARLANDS_TESTS_FLUID_SURFACE_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_FLUID_SURFACE_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the fluid surface tests.
//
// `MapWorld` is a world in a `std::map`: everything not listed is air. The surface
// maths reads nothing else, which is the whole reason mesh_fluid.hpp exists. The
// height constants are the three states a surface can be in (rim, two-to-one, and
// a level cell), and the readers pull the heights a mesh drew and the columns of
// one chunk it turned into sea.
//
// These were file-local to test_fluid_surface.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "mesh/mesh_fluid.hpp"
#include "mesh/mesh_builder.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <vector>

using namespace VoxelEngine;

namespace fluid_surface_test {

// ---------------------------------------------------------------------------
// The rule, on a world in a map: everything not listed is air. The surface maths
// reads nothing else, which is the whole reason mesh_fluid.hpp exists.
// ---------------------------------------------------------------------------
struct MapWorld {
    std::map<std::array<int32_t, 3>, BlockID> cells;
    const BlockRegistry* registry = nullptr;

    void put(int32_t x, int32_t y, int32_t z, BlockID id) { cells[{x, y, z}] = id; }

    mesh_fluid::CellInfo operator()(int32_t x, int32_t y, int32_t z) const {
        const auto it = cells.find({x, y, z});
        const BlockID id = (it == cells.end()) ? BlockIDs::AIR : it->second;
        return mesh_fluid::classify(id, registry->get_block_fast(id));
    }
};

// Heights the reference rule lands on, worked out by hand from its weights:
// a corner with one source and three empty cells, and one with four sources.
constexpr float kRimHeight = 0.698413f;
constexpr float kTwoToOneHeight = 0.814815f;
constexpr float kLevelHeight = 0.888889f;  // 8/9

inline bool near(float a, float b) { return std::fabs(a - b) < 0.002f; }

// Fractional Y of every water vertex: how high the surface sits inside its cell.
inline std::vector<float> water_surface_heights(const MeshBuilder& mb) {
    std::vector<float> out;
    out.reserve(mb.get_water_vertices().size());
    for (const Vertex& v : mb.get_water_vertices()) {
        const float y = static_cast<float>(v.y) / 256.0f;
        out.push_back(y - std::floor(y));
    }
    return out;
}

inline bool has_height(const std::vector<float>& heights, float want) {
    for (const float h : heights) {
        if (std::fabs(h - want) < 0.005f) return true;
    }
    return false;
}

// A chunk full of sea: a stone floor and water the rest of the way up. Meshed
// with the same chunk on all eight sides, which is what an ocean chunk in the
// middle of a sea actually looks like.
inline ChunkData make_sea_chunk(int32_t sea_top) {
    ChunkData c;
    c.fill_blocks(BlockIDs::AIR);
    for (int32_t y = 0; y <= sea_top; ++y) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
                c.set_block(x, y, z, (y == 0) ? BlockIDs::STONE : BlockIDs::SURFACE_WATER);
            }
        }
    }
    c.compute_section_flags();
    return c;
}

inline int count_water_tops(const MeshBuilder& mb, int32_t run_length) {
    int n = 0;
    for (const CachedQuad& q : mb.get_quads()) {
        if (q.water && q.direction == FaceDirection::Top && q.ez == run_length) ++n;
    }
    return n;
}


} // namespace fluid_surface_test

#endif // FARLANDS_TESTS_FLUID_SURFACE_TEST_SUPPORT_HPP
