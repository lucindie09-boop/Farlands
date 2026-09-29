#ifndef FARLANDS_TESTS_LIQUID_TEXTURE_TEST_SUPPORT_HPP
#define FARLANDS_TESTS_LIQUID_TEXTURE_TEST_SUPPORT_HPP

// -----------------------------------------------------------------------------
// Fixtures for the liquid strip tests.
//
// `quiet` is the settings every kernel test uses: random re-ignition off, so each
// step is pure arithmetic, and looping off so a frame is exactly what the kernel
// produced. The other three read a generated strip: one pixel, whether two frames
// are identical, and how far apart two frames are.
//
// These were file-local to test_liquid_texture.cpp; the split moved them here as
// `inline` in a named namespace.
// -----------------------------------------------------------------------------

#include "render/liquid_texture.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <set>
#include <string>
#include <vector>

using namespace VoxelEngine;
using namespace VoxelEngine::liquid;

namespace liquid_texture_test {


// A settings set with the random re-ignition switched off: every step is then
// pure arithmetic, which is what the kernel tests need. Looping is off too, so
// a test about the shift, the grain or the ramp sees the frames it asked for
// and not the seam morph on top of them.
inline Settings quiet(Style style, int resolution = 8) {
    Settings s = style_settings(style, resolution);
    s.excite_chance = 0.0f;
    s.warmup_steps = 0;
    s.seed = 4242u;
    s.loop = false;
    return clamped(s);
}

// One cell of one frame, as RGBA.
inline std::vector<unsigned char> pixel(const Strip& strip, int frame, int r, int c) {
    const unsigned char* data = strip.frame_data(frame);
    const size_t index = (static_cast<size_t>(r) * strip.frame_size + c) * 4;
    return {data[index], data[index + 1], data[index + 2], data[index + 3]};
}

inline bool frames_equal(const Strip& a, int fa, const Strip& b, int fb) {
    const unsigned char* pa = a.frame_data(fa);
    const unsigned char* pb = b.frame_data(fb);
    const size_t bytes = static_cast<size_t>(a.frame_size) * a.frame_size * 4;
    return std::equal(pa, pa + bytes, pb);
}

// Worst per-channel difference between two frames of the same strip.
inline int frame_difference(const Strip& strip, int fa, int fb) {
    const unsigned char* a = strip.frame_data(fa);
    const unsigned char* b = strip.frame_data(fb);
    const size_t bytes = static_cast<size_t>(strip.frame_size) * strip.frame_size * 4;
    int worst = 0;
    for (size_t i = 0; i < bytes; ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}


} // namespace liquid_texture_test

#endif // FARLANDS_TESTS_LIQUID_TEXTURE_TEST_SUPPORT_HPP
