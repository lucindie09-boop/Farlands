#include "worldgen/biome_config.hpp"
#include "core/json_config.hpp"

#include <cstring>

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
#include <godot_cpp/core/error_macros.hpp>
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

// ASCII case-insensitive compare (no locale / platform dependencies).
bool iequals(const char* a, const char* b) {
    while (*a != '\0' && *b != '\0') {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + ('a' - 'A'));
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + ('a' - 'A'));
        if (ca != cb) return false;
        ++a;
        ++b;
    }
    return *a == *b;
}

} // namespace

const char* biome_name(BiomeType b) {
    switch (b) {
        case BiomeType::Ocean: return "ocean";
        case BiomeType::Hills: return "hills";
        default:               return "unknown";
    }
}

bool biome_from_name(const char* name, BiomeType& out) {
    if (name == nullptr) return false;
    for (int i = 0; i < static_cast<int>(BiomeType::Count); ++i) {
        const BiomeType b = static_cast<BiomeType>(i);
        if (iequals(name, biome_name(b))) {
            out = b;
            return true;
        }
    }
    return false;
}

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
    surfaces[static_cast<size_t>(BiomeType::Hills)] =
        {BlockIDs::GRASS, BlockIDs::DIRT, BlockIDs::MUD, BlockIDs::DIRT};

    for (auto& v : vegetation) {
        v = BiomeVegetation{};
    }
    vegetation[static_cast<size_t>(BiomeType::Hills)].tree_density  = 1.0f;
    vegetation[static_cast<size_t>(BiomeType::Hills)].tree_variants = {1.0f, 0.0f};

    for (auto& a : amplification) {
        a = BiomeAmplification{};  // 1.0 / 1.0 = neutral
    }
}

bool BiomeConfig::load(const godot::String& json_path, BiomeConfig& out) {
    out.reset_defaults();

#ifndef FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION
    std::optional<godot::Dictionary> root_opt = read_json_dictionary(json_path);
    if (!root_opt.has_value()) {
        return false;
    }
    const godot::Dictionary& root = *root_opt;

    if (root.has("underwater_surface")) {
        godot::String name = root["underwater_surface"];
        out.underwater_surface = resolve_block(name.utf8().get_data(), out.underwater_surface);
    }

    // Climate-grid thresholds. Dormant while the temperature/humidity samplers
    // are flat stubs (see chunk_generator), but kept here so the grid can be
    // tuned without recompiling once they exist.
    if (root.has("climate")) {
        godot::Dictionary c = root["climate"];
        if (c.has("temp_cold_max")) out.temp_cold_max = static_cast<float>(static_cast<double>(c["temp_cold_max"]));
        if (c.has("temp_hot_min"))  out.temp_hot_min  = static_cast<float>(static_cast<double>(c["temp_hot_min"]));
        if (c.has("hum_dry_max"))   out.hum_dry_max   = static_cast<float>(static_cast<double>(c["hum_dry_max"]));
        if (c.has("hum_humid_min")) out.hum_humid_min = static_cast<float>(static_cast<double>(c["hum_humid_min"]));
    }

    if (root.has("biomes")) {
        // Keyed by biome name (see biome_name) — the name IS the identity, so
        // there is no index to keep in sync with the BiomeType enum.
        godot::Dictionary biomes = root["biomes"];
        godot::Array names = biomes.keys();
        for (int i = 0; i < static_cast<int>(names.size()); ++i) {
            godot::String name = names[i];
            BiomeType biome;
            if (!biome_from_name(name.utf8().get_data(), biome)) {
                WARN_PRINT("biomes.json: skipping unknown biome name \"" + name + "\"");
                continue;
            }
            const size_t ix = static_cast<size_t>(biome);
            godot::Dictionary b = biomes[name];

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
            if (b.has("height_amplification")) {
                out.amplification[ix].height = static_cast<float>(static_cast<double>(b["height_amplification"]));
            }
            if (b.has("weirdness_amplification")) {
                out.amplification[ix].weirdness = static_cast<float>(static_cast<double>(b["weirdness_amplification"]));
            }
            if (b.has("tree_density")) {
                out.vegetation[ix].tree_density = static_cast<float>(static_cast<double>(b["tree_density"]));
            }
            if (b.has("tree_variants")) {
                godot::Dictionary tv = b["tree_variants"];
                // Fixed-size contract: exactly 2 weights, order-fixed
                // (index 0 = oak, index 1 = spruce). Extra keys (e.g. "birch")
                // are ignored; warn so they are not silently dead config.
                const char* keys[2] = {"oak", "spruce"};
                for (int v = 0; v < 2; ++v) {
                    if (tv.has(godot::String(keys[v]))) {
                        out.vegetation[ix].tree_variants[static_cast<size_t>(v)] =
                            static_cast<float>(static_cast<double>(tv[godot::String(keys[v])]));
                    }
                }
                if (tv.size() > 2) {
                    WARN_PRINT("biomes.json tree_variants has more than 2 entries; extras are ignored (exactly 2: oak, spruce)");
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
