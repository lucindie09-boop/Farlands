#ifndef FARLANDS_FRAME_CAPTURE_HPP
#define FARLANDS_FRAME_CAPTURE_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "core/performance_timer.hpp"

namespace VoxelEngine {

// Renderer values sampled for a frame. The CPU/GPU times are the viewport's
// measurements of the last completed frame; the counts are the engine's render
// totals for it. Kept plain so this header stays free of godot-cpp types.
struct FrameRenderStats {
    double setup_cpu_ms = 0.0;
    double render_cpu_ms = 0.0;
    double render_gpu_ms = 0.0;
    uint64_t draw_calls = 0;
    uint64_t objects = 0;
    uint64_t primitives = 0;
};

// Godot's main-thread monitors. TIME_PROCESS is the whole-frame time in the
// engine's own accounting; the other two are its physics and navigation steps.
struct FrameEngineStats {
    double process_ms = 0.0;
    double physics_ms = 0.0;
    double navigation_ms = 0.0;
};

// A short per-frame history beside PerformanceTimer's interval aggregates. The
// aggregate answers "what did the last two seconds cost"; this answers "which
// frame in the 0.25 s FPS sample was slow", which is what a border hitch needs.
// A frame is recorded at the end of the ChunkManager main-thread callback, after
// every ScopedTimer inside it has ended: the capture reads the timer's cumulative
// totals and subtracts the previous frame's totals, so every phase already
// instrumented by the engine is covered without adding a second timer to each.
class FrameCapture {
public:
    static constexpr size_t kHistoryFrames = 2048;
    static constexpr size_t kTimerCount = static_cast<size_t>(TimerID::Count);

    struct Frame {
        double delta_ms = 0.0;
        std::array<double, kTimerCount> phase_ms{};
        FrameRenderStats render;
        FrameEngineStats engine;
    };

    void capture(const PerformanceTimer& timer, double delta, const FrameRenderStats& render,
                 const FrameEngineStats& engine);
    // Call after PerformanceTimer::reset_all(): the next capture starts a new
    // baseline instead of subtracting the pre-reset totals.
    void restart_baseline();
    void clear();

    // `frames` is the number of frames in the FPS sample (the FPS label's own
    // `_frames`), clamped to the history. The three slowest frames and every
    // non-zero phase of the slowest one are included.
    [[nodiscard]] std::string describe_window(const PerformanceTimer& timer,
                                              int32_t frames,
                                              double slow_frame_ms) const;

private:
    [[nodiscard]] const Frame& frame_from_newest(size_t age) const;

    std::array<Frame, kHistoryFrames> history_{};
    size_t next_ = 0;
    size_t count_ = 0;
    std::array<double, kTimerCount> last_total_ms_{};
    bool have_last_ = false;
};

// Declare this BEFORE the callback's ScopedTimers: destructors run in reverse
// order, so the callback timer records its sample first and the capture then sees
// the complete frame.
class FrameCaptureGuard {
public:
    FrameCaptureGuard(FrameCapture& capture, const PerformanceTimer& timer, double delta,
                      FrameRenderStats render, FrameEngineStats engine)
        : capture_(capture), timer_(timer), delta_(delta), render_(render), engine_(engine) {}
    ~FrameCaptureGuard() { capture_.capture(timer_, delta_, render_, engine_); }

private:
    FrameCapture& capture_;
    const PerformanceTimer& timer_;
    double delta_;
    FrameRenderStats render_;
    FrameEngineStats engine_;
};

inline void FrameCapture::capture(const PerformanceTimer& timer, double delta,
                                  const FrameRenderStats& render,
                                  const FrameEngineStats& engine) {
    Frame& frame = history_[next_];
    frame.delta_ms = std::max(0.0, delta * 1000.0);
    frame.render = render;
    frame.engine = engine;
    for (size_t i = 0; i < kTimerCount; ++i) {
        const double total = timer.get_total_ms(static_cast<TimerID>(i));
        frame.phase_ms[i] = have_last_ ? std::max(0.0, total - last_total_ms_[i]) : total;
        last_total_ms_[i] = total;
    }
    have_last_ = true;
    next_ = (next_ + 1) % kHistoryFrames;
    count_ = std::min(count_ + 1, kHistoryFrames);
}

inline void FrameCapture::restart_baseline() {
    have_last_ = false;
}

inline void FrameCapture::clear() {
    next_ = 0;
    count_ = 0;
    have_last_ = false;
    last_total_ms_.fill(0.0);
}

inline const FrameCapture::Frame& FrameCapture::frame_from_newest(size_t age) const {
    const size_t newest = (next_ + kHistoryFrames - 1) % kHistoryFrames;
    return history_[(newest + kHistoryFrames - age) % kHistoryFrames];
}

inline std::string FrameCapture::describe_window(const PerformanceTimer& timer,
                                                 int32_t frames,
                                                 double slow_frame_ms) const {
    if (count_ == 0) {
        return "=== Slow-frame dump: no captured frame ===\n";
    }

    const size_t requested = frames > 0 ? static_cast<size_t>(frames) : count_;
    const size_t window = std::min(requested, count_);
    const double threshold = slow_frame_ms > 0.0 ? slow_frame_ms : (1000.0 / 600.0);
    struct Ranked {
        size_t age;
        double cost;
    };
    std::vector<Ranked> ranked;
    ranked.reserve(window);
    size_t over_threshold = 0;
    for (size_t age = 0; age < window; ++age) {
        const Frame& frame = frame_from_newest(age);
        const double process_ms = frame.phase_ms[static_cast<size_t>(TimerID::ProcessTotal)];
        const double cost = std::max(frame.delta_ms, process_ms);
        if (cost >= threshold) {
            ++over_threshold;
        }
        ranked.push_back({age, cost});
    }
    std::sort(ranked.begin(), ranked.end(),
              [](const Ranked& a, const Ranked& b) { return a.cost > b.cost; });

    const Frame& worst = frame_from_newest(ranked.front().age);
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "=== Slow-frame dump: last " << window << " frames, " << over_threshold
        << " at or above " << threshold << " ms ===\n";
    out << "slowest 3 (age=0 is the newest frame; ranked by delta or process_total; render_* is the last completed frame):\n";
    const size_t ranked_count = std::min<size_t>(3, ranked.size());
    for (size_t i = 0; i < ranked_count; ++i) {
        const Frame& frame = frame_from_newest(ranked[i].age);
        const double process_ms = frame.phase_ms[static_cast<size_t>(TimerID::ProcessTotal)];
        const double outside_ms = std::max(0.0, frame.delta_ms - process_ms);
        out << "  age " << ranked[i].age << "  delta=" << frame.delta_ms
            << " ms  process_total=" << process_ms << " ms  outside_process="
            << outside_ms << " ms  render_cpu=" << frame.render.render_cpu_ms
            << " ms  render_gpu=" << frame.render.render_gpu_ms << " ms\n";
    }
    out << "phases of the slowest frame (non-zero only):\n";
    for (size_t i = 0; i < kTimerCount; ++i) {
        if (worst.phase_ms[i] < 0.001) {
            continue;
        }
        out << "  " << timer.get_name(static_cast<TimerID>(i)) << "="
            << worst.phase_ms[i] << " ms\n";
    }
    const double process_ms = worst.phase_ms[static_cast<size_t>(TimerID::ProcessTotal)];
    const double callback_ms = worst.phase_ms[static_cast<size_t>(TimerID::ChunkManagerProcess)];
    const double callback_outside_ms = std::max(0.0, callback_ms - process_ms);
    const double outside_callback_ms = std::max(0.0, worst.delta_ms - callback_ms);
    out << "outside-ProcessTotal split of the slowest frame:\n";
    out << "  ChunkManagerProcess=" << callback_ms
        << " ms  callback_outside_controller=" << callback_outside_ms
        << " ms  outside_callback=" << outside_callback_ms << " ms\n";
    out << "  WorldCull=" << worst.phase_ms[static_cast<size_t>(TimerID::WorldCull)]
        << " ms  EnvironmentUpdate="
        << worst.phase_ms[static_cast<size_t>(TimerID::EnvironmentUpdate)] << " ms\n";
    out << "main-thread monitors of the slowest frame (Godot, last completed frame):\n";
    out << "  TIME_PROCESS=" << worst.engine.process_ms
        << " ms  TIME_PHYSICS_PROCESS=" << worst.engine.physics_ms
        << " ms  TIME_NAVIGATION_PROCESS=" << worst.engine.navigation_ms << " ms\n";
    const double player_ms = worst.phase_ms[static_cast<size_t>(TimerID::PlayerProcess)];
    const double outline_ms = worst.phase_ms[static_cast<size_t>(TimerID::BlockOutlineProcess)];
    const double other_scene_ms = player_ms + outline_ms;
    out << "other C++ scene-node timers of the slowest frame (current frame):\n";
    out << "  PlayerProcess=" << player_ms << " ms  BlockOutlineProcess=" << outline_ms
        << " ms  total=" << other_scene_ms << " ms\n";
    out << "  outside_callback_remaining="
        << std::max(0.0, outside_callback_ms - other_scene_ms) << " ms\n";
    out << "render of the slowest frame (last completed frame):\n";
    out << "  setup_cpu=" << worst.render.setup_cpu_ms
        << " ms  draw_calls=" << worst.render.draw_calls
        << "  objects=" << worst.render.objects
        << "  primitives=" << worst.render.primitives << "\n";
    return out.str();
}

} // namespace VoxelEngine

#endif // FARLANDS_FRAME_CAPTURE_HPP
