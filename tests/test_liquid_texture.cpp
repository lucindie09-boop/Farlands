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
        s.loop = false;  // a looping strip's last frame IS the first: see the loop tests
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

TEST_CASE("a looping strip ends on its own first frame") {
    for (const Style style : {Style::Water, Style::Lava, Style::Acid}) {
        Settings s = style_settings(style, 12);
        s.frames = 12;
        s.loop = true;
        s.loop_window = 4;
        const Strip strip = generate(s);

        CHECK(strip.looped);
        // The last frame is the first frame, byte for byte, through the ramp.
        CHECK(frames_equal(strip, 0, strip, strip.frames - 1));
        // ...and the strip is not degenerate: the frames before it moved.
        int differing = 0;
        for (int f = 1; f < strip.frames - 1; ++f) {
            if (!frames_equal(strip, 0, strip, f)) ++differing;
        }
        CHECK(differing >= strip.frames - 3);
    }
}

TEST_CASE("looping spreads the seam instead of moving it") {
    // The point of the morph window: with a bare duplicate (or no loop at all)
    // the wrap is one huge step. A comparable step inside the strip is the
    // measure of what a frame-to-frame change should look like.
    Settings loopy = style_settings(Style::Water, 16);
    loopy.frames = 16;
    loopy.loop = true;
    // The shipped default window: wide enough that the seam step is no larger
    // than a normal frame-to-frame step. (The assert below is what says so.)
    const Strip looped = generate(loopy);

    Settings open = loopy;
    open.loop = false;
    const Strip unlooped = generate(open);

    // The step into the last frame (the image the player repeats), versus the
    // wrap the user saw before: frame N-1 back to frame 0.
    const int seam = frame_difference(looped, looped.frames - 2, looped.frames - 1);
    const int old_pop = frame_difference(unlooped, unlooped.frames - 2, 0);
    CHECK(seam < old_pop);

    int worst_inside = 0;
    for (int f = 1; f < looped.frames - 1; ++f) {
        worst_inside = std::max(worst_inside, frame_difference(looped, f - 1, f));
    }
    // Measured with the shipped defaults: the step into the repeated frame is 5
    // (worst normal step inside the strip: 6) where the wrap used to be 36.
    CHECK(seam <= worst_inside);

    // A window of one is the bare duplicate: still looped, still ends on frame
    // 0, but the pop simply lands one frame earlier.
    Settings bare = loopy;
    bare.loop_window = 1;
    const Strip duplicated = generate(bare);
    CHECK(duplicated.looped);
    CHECK(frames_equal(duplicated, 0, duplicated, duplicated.frames - 1));
    CHECK(frame_difference(duplicated, duplicated.frames - 2, duplicated.frames - 1) > seam);

    // A window as long as the strip still loops.
    Settings long_window = loopy;
    long_window.loop_window = 64;
    const Strip wide = generate(long_window);
    CHECK(wide.looped);
    CHECK(frames_equal(wide, 0, wide, wide.frames - 1));

    // Single-frame and loop-off strips report themselves honestly.
    Settings tiny = loopy;
    tiny.frames = 1;
    CHECK(!generate(tiny).looped);
    CHECK(!unlooped.looped);
}
