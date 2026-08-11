#include "godot_bindings/chunk_manager.hpp"

#include "engine/voxel_engine_controller.hpp"

#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/node3d.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/variant/string.hpp>

using namespace godot;
using namespace VoxelEngine;

ChunkManager::ChunkManager() {
    controller = std::make_unique<VoxelEngineController>();
    controller->initialize();
}

ChunkManager::~ChunkManager() {
    controller->shutdown(this);
    controller.reset();
}

PerformanceTimer& ChunkManager::get_perf_timer() {
    return VoxelEngineController::get_perf_timer();
}

void ChunkManager::_ready() {
    print_line("_ready: called, is_inside_tree=" + String::num(is_inside_tree()));
    controller->set_owner(this);
    if (!player_path.is_empty()) {
        Node* player_node = get_node_or_null(player_path);
        cached_player = Object::cast_to<Node3D>(player_node);
        if (cached_player) {
            controller->set_player_position(cached_player->get_global_position());
        }
    }
    controller->get_environment_controller().update_environment(get_parent());

    // Load world metadata if it exists, otherwise save initial metadata
    if (controller->world_metadata_exists()) {
        if (!controller->load_world_metadata()) {
            WARN_PRINT("Failed to load world metadata, using current defaults");
        }
    } else {
        controller->save_world_metadata();
    }

    ready_for_auto_update = true;
}

void ChunkManager::_enter_tree() {
    controller->set_owner(this);
    set_process(true);
    print_line("_enter_tree: called, is_inside_tree=" + String::num(is_inside_tree()));
    RenderingServer* rs = RenderingServer::get_singleton();
    Ref<World3D> world = get_world_3d();
    if (world.is_valid()) {
        RID scenario = world->get_scenario();
        print_line("_enter_tree: world is valid, scenario is valid=" + String::num(scenario.is_valid()));
        controller->get_chunk_world().get_chunk_map().for_each([&](uint64_t key, const std::unique_ptr<ChunkRenderData>& render_data) {
            if (render_data->instance_rid.is_valid()) {
                rs->instance_set_scenario(render_data->instance_rid, scenario);
            }
        });
        print_line("_enter_tree: set scenario for " + String::num(controller->get_chunk_world().get_chunk_map().size()) + " chunks");
    } else {
        print_line("_enter_tree: world is null");
    }
}

void ChunkManager::_process(double delta) {
    if (!controller->get_auto_update()) return;
    Engine* engine = Engine::get_singleton();
    bool is_editor = engine && engine->is_editor_hint();
    if (is_editor && !controller->get_editor_enabled()) return;

    godot::Vector3 player_pos;
    godot::Camera3D* cam = Object::cast_to<godot::Camera3D>(get_viewport()->get_camera_3d());
    if (cam) {
        player_pos = cam->get_global_position();
        cached_camera = cam;
    } else if (cached_player) {
        player_pos = cached_player->get_global_position();
    } else if (!player_path.is_empty()) {
        Node* player_node = get_node_or_null(player_path);
        Node3D* player = Object::cast_to<Node3D>(player_node);
        if (player) {
            cached_player = player;
            player_pos = player->get_global_position();
        }
    }

    // Extract camera frustum planes for frustum-prioritized chunk loading
    if (cached_camera) {
        godot::TypedArray<godot::Plane> frustum_planes = cached_camera->get_frustum();
        if (frustum_planes.size() >= 6) {
            std::array<godot::Plane, 6> planes;
            for (int i = 0; i < 6; ++i) {
                planes[i] = frustum_planes[i];
            }
            controller->update_frustum(planes);
        }
    } else if (cached_player) {
        cam = Object::cast_to<godot::Camera3D>(cached_player->get_node_or_null(NodePath("Camera3D")));
        if (cam) {
            cached_camera = cam;
            godot::TypedArray<godot::Plane> frustum_planes = cached_camera->get_frustum();
            if (frustum_planes.size() >= 6) {
                std::array<godot::Plane, 6> planes;
                for (int i = 0; i < 6; ++i) {
                    planes[i] = frustum_planes[i];
                }
                controller->update_frustum(planes);
            }
        }
    }

    controller->update(delta, is_editor, player_pos, this);
    controller->get_environment_controller().update_environment(get_parent());
}

void ChunkManager::_exit_tree() {
    ready_for_auto_update = false;
    cached_player = nullptr;
    cached_camera = nullptr;
    // Flush any dirty chunks while the world is still fully alive (all chunks
    // loaded, thread pool running, controller owned by this node). Without this,
    // edits made since the last 5s periodic flush are lost on quit. Blocking:
    // we must not let the process exit while background saves are outstanding.
    if (controller) {
        controller->flush_dirty_chunks(true, 5.0);
    }
}

// -------------------------------------------------------------------------
// Property thin wrappers — every method below just delegates to controller
// -------------------------------------------------------------------------

void ChunkManager::set_seed(int32_t p_seed) { controller->set_seed(p_seed); }
int32_t ChunkManager::get_seed() const { return controller->get_seed(); }

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

String ChunkManager::get_performance_report() { return controller->get_performance_report(); }

void ChunkManager::set_chunk_scenario(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    controller->set_chunk_scenario(chunk_x, chunk_y, chunk_z, this);
}

void ChunkManager::clear_editor_chunks() { controller->clear_editor_chunks(this); }

void ChunkManager::set_editor_enabled(bool enabled) {
    controller->set_editor_enabled(enabled);
    if (ready_for_auto_update && enabled && controller->get_auto_update()) {
        update_chunks();
    }
}
bool ChunkManager::get_editor_enabled() const { return controller->get_editor_enabled(); }

void ChunkManager::set_editor_render_distance(int32_t distance) { controller->set_editor_render_distance(distance); }
int32_t ChunkManager::get_editor_render_distance() const { return controller->get_editor_render_distance(); }

Dictionary ChunkManager::raycast_from_camera(double max_distance) {
    return controller->raycast_from_camera(this, player_path, max_distance);
}

void ChunkManager::set_block(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    controller->set_block_world(world_x, world_y, world_z, block_id);
}

int ChunkManager::get_block(int32_t world_x, int32_t world_y, int32_t world_z) {
    return controller->get_block_world(world_x, world_y, world_z);
}

String ChunkManager::get_block_name(int block_id) {
    return controller->get_block_name(block_id);
}

Dictionary ChunkManager::resolve_voxel_collision(const godot::Vector3& position, const godot::Vector3& motion, const godot::Vector3& size) {
    auto result = controller->resolve_voxel_collision(position, motion, size);
    Dictionary dict;
    dict["position"] = result.position;
    dict["collided_x"] = result.collided_x;
    dict["collided_y"] = result.collided_y;
    dict["collided_z"] = result.collided_z;
    dict["on_floor"] = result.on_floor;
    return dict;
}

VoxelEngine::CollisionResolver* ChunkManager::get_collision_resolver() {
    return &controller->get_collision_resolver();
}

void ChunkManager::set_smooth_lighting(bool enabled) { controller->set_smooth_lighting(enabled); }
bool ChunkManager::get_smooth_lighting() const { return controller->get_smooth_lighting(); }

void ChunkManager::set_lod_distance(int32_t distance) { controller->set_lod_distance(distance); }
int32_t ChunkManager::get_lod_distance() const { return controller->get_lod_distance(); }
void ChunkManager::set_lod_detail_level(float level) { controller->set_lod_detail_level(level); }
float ChunkManager::get_lod_detail_level() const { return controller->get_lod_detail_level(); }
void ChunkManager::set_lod_far_distance(int32_t distance) { controller->set_lod_far_distance(distance); }
int32_t ChunkManager::get_lod_far_distance() const { return controller->get_lod_far_distance(); }
void ChunkManager::set_far_detail_level(float level) { controller->set_far_detail_level(level); }
float ChunkManager::get_far_detail_level() const { return controller->get_far_detail_level(); }

void ChunkManager::set_player_light_enabled(bool enabled) { controller->set_player_light_enabled(enabled); }
bool ChunkManager::get_player_light_enabled() const { return controller->get_player_light_enabled(); }

void ChunkManager::set_player_light_level(int32_t level) { controller->set_player_light_level(level); }
int32_t ChunkManager::get_player_light_level() const { return controller->get_player_light_level(); }

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

// -------------------------------------------------------------------------
// _bind_methods
// -------------------------------------------------------------------------
void ChunkManager::_bind_methods() {
    // Non-property API (manual — each has unique signatures)
    ClassDB::bind_method(D_METHOD("update_chunks"), &ChunkManager::update_chunks);
    ClassDB::bind_method(D_METHOD("generate_chunk", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::generate_chunk);
    ClassDB::bind_method(D_METHOD("unload_chunk", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::unload_chunk);
    ClassDB::bind_method(D_METHOD("get_performance_report"), &ChunkManager::get_performance_report);
    ClassDB::bind_method(D_METHOD("set_chunk_scenario", "chunk_x", "chunk_y", "chunk_z"), &ChunkManager::set_chunk_scenario);
    ClassDB::bind_method(D_METHOD("clear_editor_chunks"), &ChunkManager::clear_editor_chunks);
    ClassDB::bind_method(D_METHOD("raycast_from_camera", "max_distance"), &ChunkManager::raycast_from_camera);
    ClassDB::bind_method(D_METHOD("set_block", "world_x", "world_y", "world_z", "block_id"), &ChunkManager::set_block);
    ClassDB::bind_method(D_METHOD("get_block", "world_x", "world_y", "world_z"), &ChunkManager::get_block);
    ClassDB::bind_method(D_METHOD("get_block_name", "block_id"), &ChunkManager::get_block_name);
    ClassDB::bind_method(D_METHOD("resolve_voxel_collision", "position", "motion", "size"), &ChunkManager::resolve_voxel_collision);

    ClassDB::bind_method(D_METHOD("save_world_metadata"), &ChunkManager::save_world_metadata);
    ClassDB::bind_method(D_METHOD("load_world_metadata"), &ChunkManager::load_world_metadata);
    ClassDB::bind_method(D_METHOD("world_metadata_exists"), &ChunkManager::world_metadata_exists);
    ClassDB::bind_method(D_METHOD("flush_dirty_chunks"), &ChunkManager::flush_dirty_chunks);

ClassDB::bind_method(D_METHOD("set_time", "time"), &ChunkManager::set_time);
ClassDB::bind_method(D_METHOD("get_time"), &ChunkManager::get_time);
ClassDB::bind_method(D_METHOD("toggle_day_night_cycle"), &ChunkManager::toggle_day_night_cycle);
ClassDB::bind_method(D_METHOD("get_sun_direction"), &ChunkManager::get_sun_direction);

    // Block ID constants (exposed to GDScript so block types can be referenced without hardcoding)
#define BIND_BLOCK_CONSTANT(name, id) ClassDB::bind_integer_constant("ChunkManager", "", #name, id)
    BIND_BLOCK_CONSTANT(BLOCK_AIR,           BlockIDs::AIR);
    BIND_BLOCK_CONSTANT(BLOCK_STONE,         BlockIDs::STONE);
    BIND_BLOCK_CONSTANT(BLOCK_DIRT,          BlockIDs::DIRT);
    BIND_BLOCK_CONSTANT(BLOCK_GRASS,         BlockIDs::GRASS);
    BIND_BLOCK_CONSTANT(BLOCK_SAND,          BlockIDs::SAND);
    BIND_BLOCK_CONSTANT(BLOCK_SURFACE_WATER, BlockIDs::SURFACE_WATER);
    BIND_BLOCK_CONSTANT(BLOCK_WATER,         BlockIDs::WATER);
    BIND_BLOCK_CONSTANT(BLOCK_WOOD,          BlockIDs::WOOD);
    BIND_BLOCK_CONSTANT(BLOCK_LEAVES,        BlockIDs::LEAVES);
    BIND_BLOCK_CONSTANT(BLOCK_BEDROCK,       BlockIDs::BEDROCK);
    BIND_BLOCK_CONSTANT(BLOCK_MUD,           BlockIDs::MUD);
    BIND_BLOCK_CONSTANT(BLOCK_WET_SAND,      BlockIDs::WET_SAND);
    BIND_BLOCK_CONSTANT(BLOCK_MUD_FULL,      BlockIDs::MUD_FULL);
    BIND_BLOCK_CONSTANT(BLOCK_WET_SAND_FULL, BlockIDs::WET_SAND_FULL);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_BLOCK,   BlockIDs::LIGHT_BLOCK);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_RED,     BlockIDs::LIGHT_RED);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_GREEN,   BlockIDs::LIGHT_GREEN);
    BIND_BLOCK_CONSTANT(BLOCK_LIGHT_BLUE,    BlockIDs::LIGHT_BLUE);
#undef BIND_BLOCK_CONSTANT

    // Editor properties: each macro emits the getter, setter, and ADD_PROPERTY line.
    // type   = Godot Variant type
    // base   = property name (also used to build get_*/set_* method names)
    // param  = D_METHOD parameter name for the setter
#define BIND_PROP(type, base, param) \
    ClassDB::bind_method(D_METHOD("set_" #base, param), &ChunkManager::set_##base); \
    ClassDB::bind_method(D_METHOD("get_" #base), &ChunkManager::get_##base); \
    ADD_PROPERTY(PropertyInfo(type, #base), "set_" #base, "get_" #base)

    BIND_PROP(Variant::INT,     seed,                      "seed");
    BIND_PROP(Variant::INT,     render_distance,           "distance");
    BIND_PROP(Variant::NODE_PATH, player_path,             "path");
    BIND_PROP(Variant::VECTOR3, player_position,           "position");
    BIND_PROP(Variant::BOOL,    auto_update,               "enabled");
    BIND_PROP(Variant::FLOAT,   sea_level,                 "level");
    ClassDB::bind_method(D_METHOD("set_biome_size", "size"), &ChunkManager::set_biome_size);
    ClassDB::bind_method(D_METHOD("get_biome_size"), &ChunkManager::get_biome_size);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "biome_size", PROPERTY_HINT_RANGE, "0.25,4.0,0.05"), "set_biome_size", "get_biome_size");
    BIND_PROP(Variant::BOOL,    editor_enabled,            "enabled");
    BIND_PROP(Variant::INT,     editor_render_distance,    "distance");
BIND_PROP(Variant::BOOL, smooth_lighting, "enabled");
    ClassDB::bind_method(D_METHOD("set_lod_distance", "distance"), &ChunkManager::set_lod_distance);
    ClassDB::bind_method(D_METHOD("get_lod_distance"), &ChunkManager::get_lod_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_distance", PROPERTY_HINT_RANGE, "0,64,1"), "set_lod_distance", "get_lod_distance");
    ClassDB::bind_method(D_METHOD("set_lod_detail_level", "level"), &ChunkManager::set_lod_detail_level);
    ClassDB::bind_method(D_METHOD("get_lod_detail_level"), &ChunkManager::get_lod_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "lod_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_lod_detail_level", "get_lod_detail_level");
    ClassDB::bind_method(D_METHOD("set_lod_far_distance", "distance"), &ChunkManager::set_lod_far_distance);
    ClassDB::bind_method(D_METHOD("get_lod_far_distance"), &ChunkManager::get_lod_far_distance);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "lod_far_distance", PROPERTY_HINT_RANGE, "0,128,1"), "set_lod_far_distance", "get_lod_far_distance");
    ClassDB::bind_method(D_METHOD("set_far_detail_level", "level"), &ChunkManager::set_far_detail_level);
    ClassDB::bind_method(D_METHOD("get_far_detail_level"), &ChunkManager::get_far_detail_level);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "far_detail_level", PROPERTY_HINT_RANGE, "0.125,1.0,0.005"), "set_far_detail_level", "get_far_detail_level");
    BIND_PROP(Variant::BOOL,    player_light_enabled,      "enabled");
    BIND_PROP(Variant::INT,     player_light_level,        "level");
BIND_PROP(Variant::FLOAT, day_time, "time");
    BIND_PROP(Variant::BOOL,    day_night_cycle_enabled,   "enabled");
    BIND_PROP(Variant::FLOAT,   day_duration,              "duration");
    BIND_PROP(Variant::FLOAT,   day_sky_intensity,         "intensity");
    BIND_PROP(Variant::FLOAT,   night_sky_intensity,       "intensity");
    BIND_PROP(Variant::COLOR,   day_sky_color,             "color");
    BIND_PROP(Variant::COLOR,   night_sky_color,           "color");
    BIND_PROP(Variant::FLOAT,   contrast,                  "value");
    BIND_PROP(Variant::FLOAT,   saturation,                "value");
    BIND_PROP(Variant::COLOR,   ao_color,                  "color");
    BIND_PROP(Variant::FLOAT,   ao_strength,               "strength");
    BIND_PROP(Variant::COLOR,   darkness_color,            "color");
    BIND_PROP(Variant::FLOAT,   fog_density,               "density");
    ClassDB::bind_method(D_METHOD("set_fog_mode", "mode"), &ChunkManager::set_fog_mode);
    ClassDB::bind_method(D_METHOD("get_fog_mode"), &ChunkManager::get_fog_mode);
    ADD_PROPERTY(PropertyInfo(Variant::INT, "fog_mode", PROPERTY_HINT_ENUM, "Disabled:0,Edge:1,Linear:2,Exponential:3"), "set_fog_mode", "get_fog_mode");
    BIND_PROP(Variant::BOOL,    mipmaps_enabled,            "enabled");
    ClassDB::bind_method(D_METHOD("set_mipmap_bias", "bias"), &ChunkManager::set_mipmap_bias);
    ClassDB::bind_method(D_METHOD("get_mipmap_bias"), &ChunkManager::get_mipmap_bias);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "mipmap_bias", PROPERTY_HINT_RANGE, "-4.0,4.0,0.01"), "set_mipmap_bias", "get_mipmap_bias");
    BIND_PROP(Variant::BOOL,    textures_enabled,           "enabled");
    ClassDB::bind_method(D_METHOD("set_compression_enabled", "enabled"), &ChunkManager::set_compression_enabled);
    ClassDB::bind_method(D_METHOD("get_compression_enabled"), &ChunkManager::get_compression_enabled);
    ADD_PROPERTY(PropertyInfo(Variant::BOOL, "compression_enabled"), "set_compression_enabled", "get_compression_enabled");
    BIND_PROP(Variant::BOOL,    vegetation_enabled,         "enabled");
    ClassDB::bind_method(D_METHOD("set_move_speed_multiplier", "multiplier"), &ChunkManager::set_move_speed_multiplier);
    ClassDB::bind_method(D_METHOD("get_move_speed_multiplier"), &ChunkManager::get_move_speed_multiplier);
    ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "move_speed_multiplier", PROPERTY_HINT_RANGE, "0.1,16.0,0.1"), "set_move_speed_multiplier", "get_move_speed_multiplier");
#undef BIND_PROP
}
