#include "doctest.h"

#include "liquid_texture_test_support.hpp"

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
using namespace liquid_texture_test;

TEST_CASE("interpolation past the seam heads for index 1, not 0") {
    // Index 0 is the last frame's own image on a looping strip, so a sub-frame
    // that blended towards it would freeze the tail for the whole window.
    Settings s = style_settings(Style::Acid, 8);
    s.frames = 6;
    s.loop = true;
    s.interpolate = 3;
    const Strip strip = generate(s);

    // Last group of sub-frames: from frame 0's image (the last frame) towards
    // frame 1's image, so they must not all equal each other or the last frame.
    const int last = strip.frames - 1;
    CHECK(!frames_equal(strip, last, strip, last - 1));
    CHECK(!frames_equal(strip, last - 1, strip, last - 2));

    Settings no_blend = s;
    no_blend.interpolate = 1;
    const Strip plain = generate(no_blend);
    // The sampled frames are the same either way; only sub-frames were added.
    CHECK(frames_equal(strip, 0, plain, 0));
    CHECK(frames_equal(strip, 3, plain, 1));
    // The cycle closes at the last group's first sub-frame (the strip's own
    // first image); the sub-frames after it head for index 1, which is the frame
    // the player shows next.
    CHECK(frames_equal(strip, 0, strip, strip.frames - 3));
    CHECK(frames_equal(strip, strip.frames - 3, plain, plain.frames - 1));
}

TEST_CASE("shipped presets put their pattern inside the ramp") {
    // The generator's job is to land on the ramp, not to pin against it: a
    // field whose range sits above 1.0 is one flat colour, and one that sits
    // near 0 is a black square. This pins the field_scale calibration of every
    // preset to the range the automaton actually settles at (both failures
    // above were found this way).
    for (const Style style : {Style::Water, Style::Lava, Style::Acid}) {
        const Settings s = style_settings(style, 16);
        Generator gen(s);
        gen.warm_up(s.warmup_steps);

        float lo = gen.surface()[0] * s.field_scale;
        float hi = lo;
        for (const float value : gen.surface()) {
            const float position = value * s.field_scale;
            lo = std::min(lo, position);
            hi = std::max(hi, position);
        }
        CHECK(lo >= 0.0f);
        CHECK(hi <= 1.0f);
        // ...and uses a decent share of it, so the liquid has visible shape.
        CHECK(hi - lo > 0.15f);
    }
}

TEST_CASE("ramp endpoints are honoured and alpha is not posterized") {
    // A quiet field is exactly zero, which lands on the first ramp stop: the
    // classic still-water base colour, alpha included.
    Settings s = quiet(Style::Water, 8);
    s.frames = 1;
    const Strip strip = generate(s);
    const std::vector<unsigned char> base = pixel(strip, 0, 4, 4);
    CHECK(base[0] == 32);
    CHECK(base[1] == 50);
    CHECK(base[2] == 255);
    CHECK(base[3] == 146);

    // The other end: a bright field saturates on the last stop, alpha included.
    Settings bright = s;
    bright.field_scale = 40.0f;
    const std::array<unsigned char, 4> top = shade(clamped(bright), 1.0f);
    CHECK(top[0] == 64);
    CHECK(top[1] == 114);
    CHECK(top[2] == 255);
    CHECK(top[3] == 196);

    // Posterizing is for the colours: alpha keeps its smooth range, or a
    // liquid's transparency would band along with its palette.
    Settings acid = style_settings(Style::Acid, 16);
    acid.frames = 2;
    acid.posterize = 3;
    const Strip posterized = generate(acid);
    std::set<unsigned char> alphas;
    for (size_t i = 3; i < posterized.pixels.size(); i += 4) {
        alphas.insert(posterized.pixels[i]);
    }
    CHECK(alphas.size() > 3);
}

TEST_CASE("posterize limits the number of distinct colours") {
    Settings s = style_settings(Style::Lava, 16);
    s.frames = 3;
    s.posterize = 3;

    const Strip strip = generate(s);
    std::set<unsigned char> reds;
    for (size_t i = 0; i < strip.pixels.size(); i += 4) {
        reds.insert(strip.pixels[i]);
    }
    // Three levels per channel means at most three distinct reds.
    CHECK(reds.size() <= 3);

    // The control: the same lava without posterizing has a smooth ramp.
    Settings smooth = s;
    smooth.posterize = 0;
    std::set<unsigned char> smooth_reds;
    const Strip raw = generate(smooth);
    for (size_t i = 0; i < raw.pixels.size(); i += 4) {
        smooth_reds.insert(raw.pixels[i]);
    }
    CHECK(smooth_reds.size() > 3);
}

TEST_CASE("interpolation adds sub-frames without touching the sampled ones") {
    Settings s = style_settings(Style::Water, 8);
    s.frames = 4;
    s.interpolate = 1;
    const Strip base = generate(s);

    Settings smooth = s;
    smooth.interpolate = 3;
    const Strip interpolated = generate(smooth);

    CHECK(interpolated.frames == base.frames * 3);
    for (int f = 0; f < base.frames; ++f) {
        // Sub-frame 0 of each group is the sampled frame, byte for byte.
        CHECK(frames_equal(interpolated, f * 3, base, f));
    }
    // The blend frames sit between their neighbours rather than repeating them.
    CHECK(!frames_equal(interpolated, 0, interpolated, 1));
}

TEST_CASE("grain is a fixed pattern, not shimmer") {
    Settings s = quiet(Style::Acid, 8);
    s.steps_per_frame = 0;
    s.frames = 3;
    s.grain = 0.15f;

    const Strip grainy = generate(s);
    // With no simulation steps and no excitation, only the static grain varies
    // per cell; every frame must therefore be identical.
    CHECK(frames_equal(grainy, 0, grainy, 1));
    CHECK(frames_equal(grainy, 0, grainy, 2));

    Settings smooth = s;
    smooth.grain = 0.0f;
    CHECK(generate(smooth).pixels != grainy.pixels);
}

TEST_CASE("styles are distinct and name lookup round-trips") {
    const Settings water = style_settings(Style::Water, 16);
    const Settings lava = style_settings(Style::Lava, 16);
    const Settings acid = style_settings(Style::Acid, 16);

    CHECK(water.kernel != lava.kernel);
    CHECK(water.ramp_stops != lava.ramp_stops);
    CHECK(lava.surface_divisor != acid.surface_divisor);
    CHECK(!(generate(water).pixels == generate(lava).pixels));
    CHECK(!(generate(acid).pixels == generate(water).pixels));

    Style parsed = Style::Lava;
    CHECK(style_from_name("acid", parsed));
    CHECK(parsed == Style::Acid);
    CHECK(!style_from_name("mercury", parsed));
    CHECK(parsed == Style::Acid);  // untouched on failure
    CHECK(std::string(style_name(Style::Water)) == "water");
    CHECK(std::string(kernel_name(Kernel::Warp)) == "warp");
}

TEST_CASE("absurd settings are clamped rather than trusted") {
    Settings s;
    s.resolution = 0;
    s.frames = -5;
    s.interpolate = 999;
    s.ramp_stops = 9;
    s.posterize = -3;
    s.steps_per_frame = 99;
    s.shift_rows = 1000;
    s.shift_period = 0;

    const Settings fixed = clamped(s);
    CHECK(fixed.resolution == kMinResolution);
    CHECK(fixed.frames == kMinFrames);
    CHECK(fixed.interpolate == kMaxInterpolate);
    CHECK(fixed.ramp_stops == kMaxRampStops);
    CHECK(fixed.posterize == 0);
    CHECK(fixed.steps_per_frame == kMaxSteps);
    CHECK(fixed.shift_rows == kMaxShiftRows);
    CHECK(fixed.shift_period == 1);

    // And generating from them produces a usable strip, not a crash.
    const Strip strip = generate(s);
    CHECK(strip.width == kMinResolution);
    CHECK(strip.frames == kMaxInterpolate);
    CHECK(strip.pixels.size() == static_cast<size_t>(kMinResolution) * kMinResolution * 4 * kMaxInterpolate);
}
