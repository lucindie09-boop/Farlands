#include "engine/voxel_engine_controller.hpp"

#include <cmath>
#include <algorithm>

#include "debug/perf_report.hpp"
#include "world/block_editor.hpp"
#include "core/thread_pool.hpp"
#include "render/texture_pack_manager.hpp"
#include "mesh/mesh_builder.hpp"
#include "worldgen/chunk_generator.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include "core/item_registry.hpp"
#include "core/chunk_coords.hpp"
#include "schematic/paste_plan.hpp"
#include "schematic/schematic_reader.hpp"
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <mutex>

namespace VoxelEngine {
using namespace godot;

PerformanceTimer VoxelEngineController::perf_timer;

PerformanceTimer& VoxelEngineController::get_perf_timer() {
    return perf_timer;
}

VoxelEngineController::VoxelEngineController()
    : block_editor(&chunk_world, &mesh_manager, &light_propagator) {
    static std::once_flag registry_init_flag;
    std::call_once(registry_init_flag, []() {
        auto& registry = BlockRegistry::get_instance();
        if (!registry.load_from_json("res://data/block_definitions.json")) {
            registry.initialize_default_blocks();
        }
    });
    // Fluid states are resolved out of whatever registry just loaded (see
    // fluids/fluid_state_table.hpp), so this has to happen after it and before
    // anything can tick. A registry with no fluid states leaves the simulation
    // disabled rather than broken.
    fluid_state_table.build_from(BlockRegistry::get_instance());

    // Note: reserve(5000) is a placeholder. Real reserve happens in set_render_distance()
    // where the actual render distance value is known.
    chunk_world.get_chunk_map().reserve(5000);
    load_world_configs();
    // Load optional texture packs from user://packs (no-op when absent).
    TexturePackManager::get_instance().load_packs("user://packs");
    create_thread_pool();
    chunk_world.set_thread_pool(thread_pool.get());
    mesh_manager.set_chunk_map(chunk_world.get_chunk_map_ptr());
    mesh_manager.set_chunk_scheduler(&chunk_world.get_scheduler());
    mesh_manager.set_thread_pool(thread_pool.get());
    mesh_manager.set_performance_timer(&perf_timer);
    mesh_manager.set_async_epoch(chunk_world.get_epoch_ptr());
    mesh_manager.set_owner(nullptr);
    light_propagator.set_chunk_map(chunk_world.get_chunk_map_ptr());
    light_propagator.set_mesh_manager(&mesh_manager);
    chunk_world.set_mesh_manager(&mesh_manager);
    chunk_world.set_light_propagator(&light_propagator);
    chunk_world.set_owner(nullptr);
    world_updater.set_chunk_world(&chunk_world);
    world_updater.set_mesh_manager(&mesh_manager);
    world_updater.set_fluid_state_table(&fluid_state_table);
    // Every edit that lands in an edit map wakes the fluid simulation, which is
    // how a player's block change (and a chunk coming back with water in it)
    // gets fluid to re-evaluate. The simulation is the only listener.
    chunk_world.set_edit_listener([this](int32_t x, int32_t y, int32_t z) {
        world_updater.notify_block_edit(x, y, z);
    });
    world_updater.set_thread_pool(thread_pool.get());
    world_updater.set_performance_timer(&perf_timer);
    world_updater.set_material_manager(&environment_controller.get_material_manager());
    world_updater.set_owner(nullptr);
    world_updater.set_seed(seed);
    world_updater.set_sea_level(sea_level);
    world_updater.set_render_distance(render_distance);
    world_updater.set_editor_render_distance(editor_render_distance);
    world_updater.set_lod_distance(lod_distance);
    world_updater.set_lod_detail_level(lod_detail_level);
    world_updater.set_far_lod_distance(far_lod_distance);
    world_updater.set_far_lod_detail_level(far_lod_detail_level);
    mesh_manager.set_mesh_render_distance(render_distance);
    // Buried-chunk mesh culling asks the world updater whether an ungenerated
    // neighbor would be solid (deep underground) so it can skip rendering box
    // walls into the void instead of drawing them.
    mesh_manager.set_chunk_would_be_solid_fn([this](int32_t cx, int32_t cy, int32_t cz) {
        return world_updater.chunk_would_be_solid(cx, cy, cz);
    });
}

VoxelEngineController::~VoxelEngineController() {
    reset_runtime_state(false);
}

void VoxelEngineController::initialize() {
    set_render_distance(render_distance);
}

void VoxelEngineController::shutdown() {
    reset_runtime_state(false);
}

void VoxelEngineController::set_owner(godot::Node* node) {
    mesh_manager.set_owner(node);
    chunk_world.set_owner(node);
    world_updater.set_owner(node);
}

void VoxelEngineController::create_thread_pool() {
    unsigned int hw_threads = std::thread::hardware_concurrency();
    size_t num_threads = hw_threads > 1 ? static_cast<size_t>(hw_threads - 1) : 1;
    thread_pool = std::make_unique<ThreadPool>(num_threads);
    chunk_world.set_thread_pool(thread_pool.get());
    mesh_manager.set_thread_pool(thread_pool.get());
    world_updater.set_thread_pool(thread_pool.get());
}

void VoxelEngineController::shutdown_thread_pool() {
    if (thread_pool) {
        thread_pool->shutdown();
        thread_pool.reset();
    }
    chunk_world.set_thread_pool(nullptr);
    mesh_manager.set_thread_pool(nullptr);
    world_updater.set_thread_pool(nullptr);
}

void VoxelEngineController::clear_async_queues() {
    chunk_world.clear();
    mesh_manager.clear();
    world_updater.clear();
}

void VoxelEngineController::free_loaded_chunks() {
    chunk_world.free_loaded_chunks();
}

void VoxelEngineController::reset_runtime_state(bool restart_thread_pool) {
    chunk_world.increment_epoch();
    shutdown_thread_pool();
    chunk_world.free_loaded_chunks();
    clear_async_queues();
    world_updater.reset();
    runtime_elapsed = 0.0;
    frame_time_accumulator = 0.0;
    frame_count = 0;

    if (restart_thread_pool) {
        create_thread_pool();
    }
}

void VoxelEngineController::update(double delta, bool is_editor, const godot::Vector3& player_pos) {
    if (!auto_update) return;
    if (is_editor && !editor_enabled) return;

    ScopedTimer process_timer(perf_timer, TimerID::ProcessTotal);
    frame_count++;
    runtime_elapsed += delta;
    frame_time_accumulator += delta;
    last_delta = delta;

    player_position = player_pos;

    environment_controller.update(delta, runtime_elapsed, player_position,
                                  chunk_world, light_propagator, mesh_manager,
                                  world_updater.get_initial_loading_duration());

    {
        ScopedTimer t(perf_timer, TimerID::PlayerPosUpdate);
        world_updater.set_player_position(player_position);
    }

    {
        ScopedTimer t(perf_timer, TimerID::WorldUpdate);
        update_chunks(is_editor);
    }

    {
        ScopedTimer t(perf_timer, TimerID::SceneUpdate);
        print_debug_info(delta);
    }
}

void VoxelEngineController::update_frustum(const std::array<godot::Plane, 6>& planes) {
    Frustum f;
    f.update(planes);
    world_updater.set_frustum(f);
}

void VoxelEngineController::update_chunks(bool is_editor) {
    world_updater.update(is_editor, chunk_world.get_epoch(), chunks_processed_total, last_delta);
}

// -------------------------------------------------------------------------
// Block editing (delegated to BlockEditor)
// -------------------------------------------------------------------------

void VoxelEngineController::set_block_world(int32_t world_x, int32_t world_y, int32_t world_z, int block_id) {
    block_editor.place_block(world_x, world_y, world_z, static_cast<BlockID>(block_id));
}

int VoxelEngineController::get_block_world(int32_t world_x, int32_t world_y, int32_t world_z) {
    return block_editor.query_block(world_x, world_y, world_z);
}

// -------------------------------------------------------------------------
// Block files from other tools
// -------------------------------------------------------------------------

bool VoxelEngineController::ensure_minecraft_palette() {
    if (minecraft_palette_loaded_) return true;

    Ref<FileAccess> file = FileAccess::open("res://data/minecraft_blocks.json", FileAccess::READ);
    if (file.is_null()) {
        minecraft_palette_error_ =
            "cannot open res://data/minecraft_blocks.json (no translation table)";
        return false;
    }
    const String text = file->get_as_text();
    std::string table_error;
    if (!minecraft_palette_.load(std::string(text.utf8().get_data()), &table_error)) {
        minecraft_palette_error_ = table_error;
        return false;
    }

    // Resolve every name the table uses to a block id once, by scanning the
    // registry once. Doing it per cell would be a linear scan over the block
    // table for every block in a build.
    std::unordered_map<std::string, BlockID> by_name;
    const BlockRegistry& registry = BlockRegistry::get_instance();
    for (size_t i = 0; i < registry.get_count(); ++i) {
        const BlockType& block = registry.get_block_fast(static_cast<BlockID>(i));
        if (block.name == nullptr) continue;
        by_name.emplace(block.name, static_cast<BlockID>(i));
    }

    std::string missing;
    for (const std::string& name : schematic::target_names(minecraft_palette_)) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
            if (missing.empty()) missing = name;
            continue;
        }
        minecraft_name_ids_.emplace(name, found->second);
    }
    if (!missing.empty()) {
        // A table naming a block this build does not have would paste holes, so
        // the paste is refused with the name in the message rather than
        // silently dropping that state (the report tool catches this offline).
        minecraft_palette_error_ =
            "data/minecraft_blocks.json maps a state to \"" + missing +
            "\", which is not a block in this build";
        return false;
    }

    minecraft_palette_loaded_ = true;
    return true;
}

Dictionary VoxelEngineController::plan_counters(const schematic::PastePlan& plan) {
    Dictionary out;
    out["planned"] = static_cast<int64_t>(plan.cells.size());
    out["placed"] = static_cast<int64_t>(plan.stats.placed);
    out["substituted"] = static_cast<int64_t>(plan.stats.substituted);
    out["declined_fluid"] = static_cast<int64_t>(plan.stats.declined_fluid);
    out["declined_substitute"] = static_cast<int64_t>(plan.stats.declined_substitute);
    out["skipped"] = static_cast<int64_t>(plan.stats.skipped);
    out["unknown"] = static_cast<int64_t>(plan.stats.unknown);
    out["unresolved"] = static_cast<int64_t>(plan.stats.unresolved);
    out["air_ignored"] = static_cast<int64_t>(plan.stats.air_ignored);
    return out;
}

bool VoxelEngineController::decode_and_plan(const PackedByteArray& bytes,
                                            const Dictionary& options,
                                            int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                            schematic::PasteOptions& out_options,
                                            schematic::SchematicData& out_file,
                                            schematic::PastePlan& out_plan, std::string& error) {
    if (!ensure_minecraft_palette()) {
        error = minecraft_palette_error_;
        return false;
    }
    if (bytes.size() == 0) {
        error = "the file is empty";
        return false;
    }
    if (!schematic::load_schematic_bytes(reinterpret_cast<const uint8_t*>(bytes.ptr()),
                                         static_cast<size_t>(bytes.size()), out_file, &error)) {
        return false;
    }

    if (options.has("fluids")) out_options.fluids = static_cast<bool>(options["fluids"]);
    if (options.has("substitutes")) {
        out_options.substitutes = static_cast<bool>(options["substitutes"]);
    }
    if (options.has("replace_solid")) {
        out_options.replace_solid = static_cast<bool>(options["replace_solid"]);
    }
    if (options.has("write_air")) out_options.write_air = static_cast<bool>(options["write_air"]);
    if (options.has("max_cells")) {
        const int64_t cap = static_cast<int64_t>(options["max_cells"]);
        out_options.max_cells = cap > 0 ? static_cast<size_t>(cap) : 0;
    }

    const auto resolve = [this](const std::string& name, BlockID& out) {
        const auto found = minecraft_name_ids_.find(name);
        if (found == minecraft_name_ids_.end()) return false;
        out = found->second;
        return true;
    };
    return schematic::plan_paste(out_file, minecraft_palette_, origin_x, origin_y, origin_z,
                                 out_options, resolve, out_plan, &error);
}

Dictionary VoxelEngineController::inspect_schematic(const PackedByteArray& bytes,
                                                    const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    schematic::SchematicData file;
    schematic::PastePlan plan;
    std::string error;
    // The origin does not matter for inspection (only for the coordinates the
    // plan carries), so the plan is built from (0, 0, 0).
    if (!decode_and_plan(bytes, options, 0, 0, 0, paste_options, file, plan, error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    result["ok"] = true;
    result["file_width"] = file.width;
    result["file_height"] = file.height;
    result["file_length"] = file.length;
    result["format"] = String(schematic::block_file_format_name(file.format));
    result["format_version"] = file.format_version;
    result["data_version"] = static_cast<int64_t>(file.data_version);
    result["container"] = String(schematic::container_kind_name(file.container));
    result["data_layout"] = String(schematic::data_layout_name(file.data_layout));
    if (file.has_offset) result["offset"] = Vector3i(file.offset[0], file.offset[1], file.offset[2]);
    result["palette_states"] = static_cast<int64_t>(file.palette.size());
    result["non_air_cells"] = static_cast<int64_t>(file.non_air_cells);
    result["tile_entities"] = static_cast<int64_t>(file.tile_entity_count);
    result["entities"] = static_cast<int64_t>(file.entity_count);
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    if (!plan.empty()) {
        // Local to the file, since inspection has no anchor.
        result["min"] = Vector3i(plan.min_x, plan.min_y, plan.min_z);
        result["max"] = Vector3i(plan.max_x, plan.max_y, plan.max_z);
    }
    return result;
}

Dictionary VoxelEngineController::paste_schematic_bytes(const PackedByteArray& bytes,
                                                        int32_t origin_x, int32_t origin_y,
                                                        int32_t origin_z,
                                                        const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    schematic::SchematicData file;
    schematic::PastePlan plan;
    std::string error;
    if (!decode_and_plan(bytes, options, origin_x, origin_y, origin_z, paste_options, file, plan,
                         error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    const PasteWriteResult written = block_editor.apply_paste(plan, paste_options);

    // Both halves are reported, because they can disagree in a way the player
    // needs to see: the plan counts what the file implies, the write counts what
    // the world actually took.
    result["ok"] = true;
    result["file_width"] = file.width;
    result["file_height"] = file.height;
    result["file_length"] = file.length;
    result["format"] = String(schematic::block_file_format_name(file.format));
    result["format_version"] = file.format_version;
    // The file's own numbers as well as the world's, so a caller can tell "the
    // table could not map this" from "the world would not take it".
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    result["cells"] = static_cast<int64_t>(written.written);
    result["replaced"] = static_cast<int64_t>(written.written - written.skipped_unchanged);
    result["covered"] = static_cast<int64_t>(written.skipped_covered);
    result["unchanged"] = static_cast<int64_t>(written.skipped_unchanged);
    result["unloaded_chunks"] = static_cast<int64_t>(written.skipped_unloaded);
    result["chunks"] = static_cast<int64_t>(written.chunks_touched);
    result["undo_cells"] = static_cast<int64_t>(block_editor.paste_undo_cells());
    if (written.max_x >= written.min_x) {
        result["min"] = Vector3i(written.min_x, written.min_y, written.min_z);
        result["max"] = Vector3i(written.max_x, written.max_y, written.max_z);
    }
    return result;
}

Dictionary VoxelEngineController::undo_paste() {
    Dictionary result;
    PasteWriteResult written;
    const bool undone = block_editor.undo_paste(&written);
    result["ok"] = undone;
    if (!undone) {
        result["error"] = String("nothing to undo (no paste this session)");
        return result;
    }
    result["cells"] = static_cast<int64_t>(written.written);
    result["chunks"] = static_cast<int64_t>(written.chunks_touched);
    return result;
}

int64_t VoxelEngineController::paste_undo_cells() const {
    return static_cast<int64_t>(block_editor.paste_undo_cells());
}

// -------------------------------------------------------------------------
// Chunk scenario / editor
// -------------------------------------------------------------------------

void VoxelEngineController::clear_editor_chunks() {
    reset_runtime_state(true);
    last_player_block_x = INT32_MIN;
    last_player_block_y = INT32_MIN;
    last_player_block_z = INT32_MIN;
}

void VoxelEngineController::unload_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    uint64_t key = chunk_world.get_chunk_map().get_chunk_key(chunk_x, chunk_y, chunk_z);
    chunk_world.save_chunk_to_disk(chunk_x, chunk_y, chunk_z);
    world_updater.try_unload(key);
}

void VoxelEngineController::generate_chunk(int32_t chunk_x, int32_t chunk_y, int32_t chunk_z) {
    world_updater.generate_chunk(chunk_x, chunk_y, chunk_z, chunk_world.get_epoch());
}

// -------------------------------------------------------------------------
// Debug / perf
// -------------------------------------------------------------------------

String VoxelEngineController::get_performance_report() {
    String report = PerfReport::build(
        frame_time_accumulator,
        frame_count,
        2.0, // hardcoded interval
        chunks_processed_total,
        chunks_processed_last_interval,
        perf_timer,
        thread_pool ? thread_pool->get_worker_count() : 0,
        thread_pool ? thread_pool->get_queue_size() : 0,
        chunk_world.get_scheduler().generating_count(),
        chunk_world.get_scheduler().completed_chunk_count(),
        chunk_world.get_chunk_map().size(),
        mesh_manager.gather_render_stats()
    );
    chunks_processed_last_interval = chunks_processed_total;
    frame_count = 0;
    frame_time_accumulator = 0.0;
    perf_timer.reset_all();
    MeshBuilder::get_perf_timer().reset_all();
    ChunkGenerator::get_perf_timer().reset_all();
    MeshBuilder::reset_vertex_tracking();
    MeshBuilder::reset_greedy_vertical_stats();
    return report;
}

void VoxelEngineController::print_debug_info(double delta) {
    // Debug printing removed
}

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

void VoxelEngineController::load_world_configs() {
    TerrainParams params = world_updater.get_terrain_params();
    params.load_from_json("res://data/terrain_config.json");
    world_updater.set_terrain_params(params);

    BiomeConfig biomes;
    if (!BiomeConfig::load("res://data/biomes.json", biomes)) {
        biomes.reset_defaults();
    }
    world_updater.set_biome_config(biomes);

    VegetationConfig vegetation;
    vegetation.load("res://data/vegetation.json");
    world_updater.set_vegetation_config(vegetation);

    if (!ItemRegistry::get_instance().load_from_json("res://data/items.json")) {
        WARN_PRINT("items.json missing or unparseable; item recipes will not resolve");
    }

    recipe_book.clear();
    if (!recipe_book.load_from_json("res://data/recipes.json")) {
        WARN_PRINT("recipes.json missing or unparseable; crafting disabled");
    }
}

void VoxelEngineController::save_world_metadata() {
    const TerrainParams& params = world_updater.get_terrain_params();
    chunk_world.save_world_metadata(params);
}

bool VoxelEngineController::load_world_metadata() {
    TerrainParams params;
    int32_t chunk_version;
    if (!chunk_world.load_world_metadata(params, chunk_version)) {
        return false;
    }
    // Check for seed mismatch before applying
    if (params.seed != seed) {
        WARN_PRINT("World metadata seed mismatch: saved=" + String::num_int64(params.seed) + ", current=" + String::num_int64(seed) + ". Loading saved world may generate incoherent terrain.");
    }
    // Apply loaded params to controller state
    seed = params.seed;
    sea_level = params.sea_level;
    biome_size = params.biome_size;
    // Update world_updater with loaded params
    world_updater.set_seed(seed);
    world_updater.set_sea_level(sea_level);
    world_updater.set_biome_size(biome_size);
    return true;
}

bool VoxelEngineController::world_metadata_exists() const {
    return chunk_world.world_metadata_exists();
}

void VoxelEngineController::save_inventory(const Inventory& inventory) {
    chunk_world.save_inventory(inventory);
}

bool VoxelEngineController::load_inventory(Inventory& inventory) {
    return chunk_world.load_inventory(inventory);
}

void VoxelEngineController::flush_dirty_chunks(bool wait_for_completion, double timeout_sec) {
    chunk_world.flush_dirty_chunks(wait_for_completion, timeout_sec);
}

Dictionary VoxelEngineController::find_biome(const String& biome_name, int32_t center_x,
                                            int32_t center_z, int32_t max_radius_blocks) {
    Dictionary result;
    BiomeType target;
    if (!biome_from_name(biome_name.utf8().get_data(), target)) {
        result["found"] = false;
        return result;
    }
    int32_t out_x = 0;
    int32_t out_z = 0;
    float out_height = 0.0f;
    const bool found = world_updater.find_nearest_biome(
        target, center_x, center_z, max_radius_blocks, out_x, out_z, out_height);
    result["found"] = found;
    result["x"] = out_x;
    result["y"] = static_cast<int32_t>(std::round(out_height));
    result["z"] = out_z;
    return result;
}

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
