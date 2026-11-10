#ifndef FARLANDS_LOADING_LATCH_HPP
#define FARLANDS_LOADING_LATCH_HPP

namespace VoxelEngine {

// The initial-loading mesh-budget latch, lifted out of WorldUpdater so every
// transition can be pinned without a chunk map, a mesh manager or a frame.
//
// The mode starts only when the fill is settled (the caller's `fill_settled`)
// and mesh work is outstanding. It ends on the first of:
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
// The hold is not a mode: it exists only until the FIRST chunk-load settle, or
// until its deadline, whichever comes first. `settled` is recomputed every
// frame, so keying the hold directly to it would re-arm during a later flight
// and pause edits/light remeshes with the chunk load. The deadline matters on
// small render distances: a chunk-count gate can never be met by a disc smaller
// than the gate, and without a deadline that world would never mesh. It also
// bounds the blank world on constrained hardware, at the price of the hold's
// end-to-end batching. This keeps the one-shot decision testable on its own.
class InitialMeshHold {
public:
    explicit InitialMeshHold(double max_sec = 2.0) : max_sec_(max_sec) {}

    // Call once per frame before the mesh stages. The first settle disarms the
    // hold for the rest of this world; so does the deadline. Returns the state
    // AFTER the transition, so the frame that releases the hold reports false
    // and its mesh work runs that frame.
    [[nodiscard]] bool tick(double delta, bool enabled, bool settled) {
        if (settled) {
            armed_ = false;
            elapsed_ = 0.0;
            return false;
        }
        if (!enabled || !armed_) return false;
        elapsed_ += delta > 0.0 ? delta : 0.0;
        if (elapsed_ >= max_sec_) {
            armed_ = false;
            elapsed_ = 0.0;
            return false;
        }
        return true;
    }

    void set_max_sec(double max_sec) { max_sec_ = max_sec; }
    void reset() { armed_ = true; elapsed_ = 0.0; }

private:
    double max_sec_ = 2.0;
    bool armed_ = true;
    double elapsed_ = 0.0;
};

class LoadingLatch {
public:
    LoadingLatch(double cap_sec = 30.0, double quiet_sec = 1.0)
        : cap_sec_(cap_sec), quiet_sec_(quiet_sec) {}

    // One update, in the order WorldUpdater::process_mesh_budgets runs it.
    // Returns the state AFTER the transition, so the frame that ends an episode
    // (quiet exit or cap) reports false.
    [[nodiscard]] bool tick(double delta, bool fill_settled, bool pending_mesh) {
        // Only a tick that STARTED capped may accumulate the release window:
        // the frame the cap fires has already added this delta to `quiet_` in
        // the active block, and adding it twice would shorten the drain window.
        const bool was_capped = capped_;
        if (!active_ && !capped_ && fill_settled && pending_mesh) {
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
