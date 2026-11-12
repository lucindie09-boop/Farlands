#ifndef FARLANDS_CHUNK_WORLD_INSTALL_BUDGET_HPP
#define FARLANDS_CHUNK_WORLD_INSTALL_BUDGET_HPP

#include <cstddef>
#include <cstdlib>

namespace VoxelEngine {

// The last ~150 installs arrive after the workers have gone quiet and the
// light/dirty stages are already empty. At the fixed 0.75 ms drain budget each
// frame installs only ~25 chunks, so the final 127 took 4.433 -> 4.518 s on RD32
// (tail trace, 2026-11-10). This is the one point where spending a little more
// main-thread time cannot delay generation, because there is no generation left;
// it only finishes the chunk map sooner. Five paired AB/BA runs at 15 workers
// moved the exact fill settle median 4.154 -> 4.051 s (paired +165 ms, B faster
// 4/5); a trace drained the last 144 completions in 37 ms against 85 ms for 127
// at the old budget. `FARLANDS_FINAL_INSTALL_BUDGET_MS` is the measurement
// switch; unset is the shipping 2 ms, and 0.75 restores the old arm.
inline double final_install_budget_ms() {
    static const double value = [] {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - read once on first use, env is fixed
        const char* raw = std::getenv("FARLANDS_FINAL_INSTALL_BUDGET_MS");
        if (raw == nullptr || *raw == '\0') return 2.0;
        char* end = nullptr;
        const double parsed = std::strtod(raw, &end);
        return end != raw && parsed > 0.0 ? parsed : 2.0;
    }();
    return value;
}

// The budget applies only when the completion queue is the last of the fill:
// no workers in flight, at most the 192 chunks of the final wave, and no
// meaningful light or dirty backlog ahead of the installs. A larger queue is a
// streaming burst, not this tail, and keeps the fixed budget.
inline double effective_completion_budget_ms(std::size_t generating, std::size_t completed,
                                             std::size_t lighting, std::size_t dirty,
                                             double budget_ms) {
    if (generating != 0 || completed > 192 || lighting > 1 || dirty > 1) {
        return budget_ms;
    }
    const double tail = final_install_budget_ms();
    return tail > budget_ms ? tail : budget_ms;
}

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_WORLD_INSTALL_BUDGET_HPP
