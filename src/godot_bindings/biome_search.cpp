#include "godot_bindings/biome_search.hpp"

#include <cmath>
#include <utility>

#include "worldgen/biome_config.hpp"  // BiomeType, biome_from_name

using namespace godot;
using namespace VoxelEngine;

void BiomeSearch::_bind_methods() {
    ClassDB::bind_method(D_METHOD("find", "biome_name", "center_x", "center_z", "max_radius"),
                         &BiomeSearch::find);
    ClassDB::bind_method(D_METHOD("cancel"), &BiomeSearch::cancel);
}

void BiomeSearch::cancel() {
    cancelled_.store(true, std::memory_order_relaxed);
}

void BiomeSearch::set_generator(std::unique_ptr<ChunkGenerator> generator) {
    generator_ = std::move(generator);
}

Dictionary BiomeSearch::find(const String& biome_name, int32_t center_x, int32_t center_z,
                             int32_t max_radius_blocks) {
    Dictionary result;
    BiomeType target;
    if (!generator_ || !biome_from_name(biome_name.utf8().get_data(), target)) {
        result["found"] = false;
        return result;
    }
    int32_t out_x = 0;
    int32_t out_z = 0;
    float out_height = 0.0f;
    ChunkGenerator::BiomeSearchReport report;
    const bool found = generator_->find_nearest_biome(target, center_x, center_z, max_radius_blocks,
                                                      out_x, out_z, out_height, &report,
                                                      &cancelled_);
    result["found"] = found;
    result["gave_up"] = report.gave_up;
    result["searched"] = report.reached_blocks;
    result["x"] = out_x;
    result["y"] = static_cast<int32_t>(std::round(out_height));
    result["z"] = out_z;
    return result;
}
