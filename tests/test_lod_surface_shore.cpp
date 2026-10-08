// The far field's shoreline: what a cell does when the water and the land are both inside
// it, and the property the whole rule exists for -- that the mode never draws two surfaces
// over the same ground.
//
// Split out of test_lod_surface.cpp, which keeps the geometry of a tile that is not a
// coastline (the winding, the sampling count, the world coordinates, the chord snapping).
#include "doctest.h"
#include "lod/lod_surface.hpp"
#include "lod_surface_test_support.hpp"

// The rays below are cast in the engine's own vector type.
#include <godot_cpp/variant/vector3.hpp>

#include <cmath>
#include <cstdint>

using VoxelEngine::lod::build_tile_mesh;
using VoxelEngine::lod::kNoWater;
using VoxelEngine::lod::SurfaceSample;
using VoxelEngine::lod::TileMesh;

namespace {

godot::Vector3 mesh_vertex(const TileMesh& mesh, size_t index) {
    const VoxelEngine::lod::LodVertex& v = mesh.vertices[index];
    return godot::Vector3(v.x, v.y, v.z);
}

// Moller-Trumbore, two-sided: the far field is drawn with cull_disabled, so a hit
// on either winding is the frame being stopped.
bool triangle_hit(const godot::Vector3& o, const godot::Vector3& d, const godot::Vector3& a,
                  const godot::Vector3& b, const godot::Vector3& c) {
    const godot::Vector3 e1 = b - a;
    const godot::Vector3 e2 = c - a;
    const godot::Vector3 p = d.cross(e2);
    const float det = e1.dot(p);
    if (std::abs(det) < 1.0e-6f) return false;
    const float inv = 1.0f / det;
    const godot::Vector3 s = o - a;
    const float u = s.dot(p) * inv;
    if (u < 0.0f || u > 1.0f) return false;
    const godot::Vector3 q = s.cross(e1);
    const float v = d.dot(q) * inv;
    if (v < 0.0f || u + v > 1.0f) return false;
    return e2.dot(q) * inv > 0.0f;
}

// A ray travelling +x at height `y`, the ray a player looks along when a far
// shoreline opens up. It starts outside the tile and crosses all of it.
bool ray_hits(const TileMesh& mesh, float y) {
    const godot::Vector3 origin(-512.0f, y, 144.0f);
    const godot::Vector3 dir(1.0f, 0.0f, 0.0f);
    for (size_t base = 0; base + 2 < mesh.vertices.size(); base += 3) {
        if (triangle_hit(origin, dir, mesh_vertex(mesh, base), mesh_vertex(mesh, base + 1),
                         mesh_vertex(mesh, base + 2))) {
            return true;
        }
    }
    return false;
}

// The ground the mesh covers, in the XZ plane: the sum of every triangle's horizontal
// area. Two surfaces over one cell each contribute their own, so a cell that emitted
// both adds its whole area twice however the quad counts are written.
double xz_area(const TileMesh& mesh) {
    double total = 0.0;
    for (size_t base = 0; base + 2 < mesh.vertices.size(); base += 3) {
        const godot::Vector3 a = mesh_vertex(mesh, base);
        const godot::Vector3 b = mesh_vertex(mesh, base + 1);
        const godot::Vector3 c = mesh_vertex(mesh, base + 2);
        total += 0.5 * std::abs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x));
    }
    return total;
}

// A shoreline tile, 8x8 cells at 32 blocks: two columns of ocean, the cell where the
// water runs onto the beach, and five columns of land behind it. The measured heights
// are 40 under the water and 70 on land, with the water level at 64.
constexpr float kFloor = 40.0f;
constexpr float kBeach = 70.0f;
constexpr float kSeaLevel = 64.0f;

SurfaceSample shore_sample(int32_t x, int32_t z) {
    (void)z;
    SurfaceSample s;
    s.valid = true;
    if (x <= 64) {
        s.height = kFloor;
        s.water = kSeaLevel;
    } else {
        s.height = kBeach;
        s.water = kNoWater;
    }
    s.layer = 3;
    return s;
}

} // namespace

TEST_CASE("a cell with a land sample in it is land, whole, and draws no water") {
    VoxelEngine::lod::SurfaceSampler sampler = [](int32_t x, int32_t z) {
        return shore_sample(x, z);
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, sampler, 9);

    // The two wholly-wet columns are sheets. The shoreline column is LAND, and so are
    // the five dry ones: a cell that has a land sample anywhere on it is not water.
    CHECK(mesh.terrain_quads == 6 * 8);
    CHECK(mesh.water_quads == 2 * 8);
    int32_t water_verts = 0;
    int32_t land_verts = 0;
    for (const auto& v : mesh.vertices) {
        if (v.water > 0.5f) {
            ++water_verts;
            CHECK(v.y == doctest::Approx(kSeaLevel));
            CHECK(v.layer == doctest::Approx(9.0f));
            // Nothing wet crosses into the shoreline column: its samples are not all
            // under their own water, so it contributes no sheet at all.
            CHECK(v.x <= 64.0f);
        } else {
            ++land_verts;
            // The land is drawn from its own samples -- the shoreline column ramps
            // from the seabed to the beach -- and never from the water level.
            CHECK((v.y == doctest::Approx(kFloor) || v.y == doctest::Approx(kBeach)));
        }
    }
    CHECK(water_verts == 2 * 8 * 6);
    CHECK(land_verts > 0);

    // What the rule costs, in the same geometry: the shoreline cell is a ramp from 40
    // to 70 and the water does not follow it up, so the sea ends up to one cell short
    // of the beach. The rays are the frame, not the counts -- every one a player would
    // look along just over the waterline is STOPPED by that ramp -- and the last two
    // are the control: above the land there is nothing to hit, so a probe that "hits"
    // everywhere is a probe that measures nothing.
    for (float y = kSeaLevel + 1.0f; y <= kBeach - 1.0f; y += 2.0f) {
        CHECK_MESSAGE(ray_hits(mesh, y), "a ray at height " << y << " crossed the shoreline");
    }
    CHECK_FALSE(ray_hits(mesh, kBeach + 4.0f));
    CHECK_FALSE(ray_hits(mesh, kBeach + 40.0f));
}

TEST_CASE("no cell's ground is covered by both surfaces") {
    // THE PROPERTY THE RULE IS FOR, measured on the ground rather than on the counts: sum
    // the XZ area of every triangle and compare it with the cells the tile drew. An
    // ordinary shoreline cell that emitted its land AND a sheet over the whole cell added
    // its own area twice, which is the far field's own light blue and grass over the same
    // pixels -- the report this answers. Whatever a cell is built from, its area is its
    // own once.
    VoxelEngine::lod::SurfaceSampler sampler = [](int32_t x, int32_t z) {
        return shore_sample(x, z);
    };
    constexpr int32_t kSpacing = 32;
    constexpr int32_t kCellBlocks = kSpacing * kSpacing;
    const int32_t cells = 8 * 8;
    const TileMesh mesh = build_tile_mesh(0, 0, 256, kSpacing, sampler, 9);
    CHECK(mesh.terrain_quads + mesh.water_quads == cells);
    CHECK(xz_area(mesh) == doctest::Approx(static_cast<double>(cells * kCellBlocks)));

    // ...and the same property on a coastal cell whose water is DEEPER than the cell is
    // wide, which is the shape the old wholly-submerged fast path took: the floor is not
    // drawn and the sheet is the whole surface, so the area is the cell's once.
    VoxelEngine::lod::SurfaceSampler deep = [](int32_t, int32_t) {
        SurfaceSample s;
        s.valid = true;
        s.height = 10.0f;
        s.water = 90.0f;
        s.layer = 3;
        return s;
    };
    const TileMesh ocean = build_tile_mesh(0, 0, 256, kSpacing, deep, 9);
    CHECK(ocean.water_quads == cells);
    CHECK(ocean.terrain_quads == 0);
    CHECK(xz_area(ocean) == doctest::Approx(static_cast<double>(cells * kCellBlocks)));
}
