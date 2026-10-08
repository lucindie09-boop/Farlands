// The far-field surface tile builder (src/lod/lod_surface.cpp).
//
// This is the geometry the seed-grid mode draws instead of chunks, so the things
// worth pinning are the ones that would show up as broken terrain at distance:
// the winding (a backwards quad is invisible under cull_back), the sampling count
// (a tile that samples every cell corner four times is four times the cost), the
// water quad (an ocean drawn without it is a hole, and an ocean is one quad with no
// corner work at all), and the failure mode (an unsampled column is a hole, never
// invented terrain). The shoreline -- what a cell does when the water and the land
// are both inside it -- is test_lod_surface_shore.cpp.
#include "doctest.h"
#include "lod/lod_surface.hpp"
#include "lod_surface_test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <vector>

using VoxelEngine::lod::build_tile_mesh;
using VoxelEngine::lod::kNoWater;
using VoxelEngine::lod::kShadeTop;
using VoxelEngine::lod::SurfaceSample;
using VoxelEngine::lod::TileMesh;

using lod_surface_test::as_sampler;
using lod_surface_test::CountingSampler;

TEST_CASE("a flat tile is the cells it says it is") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, as_sampler(sampler), 9);

    const int32_t cells = 256 / 32;
    CHECK(cells == 8);
    CHECK(mesh.terrain_quads == cells * cells);
    CHECK(mesh.water_quads == 0);
    CHECK(mesh.vertices.size() == static_cast<size_t>(cells) * cells * 6);
    CHECK(mesh.min_y == doctest::Approx(64.0f));
    CHECK(mesh.max_y == doctest::Approx(64.0f));
    // Every node is sampled once, not once per cell that touches it.
    CHECK(sampler.calls == (cells + 1) * (cells + 1));
    // A level quad takes the top constant.
    for (const auto& v : mesh.vertices) {
        CHECK(v.shade == doctest::Approx(kShadeTop));
        CHECK(v.water == doctest::Approx(0.0f));
        CHECK(v.layer == doctest::Approx(3.0f));
        CHECK(v.y == doctest::Approx(64.0f));
    }
}

TEST_CASE("quads are wound the way the world winds a top face") {
    // A flat tile at y = 0: the first triangle's corners must go
    // (x0,z0) -> (x1,z0) -> (x1,z1), which is MeshBuilder::kFaceVertices order for
    // a +Y face. Reversed here and every far quad is culled away.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 0.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(mesh.vertices.size() >= 6);
    CHECK(mesh.vertices[0].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[0].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[1].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[1].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[2].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[2].z == doctest::Approx(32.0f));
    CHECK(mesh.vertices[3].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[3].z == doctest::Approx(0.0f));
    CHECK(mesh.vertices[4].x == doctest::Approx(32.0f));
    CHECK(mesh.vertices[4].z == doctest::Approx(32.0f));
    CHECK(mesh.vertices[5].x == doctest::Approx(0.0f));
    CHECK(mesh.vertices[5].z == doctest::Approx(32.0f));
}

TEST_CASE("a corner height is shared, so slopes are continuous") {
    // One node raised: every cell that touches it uses the same height there, so
    // no crack opens along the cell edge. On a 2x2 tile the corner node appears
    // twice and the interior node six times, all at their own height.
    CountingSampler sampler;
    sampler.height = [](int32_t x, int32_t z) {
        return (x == 0 && z == 0) ? 100.0f : 50.0f;
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    int32_t at_origin = 0;
    int32_t at_interior = 0;
    for (const auto& v : mesh.vertices) {
        if (v.x == doctest::Approx(0.0f) && v.z == doctest::Approx(0.0f)) {
            CHECK(v.y == doctest::Approx(100.0f));
            ++at_origin;
        }
        if (v.x == doctest::Approx(32.0f) && v.z == doctest::Approx(32.0f)) {
            CHECK(v.y == doctest::Approx(50.0f));
            ++at_interior;
        }
    }
    CHECK(at_origin == 2);
    CHECK(at_interior == 6);
}

TEST_CASE("a finer edge takes its coarser neighbour's chord, so no crack opens") {
    // Two tiles at different spacings share an edge and agree only AT the nodes
    // they have in common: between them the fine surface follows the terrain and
    // the coarse one chords across, and the wedge between the two surfaces is the
    // crack visible along every spacing-level boundary. The finer tile's edge is
    // snapped onto that chord instead, which is what closes it.
    CountingSampler sampler;
    // A ridge along z = 32 that only the finer sampling sees.
    sampler.height = [](int32_t, int32_t z) { return (z % 64 == 32) ? 80.0f : 40.0f; };
    const std::array<int32_t, 4> neighbours{0, 64, 0, 0};  // coarser across +x only
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9, neighbours);

    int32_t on_snapped_edge = 0;
    int32_t on_open_edge = 0;
    for (const auto& v : mesh.vertices) {
        // The snapped edge is the chord between the two nodes the neighbour has.
        if (v.x == doctest::Approx(64.0f)) {
            CHECK(v.y == doctest::Approx(40.0f));
            ++on_snapped_edge;
        }
        // The edge with no coarser neighbour keeps the ridge.
        if (v.x == doctest::Approx(0.0f) && v.z == doctest::Approx(32.0f)) {
            CHECK(v.y == doctest::Approx(80.0f));
            ++on_open_edge;
        }
    }
    CHECK(on_snapped_edge > 0);
    CHECK(on_open_edge > 0);
    // Snapping moves heights, so the box is taken after it -- the ridge is still
    // inside the tile and the cull box has to say so.
    CHECK(mesh.max_y == doctest::Approx(80.0f));
}

TEST_CASE("water is drawn over the cell it covers, and the floor under it is not") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 60.0f; };
    sampler.water = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);

    // The floor is under the sheet, so it is not a second surface: four cells of
    // sheet at the water level and no terrain at all. The floor used to be kept here
    // (it is only four blocks down) and it was drawn under the sheet, which is a
    // surface no frame can show through an opaque one and one a depth buffer can only
    // fight with.
    CHECK(mesh.terrain_quads == 0);
    CHECK(mesh.water_quads == 4);
    int32_t water_verts = 0;
    for (const auto& v : mesh.vertices) {
        if (v.water > 0.5f) {
            ++water_verts;
            CHECK(v.y == doctest::Approx(64.0f));
            CHECK(v.layer == doctest::Approx(9.0f));
        }
    }
    CHECK(water_verts == 24);
    // The water plane is in the cull box, or a lake beyond the horizon would be
    // culled with the terrain that is under it.
    CHECK(mesh.max_y == doctest::Approx(64.0f));
}

TEST_CASE("a floor under a sheet is never drawn, deep or shallow") {
    // A cell whose four samples are all under their own water is its sheet, whatever
    // its depth: there is no second path to disagree with, and no corner work is done
    // for it either (the sheet needs no shading and no biome pair).
    CountingSampler deep;
    deep.height = [](int32_t, int32_t) { return 20.0f; };
    deep.water = [](int32_t, int32_t) { return 90.0f; };
    const TileMesh deep_mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(deep), 9);
    CHECK(deep_mesh.terrain_quads == 0);
    CHECK(deep_mesh.water_quads == 4);

    CountingSampler shallow;
    shallow.height = [](int32_t, int32_t) { return 62.0f; };
    shallow.water = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh shallow_mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(shallow), 9);
    CHECK(shallow_mesh.terrain_quads == 0);
    CHECK(shallow_mesh.water_quads == 4);

    // ...and the same surface, too: six vertices per cell, all of them the sheet.
    CHECK(deep_mesh.vertices.size() == shallow_mesh.vertices.size());
    CHECK(shallow_mesh.vertices.size() == 24);
    for (const auto& v : shallow_mesh.vertices) {
        CHECK(v.water > 0.5f);
        CHECK(v.y == doctest::Approx(64.0f));
    }
}

TEST_CASE("an unsampled column is a hole, not invented terrain") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.valid_until_x = 64;  // the nodes at x = 0 and x = 32 sample
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    // A 2x2 tile: the two cells whose corners are all sampled are drawn, the two
    // that need the missing node at x = 64 are holes.
    CHECK(mesh.terrain_quads == 2);
    CHECK(mesh.vertices.size() == 12);
}

TEST_CASE("a tile that samples nothing at all is empty") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    sampler.valid_until_x = 0;
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(mesh.terrain_quads == 0);
    CHECK(mesh.water_quads == 0);
    CHECK(mesh.vertices.empty());
    CHECK(mesh.min_y == doctest::Approx(0.0f));
    CHECK(mesh.max_y == doctest::Approx(0.0f));
}

TEST_CASE("degenerate tile specifications are refused") {
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    CHECK(build_tile_mesh(0, 0, 0, 32, as_sampler(sampler), 9).vertices.empty());
    CHECK(build_tile_mesh(0, 0, 64, 0, as_sampler(sampler), 9).vertices.empty());
    // A spacing that does not divide the tile would place nodes off the global
    // lattice, so neighbouring tiles would not share their edges.
    CHECK(build_tile_mesh(0, 0, 64, 48, as_sampler(sampler), 9).vertices.empty());
    CHECK(sampler.calls == 0);
}

TEST_CASE("nodes land on the global lattice, so two tiles share their edge") {
    // The reason origin/tile/spacing are constrained: the node at the shared edge
    // is the same world position in both tiles, so the faces coincide exactly.
    CountingSampler sampler;
    sampler.height = [](int32_t x, int32_t) { return static_cast<float>(x % 7); };
    const TileMesh left = build_tile_mesh(0, 0, 256, 32, as_sampler(sampler), 9);
    const TileMesh right = build_tile_mesh(256, 0, 256, 32, as_sampler(sampler), 9);
    CHECK(!left.vertices.empty());
    CHECK(!right.vertices.empty());

    float left_edge_max = -1.0e9f;
    for (const auto& v : left.vertices) {
        if (v.x == doctest::Approx(256.0f)) left_edge_max = std::max(left_edge_max, v.y);
    }
    float right_edge_min = 1.0e9f;
    for (const auto& v : right.vertices) {
        if (v.x == doctest::Approx(256.0f)) right_edge_min = std::min(right_edge_min, v.y);
    }
    CHECK(left_edge_max > -1.0e9f);
    // The same world column sampled in both tiles gives the same height.
    sampler.height = [](int32_t x, int32_t) { return static_cast<float>(x % 7); };
    const auto at_edge = sampler(256, 0);
    for (const auto& v : left.vertices) {
        if (v.x == doctest::Approx(256.0f)) CHECK(v.y == doctest::Approx(at_edge.height));
    }
    for (const auto& v : right.vertices) {
        if (v.x == doctest::Approx(256.0f)) CHECK(v.y == doctest::Approx(at_edge.height));
    }
    CHECK(right_edge_min < 1.0e9f);
}

TEST_CASE("the mesh is in world coordinates and says so in its bounds") {
    // The convention the engine layer depends on: tiled geometry is emitted where
    // it belongs in the world, and the instance transform stays identity. It was
    // mixed with the opposite convention once (local geometry plus an origin
    // transform), which drew every tile but the first at double its offset -- one
    // visible mesh out of a ring. Both halves are pinned here, and the engine takes
    // the instance's cull box from these bounds rather than from the tile index, so
    // the two cannot drift apart again.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };
    const TileMesh mesh = build_tile_mesh(1024, 2048, 256, 32, as_sampler(sampler), 9);
    CHECK(mesh.min_x == doctest::Approx(1024.0f));
    CHECK(mesh.max_x == doctest::Approx(1024.0f + 256.0f));
    CHECK(mesh.min_z == doctest::Approx(2048.0f));
    CHECK(mesh.max_z == doctest::Approx(2048.0f + 256.0f));
    for (const auto& v : mesh.vertices) {
        CHECK(v.x >= 1024.0f);
        CHECK(v.x <= 1280.0f);
        CHECK(v.z >= 2048.0f);
        CHECK(v.z <= 2304.0f);
    }
    // A negative tile is the interesting case for a mesh that must land where it
    // says: the bounds follow the origin's sign rather than an absolute value.
    const TileMesh negative = build_tile_mesh(-768, -256, 256, 32, as_sampler(sampler), 9);
    CHECK(negative.min_x == doctest::Approx(-768.0f));
    CHECK(negative.max_x == doctest::Approx(-512.0f));
}

TEST_CASE("the texture coordinates are world block coordinates") {
    // One block of world is one texture repeat, as a chunk face maps it, and a
    // quad advances the coordinate by its own width -- which is what makes the
    // sampler take a mip level instead of one stretched texel.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 0.0f; };
    const TileMesh mesh = build_tile_mesh(1024, 2048, 64, 32, as_sampler(sampler), 9);
    CHECK(!mesh.vertices.empty());
    float max_u = 0.0f;
    for (const auto& v : mesh.vertices) {
        CHECK(v.u == doctest::Approx(v.x));
        CHECK(v.v == doctest::Approx(v.z));
        max_u = std::max(max_u, v.u);
    }
    // A 32-block quad spans 32 texture repeats.
    CHECK(max_u == doctest::Approx(1024.0f + 32.0f * 2.0f));
}
