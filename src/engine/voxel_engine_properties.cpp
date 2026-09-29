// The property surface: every setter/getter that writes a member and forwards it to
// the world updater or the environment controller. Kept apart from
// engine/voxel_engine_controller.cpp so the GDScript-facing knobs are one file.

#include "engine/voxel_engine_controller.hpp"

namespace VoxelEngine {
using namespace godot;

// -------------------------------------------------------------------------
// Property accessors
// -------------------------------------------------------------------------

void VoxelEngineController::set_seed(int32_t s) { seed = s; world_updater.set_seed(seed); }
int32_t VoxelEngineController::get_seed() const { return seed; }

void VoxelEngineController::set_render_distance(int32_t rd) { 
    render_distance = rd; 
    world_updater.set_render_distance(render_distance); 
    environment_controller.set_render_distance_blocks(static_cast<float>(rd * CHUNK_WIDTH));
    
    // Reserve ChunkMap based on render distance to avoid rehashing during load
    // Total chunks ≈ (2*RD + 1)^3, reserve() divides by 64 internally
    size_t total_chunks = static_cast<size_t>(2 * rd + 1) * static_cast<size_t>(2 * rd + 1) * static_cast<size_t>(2 * rd + 1);
    chunk_world.get_chunk_map().reserve(total_chunks);
}
int32_t VoxelEngineController::get_render_distance() const { return render_distance; }

void VoxelEngineController::set_editor_render_distance(int32_t rd) { editor_render_distance = rd; world_updater.set_editor_render_distance(editor_render_distance); }
int32_t VoxelEngineController::get_editor_render_distance() const { return editor_render_distance; }

void VoxelEngineController::set_player_position(const godot::Vector3& pos) { player_position = pos; world_updater.set_player_position(player_position); }
godot::Vector3 VoxelEngineController::get_player_position() const { return player_position; }

void VoxelEngineController::set_sea_level(float level) { sea_level = level; world_updater.set_sea_level(sea_level); }
float VoxelEngineController::get_sea_level() const { return sea_level; }
void VoxelEngineController::set_biome_size(float size) { biome_size = size; world_updater.set_biome_size(biome_size); }
float VoxelEngineController::get_biome_size() const { return biome_size; }

void VoxelEngineController::set_vegetation_enabled(bool enabled) { vegetation_enabled = enabled; world_updater.set_vegetation_enabled(enabled); }
bool VoxelEngineController::is_vegetation_enabled() const { return vegetation_enabled; }


void VoxelEngineController::set_auto_update(bool enabled) { auto_update = enabled; }
bool VoxelEngineController::get_auto_update() const { return auto_update; }

void VoxelEngineController::set_smooth_lighting(bool enabled) {
smooth_lighting = enabled;
mesh_manager.set_smooth_lighting(enabled);
mesh_manager.mark_all_chunks_dirty();
}
bool VoxelEngineController::get_smooth_lighting() const { return smooth_lighting; }

void VoxelEngineController::set_lod_distance(int32_t d) { lod_distance = d; world_updater.set_lod_distance(d); }
int32_t VoxelEngineController::get_lod_distance() const { return lod_distance; }
void VoxelEngineController::set_lod_detail_level(float l) { lod_detail_level = l; world_updater.set_lod_detail_level(l); }
float VoxelEngineController::get_lod_detail_level() const { return lod_detail_level; }
void VoxelEngineController::set_far_lod_distance(int32_t d) { far_lod_distance = d; world_updater.set_far_lod_distance(d); }
int32_t VoxelEngineController::get_far_lod_distance() const { return far_lod_distance; }
void VoxelEngineController::set_far_lod_detail_level(float l) { far_lod_detail_level = l; world_updater.set_far_lod_detail_level(l); }
float VoxelEngineController::get_far_lod_detail_level() const { return far_lod_detail_level; }

void VoxelEngineController::set_editor_enabled(bool enabled) { editor_enabled = enabled; }
bool VoxelEngineController::get_editor_enabled() const { return editor_enabled; }


void VoxelEngineController::set_player_light_enabled(bool enabled) { environment_controller.set_player_light_enabled(enabled); }
bool VoxelEngineController::get_player_light_enabled() const { return environment_controller.get_player_light_enabled(); }

void VoxelEngineController::set_player_light_level(int32_t level) { environment_controller.set_player_light_level(level); }
int32_t VoxelEngineController::get_player_light_level() const { return environment_controller.get_player_light_level(); }

void VoxelEngineController::set_player_light_color(const godot::Color& color) { environment_controller.set_player_light_color(color); }
godot::Color VoxelEngineController::get_player_light_color() const { return environment_controller.get_player_light_color(); }

void VoxelEngineController::set_day_time(double t) { environment_controller.set_day_time(t); }
double  VoxelEngineController::get_day_time() const { return environment_controller.get_day_time(); }

void VoxelEngineController::set_time(double t) { set_day_time(t); }
double VoxelEngineController::get_time() const { return get_day_time(); }
godot::Vector3 VoxelEngineController::get_sun_direction() const { return environment_controller.get_sun_direction(); }

void VoxelEngineController::set_day_night_cycle_enabled(bool enabled) { environment_controller.set_day_night_cycle_enabled(enabled); }
bool VoxelEngineController::get_day_night_cycle_enabled() const { return environment_controller.get_day_night_cycle_enabled(); }
void VoxelEngineController::toggle_day_night_cycle() {set_day_night_cycle_enabled(!get_day_night_cycle_enabled()); }

void VoxelEngineController::set_day_duration(double duration) { environment_controller.set_day_duration(duration); }
double VoxelEngineController::get_day_duration() const { return environment_controller.get_day_duration(); }

void VoxelEngineController::set_day_sky_intensity(double intensity) { environment_controller.set_day_sky_intensity(intensity); }
double VoxelEngineController::get_day_sky_intensity() const { return environment_controller.get_day_sky_intensity(); }

void VoxelEngineController::set_night_sky_intensity(double intensity) { environment_controller.set_night_sky_intensity(intensity); }
double VoxelEngineController::get_night_sky_intensity() const { return environment_controller.get_night_sky_intensity(); }

void VoxelEngineController::set_day_sky_color(const godot::Color& color) { environment_controller.set_day_sky_color(color); }
godot::Color VoxelEngineController::get_day_sky_color() const { return environment_controller.get_day_sky_color(); }

void VoxelEngineController::set_night_sky_color(const godot::Color& color) { environment_controller.set_night_sky_color(color); }
godot::Color VoxelEngineController::get_night_sky_color() const { return environment_controller.get_night_sky_color(); }

void VoxelEngineController::set_contrast(double contrast) { environment_controller.set_contrast(contrast); }
double VoxelEngineController::get_contrast() const { return environment_controller.get_contrast(); }
void VoxelEngineController::set_saturation(double saturation) { environment_controller.set_saturation(saturation); }
double VoxelEngineController::get_saturation() const { return environment_controller.get_saturation(); }
void VoxelEngineController::set_ao_color(const godot::Color& color) { environment_controller.set_ao_color(color); }
godot::Color VoxelEngineController::get_ao_color() const { return environment_controller.get_ao_color(); }
void VoxelEngineController::set_ao_strength(double strength) { environment_controller.set_ao_strength(strength); }
double VoxelEngineController::get_ao_strength() const { return environment_controller.get_ao_strength(); }
void VoxelEngineController::set_darkness_color(const godot::Color& color) { environment_controller.set_darkness_color(color); }
godot::Color VoxelEngineController::get_darkness_color() const { return environment_controller.get_darkness_color(); }

void VoxelEngineController::set_fog_density(double density) { environment_controller.set_fog_density(density); }
double VoxelEngineController::get_fog_density() const { return environment_controller.get_fog_density(); }
void VoxelEngineController::set_fog_mode(int32_t mode) { environment_controller.set_fog_mode(mode); }
int32_t VoxelEngineController::get_fog_mode() const { return environment_controller.get_fog_mode(); }
void VoxelEngineController::set_mipmaps_enabled(bool enabled) { environment_controller.set_mipmaps_enabled(enabled); }
bool VoxelEngineController::get_mipmaps_enabled() const { return environment_controller.get_mipmaps_enabled(); }
void VoxelEngineController::set_mipmap_bias(double bias) { environment_controller.set_mipmap_bias(bias); }
double VoxelEngineController::get_mipmap_bias() const { return environment_controller.get_mipmap_bias(); }
void VoxelEngineController::set_textures_enabled(bool enabled) { environment_controller.set_textures_enabled(enabled); }
bool VoxelEngineController::get_textures_enabled() const { return environment_controller.get_textures_enabled(); }
void VoxelEngineController::set_compression_enabled(bool enabled) { environment_controller.set_compression_enabled(enabled); }
bool VoxelEngineController::get_compression_enabled() const { return environment_controller.get_compression_enabled(); }
void VoxelEngineController::set_render_distance_blocks(float blocks) { environment_controller.set_render_distance_blocks(blocks); }
float VoxelEngineController::get_render_distance_blocks() const { return environment_controller.get_render_distance_blocks(); }

} // namespace VoxelEngine
