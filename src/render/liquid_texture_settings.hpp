#ifndef FARLANDS_LIQUID_TEXTURE_SETTINGS_HPP
#define FARLANDS_LIQUID_TEXTURE_SETTINGS_HPP
#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

// The knobs of the liquid automaton and the per-style presets fitted to them:
// what the lab GUI edits, what the runtime animator is configured with, and what
// the tests compare against. Split out of liquid_texture.hpp, which keeps the
// simulation itself.
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

} // namespace VoxelEngine::liquid

#endif // FARLANDS_LIQUID_TEXTURE_SETTINGS_HPP
