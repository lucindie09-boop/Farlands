#ifndef FARLANDS_RENDER_LIQUID_TEXTURE_HPP
#define FARLANDS_RENDER_LIQUID_TEXTURE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "render/liquid_texture_settings.hpp"

// Procedural animated liquid textures.
//
// The classic block game generated its still-water and still-lava sprites in
// code before textures became image files, by running a small cellular
// automaton over a square grid of floats and mapping the result through a
// colour ramp. This is the same idea with our own names and knobs.
//
// Three square fields track the grid, one float per cell:
//
//   surface  the visible height/heat that becomes the pixel colour
//   flow     momentum the surface pushes into (a leaky accumulator, floored at 0)
//   surge    a decaying energy source, re-ignited by a per-cell dice roll
//
// One step: every cell's surface is replaced by a neighbourhood average of the
// surface field plus a share of flow; each cell's flow gains a share of its
// surge; each surge decays, and with `excite_chance` probability is reset to
// `excite_strength` instead. Nothing here knows about Godot, blocks or files,
// so the identical simulation drives the lab GUI, the runtime animator and the
// unit tests.
namespace VoxelEngine::liquid {

// ---------------------------------------------------------------------------
// Simulation
// ---------------------------------------------------------------------------

// Small linear congruential generator. Deliberately not the engine's RNG: a
// seed must reproduce a strip exactly, in the tests and in the lab alike.
struct Rng {
    std::uint32_t state = 1u;

    [[nodiscard]] float next01() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) * (1.0f / 16777216.0f);
    }
};

// Hash of a cell index into 0..1, stable for a given seed and independent of
// the step count (so grain is a fixed pattern, not shimmer).
[[nodiscard]] inline float cell_noise(int cell, std::uint32_t seed) {
    std::uint32_t h = static_cast<std::uint32_t>(cell) * 2654435761u ^ (seed * 2246822519u);
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

class Generator {
public:
    explicit Generator(Settings settings)
        : settings_(clamped(settings)), rng_{settings_.seed}, scratch_(static_cast<size_t>(settings_.resolution) * settings_.resolution, 0.0f) {
        const size_t cells = static_cast<size_t>(settings_.resolution) * settings_.resolution;
        surface_.assign(cells, 0.0f);
        flow_.assign(cells, 0.0f);
        surge_.assign(cells, 0.0f);
    }

    [[nodiscard]] const Settings& settings() const { return settings_; }
    [[nodiscard]] const std::vector<float>& surface() const { return surface_; }

    // Seeding seam: overwrite the surface field, or drop a value on one cell.
    // Used by the tests to hand the automaton a known pattern and watch what a
    // kernel does with it, and by the lab to re-roll a single droplet.
    void set_surface(std::vector<float> field) {
        if (field.size() == surface_.size()) {
            surface_ = std::move(field);
        }
    }
    void add_surface(int r, int c, float value) {
        const int n = settings_.resolution;
        if (r < 0 || r >= n || c < 0 || c >= n) return;
        surface_[static_cast<size_t>(r) * n + c] += value;
    }

    void warm_up(int steps) {
        for (int i = 0; i < steps; ++i) {
            advance();
        }
    }

    void advance(int steps) {
        for (int i = 0; i < steps; ++i) {
            advance();
        }
    }

    // One simulation step.
    void advance() {
        const int n = settings_.resolution;
        const int cells = n * n;

        // Pass 1: every cell's surface becomes a neighbourhood average of the
        // OLD surface plus a share of the OLD flow. Both read passes use the
        // old fields, so the result never depends on traversal order.
        const bool wrap = settings_.wrap;
        for (int r = 0; r < n; ++r) {
            for (int c = 0; c < n; ++c) {
                const int i = r * n + c;
                float sum = 0.0f;
                switch (settings_.kernel) {
                    case Kernel::Row: {
                        for (int dc = -1; dc <= 1; ++dc) {
                            sum += at(surface_, r, c + dc, n, wrap);
                        }
                        break;
                    }
                    case Kernel::Box: {
                        // Nine cells: the divisor must stay above nine to be
                        // stable (see the acid preset).
                        for (int dr = -1; dr <= 1; ++dr) {
                            for (int dc = -1; dc <= 1; ++dc) {
                                sum += at(surface_, r + dr, c + dc, n, wrap);
                            }
                        }
                        break;
                    }
                    case Kernel::Warp: {
                        // The window itself wanders: a sine wave shifts it by up
                        // to warp_shift cells, row by row and column by column.
                        const float ar = static_cast<float>(r) / static_cast<float>(n) * 6.28318530718f;
                        const float ac = static_cast<float>(c) / static_cast<float>(n) * 6.28318530718f;
                        const int or_ = static_cast<int>(std::sin(ar) * settings_.warp_shift);
                        const int oc = static_cast<int>(std::cos(ac) * settings_.warp_shift);
                        for (int dr = -1; dr <= 1; ++dr) {
                            for (int dc = -1; dc <= 1; ++dc) {
                                sum += at(surface_, r + dr + or_, c + dc + oc, n, wrap);
                            }
                        }
                        break;
                    }
                    case Kernel::Plus: {
                        // Five cells, so the stable divisor is above five.
                        sum += at(surface_, r, c, n, wrap);
                        sum += at(surface_, r - 1, c, n, wrap);
                        sum += at(surface_, r + 1, c, n, wrap);
                        sum += at(surface_, r, c - 1, n, wrap);
                        sum += at(surface_, r, c + 1, n, wrap);
                        break;
                    }
                }
                // Flow is read as the cell's own value, or as the average of
                // the 2x2 block it is the upper-left of (the classic lava
                // churn spreads its momentum over a wider patch).
                float flow_here = flow_[i];
                if (settings_.flow_box) {
                    flow_here = (at(flow_, r, c, n, wrap) + at(flow_, r + 1, c, n, wrap) +
                                 at(flow_, r, c + 1, n, wrap) + at(flow_, r + 1, c + 1, n, wrap)) * 0.25f;
                }
                scratch_[i] = sum / settings_.surface_divisor + flow_here * settings_.flow_coupling;
            }
        }
        surface_.swap(scratch_);

        // Pass 2: flow and surge, per cell and independent of neighbours.
        //
        // The surge is deliberately NOT floored at zero. It decays every step
        // and is only kicked back up by the dice roll, so it settles around a
        // small NEGATIVE mean: that is what keeps the flow from accumulating
        // forever. Flow is floored at zero, so the pair sits at rest and the
        // ripples come from the ignitions. (Flooring the surge too turns flow
        // into a pure integrator: the surface climbs every step until every
        // frame is one flat ramp stop and the animation stops animating —
        // pinned by the suite.)
        for (int i = 0; i < cells; ++i) {
            const float next_flow = flow_[i] + surge_[i] * settings_.flow_gain;
            flow_[i] = next_flow < 0.0f ? 0.0f : next_flow;
            surge_[i] -= settings_.surge_decay;
            if (rng_.next01() < settings_.excite_chance) {
                surge_[i] = settings_.excite_strength;
            }
        }
    }

    // Scrolls every field down by `rows` (negative scrolls up), wrapping.
    // Applied to the fields, not the pixels, so a flowing liquid keeps
    // simulating on top of the translation.
    void shift(int rows) {
        const int n = settings_.resolution;
        const int forward = ((rows % n) + n) % n;
        if (forward == 0) return;
        shift_field(surface_, forward, n);
        shift_field(flow_, forward, n);
        shift_field(surge_, forward, n);
    }

private:
    [[nodiscard]] static float at(const std::vector<float>& field, int r, int c, int n, bool wrap) {
        if (wrap) {
            r = ((r % n) + n) % n;
            c = ((c % n) + n) % n;
        } else {
            if (r < 0 || r >= n || c < 0 || c >= n) return 0.0f;
        }
        return field[static_cast<size_t>(r) * n + c];
    }

    static void shift_field(std::vector<float>& field, int forward, int n) {
        std::vector<float> out(field.size(), 0.0f);
        for (int r = 0; r < n; ++r) {
            const int dst_r = (r + forward) % n;
            for (int c = 0; c < n; ++c) {
                out[static_cast<size_t>(dst_r) * n + c] = field[static_cast<size_t>(r) * n + c];
            }
        }
        field.swap(out);
    }

    Settings settings_;
    Rng rng_;
    std::vector<float> surface_;
    std::vector<float> flow_;
    std::vector<float> surge_;
    std::vector<float> scratch_;
};

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

struct Strip {
    int width = 0;              // cells per side (square frames)
    int frame_size = 0;         // == width
    int frames = 0;             // rows of the strip (frames * interpolate)
    int height = 0;             // == frame_size * frames
    // The cycle closes: with interpolate == 1 the last frame IS the first
    // frame's image, and with interpolate > 1 the last sub-frames are already
    // blending towards index 1. Either way a looping player must advance from
    // the last frame to index 1, not 0 (index 0 has just played), or it will
    // hold one image for two frame times at the seam.
    bool looped = false;
    std::vector<std::uint8_t> pixels;  // RGBA8, row major, frames stacked vertically

    [[nodiscard]] const std::uint8_t* frame_data(int frame) const {
        return pixels.data() + static_cast<size_t>(frame) * frame_size * frame_size * 4;
    }
};

// A ramp position (0..1) through the colour ramp into RGBA8. Alpha is taken
// straight from the ramp and is never posterized: a quantized alpha would make
// a liquid's transparency band with the palette.
[[nodiscard]] inline std::array<std::uint8_t, 4> shade(const Settings& s, float position) {
    float t = std::clamp(position, 0.0f, 1.0f);
    if (s.ramp_curve != 1.0f) {
        t = std::pow(t, s.ramp_curve);
    }
    const int stops = std::clamp(s.ramp_stops, 2, kMaxRampStops);
    const float span = static_cast<float>(stops - 1);
    const float scaled = t * span;
    int lo = static_cast<int>(scaled);
    if (lo > stops - 2) {
        lo = stops - 2;
    }
    const float frac = scaled - static_cast<float>(lo);
    std::array<std::uint8_t, 4> out{};
    for (int ch = 0; ch < 4; ++ch) {
        const float value = s.ramp[lo][ch] + (s.ramp[lo + 1][ch] - s.ramp[lo][ch]) * frac;
        float q = std::clamp(value, 0.0f, 1.0f);
        if (s.posterize > 1 && ch < 3) {
            const float levels = static_cast<float>(s.posterize - 1);
            q = std::round(q * levels) / levels;
        }
        out[ch] = static_cast<std::uint8_t>(std::lround(q * 255.0f));
    }
    return out;
}

// Renders `field` (size*size floats) into `dst` (size*size*4 bytes).
inline void render_into(const Settings& s, const std::vector<float>& field, std::uint8_t* dst) {
    const int cells = s.resolution * s.resolution;
    for (int i = 0; i < cells; ++i) {
        float t = field[static_cast<size_t>(i)] * s.field_scale;
        if (s.grain > 0.0f) {
            t += (cell_noise(i, s.seed) * 2.0f - 1.0f) * s.grain;
        }
        const std::array<std::uint8_t, 4> rgba = shade(s, t);
        dst[static_cast<size_t>(i) * 4 + 0] = rgba[0];
        dst[static_cast<size_t>(i) * 4 + 1] = rgba[1];
        dst[static_cast<size_t>(i) * 4 + 2] = rgba[2];
        dst[static_cast<size_t>(i) * 4 + 3] = rgba[3];
    }
}

// Generates the whole animation strip: `frames * interpolate` frames stacked
// vertically, so the runtime literally scrolls a window down the image.
[[nodiscard]] inline Strip generate(const Settings& raw) {
    const Settings s = clamped(raw);
    const int n = s.resolution;
    const int base_frames = s.frames;
    const int interp = s.interpolate;

    Strip strip;
    strip.width = n;
    strip.frame_size = n;
    strip.frames = base_frames * interp;
    strip.height = n * strip.frames;
    strip.pixels.assign(static_cast<size_t>(n) * n * 4 * strip.frames, 0);

    Generator gen(s);
    gen.warm_up(s.warmup_steps);

    const bool loop = s.loop && base_frames >= 2;
    const int window = loop ? std::min(s.loop_window, base_frames) : 0;

    // A looping strip needs the frames immediately BEFORE frame 0 as well: they
    // are what the tail morphs into, so the motion arriving at the seam is the
    // motion that led into the start rather than a rewind to it.
    std::vector<std::vector<float>> pre;
    if (loop && window > 1) {
        pre.reserve(static_cast<size_t>(window - 1));
        for (int i = 0; i < window - 1; ++i) {
            pre.push_back(gen.surface());
            gen.advance(s.steps_per_frame);
            if (s.shift_rows != 0 && ((i + 1) % s.shift_period) == 0) {
                gen.shift(s.shift_rows);
            }
        }
    }

    std::vector<std::vector<float>> fields;
    fields.reserve(static_cast<size_t>(base_frames));
    for (int f = 0; f < base_frames; ++f) {
        fields.push_back(gen.surface());
        gen.advance(s.steps_per_frame);
        if (s.shift_rows != 0 && ((f + 1) % s.shift_period) == 0) {
            gen.shift(s.shift_rows);
        }
    }

    // Close the cycle: over the last `window` frames the strip is morphed into
    // the run that led into frame 0, ending exactly on it (weight 1 on the last
    // frame), so `frames_equal(strip, 0, strip, frames - 1)` holds and the seam
    // is a repeat instead of a jump.
    if (loop) {
        strip.looped = true;
        std::vector<float> morphed(fields[0].size(), 0.0f);
        for (int j = 0; j < window; ++j) {
            const int frame_index = base_frames - window + j;
            const size_t index = static_cast<size_t>(frame_index);
            const float t = static_cast<float>(j + 1) / static_cast<float>(window);
            const std::vector<float>& source = fields[index];
            const std::vector<float>& target =
                (j < window - 1 && !pre.empty()) ? pre[static_cast<size_t>(j)] : fields[0];
            for (size_t c = 0; c < source.size(); ++c) {
                morphed[c] = source[c] + (target[c] - source[c]) * t;
            }
            fields[index] = morphed;
        }
    }

    std::vector<float> blended;
    int out_frame = 0;
    for (int f = 0; f < base_frames; ++f) {
        for (int k = 0; k < interp; ++k) {
            std::uint8_t* dst = strip.pixels.data() + static_cast<size_t>(out_frame) * n * n * 4;
            if (k == 0) {
                render_into(s, fields[static_cast<size_t>(f)], dst);
            } else {
                const float t = static_cast<float>(k) / static_cast<float>(interp);
                const std::vector<float>& a = fields[static_cast<size_t>(f)];
                // The frame after the last one is index 1 on a looping strip:
                // index 0 is the last frame's own image, so blending towards it
                // would freeze the tail. Without looping, the strip simply
                // wraps to 0 as it always did.
                const int next = (f + 1 < base_frames) ? (f + 1) : (loop ? 1 : 0);
                const std::vector<float>& b = fields[static_cast<size_t>(next)];
                blended.resize(a.size());
                for (size_t i = 0; i < a.size(); ++i) {
                    blended[i] = a[i] + (b[i] - a[i]) * t;
                }
                render_into(s, blended, dst);
            }
            ++out_frame;
        }
    }
    return strip;
}

} // namespace VoxelEngine::liquid

#endif // FARLANDS_RENDER_LIQUID_TEXTURE_HPP
