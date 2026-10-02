// The schematic and reporting surface: biome lookup, the inspect/preview/paste/undo
// calls and the paste completion handshake, then the fluid and generation counters
// /genstats reads. Kept apart from godot_bindings/chunk_manager.cpp, whose lifecycle
// does not touch any of it.

#include "godot_bindings/chunk_manager.hpp"

#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;
using namespace VoxelEngine;

Dictionary ChunkManager::find_biome(const String& biome_name, int32_t center_x,
                                    int32_t center_z, int32_t max_radius) {
    return controller->find_biome(biome_name, center_x, center_z, max_radius);
}

Dictionary ChunkManager::inspect_schematic(const PackedByteArray& bytes, const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->inspect_schematic(bytes, options);
}

Dictionary ChunkManager::preview_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                           int32_t origin_y, int32_t origin_z,
                                           const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->preview_schematic(bytes, origin_x, origin_y, origin_z, options);
}

Dictionary ChunkManager::paste_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                         int32_t origin_y, int32_t origin_z,
                                         const Dictionary& options) {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->paste_schematic_bytes(bytes, origin_x, origin_y, origin_z, options);
}

Dictionary ChunkManager::get_pending_paste() {
    if (!controller) return Dictionary();
    return controller->get_pending_paste();
}

Dictionary ChunkManager::take_paste_completion() {
    if (!controller) return Dictionary();
    return controller->take_paste_completion();
}

Dictionary ChunkManager::undo_paste() {
    if (!controller) {
        Dictionary out;
        out["ok"] = false;
        out["error"] = String("the engine is not running");
        return out;
    }
    return controller->undo_paste();
}

int64_t ChunkManager::paste_undo_cells() {
    if (!controller) return 0;
    return controller->paste_undo_cells();
}

Dictionary ChunkManager::get_fluid_stats() {
    // A window onto the flow simulation's ticking: how much work it did on its
    // last tick and how much is still queued. A flood that looks stuck is either
    // pending > 0 (still working through its cells, one budget slice a tick) or
    // 0 with no cells ticked (settled, and waiting to be woken by an edit).
    const fluids::FluidSim& sim = controller->get_fluid_sim();
    const fluids::FluidSim::Stats& stats = sim.stats();
    Dictionary out;
    out["enabled"] = sim.enabled();
    out["ticks"] = static_cast<int64_t>(stats.ticks);
    out["cells_ticked"] = static_cast<int64_t>(stats.cells_ticked);
    out["last_tick_cells"] = stats.last_tick_cells;
    out["last_tick_writes"] = stats.last_tick_writes;
    out["last_tick_ms"] = stats.last_tick_ms;
    out["pending"] = static_cast<int64_t>(stats.pending);
    out["frozen"] = stats.frozen;
    out["settled"] = stats.settled;
    return out;
}

Dictionary ChunkManager::get_generation_stats() {
    // The sweep's own accounting. `checks` and the reject counters describe the
    // whole history of the session, which is dominated by the initial load; the
    // rolling window is the part that answers "what does this cost while I am
    // flying right now". Both are here so the two are never confused.
    const VoxelEngine::WorldUpdater::GenerationStats& stats =
        controller->get_world_updater().get_generation_stats();
    Dictionary out;
    out["frames"] = static_cast<int64_t>(stats.frames);
    out["candidate_offsets"] = static_cast<int64_t>(stats.candidate_offsets);
    out["candidate_columns"] = static_cast<int64_t>(stats.candidate_columns);
    out["cursor_resets"] = static_cast<int64_t>(stats.cursor_resets);

    out["checks"] = static_cast<int64_t>(stats.checks);
    out["band_pass"] = static_cast<int64_t>(stats.band_pass);
    out["generations"] = static_cast<int64_t>(stats.generations);
    out["generate_refused"] = static_cast<int64_t>(stats.generate_refused);
    out["reject_loaded"] = static_cast<int64_t>(stats.reject_loaded);
    out["reject_inflight"] = static_cast<int64_t>(stats.reject_inflight);
    out["reject_above"] = static_cast<int64_t>(stats.reject_above);
    out["reject_below"] = static_cast<int64_t>(stats.reject_below);
    out["reject_oob"] = static_cast<int64_t>(stats.reject_oob);
    out["sweeps_completed"] = static_cast<int64_t>(stats.sweeps_completed);
    out["columns_built"] = static_cast<int64_t>(stats.columns_built);
    out["columns_skipped"] = static_cast<int64_t>(stats.columns_skipped);

    out["frustum_checks"] = static_cast<int64_t>(stats.frustum_checks);
    out["frustum_visible"] = static_cast<int64_t>(stats.frustum_visible);
    out["frustum_loaded"] = static_cast<int64_t>(stats.frustum_loaded);
    out["frustum_band_pass"] = static_cast<int64_t>(stats.frustum_band_pass);
    out["frustum_generations"] = static_cast<int64_t>(stats.frustum_generations);
    out["frustum_refused"] = static_cast<int64_t>(stats.frustum_refused);
    out["frustum_inflight"] = static_cast<int64_t>(stats.frustum_inflight);

    // The chain queue (the current chunk-selection priority).
    out["chain_offered"] = static_cast<int64_t>(stats.chain_offered);
    out["chain_generations"] = static_cast<int64_t>(stats.chain_generations);
    out["chain_refused"] = static_cast<int64_t>(stats.chain_refused);
    out["chain_skipped"] = static_cast<int64_t>(stats.chain_skipped);
    out["chain_seeds"] = static_cast<int64_t>(stats.chain_seeds);
    out["chain_vertical_offered"] = static_cast<int64_t>(stats.chain_vertical_offered);
    out["chain_vertical_generated"] = static_cast<int64_t>(stats.chain_vertical_generated);

    out["urgent_requested"] = static_cast<int64_t>(stats.urgent_requested);
    out["urgent_generated"] = static_cast<int64_t>(stats.urgent_generated);

    // Band SHAPE, as opposed to the candidate_offsets/candidate_columns mean:
    // per-bucket column counts over the 32 world slices, the tallest band seen,
    // and the fill columns kept out of the histogram. Read together with the
    // install classification below: a fat 24-31 bucket plus pad-only installs
    // that came back empty is the ±32 pad buying nothing.
    PackedInt32Array band_hist;
    band_hist.resize(static_cast<int64_t>(GenerationStats::kBandBuckets));
    for (size_t i = 0; i < GenerationStats::kBandBuckets; ++i) {
        band_hist.set(static_cast<int64_t>(i), static_cast<int32_t>(stats.band_size_hist[i]));
    }
    out["band_size_hist"] = band_hist;
    out["band_max_slices"] = static_cast<int64_t>(stats.band_max_slices);
    out["fill_columns_read"] = static_cast<int64_t>(stats.fill_columns_read);
    out["fill_band_slices"] = static_cast<int64_t>(stats.fill_band_slices);

    // What the generations produced, classified by where the chunk sat relative
    // to its column's estimated bounds. `*_empty` are the ones the pad (or the
    // estimate's own slack) paid for and got nothing: above_top/below_land can
    // ONLY be offered because of the ±32 pad or the fill rule, while in_range
    // is terrain the estimate itself claimed. bounds_unknown is not split — the
    // column had no cached bounds to judge against.
    out["installs_total"] = static_cast<int64_t>(stats.installs_total);
    out["installs_empty"] = static_cast<int64_t>(stats.installs_empty);
    out["installs_above_top"] = static_cast<int64_t>(stats.installs_above_top);
    out["installs_above_top_empty"] = static_cast<int64_t>(stats.installs_above_top_empty);
    out["installs_below_land"] = static_cast<int64_t>(stats.installs_below_land);
    out["installs_below_land_empty"] = static_cast<int64_t>(stats.installs_below_land_empty);
    out["installs_in_range"] = static_cast<int64_t>(stats.installs_in_range);
    out["installs_in_range_empty"] = static_cast<int64_t>(stats.installs_in_range_empty);
    out["installs_bounds_unknown"] = static_cast<int64_t>(stats.installs_bounds_unknown);

    out["total_ms"] = stats.total_ms;
    out["last_ms"] = stats.last_ms;
    out["max_ms"] = stats.max_ms;
    out["avg_ms"] = stats.frames > 0 ? stats.total_ms / static_cast<double>(stats.frames) : 0.0;
    out["rebuilds"] = static_cast<int64_t>(stats.rebuilds);
    out["last_rebuild_ms"] = stats.last_rebuild_ms;
    out["total_rebuild_ms"] = stats.total_rebuild_ms;
    out["max_rebuild_ms"] = stats.max_rebuild_ms;
    out["band_reads"] = static_cast<int64_t>(stats.band_reads);
    out["total_band_ms"] = stats.total_band_ms;
    out["max_band_ms"] = stats.max_band_ms;
    out["max_band_columns"] = static_cast<int64_t>(stats.max_band_columns);
    out["max_band_column_ms"] = stats.max_band_column_ms;
    out["max_band_bounds_ms"] = stats.max_band_bounds_ms;
    out["max_band_resident_ms"] = stats.max_band_resident_ms;
    out["max_contains_ms"] = stats.max_contains_ms;
    out["max_cold_bounds_ms"] = stats.max_cold_bounds_ms;
    // The other half of a band read: how many were answered by a worker instead of
    // computed here, and how many columns this thread still had to derive itself.
    // The pair is what says whether the prefetch is running ahead of the frontier
    // or behind it — a lagging prefetch is slower, never wrong.
    out["cold_bounds"] = static_cast<int64_t>(stats.cold_bounds);
    out["prefetch_taken"] = static_cast<int64_t>(stats.prefetch_taken);
    const VoxelEngine::ColumnPrefetch::Stats pf =
        controller->get_world_updater().get_prefetch_stats();
    out["prefetch_requested"] = static_cast<int64_t>(pf.requested);
    out["prefetch_published"] = static_cast<int64_t>(pf.published);
    out["prefetch_stale"] = static_cast<int64_t>(pf.stale);
    out["prefetch_dropped"] = static_cast<int64_t>(pf.dropped);
    out["prefetch_outstanding"] = static_cast<int64_t>(pf.outstanding);

    // Window sums, computed here rather than in GDScript so the caller reads the
    // same numbers the counters hold (and so a mid-window reset cannot mix two
    // different frame sets).
    uint64_t window_checks = 0;
    uint64_t window_generations = 0;
    for (size_t i = 0; i < stats.window_frames; ++i) {
        window_checks += stats.window_checks[i];
        window_generations += stats.window_generations[i];
    }
    out["window_frames"] = static_cast<int64_t>(stats.window_frames);
    out["window_checks"] = static_cast<int64_t>(window_checks);
    out["window_generations"] = static_cast<int64_t>(window_generations);

    const int32_t render_distance = get_render_distance();
    out["render_distance"] = render_distance;
    return out;
}

void ChunkManager::reset_generation_stats() {
    controller->get_world_updater().reset_generation_stats();
}
