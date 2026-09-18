#ifndef FARLANDS_RENDER_LIQUID_TEXTURE_HPP
#define FARLANDS_RENDER_LIQUID_TEXTURE_HPP

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

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
// Settings
// ---------------------------------------------------------------------------

// Which cells of the surface field feed a cell's neighbourhood average.
//
//   Row   the three cells in the cell's own row (the classic still water)
//   Box   the full 3x3 block
//   Warp  the 3x3 block, shifted per row and per column by a sine wave
//         (the classic still lava: the churn comes from the wandering window)
//   Plus  the cell and its four orthogonal neighbours
enum class Kernel : uint8_t { Row = 0, Box = 1, Warp = 2, Plus = 3 };

// A palette + physics preset. The three styles share every knob; only the
// defaults differ (see style_settings).
enum class Style : uint8_t { Water = 0, Lava = 1, Acid = 2 };

inline constexpr int32_t kKernelCount = 4;
inline constexpr int32_t kStyleCount = 3;
inline constexpr int32_t kMaxRampStops = 4;
inline constexpr int32_t kMinResolution = 4;
inline constexpr int32_t kMaxResolution = 256;
inline constexpr int32_t kMinFrames = 1;
inline constexpr int32_t kMaxFrames = 512;
inline constexpr int32_t kMaxInterpolate = 8;
inline constexpr int32_t kMaxSteps = 8;
inline constexpr int32_t kMaxShiftRows = 16;

[[nodiscard]] inline const char* style_name(Style style) {
    switch (style) {
        case Style::Lava: return "lava";
        case Style::Acid: return "acid";
        case Style::Water: break;
    }
    return "water";
}

[[nodiscard]] inline bool style_from_name(const std::string& name, Style& out) {
    if (name == "water") { out = Style::Water; return true; }
    if (name == "lava") { out = Style::Lava; return true; }
    if (name == "acid") { out = Style::Acid; return true; }
    return false;
}

[[nodiscard]] inline const char* kernel_name(Kernel kernel) {
    switch (kernel) {
        case Kernel::Box: return "box";
        case Kernel::Warp: return "warp";
        case Kernel::Plus: return "plus";
        case Kernel::Row: break;
    }
    return "row";
}

struct Settings {
    // Grid + playback shape.
    int resolution = 16;        // cells per side of one frame (square)
    int frames = 32;            // stored animation frames (rows of the strip)
    int warmup_steps = 48;      // steps before the first frame (skip the flat start)
    int steps_per_frame = 1;    // simulation steps between stored frames
    int frame_time = 3;         // ticks the animator holds each frame (data only)
    int interpolate = 1;        // >1 emits blended sub-frames between stored frames

    // Physics.
    Kernel kernel = Kernel::Row;
    float surface_divisor = 3.3f;   // divisor of the neighbourhood sum
    float flow_coupling = 0.8f;     // how much flow feeds back into the surface
    bool flow_box = false;          // average flow over a 2x2 block instead of one cell
    float flow_gain = 0.05f;        // surges converted into flow per step
    float surge_decay = 0.1f;       // surge lost per step
    float excite_chance = 0.05f;    // per-cell chance a surge is re-ignited
    float excite_strength = 0.5f;   // value a re-ignited surge is set to
    float warp_shift = 1.2f;        // Warp kernel: window offset amplitude (cells)
    bool wrap = true;               // sample the grid as a torus

    // Look.
    std::uint32_t seed = 0x9E3779B9u;
    float field_scale = 1.0f;                              // field -> ramp position
    float ramp_curve = 1.0f;                               // gamma on the ramp position
    int ramp_stops = 2;                                    // 2..4
    std::array<std::array<float, 4>, kMaxRampStops> ramp{};  // RGBA 0..1 at t = i/(stops-1)
    int posterize = 0;                                     // 0 = off, else levels per channel
    float grain = 0.0f;                                    // static per-cell noise in ramp space

    // Flowing liquids: scroll the whole field down by this many rows every
    // `shift_period` frames (the classic "flowing water/lava" translation).
    int shift_rows = 0;
    int shift_period = 4;

    // Seamless looping. With `loop` on, the strip's LAST frame is the first
    // frame's image, so a player that wraps back to the start shows a repeat
    // rather than a jump. The automaton is not periodic, so the frames leading
    // up to the seam are morphed into the run that led into frame 0 over
    // `loop_window` frames (load-bearing: a bare duplicate would simply move
    // the pop one frame earlier).
    //
    // The window is the knob that trades a longer "arriving" tail for a smaller
    // seam step: the tail converges on the head by 1/window of the gap each
    // frame, so a window near the field's own decorrelation time (5-8 frames
    // for these kernels) makes the seam step no larger than a normal frame.
    // A looping player should advance from the last frame to index 1 — see
    // Strip::looped.
    bool loop = true;
    int loop_window = 8;
};

// Forces every field into its legal range. Anything the caller sends is
// accepted; nothing here can build a grid of zero cells or a ramp with one stop.
[[nodiscard]] inline Settings clamped(Settings s) {
    s.resolution = std::clamp(s.resolution, kMinResolution, kMaxResolution);
    s.frames = std::clamp(s.frames, kMinFrames, kMaxFrames);
    s.warmup_steps = std::clamp(s.warmup_steps, 0, 4096);
    s.steps_per_frame = std::clamp(s.steps_per_frame, 0, kMaxSteps);
    s.frame_time = std::clamp(s.frame_time, 1, 200);
    s.interpolate = std::clamp(s.interpolate, 1, kMaxInterpolate);
    s.ramp_stops = std::clamp(s.ramp_stops, 2, kMaxRampStops);
    s.posterize = std::clamp(s.posterize, 0, 64);
    s.grain = std::clamp(s.grain, 0.0f, 0.5f);
    s.surface_divisor = std::clamp(s.surface_divisor, 1.0f, 64.0f);
    s.flow_coupling = std::clamp(s.flow_coupling, 0.0f, 1.0f);
    s.flow_gain = std::clamp(s.flow_gain, 0.0f, 0.5f);
    s.surge_decay = std::clamp(s.surge_decay, 0.0f, 1.0f);
    s.excite_chance = std::clamp(s.excite_chance, 0.0f, 1.0f);
    s.excite_strength = std::clamp(s.excite_strength, 0.0f, 8.0f);
    s.warp_shift = std::clamp(s.warp_shift, 0.0f, 8.0f);
    s.field_scale = std::clamp(s.field_scale, 0.0f, 16.0f);
    s.ramp_curve = std::clamp(s.ramp_curve, 0.1f, 8.0f);
    s.shift_rows = std::clamp(s.shift_rows, -kMaxShiftRows, kMaxShiftRows);
    s.shift_period = std::clamp(s.shift_period, 1, 64);
    s.loop_window = std::clamp(s.loop_window, 1, 64);
    for (std::array<float, 4>& stop : s.ramp) {
        for (float& channel : stop) {
            channel = std::clamp(channel, 0.0f, 1.0f);
        }
    }
    return s;
}

// Defaults per style. The water and lava ramps and physics are fitted to the
// colours the classic generators produced (their per-channel curves become
// ramp stops plus a ramp_curve), so they read as the textures people
// remember; acid is our own.
[[nodiscard]] inline Settings style_settings(Style style, int resolution = 16) {
    Settings s;
    s.resolution = resolution;
    switch (style) {
        case Style::Lava:
            // Slow churn: a wandering 3x3 window, a 2x2 flow average, and rare
            // but strong re-ignitions so the crust stays mostly still.
            s.kernel = Kernel::Warp;
            s.frames = 32;
            s.frame_time = 4;
            s.warmup_steps = 64;
            s.surface_divisor = 10.0f;
            s.flow_coupling = 0.8f;
            s.flow_box = true;
            s.flow_gain = 0.01f;
            s.surge_decay = 0.06f;
            s.excite_chance = 0.005f;
            s.excite_strength = 1.5f;
            s.field_scale = 2.0f;
            s.seed = 0x1A7A5EEDu;
            s.ramp_stops = 3;
            s.ramp[0] = {155.0f / 255.0f, 0.0f, 0.0f, 1.0f};
            s.ramp[1] = {205.0f / 255.0f, 64.0f / 255.0f, 8.0f / 255.0f, 1.0f};
            s.ramp[2] = {1.0f, 1.0f, 128.0f / 255.0f, 1.0f};
            break;
        case Style::Acid:
            // Fizzier than water: more energy, shorter-lived surges, and a
            // ramp that goes from deep green to a bright, near-opaque core.
            //
            // The divisor has to exceed the number of cells the kernel sums,
            // or the surface amplifies itself every step until the whole frame
            // sits on one ramp stop (a static image). Box sums nine cells, so
            // 9.9 is the stable counterpart of the row kernel's classic 3.3
            // (sums three). The suite pins this: a quiet field must flatten.
            s.kernel = Kernel::Box;
            s.frames = 32;
            s.frame_time = 3;
            s.warmup_steps = 40;
            s.surface_divisor = 9.9f;
            s.flow_coupling = 0.75f;
            s.flow_gain = 0.03f;
            s.surge_decay = 0.15f;
            s.excite_chance = 0.04f;
            s.excite_strength = 1.2f;
            // Chosen from the field the automaton actually settles at (see the
            // lab's field readout): the pattern has to sit inside the ramp,
            // not pinned against its top.
            s.field_scale = 0.6f;
            s.seed = 0xAC1D0007u;
            s.ramp_stops = 3;
            s.ramp[0] = {24.0f / 255.0f, 52.0f / 255.0f, 24.0f / 255.0f, 0.60f};
            s.ramp[1] = {74.0f / 255.0f, 156.0f / 255.0f, 40.0f / 255.0f, 0.78f};
            s.ramp[2] = {198.0f / 255.0f, 240.0f / 255.0f, 96.0f / 255.0f, 0.88f};
            break;
        case Style::Water:
        default:
            break;
    }
    if (style == Style::Water) {
        // The classic still water: a row-wide ripple, a single-cell flow, and
        // a ramp that only brightens a couple of steps, held half transparent.
        s.kernel = Kernel::Row;
        s.frames = 32;
        s.frame_time = 2;
        s.warmup_steps = 48;
        s.surface_divisor = 3.3f;
        s.flow_coupling = 0.8f;
        s.flow_box = false;
        s.flow_gain = 0.05f;
        s.surge_decay = 0.1f;
        s.excite_chance = 0.05f;
        s.excite_strength = 0.5f;
        s.field_scale = 1.0f;
        s.seed = 0x5EED1234u;
        s.ramp_stops = 2;
        s.ramp_curve = 2.0f;
        s.ramp[0] = {32.0f / 255.0f, 50.0f / 255.0f, 1.0f, 146.0f / 255.0f};
        s.ramp[1] = {64.0f / 255.0f, 114.0f / 255.0f, 1.0f, 196.0f / 255.0f};
    }
    return clamped(s);
}

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
