#pragma once

#include "core/frame_budgets.hpp"

#include <cstdlib>

namespace VoxelEngine {

// Measurement-only env override: parse a positive double (or zero when
// allow_zero). Invalid or missing values fall back rather than disabling the
// setting. Shared by WorldUpdater's constructor and the budget overrides it
// applies, so the parsing rule lives in one place.
inline double latch_seconds_from_env(const char* name, double fallback, bool allow_zero = false) {
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - startup only, before the pool starts
    const char* raw = std::getenv(name);
    if (raw == nullptr || *raw == '\0') return fallback;
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    return (end != raw && (value > 0.0 || (allow_zero && value == 0.0))) ? value : fallback;
}

// Applies the measurement-only budget overrides (post-load clocks, the mesh
// hold, the admission controller, the held mesh drain and the main-thread
// install clock) and returns the hold's blank-world ceiling, which the caller
// hands to its own latch. Defined in world_updater_budgets.cpp.
double apply_measurement_overrides(FrameBudgets& budgets);

} // namespace VoxelEngine
