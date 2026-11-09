#include "world/install_profile.hpp"

#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace VoxelEngine {

namespace {
constexpr const char* kPhaseNames[InstallProfile::PhaseCount] = {
    "poll", "prepare", "data", "place", "insert", "listen", "tail", "stage_light", "stage_dirty", "stage_results"
};
} // namespace

bool InstallProfile::enabled() {
#ifdef DEBUG_ENABLED
    static const bool on = std::getenv("FARLANDS_INSTALL_PROFILE") != nullptr;
    return on;
#else
    return false;
#endif
}

void InstallProfile::reset() {
    for (Bucket& bucket : buckets_) bucket = Bucket{};
    last_ = {};
}

void InstallProfile::begin() {
    if (!enabled()) return;
    last_ = std::chrono::steady_clock::now();
}

void InstallProfile::lap(Phase phase, bool empty) {
    if (!enabled()) return;
    const auto now = std::chrono::steady_clock::now();
    Bucket& bucket = buckets_[empty ? 0 : 1];
    bucket.ns[phase] += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_).count());
    if (phase == Poll) ++bucket.count;
    last_ = now;
}

void InstallProfile::lap_elapsed(Phase phase, bool empty, std::chrono::steady_clock::time_point start) {
    if (!enabled()) return;
    const auto now = std::chrono::steady_clock::now();
    Bucket& bucket = buckets_[empty ? 0 : 1];
    bucket.ns[phase] += static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - start).count());
}

std::string InstallProfile::report() const {
    if (!enabled()) return "disabled";
    std::ostringstream out;
    out << std::fixed << std::setprecision(2);
    out << "install profile: empty=" << buckets_[0].count << " real=" << buckets_[1].count;
    for (int b = 0; b < 2; ++b) {
        const Bucket& bucket = buckets_[b];
        out << " | " << (b == 0 ? "empty" : "real") << " us:";
        uint64_t total = 0;
        for (int p = 0; p < PhaseCount; ++p) {
            const double us = bucket.count == 0 ? 0.0
                : static_cast<double>(bucket.ns[p]) / static_cast<double>(bucket.count) / 1000.0;
            total += bucket.ns[p];
            out << ' ' << kPhaseNames[p] << '=' << us;
        }
        const double total_us = bucket.count == 0 ? 0.0
            : static_cast<double>(total) / static_cast<double>(bucket.count) / 1000.0;
        out << " total=" << total_us;
    }
    return out.str();
}

} // namespace VoxelEngine
