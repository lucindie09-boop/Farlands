// The property surface: every method whose whole body is a delegation to the
// controller (or a value this binding caches itself). One line each is the point
// — the engine side of each name lives in engine/voxel_engine_controller.cpp, and
// this file exists so that reading the GDScript-facing API does not mean scrolling
// past the raycast, the schematic and the texture code.

#include "godot_bindings/chunk_manager.hpp"

#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;
using namespace VoxelEngine;

// -------------------------------------------------------------------------
// Property thin wrappers — every method below just delegates to controller
// -------------------------------------------------------------------------

void ChunkManager::set_seed(int32_t p_seed) { controller->set_seed(p_seed); }
int32_t ChunkManager::get_seed() const { return controller->get_seed(); }

godot::String ChunkManager::engine_build_stamp() const {
    // The compiler's own timestamps for this translation unit, so it cannot be
    // forgotten when the code changes.
    return godot::String("built " __DATE__ " " __TIME__);
}

void ChunkManager::set_render_distance(int32_t distance) { controller->set_render_distance(distance); }
int32_t ChunkManager::get_render_distance() const { return controller->get_render_distance(); }

void ChunkManager::set_player_position(const godot::Vector3& position) {
    controller->set_player_position(position);
    if (ready_for_auto_update && controller->get_auto_update()) {
        update_chunks();
    }
}
godot::Vector3 ChunkManager::get_player_position() const { return controller->get_player_position(); }

void ChunkManager::set_player_path(const godot::NodePath& path) { player_path = path; }
godot::NodePath ChunkManager::get_player_path() const { return player_path; }

void ChunkManager::set_auto_update(bool enabled) { controller->set_auto_update(enabled); }
bool ChunkManager::get_auto_update() const { return controller->get_auto_update(); }

void ChunkManager::update_chunks() { controller->update_chunks(Engine::get_singleton() && Engine::get_singleton()->is_editor_hint()); }

void ChunkManager::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    controller->generate_chunk(chunk_x, chunk_y, chunk_z);
}

void ChunkManager::unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    controller->unload_chunk(chunk_x, chunk_y, chunk_z);
}

void ChunkManager::set_sea_level(float level) { controller->set_sea_level(level); }
float ChunkManager::get_sea_level() const { return controller->get_sea_level(); }

void ChunkManager::set_biome_size(float size) { controller->set_biome_size(size); }
float ChunkManager::get_biome_size() const { return controller->get_biome_size(); }

void ChunkManager::set_squish_enabled(bool enabled) { controller->set_squish_enabled(enabled); }
bool ChunkManager::get_squish_enabled() const { return controller->get_squish_enabled(); }
void ChunkManager::set_squish_slice(int32_t slice) { controller->set_squish_slice(slice); }
int32_t ChunkManager::get_squish_slice() const { return controller->get_squish_slice(); }
void ChunkManager::set_squish_span(int32_t span) { controller->set_squish_span(span); }
int32_t ChunkManager::get_squish_span() const { return controller->get_squish_span(); }

String ChunkManager::get_performance_report() { return controller->get_performance_report(); }

Dictionary ChunkManager::get_chunk_storage_histogram() const {
    int64_t all_air = 0;
    int64_t fully_solid = 0;
    int64_t mixed = 0;
    int64_t uniform_sections = 0;
    int64_t block_bytes = 0;
    int64_t total_bytes = 0;
    controller->get_chunk_world().get_chunk_map().for_each(
        [&](uint64_t, const std::unique_ptr<ChunkRenderData>& render_data) {
            if (!render_data || !render_data->data) return;
            if (render_data->data->is_all_air()) ++all_air;
            else if (render_data->data->fully_solid()) ++fully_solid;
            else ++mixed;
            uniform_sections += render_data->data->uniform_block_sections();
            block_bytes += static_cast<int64_t>(render_data->data->block_storage_bytes());
            total_bytes += static_cast<int64_t>(render_data->data->memory_usage());
        });
    Dictionary out;
    out["all_air"] = all_air;
    out["fully_solid"] = fully_solid;
    out["mixed"] = mixed;
    out["uniform_sections"] = uniform_sections;
    out["block_bytes"] = block_bytes;
    out["total_bytes"] = total_bytes;
    return out;
}

String ChunkManager::get_light_profile_report() { return controller->get_light_profile_report(); }

String ChunkManager::get_generation_path_report() { return controller->get_generation_path_report(); }

String ChunkManager::get_install_profile_report() { return controller->get_install_profile_report(); }

String ChunkManager::get_slow_frame_report(int32_t frames, double slow_frame_ms) {
    return controller->get_slow_frame_report(frames, slow_frame_ms);
}

void ChunkManager::set_smooth_lighting(bool enabled) { controller->set_smooth_lighting(enabled); }
bool ChunkManager::get_smooth_lighting() const { return controller->get_smooth_lighting(); }

// The seed-grid far mode. Every one of these forwards; the grid's own state, its
// tiles and its worker tasks all live behind the controller (lod/lod_grid.cpp).
void ChunkManager::set_lod_grid_enabled(bool enabled) { controller->set_lod_grid_enabled(enabled); }
bool ChunkManager::get_lod_grid_enabled() const { return controller->get_lod_grid_enabled(); }
void ChunkManager::set_lod_grid_spacing(int32_t blocks) { controller->set_lod_grid_spacing(blocks); }
int32_t ChunkManager::get_lod_grid_spacing() const { return controller->get_lod_grid_spacing(); }
void ChunkManager::set_lod_grid_rings(int32_t rings) { controller->set_lod_grid_rings(rings); }
int32_t ChunkManager::get_lod_grid_rings() const { return controller->get_lod_grid_rings(); }
void ChunkManager::set_lod_grid_outer_rings(int32_t rings) {
    controller->set_lod_grid_outer_rings(rings);
}
int32_t ChunkManager::get_lod_grid_outer_rings() const {
    return controller->get_lod_grid_outer_rings();
}
godot::Dictionary ChunkManager::get_lod_grid_stats() const { return controller->get_lod_grid_stats(); }
godot::Dictionary ChunkManager::debug_lod_grid_column(int32_t x, int32_t z) const {
    return controller->debug_lod_grid_column(x, z);
}
godot::Dictionary ChunkManager::debug_lod_grid_water() const {
    return controller->debug_lod_grid_water();
}

void ChunkManager::set_world_bend(bool enabled, double amount, double radius, double rise) {
    controller->set_world_bend(enabled, amount, radius, rise);
}

void ChunkManager::set_world_horizon(bool enabled, double radius) {
    controller->set_world_horizon(enabled, radius);
}

bool ChunkManager::has_pending_mesh_work() const { return controller->has_pending_mesh_work(); }
bool ChunkManager::is_fill_settled() const {
    return controller->get_world_updater().is_fill_settled();
}
Dictionary ChunkManager::get_stream_snapshot() const {
    ChunkWorld& world = controller->get_chunk_world();
    Dictionary out;
    out["loaded"] = static_cast<int64_t>(world.get_chunk_map().size());
    out["generating"] = static_cast<int64_t>(world.get_scheduler().generating_count());
    out["completed"] = static_cast<int64_t>(world.get_scheduler().completed_chunk_count());
    out["lighting"] = static_cast<int64_t>(world.pending_lighting_count());
    out["dirty_stage"] = static_cast<int64_t>(world.pending_dirty_stage_count());
    out["settled"] = controller->get_world_updater().is_fill_settled();
    return out;
}
int32_t ChunkManager::get_pending_chunk_mesh_count() const {
    return static_cast<int32_t>(controller->pending_chunk_mesh_count());
}
int32_t ChunkManager::get_visible_pending_chunk_mesh_count() const {
    return static_cast<int32_t>(controller->visible_pending_chunk_mesh_count());
}
godot::Dictionary ChunkManager::get_visible_pending_lod_split() const {
    const PendingLodSplit split = controller->visible_pending_lod_split();
    godot::Dictionary out;
    out["full"] = static_cast<int32_t>(split.full);
    out["mid"] = static_cast<int32_t>(split.mid);
    out["far"] = static_cast<int32_t>(split.far);
    godot::Array bands;
    bands.resize(6);
    for (int32_t i = 0; i < 6; ++i) {
        bands[i] = static_cast<int32_t>(split.bands[static_cast<size_t>(i)]);
    }
    out["bands"] = bands;
    return out;
}
godot::Dictionary ChunkManager::get_pending_far_geometry() const {
    const PendingFarGeometry geometry = controller->pending_far_geometry();
    godot::Dictionary out;
    out["total"] = static_cast<int32_t>(geometry.total);
    out["no_chunk"] = static_cast<int32_t>(geometry.no_chunk);
    out["empty"] = static_cast<int32_t>(geometry.empty);
    out["no_geometry"] = static_cast<int32_t>(geometry.no_geometry);
    out["solid"] = static_cast<int32_t>(geometry.solid);
    out["enclosed"] = static_cast<int32_t>(geometry.enclosed);
    out["hidden"] = static_cast<int32_t>(geometry.hidden);
    out["drawn_chunk"] = static_cast<int32_t>(geometry.drawn_chunk);
    out["drawn_region"] = static_cast<int32_t>(geometry.drawn_region);
    return out;
}
godot::Dictionary ChunkManager::get_visible_far_holes(const godot::Vector3& eye) const {
    const VisibleFarHoles holes = controller->visible_far_holes(eye);
    godot::Dictionary out;
    out["total"] = static_cast<int32_t>(holes.total);
    out["visible"] = static_cast<int32_t>(holes.visible);
    out["occluded"] = static_cast<int32_t>(holes.occluded);
    return out;
}
godot::Dictionary ChunkManager::get_visible_far_holes_screen(
    const godot::Transform3D& camera, const godot::Projection& projection,
    const godot::Vector2& viewport) const {
    const VisibleFarHoles holes = controller->visible_far_holes_screen(
        camera, projection, viewport);
    godot::Dictionary out;
    out["total"] = static_cast<int32_t>(holes.total);
    out["visible"] = static_cast<int32_t>(holes.visible);
    out["occluded"] = static_cast<int32_t>(holes.occluded);
    out["screen_visible"] = static_cast<int32_t>(holes.screen_visible);
    out["screen_pixels"] = holes.screen_pixels;
    const double viewport_area = static_cast<double>(viewport.x) * viewport.y;
    out["screen_coverage"] = viewport_area > 0.0 ? holes.screen_pixels / viewport_area : 0.0;
    return out;
}
Dictionary ChunkManager::get_region_spike_stats() const {
    return controller->region_spike_stats();
}
int32_t ChunkManager::get_mesh_rebuild_budget() const { return controller->mesh_rebuild_budget(); }
int32_t ChunkManager::get_pending_mesh_reasons() const {
    return static_cast<int32_t>(controller->pending_mesh_reasons());
}
Dictionary ChunkManager::get_mesh_pop_outcomes() const {
    const MeshManager::MeshPopOutcomes& o = controller->mesh_pop_outcomes();
    Dictionary d;
    d["callbacks"] = static_cast<int64_t>(o.callbacks);
    d["total"] = static_cast<int64_t>(o.total);
    d["missing"] = static_cast<int64_t>(o.missing);
    d["no_pipeline"] = static_cast<int64_t>(o.no_pipeline);
    d["distance"] = static_cast<int64_t>(o.distance);
    d["clean"] = static_cast<int64_t>(o.clean);
    d["air"] = static_cast<int64_t>(o.air);
    d["buried"] = static_cast<int64_t>(o.buried);
    d["in_flight"] = static_cast<int64_t>(o.in_flight);
    d["built"] = static_cast<int64_t>(o.built);
    return d;
}
int64_t ChunkManager::get_mesh_queue_stop_counts_view() const { return controller->mesh_queue_stop_counts_view(); }
int64_t ChunkManager::get_mesh_queue_stop_counts_backlog() const {
    return controller->mesh_queue_stop_counts_backlog();
}
int64_t ChunkManager::get_mesh_queue_builds() const { return controller->mesh_queue_builds(); }
int64_t ChunkManager::get_mesh_queue_rejects() const { return controller->mesh_queue_rejects(); }

void ChunkManager::set_lod_distance(int32_t distance) { controller->set_lod_distance(distance); }
int32_t ChunkManager::get_lod_distance() const { return controller->get_lod_distance(); }
void ChunkManager::set_lod_detail_level(float level) { controller->set_lod_detail_level(level); }
float ChunkManager::get_lod_detail_level() const { return controller->get_lod_detail_level(); }
void ChunkManager::set_far_lod_distance(int32_t distance) { controller->set_far_lod_distance(distance); }
int32_t ChunkManager::get_far_lod_distance() const { return controller->get_far_lod_distance(); }
void ChunkManager::set_far_lod_detail_level(float level) { controller->set_far_lod_detail_level(level); }
float ChunkManager::get_far_lod_detail_level() const { return controller->get_far_lod_detail_level(); }

void ChunkManager::set_player_light_enabled(bool enabled) { controller->set_player_light_enabled(enabled); }
bool ChunkManager::get_player_light_enabled() const { return controller->get_player_light_enabled(); }

void ChunkManager::set_player_light_level(int32_t level) { controller->set_player_light_level(level); }
int32_t ChunkManager::get_player_light_level() const { return controller->get_player_light_level(); }

void ChunkManager::set_player_light_color(const Color& color) { controller->set_player_light_color(color); }
Color ChunkManager::get_player_light_color() const { return controller->get_player_light_color(); }

void ChunkManager::set_day_time(double t) { controller->set_day_time(t); }
double ChunkManager::get_day_time() const { return controller->get_day_time(); }
void ChunkManager::set_time(double t) { controller->set_time(t); }
double ChunkManager::get_time() const { return controller->get_time(); }
Vector3 ChunkManager::get_sun_direction() const { return controller->get_sun_direction(); }

void ChunkManager::set_day_night_cycle_enabled(bool enabled) { controller->set_day_night_cycle_enabled(enabled); }
bool ChunkManager::get_day_night_cycle_enabled() const { return controller->get_day_night_cycle_enabled(); }
void ChunkManager::toggle_day_night_cycle() {controller->toggle_day_night_cycle(); }

void ChunkManager::set_day_duration(double duration) { controller->set_day_duration(duration); }
double ChunkManager::get_day_duration() const { return controller->get_day_duration(); }

void ChunkManager::set_day_sky_intensity(double intensity) { controller->set_day_sky_intensity(intensity); }
double ChunkManager::get_day_sky_intensity() const { return controller->get_day_sky_intensity(); }

void ChunkManager::set_night_sky_intensity(double intensity) { controller->set_night_sky_intensity(intensity); }
double ChunkManager::get_night_sky_intensity() const { return controller->get_night_sky_intensity(); }

void ChunkManager::set_day_sky_color(const godot::Color& color) { controller->set_day_sky_color(color); }
godot::Color ChunkManager::get_day_sky_color() const { return controller->get_day_sky_color(); }

void ChunkManager::set_night_sky_color(const godot::Color& color) { controller->set_night_sky_color(color); }
godot::Color ChunkManager::get_night_sky_color() const { return controller->get_night_sky_color(); }

void ChunkManager::set_day_zenith_color(const godot::Color& color) { controller->set_day_zenith_color(color); }
godot::Color ChunkManager::get_day_zenith_color() const { return controller->get_day_zenith_color(); }

void ChunkManager::set_night_zenith_color(const godot::Color& color) { controller->set_night_zenith_color(color); }
godot::Color ChunkManager::get_night_zenith_color() const { return controller->get_night_zenith_color(); }

void ChunkManager::set_day_horizon_color(const godot::Color& color) { controller->set_day_horizon_color(color); }
godot::Color ChunkManager::get_day_horizon_color() const { return controller->get_day_horizon_color(); }

void ChunkManager::set_night_horizon_color(const godot::Color& color) { controller->set_night_horizon_color(color); }
godot::Color ChunkManager::get_night_horizon_color() const { return controller->get_night_horizon_color(); }

void ChunkManager::set_contrast(double contrast) { controller->set_contrast(contrast); }
double ChunkManager::get_contrast() const { return controller->get_contrast(); }

void ChunkManager::set_saturation(double saturation) { controller->set_saturation(saturation); }
double ChunkManager::get_saturation() const { return controller->get_saturation(); }

void ChunkManager::set_ao_color(const godot::Color& color) { controller->set_ao_color(color); }
godot::Color ChunkManager::get_ao_color() const { return controller->get_ao_color(); }

void ChunkManager::set_ao_strength(double strength) { controller->set_ao_strength(strength); }
double ChunkManager::get_ao_strength() const { return controller->get_ao_strength(); }

void ChunkManager::set_darkness_color(const godot::Color& color) { controller->set_darkness_color(color); }
godot::Color ChunkManager::get_darkness_color() const { return controller->get_darkness_color(); }

void ChunkManager::set_fog_density(double density) { controller->set_fog_density(density); }
double ChunkManager::get_fog_density() const { return controller->get_fog_density(); }
void ChunkManager::set_fog_mode(int32_t mode) { controller->set_fog_mode(mode); }
int32_t ChunkManager::get_fog_mode() const { return controller->get_fog_mode(); }
void ChunkManager::set_mipmaps_enabled(bool enabled) { controller->set_mipmaps_enabled(enabled); }
bool ChunkManager::get_mipmaps_enabled() const { return controller->get_mipmaps_enabled(); }
void ChunkManager::set_mipmap_bias(double bias) { controller->set_mipmap_bias(bias); }
double ChunkManager::get_mipmap_bias() const { return controller->get_mipmap_bias(); }

void ChunkManager::set_textures_enabled(bool enabled) { controller->set_textures_enabled(enabled); }
bool ChunkManager::get_textures_enabled() const { return controller->get_textures_enabled(); }
void ChunkManager::set_compression_enabled(bool enabled) { controller->set_compression_enabled(enabled); }
bool ChunkManager::get_compression_enabled() const { return controller->get_compression_enabled(); }
void ChunkManager::set_vegetation_enabled(bool enabled) { controller->set_vegetation_enabled(enabled); }

bool ChunkManager::get_vegetation_enabled() const { return controller->is_vegetation_enabled(); }

void ChunkManager::set_move_speed_multiplier(float multiplier) { move_speed_multiplier_ = multiplier; }

float ChunkManager::get_move_speed_multiplier() const { return move_speed_multiplier_; }

void ChunkManager::save_world_metadata() { controller->save_world_metadata(); }
bool ChunkManager::load_world_metadata() { return controller->load_world_metadata(); }
bool ChunkManager::world_metadata_exists() const { return controller->world_metadata_exists(); }
void ChunkManager::flush_dirty_chunks() { controller->flush_dirty_chunks(); }
