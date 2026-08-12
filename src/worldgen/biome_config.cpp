#include "worldgen/biome_config.hpp"

#include <cstring>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/json.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/variant.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#endif

namespace VoxelEngine {

namespace {

// Resolve a block name from block_definitions.json. Unresolvable names (or
// air, which is never a valid terrain material) fall back to `fallback`.
BlockID resolve_block(const char* name, BlockID fallback) {
    const BlockRegistry& reg = BlockRegistry::get_instance();
    BlockID id = reg.get_block_id_by_name(name);
    return (id == BlockIDs::AIR) ? fallback : id;
}

} // namespace

BiomeConfig::BiomeConfig() {
    reset_defaults();
}

void BiomeConfig::reset_defaults() {
    underwater_surface = BlockIDs::SAND;
    temp_cold_max      = 0.43f;
    temp_hot_min       = 0.57f;
    hum_dry_max        = 0.43f;
    hum_humid_min      = 0.57f;

    surfaces[static_cast<size_t>(BiomeType::Ocean)] =
        {BlockIDs::SAND, BlockIDs::SAND, BlockIDs::SAND, BlockIDs::SAND};
    surfaces[static_cast<size_t>(BiomeType::Beach)] =
        {BlockIDs::SAND, BlockIDs::SAND, BlockIDs::WET_SAND, BlockIDs::WET_SAND_FULL};
    surfaces[static_cast<size_t>(BiomeType::Plains)] =
        {BlockIDs::GRASS, BlockIDs::DIRT, BlockIDs::MUD, BlockIDs::DIRT};
    surfaces[static_cast<size_t>(BiomeType::Forest)] =
        {BlockIDs::GRASS, BlockIDs::DIRT, BlockIDs::MUD, BlockIDs::DIRT};
    surfaces[static_cast<size_t>(BiomeType::Desert)] =
        {BlockIDs::SAND, BlockIDs::SAND, BlockIDs::SAND, BlockIDs::SAND};

    for (auto& v : vegetation) {
        v = BiomeVegetation{};
    }
    vegetation[static_cast<size_t>(BiomeType::Plains)].tree_density  = 1.0f;
    vegetation[static_cast<size_t>(BiomeType::Plains)].tree_variants = {1.0f, 0.0f, 0.0f};
    vegetation[static_cast<size_t>(BiomeType::Forest)].tree_density  = 1.0f;
    vegetation[static_cast<size_t>(BiomeType::Forest)].tree_variants = {0.5f, 0.5f, 0.0f};
}

bool BiomeConfig::load(const godot::String& json_path, BiomeConfig& out) {
    out.reset_defaults();

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    godot::Ref<godot::FileAccess> file = godot::FileAccess::open(json_path, godot::FileAccess::READ);
    if (!file.is_valid()) {
        return false;
    }
    godot::String text = file->get_as_text();
    file->close();

    godot::Variant parsed = godot::JSON::parse_string(text);
    if (parsed.get_type() != godot::Variant::DICTIONARY) {
        return false;
    }
    godot::Dictionary root = parsed;

    if (root.has("underwater_surface")) {
        godot::String name = root["underwater_surface"];
        out.underwater_surface = resolve_block(name.utf8().get_data(), out.underwater_surface);
    }
    if (root.has("temp_cold_max"))  out.temp_cold_max  = static_cast<float>(static_cast<double>(root["temp_cold_max"]));
    if (root.has("temp_hot_min"))   out.temp_hot_min   = static_cast<float>(static_cast<double>(root["temp_hot_min"]));
    if (root.has("hum_dry_max"))    out.hum_dry_max    = static_cast<float>(static_cast<double>(root["hum_dry_max"]));
    if (root.has("hum_humid_min"))  out.hum_humid_min  = static_cast<float>(static_cast<double>(root["hum_humid_min"]));

    if (root.has("biomes")) {
        godot::Array biomes = root["biomes"];
        for (int i = 0; i < static_cast<int>(biomes.size()); ++i) {
            godot::Dictionary b = biomes[i];
            if (!b.has("index")) continue;
            const int64_t idx = b["index"];
            if (idx < 0 || idx >= static_cast<int64_t>(BiomeType::Count)) continue;
            const size_t ix = static_cast<size_t>(idx);

            if (b.has("surface")) {
                godot::String name = b["surface"];
                out.surfaces[ix].surface = resolve_block(name.utf8().get_data(), out.surfaces[ix].surface);
            }
            if (b.has("subsurface")) {
                godot::String name = b["subsurface"];
                out.surfaces[ix].subsurface = resolve_block(name.utf8().get_data(), out.surfaces[ix].subsurface);
            }
            if (b.has("near_water_surface")) {
                godot::String name = b["near_water_surface"];
                out.surfaces[ix].near_water_surface = resolve_block(name.utf8().get_data(), out.surfaces[ix].near_water_surface);
            }
            if (b.has("near_water_subsurface")) {
                godot::String name = b["near_water_subsurface"];
                out.surfaces[ix].near_water_subsurface = resolve_block(name.utf8().get_data(), out.surfaces[ix].near_water_subsurface);
            }
            if (b.has("tree_density")) {
                out.vegetation[ix].tree_density = static_cast<float>(static_cast<double>(b["tree_density"]));
            }
            if (b.has("tree_variants")) {
                godot::Dictionary tv = b["tree_variants"];
                const char* keys[3] = {"oak", "spruce", "birch"};
                for (int v = 0; v < 3; ++v) {
                    if (tv.has(godot::String(keys[v]))) {
                        out.vegetation[ix].tree_variants[static_cast<size_t>(v)] =
                            static_cast<float>(static_cast<double>(tv[godot::String(keys[v])]));
                    }
                }
            }
        }
    }
    return true;
#else
    (void)json_path;
    return false;
#endif
}

} // namespace VoxelEngine
