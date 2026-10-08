// The far field's shoreline: what a cell does when the water and the land are both inside
// it, and the two properties the rule exists for -- that no piece of ground is missed, and
// that no piece of ground is drawn twice.
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
#include <limits>

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
                  const godot::Vector3& b, const godot::Vector3& c, float* out_t = nullptr) {
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
    const float t = e2.dot(q) * inv;
    if (t <= 0.0f) return false;
    if (out_t) *out_t = t;
    return true;
}

// The ground the mesh covers, in the XZ plane: the sum of every triangle's horizontal
// area, with the water quads named apart from the rest so a case can talk about either.
struct Coverage {
    double ground_area = 0.0;
    double water_area = 0.0;
};

double triangle_xz_area(const godot::Vector3& a, const godot::Vector3& b, const godot::Vector3& c) {
    return 0.5 * std::abs((b.x - a.x) * (c.z - a.z) - (b.z - a.z) * (c.x - a.x));
}

Coverage xz_area(const TileMesh& mesh) {
    Coverage out;
    for (size_t base = 0; base + 2 < mesh.vertices.size(); base += 3) {
        const double area = triangle_xz_area(mesh_vertex(mesh, base), mesh_vertex(mesh, base + 1),
                                             mesh_vertex(mesh, base + 2));
        if (mesh.vertices[base].water > 0.5f) {
            out.water_area += area;
        } else {
            out.ground_area += area;
        }
    }
    return out;
}

// What a frame sees, from above: for every sample point, the TOPMOST surface, taken by
// casting a ray straight down through the whole mesh. This is the property the rule is
// for and the one a quad count cannot answer -- two surfaces over one piece of ground are
// one hit here, and a coastline one cell short of the water is a point with no hit at all.
//
// The sample points sit off the cells' own grid (x is odd, z is 2 modulo 4) and off every
// quad diagonal (a diagonal runs along x - z = a multiple of 32, and an odd x - z is
// never one), because a point exactly on a shared edge or diagonal is a hit on both of
// the triangles that share it -- a fact about the sampling and not about the mesh.
struct Topmost {
    int32_t water = 0;
    int32_t ground = 0;
    int32_t missed = 0;
    // Points where TWO triangles of the same kind were hit, which no pair of surfaces
    // that cover the ground once can produce.
    int32_t water_double = 0;
    int32_t ground_double = 0;
};

Topmost topmost_surface(const TileMesh& mesh, int32_t extent, int32_t step) {
    Topmost out;
    const godot::Vector3 dir(0.0f, -1.0f, 0.0f);
    for (int32_t x = 1; x < extent; x += step) {
        for (int32_t z = 2; z < extent; z += 2 * step) {
            const godot::Vector3 from(static_cast<float>(x), 4096.0f, static_cast<float>(z));
            float best = std::numeric_limits<float>::max();
            bool best_is_water = false;
            int32_t water_hits = 0;
            int32_t ground_hits = 0;
            for (size_t base = 0; base + 2 < mesh.vertices.size(); base += 3) {
                float t = 0.0f;
                if (!triangle_hit(from, dir, mesh_vertex(mesh, base), mesh_vertex(mesh, base + 1),
                                  mesh_vertex(mesh, base + 2), &t)) {
                    continue;
                }
                const bool is_water = mesh.vertices[base].water > 0.5f;
                if (is_water) {
                    ++water_hits;
                } else {
                    ++ground_hits;
                }
                if (t < best) {
                    best = t;
                    best_is_water = is_water;
                }
            }
            if (water_hits + ground_hits == 0) {
                ++out.missed;
            } else if (best_is_water) {
                ++out.water;
            } else {
                ++out.ground;
            }
            if (water_hits > 1) ++out.water_double;
            if (ground_hits > 1) ++out.ground_double;
        }
    }
    return out;
}

// A shoreline tile, 8x8 cells at 32 blocks: two columns of ocean, the cell where the
// water runs onto the beach, and five columns of land behind it. The measured heights
// are 40 under the water and 70 on land, with the water level at 64 -- so the ramp in
// the straddling cell crosses the water at x = 64 + 32 * (64 - 40) / (70 - 40) = 89.6.
constexpr float kFloor = 40.0f;
constexpr float kBeach = 70.0f;
constexpr float kSeaLevel = 64.0f;
constexpr float kCoastX = 89.6f;

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

TEST_CASE("a straddling cell is a sheet over the whole cell and ground only above the water") {
    VoxelEngine::lod::SurfaceSampler sampler = [](int32_t x, int32_t z) {
        return shore_sample(x, z);
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, sampler, 9);

    // Three columns of sheet: the two wholly wet ones and the straddling one, which now
    // draws water too (that is the whole change). Six columns of ground: the straddling
    // one's clipped half and the five dry ones.
    CHECK(mesh.water_quads == 3 * 8);
    CHECK(mesh.terrain_quads == 6 * 8);
    int32_t water_verts = 0;
    int32_t land_verts = 0;
    for (const auto& v : mesh.vertices) {
        if (v.water > 0.5f) {
            ++water_verts;
            CHECK(v.y == doctest::Approx(kSeaLevel));
            CHECK(v.layer == doctest::Approx(9.0f));
            // The sheet reaches the far side of the straddling cell: water over ground
            // that stands above it is hidden by depth, which is what makes the coast
            // continuous instead of ending at the last wholly wet cell.
            CHECK(v.x <= 96.0f);
        } else {
            ++land_verts;
            // THE PROPERTY THE CLIP EXISTS FOR: no ground is ever drawn under the sheet.
            // A sea floor a block under the water, kilometres out, is two surfaces the
            // depth buffer picks between a pixel at a time.
            CHECK(v.y >= kSeaLevel);
            CHECK(v.y <= kBeach);
        }
    }
    CHECK(water_verts == 3 * 8 * 6);
    CHECK(land_verts > 0);

    // ...and the ground's own edge is the crossing, not the cell's boundary: the beach
    // starts where the samples' straight line meets the water. That is where the sea has
    // to reach, and up to 25.6 blocks of it were missing before.
    float ground_min_x = std::numeric_limits<float>::max();
    for (const auto& v : mesh.vertices) {
        if (v.water <= 0.5f) ground_min_x = std::min(ground_min_x, v.x);
    }
    CHECK(ground_min_x == doctest::Approx(kCoastX).epsilon(0.001));

    // The ground's own area, per row of cells: five whole dry cells and the clipped part
    // of the sixth (96 - 89.6 blocks of a 32-block face).
    const Coverage area = xz_area(mesh);
    CHECK(area.ground_area ==
          doctest::Approx(static_cast<double>(8) * (5.0 * 32.0 * 32.0 + (96.0 - kCoastX) * 32.0)));
    CHECK(area.water_area == doctest::Approx(static_cast<double>(8) * 3.0 * 32.0 * 32.0));
}

TEST_CASE("every piece of ground is seen once: no gap at the coast and no double cover") {
    VoxelEngine::lod::SurfaceSampler sampler = [](int32_t x, int32_t z) {
        return shore_sample(x, z);
    };
    const TileMesh mesh = build_tile_mesh(0, 0, 256, 32, sampler, 9);
    // Two blocks apart in x, four in z, over the whole tile: 128 x 64 sample points.
    // Every one of them must be answered, by one surface and not two, and the answer
    // must change at the crossing and nowhere else -- so the counts are what a straight
    // shoreline at x = 89.6 predicts, computed here rather than written down.
    const Topmost seen = topmost_surface(mesh, 256, 2);
    CHECK(seen.missed == 0);
    CHECK(seen.water_double == 0);
    CHECK(seen.ground_double == 0);
    int32_t expected_water = 0;
    int32_t expected_ground = 0;
    for (int32_t x = 1; x < 256; x += 2) {
        for (int32_t z = 2; z < 256; z += 4) {
            if (static_cast<float>(x) < kCoastX) {
                ++expected_water;
            } else {
                ++expected_ground;
            }
        }
    }
    CHECK(seen.water == expected_water);
    CHECK(seen.ground == expected_ground);
    CHECK(seen.water > 0);
    CHECK(seen.ground > 0);
}

TEST_CASE("an ocean cell is its sheet alone and a dry cell is its ground alone") {
    constexpr int32_t kSpacing = 32;
    constexpr int32_t kCells = 8 * 8;
    // Deep water everywhere: every corner is under its own water, so every cell's ground
    // is clipped away entirely and the sheet is that cell's whole surface.
    VoxelEngine::lod::SurfaceSampler deep = [](int32_t, int32_t) {
        SurfaceSample s;
        s.valid = true;
        s.height = 10.0f;
        s.water = 90.0f;
        s.layer = 3;
        return s;
    };
    const TileMesh ocean = build_tile_mesh(0, 0, 256, kSpacing, deep, 9);
    CHECK(ocean.water_quads == kCells);
    CHECK(ocean.terrain_quads == 0);
    const Coverage ocean_area = xz_area(ocean);
    CHECK(ocean_area.ground_area == doctest::Approx(0.0));
    CHECK(ocean_area.water_area == doctest::Approx(static_cast<double>(kCells * kSpacing * kSpacing)));

    // ...and dry land everywhere: no sheet at all, and every cell is one quad of ground.
    VoxelEngine::lod::SurfaceSampler dry = [](int32_t, int32_t) {
        SurfaceSample s;
        s.valid = true;
        s.height = 200.0f;
        s.water = kNoWater;
        s.layer = 3;
        return s;
    };
    const TileMesh land = build_tile_mesh(0, 0, 256, kSpacing, dry, 9);
    CHECK(land.water_quads == 0);
    CHECK(land.terrain_quads == kCells);
    const Coverage land_area = xz_area(land);
    CHECK(land_area.water_area == doctest::Approx(0.0));
    CHECK(land_area.ground_area == doctest::Approx(static_cast<double>(kCells * kSpacing * kSpacing)));
}
