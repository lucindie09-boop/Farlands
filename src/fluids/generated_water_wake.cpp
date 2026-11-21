#include "fluids/generated_water_wake.hpp"

#include <atomic>
#include <cstdlib>

namespace VoxelEngine {
namespace fluids {

namespace {

// -1 means "not set": the environment variable or the default below decides. The
// knob exists for the probe that measures the wake at 0, 1 and 2 in one process;
// a game never sets it.
std::atomic<int> g_radius_override{ -1 };

// Latched on first use rather than read per wake: wakes are emitted from mesh
// workers, and the environment cannot change under a running process anyway.
int radius_from_environment() {
    static const int radius = [] {
        // NOLINTNEXTLINE(concurrency-mt-unsafe) - latched once, env is fixed at startup
        const char* raw = std::getenv("FARLANDS_WATER_WAKE_RADIUS");
        if (raw == nullptr) {
            return 2;
        }
        const int value = std::atoi(raw);
        return value < 0 ? 0 : value;
    }();
    return radius;
}

} // namespace

void set_water_wake_radius_override(int radius) {
    g_radius_override.store(radius, std::memory_order_relaxed);
}

int water_wake_radius() {
    const int override_value = g_radius_override.load(std::memory_order_relaxed);
    if (override_value >= 0) {
        return override_value;
    }
    return radius_from_environment();
}

} // namespace fluids
} // namespace VoxelEngine
