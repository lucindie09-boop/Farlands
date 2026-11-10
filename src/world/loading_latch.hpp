#ifndef FARLANDS_LOADING_LATCH_HPP
#define FARLANDS_LOADING_LATCH_HPP

namespace VoxelEngine {

// The initial-loading mesh-budget latch, lifted out of WorldUpdater so every
// transition can be pinned without a chunk map, a mesh manager or a frame.
//
// The mode starts only when the chunk load is quiet (the caller's
// `chunk_load_done`) and mesh work is outstanding. It ends on the first of:
//
//   * one quiet window without pending mesh -- the backlog drained; or
//   * the cap, which then LATCHES. The entry test cannot restart it while the
//     backlog it was capping is still pending. Releasing on the chunk load
//     leaving its quiet state is not enough: a fast flight leaves and re-enters
//     that state every few frames, so the next quiet frame restarted the window
//     and the cap never bound (measured: every 36 s flight still ended at the
//     loading budget). The cap releases once mesh work has been quiet for a
//     whole window, so a later quiet-load tail can boost again.
//
// The caller owns the two windows: they are env-overridable for measurement,
// and the shipping defaults are a 30 s cap and a 1 s quiet exit.
class LoadingLatch {
public:
    LoadingLatch(double cap_sec = 30.0, double quiet_sec = 1.0)
        : cap_sec_(cap_sec), quiet_sec_(quiet_sec) {}

    // One update, in the order WorldUpdater::process_mesh_budgets runs it.
    // Returns the state AFTER the transition, so the frame that ends an episode
    // (quiet exit or cap) reports false.
    [[nodiscard]] bool tick(double delta, bool chunk_load_done, bool pending_mesh) {
        // Only a tick that STARTED capped may accumulate the release window:
        // the frame the cap fires has already added this delta to `quiet_` in
        // the active block, and adding it twice would shorten the drain window.
        const bool was_capped = capped_;
        if (!active_ && !capped_ && chunk_load_done && pending_mesh) {
            active_ = true;
            elapsed_ = 0.0;
            quiet_ = 0.0;
        }
        if (active_) {
            elapsed_ += delta;
            quiet_ = pending_mesh ? 0.0 : quiet_ + delta;
            // The quiet exit wins when both land on the same frame: the backlog
            // has drained, so this is a completed episode and nothing needs
            // capping.
            if (quiet_ >= quiet_sec_) {
                active_ = false;
            } else if (elapsed_ >= cap_sec_) {
                active_ = false;
                capped_ = true;
            }
        }
        if (was_capped) {
            quiet_ = pending_mesh ? 0.0 : quiet_ + delta;
            if (quiet_ >= quiet_sec_) {
                capped_ = false;
                quiet_ = 0.0;
            }
        }
        return active_;
    }

    [[nodiscard]] bool active() const { return active_; }
    [[nodiscard]] bool capped() const { return capped_; }

    void reset() {
        active_ = false;
        capped_ = false;
        elapsed_ = 0.0;
        quiet_ = 0.0;
    }

private:
    double cap_sec_;
    double quiet_sec_;
    bool active_ = false;
    bool capped_ = false;
    double elapsed_ = 0.0;
    double quiet_ = 0.0;
};

} // namespace VoxelEngine

#endif // FARLANDS_LOADING_LATCH_HPP
