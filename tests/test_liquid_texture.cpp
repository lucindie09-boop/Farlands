#include "doctest.h"
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

namespace {

// A settings set with the random re-ignition switched off: every step is then
// pure arithmetic, which is what the kernel tests need.
Settings quiet(Style style, int resolution = 8) {
    Settings s = style_settings(style, resolution);
    s.excite_chance = 0.0f;
    s.warmup_steps = 0;
    s.seed = 4242u;
    return clamped(s);
}

// One cell of one frame, as RGBA.
std::vector<unsigned char> pixel(const Strip& strip, int frame, int r, int c) {
    const unsigned char* data = strip.frame_data(frame);
    const size_t index = (static_cast<size_t>(r) * strip.frame_size + c) * 4;
    return {data[index], data[index + 1], data[index + 2], data[index + 3]};
}

bool frames_equal(const Strip& a, int fa, const Strip& b, int fb) {
    const unsigned char* pa = a.frame_data(fa);
    const unsigned char* pb = b.frame_data(fb);
    const size_t bytes = static_cast<size_t>(a.frame_size) * a.frame_size * 4;
    return std::equal(pa, pa + bytes, pb);
}

// Worst per-channel difference between two frames of the same strip.
int frame_difference(const Strip& strip, int fa, int fb) {
    const unsigned char* a = strip.frame_data(fa);
    const unsigned char* b = strip.frame_data(fb);
    const size_t bytes = static_cast<size_t>(strip.frame_size) * strip.frame_size * 4;
    int worst = 0;
    for (size_t i = 0; i < bytes; ++i) {
        worst = std::max(worst, std::abs(static_cast<int>(a[i]) - static_cast<int>(b[i])));
    }
    return worst;
}

} // namespace

TEST_CASE("liquid strip has the requested shape") {
    Settings s = style_settings(Style::Water, 8);
    s.frames = 4;
    s.interpolate = 1;

    const Strip strip = generate(s);
    CHECK(strip.width == 8);
    CHECK(strip.frame_size == 8);
    CHECK(strip.frames == 4);
    // Frames stack vertically, so the image is frames * resolution tall.
    CHECK(strip.pixels.size() == static_cast<size_t>(8) * 8 * 4 * 4);

    // ...and the resolution knob really is the frame size.
    Settings big = s;
    big.resolution = 32;
    const Strip large = generate(big);
    CHECK(large.width == 32);
    CHECK(large.frames == 4);
    CHECK(large.pixels.size() == static_cast<size_t>(32) * 32 * 4 * 4);
}

TEST_CASE("liquid generation is deterministic per seed") {
    Settings s = style_settings(Style::Lava, 8);
    s.frames = 6;

    const Strip first = generate(s);
    const Strip again = generate(s);
    CHECK(first.pixels == again.pixels);

    Settings other = s;
    other.seed = s.seed + 1u;
    const Strip reseeded = generate(other);
    CHECK(reseeded.pixels != first.pixels);
}

TEST_CASE("liquid animation animates") {
    // Three styles, all of them: the whole point of the tool is that frames
    // differ. A regression that freezes the automaton must fail here.
    for (const Style style : {Style::Water, Style::Lava, Style::Acid}) {
        Settings s = style_settings(style, 12);
        s.frames = 4;
        const Strip strip = generate(s);
        int differing = 0;
        for (int f = 1; f < strip.frames; ++f) {
            if (!frames_equal(strip, 0, strip, f)) ++differing;
        }
        CHECK(differing == strip.frames - 1);
        // ...and by more than a rounding error over the strip: the field moved.
        CHECK(frame_difference(strip, 0, strip.frames - 1) > 8);
    }
}

TEST_CASE("a quiet field flattens instead of blowing up") {
    // Every style, because the kernel divisor is the one setting that can make
    // the automaton diverge: if the neighbourhood sum is divided by less than
    // the number of cells it sums, the surface multiplies itself every step
    // until the whole frame sits on one ramp stop — an animation that never
    // animates. (The acid preset shipped that way for one revision.)
    for (const Style style : {Style::Water, Style::Lava, Style::Acid}) {
        Settings s = quiet(style, 16);
        s.steps_per_frame = 0;
        Generator gen(s);
        gen.add_surface(3, 5, 4.0f);  // a spike
        gen.advance(400);

        float lo = gen.surface()[0];
        float hi = gen.surface()[0];
        for (const float value : gen.surface()) {
            lo = std::min(lo, value);
            hi = std::max(hi, value);
        }
        // Flat (the spike spread out and died)...
        CHECK(hi - lo < 1e-4f);
        // ...and bounded, not a saturated or diverging field.
        CHECK(std::fabs(hi) < 100.0f);
    }
}

TEST_CASE("row kernel wraps across the seam when wrap is on") {
    Settings s = quiet(Style::Water, 8);
    s.kernel = Kernel::Row;
    s.flow_coupling = 0.0f;

    Generator wrapped(s);
    wrapped.add_surface(0, 0, 1.0f);
    wrapped.advance();

    Settings no_wrap = s;
    no_wrap.wrap = false;
    Generator cut(no_wrap);
    cut.add_surface(0, 0, 1.0f);
    cut.advance();

    const int n = s.resolution;
    const float neighbour = 1.0f / s.surface_divisor;
    // The spike's left neighbour is the far edge of the grid: same value as its
    // right neighbour when the grid is a torus...
    CHECK(std::fabs(wrapped.surface()[0 * n + (n - 1)] - neighbour) < 1e-6f);
    CHECK(std::fabs(wrapped.surface()[0 * n + 1] - neighbour) < 1e-6f);
    // ...and nothing at all when it is not.
    CHECK(cut.surface()[0 * n + (n - 1)] == 0.0f);
}

TEST_CASE("liquid kernels are translation equivariant on the torus") {
    // Row, Box and Plus average a neighbourhood that does not depend on where
    // the cell is, so shifting the input shifts the output. (Warp is excluded:
    // its window wanders with the destination cell, by design.)
    for (const Kernel kernel : {Kernel::Row, Kernel::Box, Kernel::Plus}) {
        Settings s = quiet(Style::Acid, 8);
        s.kernel = kernel;
        s.flow_coupling = 0.0f;

        std::vector<float> field(static_cast<size_t>(s.resolution) * s.resolution, 0.0f);
        for (int r = 0; r < s.resolution; ++r) {
            for (int c = 0; c < s.resolution; ++c) {
                field[static_cast<size_t>(r) * s.resolution + c] =
                    std::sin(static_cast<float>(r) * 1.7f) + std::cos(static_cast<float>(c) * 2.3f);
            }
        }
        const int dr = 2;
        const int dc = 3;
        std::vector<float> shifted(field.size(), 0.0f);
        for (int r = 0; r < s.resolution; ++r) {
            for (int c = 0; c < s.resolution; ++c) {
                const int sr = (r + dr) % s.resolution;
                const int sc = (c + dc) % s.resolution;
                shifted[static_cast<size_t>(sr) * s.resolution + sc] = field[static_cast<size_t>(r) * s.resolution + c];
            }
        }

        Generator base(s);
        base.set_surface(field);
        base.advance();

        Generator moved(s);
        moved.set_surface(shifted);
        moved.advance();

        float worst = 0.0f;
        for (int r = 0; r < s.resolution; ++r) {
            for (int c = 0; c < s.resolution; ++c) {
                const int mr = (r + dr) % s.resolution;
                const int mc = (c + dc) % s.resolution;
                worst = std::max(worst, std::fabs(
                    moved.surface()[static_cast<size_t>(mr) * s.resolution + mc] -
                    base.surface()[static_cast<size_t>(r) * s.resolution + c]));
            }
        }
        CHECK(worst < 1e-6f);
    }
}

TEST_CASE("flow shift scrolls the animation downward, wrapping") {
    Settings s = quiet(Style::Lava, 8);
    s.steps_per_frame = 0;   // only the shift moves anything
    s.shift_rows = 1;
    s.shift_period = 1;
    s.frames = 3;

    // generate() builds its own generator from the settings, so the spike has
    // to come from the settings; what is asserted here is the relation between
    // consecutive frames of one strip — a pure translation, wrapping.
    Strip strip = generate(s);
    const int n = s.resolution;
    int matches = 0;
    int compared = 0;
    for (int f = 0; f + 1 < strip.frames; ++f) {
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) {
                const int src_r = (r - 1 + n) % n;
                ++compared;
                if (pixel(strip, f + 1, r, c) == pixel(strip, f, src_r, c)) ++matches;
            }
        }
    }
    CHECK(compared > 0);
    CHECK(matches == compared);

    // The shift period is respected: with a period of 3 and one row per shift,
    // frames 0..2 are identical and frame 3 is frame 0 moved down one row.
    Settings slow = s;
    slow.shift_period = 3;
    slow.frames = 5;
    const Strip strip2 = generate(slow);
    CHECK(frames_equal(strip2, 0, strip2, 1));
    CHECK(frames_equal(strip2, 0, strip2, 2));
    // Frame 3's last row is frame 0's first row (scrolled down by one).
    for (int c = 0; c < n; ++c) {
        CHECK(pixel(strip2, 3, 0, c) == pixel(strip2, 0, n - 1, c));
    }
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
