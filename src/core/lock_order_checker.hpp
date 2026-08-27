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

// Per-thread state: which shards are currently held and the highest index.
inline thread_local std::bitset<64> held_{};
inline thread_local size_t max_held_{0};

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

inline void reset_all() {
    held_.reset();
    max_held_ = 0;
}

} // namespace lock_order
} // namespace VoxelEngine

#define LOCK_ORDER_ACQUIRE(idx) VoxelEngine::lock_order::acquire(idx)
#define LOCK_ORDER_RELEASE(idx) VoxelEngine::lock_order::release(idx)
#define LOCK_ORDER_RESET()      VoxelEngine::lock_order::reset_all()

#else // !DEBUG_ENABLED

#define LOCK_ORDER_ACQUIRE(idx) ((void)0)
#define LOCK_ORDER_RELEASE(idx) ((void)0)
#define LOCK_ORDER_RESET()      ((void)0)

#endif // DEBUG_ENABLED

#endif // FARLANDS_LOCK_ORDER_CHECKER_HPP
