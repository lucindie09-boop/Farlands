// Fixtures shared by the far-field surface tests -- test_lod_surface.cpp keeps the
// geometry cases, test_lod_surface_shading.cpp the shading ones.
//
// A header in an anonymous namespace would give each translation unit its own
// copy of the sampler, so the helpers are `inline` in a named namespace (the
// shape tests/shape_resolver_test_support.hpp uses) -- and they are qualified in
// full, because a support header does not inherit the includer's using-directives.
#pragma once

#include "lod/lod_surface.hpp"

#include <cstdint>
#include <functional>

namespace lod_surface_test {

// A sampler that answers from a plain height function, counting its calls.
struct CountingSampler {
    std::function<float(int32_t, int32_t)> height;
    std::function<float(int32_t, int32_t)> water;
    int32_t calls = 0;
    int32_t valid_until_x = INT32_MAX;

    VoxelEngine::lod::SurfaceSample operator()(int32_t x, int32_t z) {
        ++calls;
        VoxelEngine::lod::SurfaceSample s;
        s.valid = x < valid_until_x;
        s.height = height ? height(x, z) : 0.0f;
        s.water = water ? water(x, z) : VoxelEngine::lod::kNoWater;
        s.layer = 3;
        return s;
    }
};

// build_tile_mesh takes a std::function, which COPIES its callable: the counting
// sampler has to be reached through a reference or the call count stays zero.
inline VoxelEngine::lod::SurfaceSampler as_sampler(CountingSampler& s) {
    return [&s](int32_t x, int32_t z) { return s(x, z); };
}

} // namespace lod_surface_test
