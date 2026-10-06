// The far field's shading: the per-cell face constant, the per-corner occlusion
// built from the node heights around it, and the water plane that is level by
// definition (a lake in a crater is not a dark lake).
//
// Split out of test_lod_surface.cpp, which keeps the geometry: the winding, the
// sampling count, the world coordinates, the deep-floor skip and the shoreline
// rays.
#include "doctest.h"
#include "lod/lod_surface.hpp"
#include "lod_surface_test_support.hpp"

#include <cmath>
#include <cstdint>

using VoxelEngine::lod::build_tile_mesh;
using VoxelEngine::lod::concavity_shade;
using VoxelEngine::lod::face_shade;
using VoxelEngine::lod::kAoStrength;
using VoxelEngine::lod::kShadeEastWest;
using VoxelEngine::lod::kShadeNorthSouth;
using VoxelEngine::lod::kShadeTop;
using VoxelEngine::lod::TileMesh;

using lod_surface_test::as_sampler;
using lod_surface_test::CountingSampler;

TEST_CASE("the face constant follows the cell's own normal") {
    // Level, a steep x rise, a steep z rise: top / east-west / north-south. The
    // three constants are reached in the limit of a vertical face, and a real slope
    // sits between them.
    CHECK(face_shade(0.0f, 0.0f, 8) == doctest::Approx(kShadeTop));
    const float ew = face_shade(1024.0f, 0.0f, 8);
    CHECK(ew > kShadeEastWest);
    CHECK(ew < kShadeEastWest + 0.02f);
    // The z constant is approached from just BELOW it: the two weights it blends with
    // cross over a hair under 0.8 as the face leaves the axis (the same shape
    // shaders/item_lighting.gdshaderinc has). The bracket is what the claim is.
    const float ns = face_shade(0.0f, 1024.0f, 8);
    CHECK(ns > kShadeNorthSouth - 0.001f);
    CHECK(ns < kShadeNorthSouth + 0.02f);
    // A gentle slope is still nearly a top: the constant is about which way the face
    // points, not how steep it is.
    CHECK(face_shade(2.0f, 2.0f, 32) > 0.99f);

    // And it reaches the geometry through the same call. A ramp along x at 32-block
    // spacing, whose cell slope is the mean of its two diagonals.
    CountingSampler sampler;
    sampler.height = [](int32_t x, int32_t) { return static_cast<float>(x); };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(mesh.vertices[0].shade == doctest::Approx(face_shade(32.0f, 0.0f, 32)));
    CHECK(mesh.vertices[0].shade > kShadeEastWest);
    CHECK(mesh.vertices[0].shade < kShadeTop);
}

TEST_CASE("a slope is shaded as one gradient, not as a patchwork of cells") {
    // The regression this rule exists for: the old one compared |hx| against the
    // spacing and then x against z, so a gradient a hair either side of either
    // threshold flipped a whole cell by 0.4 -- 40% of the light it receives -- and
    // the cells of ONE smooth slope came out light, dark, dark, light. On a
    // 256-block cell that is a quilt of squares on the horizon, which is what "the
    // terrain looks weird and patchy" was.
    constexpr int32_t spacing = 32;
    // Across the old top-versus-side threshold, |hx| == spacing.
    const float below = face_shade(0.99f * static_cast<float>(spacing), 0.0f, spacing);
    const float above = face_shade(1.01f * static_cast<float>(spacing), 0.0f, spacing);
    CHECK(std::abs(above - below) < 0.02f);
    // Across the old x-versus-z tie, hx == hz.
    const float tie_a = face_shade(8.0f, 8.0f - 1.0e-3f, spacing);
    const float tie_b = face_shade(8.0f, 8.0f + 1.0e-3f, spacing);
    CHECK(std::abs(tie_b - tie_a) < 0.01f);
    // Cell by cell along a ramp from a level plain to a cliff, no pair of neighbours
    // steps: the shading is a gradient a viewer reads as a slope.
    for (int32_t h = 0; h <= 62; h += 2) {
        const float a = face_shade(static_cast<float>(h), 0.0f, spacing);
        const float b = face_shade(static_cast<float>(h + 2), 0.0f, spacing);
        CHECK(std::abs(b - a) < 0.06f);
    }
    // ...and the range it covers is still the near world's own, both ends.
    CHECK(face_shade(0.0f, 0.0f, spacing) == doctest::Approx(kShadeTop));
    CHECK(face_shade(4096.0f, 0.0f, spacing) < kShadeEastWest + 0.01f);
    CHECK(face_shade(4096.0f, 0.0f, spacing) > kShadeEastWest);
}

TEST_CASE("a dip is dark and everything else is not") {
    // The far field's occlusion, and it is the only occlusion a body of geometry with
    // no blocks can have: how much LOWER a node sits than the four nodes a spacing
    // away from it, measured against its own spacing so the term means the same thing
    // at the innermost level and out at the horizon's.
    constexpr int32_t spacing = 32;
    // Standing level with its neighbours is EXACTLY open: a hair off and every plain
    // in the world would be shaded at random.
    CHECK(concavity_shade(64.0f, 64.0f, 64.0f, 64.0f, 64.0f, spacing) == doctest::Approx(1.0f));
    // A dip of a quarter of the cell is the whole of the occlusion, and half the dip
    // is half of it.
    CHECK(concavity_shade(56.0f, 64.0f, 64.0f, 64.0f, 64.0f, spacing) ==
          doctest::Approx(1.0f - kAoStrength));
    CHECK(concavity_shade(60.0f, 64.0f, 64.0f, 64.0f, 64.0f, spacing) ==
          doctest::Approx(1.0f - kAoStrength * 0.5f));
    // Deeper than the scale cannot go past the floor.
    CHECK(concavity_shade(0.0f, 64.0f, 64.0f, 64.0f, 64.0f, spacing) ==
          doctest::Approx(1.0f - kAoStrength));
    // A ridge gets nothing: occlusion is what stands ABOVE a node, and out here
    // nothing does.
    CHECK(concavity_shade(80.0f, 64.0f, 64.0f, 64.0f, 64.0f, spacing) == doctest::Approx(1.0f));
    // A uniform slope is not a hollow: two neighbours up and two down cancel, which is
    // what keeps a hillside from being darkened for being a hillside.
    CHECK(concavity_shade(64.0f, 56.0f, 72.0f, 56.0f, 72.0f, spacing) == doctest::Approx(1.0f));
    CHECK(concavity_shade(64.0f, 72.0f, 56.0f, 72.0f, 56.0f, spacing) == doctest::Approx(1.0f));
    // The scale follows the spacing: the same 8-block dip is a hollow on a fine
    // lattice and a wrinkle on a coarse one.
    CHECK(concavity_shade(56.0f, 64.0f, 64.0f, 64.0f, 64.0f, 256) >
          concavity_shade(56.0f, 64.0f, 64.0f, 64.0f, 64.0f, 32));
}

TEST_CASE("each corner wears its own occlusion, under the cell's face constant") {
    // The composition: ONE face constant for the cell (it leans one way) and one
    // occlusion per corner, so a hollow darkens ACROSS a cell instead of taking the
    // whole cell down a step -- a step is what the old per-cell table looked like at
    // 256 blocks a cell.
    CountingSampler sampler;
    sampler.height = [](int32_t, int32_t) { return 64.0f; };  // level: face_shade = top
    // A checkerboard of nodes at the 32-block lattice: two opposite corners of each
    // cell dark, the other two open.
    VoxelEngine::lod::NodeShade shade = [](int32_t x, int32_t z) {
        return ((x / 32) + (z / 32)) % 2 == 0 ? 0.5f : 1.0f;
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9, 8.0f, {}, shade);
    CHECK(mesh.terrain_quads == 4);
    // The first cell's corners in the world's own order: (0,0) dark, (32,0) open,
    // (32,32) dark, then the same pair again for the second triangle.
    CHECK(mesh.vertices[0].shade == doctest::Approx(kShadeTop * 0.5f));
    CHECK(mesh.vertices[1].shade == doctest::Approx(kShadeTop));
    CHECK(mesh.vertices[2].shade == doctest::Approx(kShadeTop * 0.5f));
    CHECK(mesh.vertices[3].shade == doctest::Approx(kShadeTop * 0.5f));
    CHECK(mesh.vertices[4].shade == doctest::Approx(kShadeTop * 0.5f));
    CHECK(mesh.vertices[5].shade == doctest::Approx(kShadeTop));

    // A tile that asks for geometry and not for occlusion is exactly what it was
    // before any of this existed: no callback, no darkening, not a hair off.
    const TileMesh plain = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9);
    CHECK(plain.terrain_quads == 4);
    for (const auto& v : plain.vertices) CHECK(v.shade == doctest::Approx(kShadeTop));
}

TEST_CASE("the liquid plane has no hollow to be dark in") {
    // Water is level, so it takes the top constant whatever the terrain around it
    // does -- a lake in a crater is not a dark lake.
    CountingSampler sampler;
    // Shallow enough that the floor is kept (see the deep skip), so both surfaces are
    // in the mesh and the two can be told apart.
    sampler.height = [](int32_t, int32_t) { return 60.0f; };
    sampler.water = [](int32_t, int32_t) { return 64.0f; };
    VoxelEngine::lod::NodeShade shade = [](int32_t, int32_t) { return 0.5f; };
    const TileMesh mesh = build_tile_mesh(0, 0, 64, 32, as_sampler(sampler), 9, 8.0f, {}, shade);
    CHECK(mesh.water_quads == 4);
    CHECK(mesh.terrain_quads == 4);
    for (const auto& v : mesh.vertices) {
        if (v.water > 0.5f) CHECK(v.shade == doctest::Approx(kShadeTop));
        // ...while the floor under it is level (its face constant is the top one) and
        // still sampled with the callback, so a submerged crater floor is not lit like
        // a submerged plain.
        else CHECK(v.shade == doctest::Approx(kShadeTop * 0.5f));
    }
}
