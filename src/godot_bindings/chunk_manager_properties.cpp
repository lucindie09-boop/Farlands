// The property surface: every method whose whole body is a delegation to the
// controller (or a value this binding caches itself). One line each is the point
// — the engine side of each name lives in engine/voxel_engine_controller.cpp, and
// this file exists so that reading the GDScript-facing API does not mean scrolling
// past the raycast, the schematic and the texture code.

#include "godot_bindings/chunk_manager.hpp"

#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/engine.hpp>
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

String ChunkManager::get_performance_report() { return controller->get_performance_report(); }

void ChunkManager::set_smooth_lighting(bool enabled) { controller->set_smooth_lighting(enabled); }
bool ChunkManager::get_smooth_lighting() const { return controller->get_smooth_lighting(); }

void ChunkManager::set_world_bend(bool enabled, double amount, double radius, double rise) {
    controller->set_world_bend(enabled, amount, radius, rise);
}

void ChunkManager::set_world_horizon(bool enabled, double radius) {
    controller->set_world_horizon(enabled, radius);
}

bool ChunkManager::has_pending_mesh_work() const { return controller->has_pending_mesh_work(); }

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
