// The measurement-only budget overrides WorldUpdater used to apply inside its
// constructor. Moved here when world_updater.cpp reached the 500-line cap, and
// extended with the main-thread install clock: at six workers an install costs
// 36 us while the fill's finished-to-installed latency averages 61 ms, so the
// frame budget that walks the drain -- not the per-install work -- is what
// paces the fill there.

#include "world/world_updater_internal.hpp"

namespace VoxelEngine {

double apply_measurement_overrides(FrameBudgets& budgets) {
    // Measurement-only: one ceiling for the three post-load clocks. The shipping
    // value is 1000 ms (effectively open); the tail-clock arm sets 8/12/16.
    const double loading_tail_budget_ms =
        latch_seconds_from_env("FARLANDS_LOADING_TAIL_BUDGET_MS", 1000.0);
    budgets.mesh_queue_loading_budget_ms = loading_tail_budget_ms;
    budgets.mesh_completion_budget_loading_ms = loading_tail_budget_ms;
    budgets.far_region_visibility_loading_budget_ms = loading_tail_budget_ms;
    // The shipping path leaves meshing to the post-load latch, so the fill's
    // first half is chunk admission and its second is the held mesh drain.
    // `FARLANDS_HOLD_MESH_UNTIL_LOADED=0` restores the overlap arm for an A/B.
    const char* hold_mesh_env = std::getenv("FARLANDS_HOLD_MESH_UNTIL_LOADED");
    if (hold_mesh_env != nullptr) {
        if (hold_mesh_env[0] == '1' && hold_mesh_env[1] == '\0') {
            budgets.hold_mesh_until_load_done = true;
            budgets.hold_mesh_force = true;
        } else if (hold_mesh_env[0] == '0' && hold_mesh_env[1] == '\0') {
            budgets.hold_mesh_until_load_done = false;
        }
    }
    // The hold is one-shot but not unbounded: `FARLANDS_HOLD_MESH_MAX_SEC` is
    // the blank-world ceiling. It also covers a disc whose fill settles below
    // the old 500-chunk gate, which would otherwise never release the hold.
    const double hold_max_sec = latch_seconds_from_env("FARLANDS_HOLD_MESH_MAX_SEC", 2.0);
    budgets.hold_mesh_max_sec = hold_max_sec;
    budgets.hold_mesh_min_workers = static_cast<int32_t>(
        latch_seconds_from_env("FARLANDS_HOLD_MESH_MIN_WORKERS", 8.0));
    // The partial-hold arm: a small nearest-only mesh budget that runs while the
    // hold is still up. The A/B's first-visible result is in FrameBudgets; the
    // shipping default is two, and zero restores the all-or-nothing hold.
    budgets.hold_mesh_partial = static_cast<int32_t>(
        latch_seconds_from_env("FARLANDS_HOLD_MESH_PARTIAL", 2.0, true));
    // Measurement-only: the admission controller's per-chunk cost model and
    // its in-flight cap. Lower the cost and raise the cap together to ask what
    // the feed-forward rate is worth before changing the rule.
    budgets.generation_work_per_chunk_ms = latch_seconds_from_env("FARLANDS_GEN_WORK_PER_CHUNK_MS", 0.0);
    budgets.generation_chain_budget_ms = latch_seconds_from_env("FARLANDS_CHAIN_BUDGET_MS", 1.5, true);
    const int32_t inflight_override = static_cast<int32_t>(
        latch_seconds_from_env("FARLANDS_GEN_MAX_INFLIGHT", 0.0));
    budgets.generation_max_in_flight = inflight_override > 0 ? inflight_override : 0;
    // The held drain uploads at most this many completed meshes per frame while
    // the initial-loading latch is up. 512 finishes the held queue inside its
    // quiet window; `FARLANDS_MESH_UPLOADS_LOADING=N` overrides it for an A/B.
    const char* uploads_env = std::getenv("FARLANDS_MESH_UPLOADS_LOADING");
    if (uploads_env != nullptr) {
        const int32_t requested = std::atoi(uploads_env);
        if (requested > 0) {
            budgets.mesh_uploads_loading = requested;
        }
    }
    // The main-thread install clock. The drain's stage profile puts a real
    // install at ~36 us, so this budget (0.75 ms, which admits about twenty)
    // is the stop that matters; `FARLANDS_PROCESSING_BUDGET_MS` is the arm.
    budgets.processing_budget_ms =
        latch_seconds_from_env("FARLANDS_PROCESSING_BUDGET_MS", budgets.processing_budget_ms);
    // The loading-only clock: the 3.0 ms the six-worker A/B liked, separated so
    // the fill can have it without spending gameplay frames.
    budgets.processing_budget_loading_ms = latch_seconds_from_env(
        "FARLANDS_PROCESSING_BUDGET_LOADING_MS", budgets.processing_budget_loading_ms);
    return hold_max_sec;
}

} // namespace VoxelEngine
