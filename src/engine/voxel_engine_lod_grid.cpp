// The seed-grid far mode's settings surface (docs/lod-modes.md), kept apart from
// engine/voxel_engine_controller.cpp the way the world config and the paste are:
// the controller owns the grid, this file is what a setting does to it.
//
// The one rule every setter here follows: hand the grid its WHOLE configuration
// before changing what it is asked to do. Sampling happens on worker threads
// holding copies, so a half-applied change would not be drawn wrong once — it
// would be sampled into a tile and kept.

#include "engine/voxel_engine_controller.hpp"

#include "core/chunk_coords.hpp"

namespace VoxelEngine {
using namespace godot;

void VoxelEngineController::set_lod_grid_enabled(bool enabled) {
    // The material is fetched every time rather than cached: it is the far mode's
    // only render state and it is valid to ask for it whether or not the mode is on.
    lod_grid.set_material(environment_controller.get_material_manager().get_lod_grid_material());
    // The far mode owns the ring beyond the loaded world and nothing inside it.
    lod_grid.set_inner_radius_blocks(render_distance * CHUNK_WIDTH);
    lod_grid.set_config(seed, world_updater.get_terrain_params(), lod_grid_biomes);
    lod_grid.set_epoch(chunk_world.get_epoch());
    lod_grid.set_enabled(enabled);
    // The world wears the far mode's fog range while the mode is on (see
    // EnvironmentController::set_lod_grid_fog_active): otherwise the loaded chunks
    // fade out at their border and the field that continues them does not.
    environment_controller.set_lod_grid_fog_active(enabled);
}

bool VoxelEngineController::get_lod_grid_enabled() const { return lod_grid.is_enabled(); }

void VoxelEngineController::set_lod_grid_spacing(int32_t blocks) {
    lod_grid.set_base_spacing(blocks);
}

int32_t VoxelEngineController::get_lod_grid_spacing() const { return lod_grid.get_base_spacing(); }

void VoxelEngineController::set_lod_grid_rings(int32_t rings) {
    lod_grid.set_rings_per_level(rings);
}

int32_t VoxelEngineController::get_lod_grid_rings() const {
    return lod_grid.get_rings_per_level();
}

void VoxelEngineController::set_lod_grid_outer_rings(int32_t rings) {
    lod_grid.set_outer_rings(rings);
}

int32_t VoxelEngineController::get_lod_grid_outer_rings() const {
    return lod_grid.get_outer_rings();
}

godot::Dictionary VoxelEngineController::get_lod_grid_stats() const {
    // Gather is what refreshes the live counts, so the caller asking for the
    // numbers is the right moment to count them.
    const LodGrid::Stats stats = const_cast<LodGrid&>(lod_grid).gather_stats();
    Dictionary out;
    out["enabled"] = stats.enabled;
    out["tiles"] = stats.tiles_live;
    out["built"] = stats.tiles_built;
    out["dropped"] = stats.tiles_dropped;
    out["failed"] = stats.tiles_failed;
    out["uploads"] = stats.uploads;
    out["quads"] = stats.quads;
    // The sampler's cost model, in the one unit that scales it: a column.
    out["columns_sampled"] = stats.columns_sampled;
    out["spacing"] = stats.spacing_blocks;
    // What the outer level samples at, and where the horizon is: the two numbers a
    // caller checks to see that the reach is a DISTANCE (the same at every spacing)
    // rather than a quality the spacing knob scales.
    out["outer_spacing"] = stats.outer_spacing_blocks;
    out["outer_radius"] = stats.outer_radius_blocks;
    // The seam with the loaded world, as two numbers a caller can check: tiles
    // that straddle the world's edge (the corners of the tile lattice its disc
    // cannot reach), and the radius those tiles are clipped against in the
    // fragment stage.
    out["straddling_tiles"] = stats.straddling_tiles;
    out["clip_radius"] = stats.clip_radius_blocks;
    // What the far field costs to DRAW: one call per spacing level with geometry,
    // not one per tile. Tiles and vertices are reported beside it because a count
    // of either says nothing about the number that matters in this engine.
    out["draw_calls"] = stats.draw_calls;
    out["vertices"] = stats.vertices;
    out["last_build_ms"] = stats.last_build_ms;
    out["last_schedule_ms"] = stats.last_schedule_ms;
    return out;
}

} // namespace VoxelEngine
