#ifndef FARLANDS_INSTALL_PROFILE_HPP
#define FARLANDS_INSTALL_PROFILE_HPP

#include <chrono>
#include <cstdint>
#include <string>

namespace VoxelEngine {

// Main-thread install breakdown, collected only when FARLANDS_INSTALL_PROFILE=1.
// Two buckets (all-air vs has blocks): the band's pad empties are over half the
// fill's installs and need not cost the same. lap() advances its own clock, so
// the phases must be taken in order around one install.
class InstallProfile {
public:
    enum Phase : int { Poll, Prepare, Data, Placements, Insert, Listener, Tail, StageLight, StageDirty, StageResults, PhaseCount };

    static bool enabled();
    void reset();
    void begin();
    void lap(Phase phase, bool empty);
    // Adds a stage branch's whole duration; unlike lap() it does not move the
    // install clock, so stage and install phases can interleave in one drain.
    void lap_elapsed(Phase phase, bool empty, std::chrono::steady_clock::time_point start);
    [[nodiscard]] std::string report() const;

private:
    struct Bucket {
        uint64_t ns[PhaseCount] = {};
        uint64_t count = 0;
    };
    Bucket buckets_[2];
    std::chrono::steady_clock::time_point last_{};
};

} // namespace VoxelEngine

#endif // FARLANDS_INSTALL_PROFILE_HPP
