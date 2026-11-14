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

} // namespace

void set_water_wake_radius_override(int radius) {
    g_radius_override.store(radius, std::memory_order_relaxed);
}

int water_wake_radius() {
    const int override_value = g_radius_override.load(std::memory_order_relaxed);
    if (override_value >= 0) {
        return override_value;
    }
    const char* raw = std::getenv("FARLANDS_WATER_WAKE_RADIUS");
    if (raw == nullptr) {
        return 2;
    }
    const int value = std::atoi(raw);
    return value < 0 ? 0 : value;
}

} // namespace fluids
} // namespace VoxelEngine
