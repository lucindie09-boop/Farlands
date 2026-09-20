#ifndef FARLANDS_LOCK_ORDER_CHECKER_HPP
#define FARLANDS_LOCK_ORDER_CHECKER_HPP

// Debug-only shard lock-order checker.
//
// Tracks which shard indices are held by each thread and asserts that new
// acquisitions always have a higher shard index than all currently held
// shards.  This catches lock-order inversions at the point of acquisition
// rather than manifesting as a hard-to-diagnose hang.
//
// Gated on DEBUG_ENABLED: zero overhead in release/template_release builds.

#ifdef DEBUG_ENABLED

#include <bitset>
#include <cstdio>
#include <cstdlib>

namespace VoxelEngine {
namespace lock_order {

// Per-thread state: which shards are currently held and the highest index, plus
// which of them are held EXCLUSIVELY.
inline thread_local std::bitset<64> held_{};
inline thread_local size_t max_held_{0};
// A separate set from held_ on purpose. held_ answers "may I acquire in this order",
// which a read does not participate in; this one answers "is this read even possible",
// because std::shared_mutex is NOT recursive. A locking accessor called while the
// same thread holds that shard exclusively cannot proceed — it blocks with no message,
// no crash report and no log line, and the only symptom is that the game stops
// responding. That is the worst shape a bug can have, and it is not an ordering
// problem, so the check above cannot see it.
inline thread_local std::bitset<64> held_exclusive_{};

// Register a shard acquisition.  Aborts if the new shard index is lower
// than any currently held shard, which is the classic deadlock recipe.
inline void acquire(size_t idx) {
    if (idx >= 64) return;
    if (held_.test(idx)) return; // already held (e.g. lock_all after lock_chunk)
    if (idx < max_held_) {
        std::fprintf(stderr,
            "LOCK ORDER VIOLATION: acquiring shard %zu while holding shard %zu\n",
            idx, max_held_);
        std::fprintf(stderr, "  Currently held shards:");
        for (size_t i = 0; i < 64; ++i)
            if (held_.test(i)) std::fprintf(stderr, " %zu", i);
        std::fprintf(stderr, "\n");
        std::fprintf(stderr, "  Fix: ensure lock acquisitions use ascending shard index order.\n");
        std::fprintf(stderr, "  Hint: lock_keys() / lock_keys_exclusive() auto-sort by shard.\n");
        std::fflush(stderr);
        std::abort();
    }
    held_.set(idx);
    if (idx > max_held_) max_held_ = idx;
}

// The exclusive forms. Everything that takes a unique lock goes through these, so
// the read-side check below knows which shards are off limits to this thread.
inline void acquire_exclusive(size_t idx) {
    acquire(idx);
    if (idx < 64) held_exclusive_.set(idx);
}

// Called by a locking READ before it takes its shared lock: after the lock is taken
// it is far too late to say anything, because it never returns.
inline void require_shared_ok(size_t idx, const char* what) {
    if (idx >= 64 || !held_exclusive_.test(idx)) return;
    std::fprintf(stderr,
        "SHARD RE-ENTRY: %s wants shard %zu SHARED while this thread holds it EXCLUSIVELY.\n",
        what, idx);
    std::fprintf(stderr, "  std::shared_mutex is not recursive, so this would hang, not fail.\n");
    std::fprintf(stderr, "  Fix: use the _fast accessor for data under a shard you already hold\n");
    std::fprintf(stderr, "       (get_block_world_fast, get_chunk_data_fast, ...), or take the read\n");
    std::fprintf(stderr, "       outside the lock.\n");
    std::fprintf(stderr, "  Currently held exclusively:");
    for (size_t i = 0; i < 64; ++i)
        if (held_exclusive_.test(i)) std::fprintf(stderr, " %zu", i);
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::abort();
}

// Unregister a shard release.  Recomputes max_held_ when the top shard
// is released so future acquisitions are validated correctly.
inline void release(size_t idx) {
    if (idx >= 64) return;
    if (!held_.test(idx)) return;
    held_.reset(idx);
    if (idx == max_held_) {
        max_held_ = 0;
        for (size_t i = 64; i > 0; --i) {
            if (held_.test(i - 1)) { max_held_ = i - 1; break; }
        }
    }
}

// Defined after release() because it calls it.
inline void release_exclusive(size_t idx) {
    if (idx < 64) held_exclusive_.reset(idx);
    release(idx);
}

inline void reset_all() {
    held_.reset();
    held_exclusive_.reset();
    max_held_ = 0;
}

} // namespace lock_order
} // namespace VoxelEngine

#define LOCK_ORDER_ACQUIRE(idx) VoxelEngine::lock_order::acquire(idx)
#define LOCK_ORDER_RELEASE(idx) VoxelEngine::lock_order::release(idx)
#define LOCK_ORDER_ACQUIRE_EX(idx) VoxelEngine::lock_order::acquire_exclusive(idx)
#define LOCK_ORDER_RELEASE_EX(idx) VoxelEngine::lock_order::release_exclusive(idx)
#define LOCK_ORDER_REQUIRE_SHARED(idx, what) VoxelEngine::lock_order::require_shared_ok(idx, what)
#define LOCK_ORDER_RESET()      VoxelEngine::lock_order::reset_all()

#else // !DEBUG_ENABLED

#define LOCK_ORDER_ACQUIRE(idx) ((void)0)
#define LOCK_ORDER_RELEASE(idx) ((void)0)
#define LOCK_ORDER_ACQUIRE_EX(idx) ((void)0)
#define LOCK_ORDER_RELEASE_EX(idx) ((void)0)
#define LOCK_ORDER_REQUIRE_SHARED(idx, what) ((void)0)
#define LOCK_ORDER_RESET()      ((void)0)

#endif // DEBUG_ENABLED

#endif // FARLANDS_LOCK_ORDER_CHECKER_HPP
