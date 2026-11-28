#ifndef FARLANDS_NAV_FIELD_SERVICE_HPP
#define FARLANDS_NAV_FIELD_SERVICE_HPP

// -----------------------------------------------------------------------------
// NavFieldService — one crowd field, built on a worker, answered to whoever asks.
//
// The service owns the decision of WHEN a field is worth building, because only
// it sees the asking: a caller that arrives first builds, and the callers behind
// it read what is there. A field is rebuilt on a clock, when the goal has moved
// more than a couple of blocks, or when a caller arrives and there is none.
//
// The threshold is what keeps this a crowd-only path. A single goblin strolling
// or chasing alone never reaches `kMinWants` asks in the window, so no field is
// built and its own A* answers exactly as it always did. A dozen chasers on one
// player do, and then one build answers all of them for a fifth of a second.
//
// Reads copy the shared pointer and run without the lock: a build finishing
// mid-read then swaps a whole new field rather than mutating one in place, so a
// route being walked can never be cut out from under its reader.
// -----------------------------------------------------------------------------

#include "core/chunk_map.hpp"
#include "core/thread_pool.hpp"
#include "pathfinding/nav_field.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

namespace VoxelEngine {
namespace nav {

class NavFieldService {
public:
    // Chasers asking inside one second before a field is worth building. Below this the per-agent
    // search is the cheaper answer, and this is the number that decides it.
    static constexpr int32_t kMinWants = 12;
    // A field is at most this old before it is rebuilt for a caller that is still asking.
    static constexpr double kRebuildMs = 400.0;
    // ...and it is rebuilt at once when the goal has moved more than this many blocks on any axis.
    static constexpr int32_t kGoalMoveBlocks = 2;
    // The search's reach around the goal: 24 columns out, 32 down and 16 up. A follower outside it
    // is not in the field and falls back to A*.
    static constexpr int32_t kHalfColumns = 24;
    static constexpr int32_t kBelow = 32;
    static constexpr int32_t kAbove = 16;

    explicit NavFieldService(const ChunkMap& map);
    ~NavFieldService();

    NavFieldService(const NavFieldService&) = delete;
    NavFieldService& operator=(const NavFieldService&) = delete;

    // The support cells from `from` toward `goal`, or empty. Empty means "ask your own search":
    // either no field is ready yet, or this cell is not in it.
    std::vector<NavNode> route(ThreadPool& pool, const NavNode& from, const NavNode& goal,
                               int32_t count);

    struct Stats {
        size_t builds = 0;
        size_t wants = 0;      // asks seen in the current one-second window
        size_t max_wants = 0;
        size_t nodes = 0;      // size of the field now held
        bool ready = false;
        bool building = false;
        double age_ms = 0.0;   // since the held field was finished
    };

    [[nodiscard]] Stats get_stats() const;

private:
    struct State {
        mutable std::mutex mutex;
        std::shared_ptr<const NavField> field;
        NavNode goal{};
        bool has_goal = false;
        bool building = false;
        double built_ms = 0.0;
        double window_start_ms = 0.0;
        size_t wants = 0;
        size_t max_wants = 0;
        size_t builds = 0;
    };

    void start_build(ThreadPool& pool, const NavNode& goal);

    // Borrowed, and outlives the service: it is a value member of the controller. The pool is
    // taken per call for the same reason PathService does — a runtime reset destroys it.
    const ChunkMap& map_;
    std::shared_ptr<State> state_;
};

} // namespace nav
} // namespace VoxelEngine

#endif // FARLANDS_NAV_FIELD_SERVICE_HPP
