// What a column is: the biome search, the per-column samplers, the chunk-scoped
// climate and amplification lattices they interpolate over, and the surface /
// subsurface material rules. The chunk writer is in chunk_generator.cpp and the
// cheap height and solidity queries are in chunk_generator_terrain.cpp.

#include "worldgen/chunk_generator.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VoxelEngine {

namespace {

// One walk of concentric Chebyshev rings `step` blocks apart, from ring
// `from_ring` to `to_ring` around (cx, cz). Every sample is offered to `hit`,
// which answers true to stop the walk. Each ring starts at a side midpoint, so a
// match comes back near the ring's nearest point rather than at a corner.
int64_t cell_key(int32_t x, int32_t z) {
    return (static_cast<int64_t>(x) << 32) | static_cast<int64_t>(static_cast<uint32_t>(z));
}

template <typename Hit>
bool walk_rings(int32_t step, int32_t from_ring, int32_t to_ring, int32_t cx, int32_t cz,
                Hit&& hit) {
    for (int32_t r = from_ring; r <= to_ring; ++r) {
        if (r == 0) {
            if (hit(cx, cz)) return true;
            continue;
        }
        for (int32_t i = -r; i <= r; ++i) {
            if (hit(cx + i * step, cz - r * step)) return true;
            if (hit(cx + i * step, cz + r * step)) return true;
        }
        for (int32_t j = -r + 1; j <= r - 1; ++j) {
            if (hit(cx - r * step, cz + j * step)) return true;
            if (hit(cx + r * step, cz + j * step)) return true;
        }
    }
    return false;
}

}  // namespace

bool ChunkGenerator::find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                                        int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                                        float& out_height, BiomeSearchReport* report,
                                        const std::atomic<bool>* cancel) const {
    // Phase 1: concentric Chebyshev rings at 16-block step, walked from a side
    // midpoint so hits come back roughly nearest-first, and stopping at the first
    // matching ring. 16 blocks is the finest step that still finds a pond, and
    // every probe is a climate-plus-surface sample (~0.3 ms), so a far ocean is
    // thousands of them and the better part of a second: this runs off the frame
    // (BiomeSearch), and the step is the one number to change here.
    constexpr int32_t STEP = biome_search::kStep;
    constexpr int32_t REFINE = 4;
    constexpr int32_t REFINE_SPAN = 16;  // blocks on each side of the found column
    constexpr int32_t ADVERTISED_RING = biome_search::kRadiusBlocks / STEP;
    // Rings 0..ADVERTISED_RING cost 1 + 4*R*(R+1) probes. The advertised radius
    // must fit the cap, or a complete miss would answer "not found" after a walk
    // that never covered what the caller was told it searched.
    static_assert(1 + 4 * ADVERTISED_RING * (ADVERTISED_RING + 1) <= biome_search::kMaxProbes,
                  "the advertised biome search radius must fit the probe budget");

    // A miss is the case that used to run unbounded: proving there is no plains
    // inside 3,000 blocks is 140,000 probes at 16 blocks, forty seconds of
    // somebody's thread. So the walk stops at MAX_PROBES or MAX_SECONDS and says
    // it gave up if it could not cover the radius, while the chat asks for the
    // radius these probes DO cover (biome_search::kRadiusBlocks) so its answer
    // never promises a horizon the walk cannot reach.
    constexpr int32_t MAX_PROBES = biome_search::kMaxProbes;
    constexpr double MAX_SECONDS = 4.0;
    BiomeSearchReport local_report;
    BiomeSearchReport& out = report != nullptr ? *report : local_report;
    const auto started = std::chrono::steady_clock::now();
    bool aborted = false;
    BiomeType last_biome = BiomeType::Ocean;
    // One probe, charged to the budget: false when the search must stop (budget,
    // clock or the caller's cancel), leaving last_biome as what it classified.
    auto probe = [&](int32_t wx, int32_t wz) -> bool {
        if (aborted) return false;
        if (cancel != nullptr && cancel->load(std::memory_order_relaxed)) {
            aborted = true;
            return false;
        }
        const bool spent = out.probes >= MAX_PROBES ||
                           ((out.probes % 64) == 0 &&
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          started).count() >= MAX_SECONDS);
        if (spent) {
            aborted = true;
            return false;
        }
        ++out.probes;
        last_biome = classify_biome(wx, wz);
        const int32_t dx = std::abs(wx - center_x);
        const int32_t dz = std::abs(wz - center_z);
        out.reached_blocks = std::max(out.reached_blocks, std::max(dx, dz));
        return true;
    };

    // The patch underfoot: asking where the nearest plains are from the middle of
    // the plains answered "here", which is not a place to go. Standing in one, the
    // search skips the columns connected to the one you are on and answers the
    // nearest of the same kind beyond it. The flood is bounded, so a patch larger
    // than MAX_PATCH_PROBES is skipped as far as the flood reached and the answer
    // is a column of the same kind past it - still somewhere to go, a few hundred
    // blocks out rather than the ground under your feet.
    constexpr std::size_t MAX_PATCH_PROBES = 512;
    std::unordered_set<int64_t> patch;    // the target-biome columns of that patch
    std::unordered_set<int64_t> visited;  // every column the flood has classified
    if (probe(center_x, center_z) && last_biome == target) {
        std::vector<std::pair<int32_t, int32_t>> frontier;
        frontier.emplace_back(center_x, center_z);
        patch.insert(cell_key(center_x, center_z));
        visited.insert(cell_key(center_x, center_z));
        // Eight-ways: a body of water connected only at a corner is still the
        // same water, and four-ways the flood left the corner cell unmarked and
        // the walk answered it - a four-block hop to the same pond.
        constexpr int32_t NEIGHBOURS[8][2] = {{STEP, 0}, {-STEP, 0}, {0, STEP}, {0, -STEP},
                                             {STEP, STEP}, {STEP, -STEP}, {-STEP, STEP},
                                             {-STEP, -STEP}};
        for (std::size_t i = 0; i < frontier.size() && patch.size() < MAX_PATCH_PROBES; ++i) {
            const int32_t x = frontier[i].first;
            const int32_t z = frontier[i].second;
            for (const auto& offset : NEIGHBOURS) {
                const int32_t nx = x + offset[0];
                const int32_t nz = z + offset[1];
                const int64_t key = cell_key(nx, nz);
                if (!visited.insert(key).second) continue;
                if (!probe(nx, nz) || last_biome != target) continue;
                patch.insert(key);
                frontier.emplace_back(nx, nz);
                if (patch.size() >= MAX_PATCH_PROBES) break;
            }
        }
    }

    auto match = [&](int32_t wx, int32_t wz) -> bool {
        if (!probe(wx, wz)) return false;  // out of budget: not a match
        if (last_biome != target) return false;
        return patch.find(cell_key(wx, wz)) == patch.end();
    };

    int32_t cand_x = center_x;
    int32_t cand_z = center_z;
    const int32_t max_ring = std::max(0, max_radius_blocks / STEP);
    bool found = false;
    // The walk stops on either a hit or the budget, so the two are told apart by
    // `found` and not by what walk_rings returns.
    walk_rings(STEP, 0, max_ring, center_x, center_z, [&](int32_t wx, int32_t wz) {
        if (match(wx, wz)) {
            cand_x = wx;
            cand_z = wz;
            found = true;
            return true;
        }
        return aborted;  // out of budget: stop the walk
    });
    if (!found) {
        out.gave_up = aborted;
        return false;
    }

    // Phase 2: fine 4-block scan of the area around the coarse hit, keeping the
    // closest matching column to the requested center.
    int64_t best_dist2 = std::numeric_limits<int64_t>::max();
    int32_t best_x = cand_x;
    int32_t best_z = cand_z;
    for (int32_t wx = cand_x - REFINE_SPAN; wx <= cand_x + REFINE_SPAN; wx += REFINE) {
        for (int32_t wz = cand_z - REFINE_SPAN; wz <= cand_z + REFINE_SPAN; wz += REFINE) {
            if (!probe(wx, wz)) break;  // out of budget: keep the best so far
            if (last_biome != target) continue;
            if (patch.find(cell_key(wx, wz)) != patch.end()) continue;
            const int64_t dx = static_cast<int64_t>(wx) - center_x;
            const int64_t dz = static_cast<int64_t>(wz) - center_z;
            const int64_t d2 = dx * dx + dz * dz;
            if (d2 < best_dist2) {
                best_dist2 = d2;
                best_x = wx;
                best_z = wz;
            }
        }
    }

    out_x = best_x;
    out_z = best_z;
    out_height = sample_column(best_x, best_z).height;
    return true;
}

BiomeType ChunkGenerator::classify_biome(int32_t world_x, int32_t world_z) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float t = sample_temperature(x, z);
    const float h = sample_humidity(x, z);
    return column_biome(t, h, sample_land_shape(x, z, t, h),
                        params.climate_blend_radius_nodes > 0
                            ? blend_amplification_at(world_x, world_z)
                            : BiomeAmplification{})
        .biome;
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column(int32_t world_x, int32_t world_z) const {
    const float x = static_cast<float>(world_x);
    const float z = static_cast<float>(world_z);
    const float t = sample_temperature(x, z);
    const float h = sample_humidity(x, z);
    return sample_column_with_climate(t, h,
                                      sample_land_shape(x, z, t, h),
                                      params.climate_blend_radius_nodes > 0
                                          ? blend_amplification_at(world_x, world_z)
                                          : BiomeAmplification{});
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column_with_climate(
        float temperature, float humidity, float land_height,
        const BiomeAmplification& blended) const {
    // The continentalness sample that used to sit here was dead work:
    // biome_from_climate ignores its `cont` argument and nothing read
    // ColumnSample::cont, so it cost a 12,000-block noise read per column (1,024
    // a chunk) for a value with no consumer - and the field is gone with it.
    // sample_continentalness stays for the debug accessor and
    // quick_height_estimate, which still ask for the real value.
    const float saved_land_height = land_height;
    const ColumnBiome column = column_biome(temperature, humidity, land_height, blended);
    const bool ocean = column.biome == BiomeType::Ocean;
    // Water tops out at the sea level the terrain was generated against. In a
    // squished world that is the slice centre, not the configured level, and
    // reading params.sea_level here would drown every squished column under
    // 150 blocks of water.
    const float water_level = ocean ? squish::sea_level(params) : -1.0f;

    // The one place the macro height becomes the terrain's height: the squish,
    // identity when the toggle is off.
    return ColumnSample{column.biome, squish::height(params, column.height), water_level, false,
                        saved_land_height, temperature, humidity};
}

ChunkGenerator::ColumnBiome ChunkGenerator::column_biome(float temperature, float humidity,
                                                         float land_height,
                                                         const BiomeAmplification& blended) const {
    // Biomes are selected and shape the terrain FIRST: the climate grid picks a
    // land biome everywhere (ocean never enters the grid), and the column's
    // height is that biome's amplification of the macro surface around sea
    // level (1.0 = neutral; the blended field ramps across borders when
    // blending is enabled).
    const BiomeType land_biome = biome_from_climate(temperature, humidity, 0.0f);
    const BiomeAmplification amp = amplification_for(land_biome, blended);
    // Amplified in RAW space: the ocean test compares against the configured sea
    // level there, and the squish map is monotone about exactly that level (its
    // centre is sea level), so deciding before or after mapping is the same
    // call. Mapping last also keeps the bedrock floor clamp in the units the
    // bedrock layer is actually placed in.
    float height = params.sea_level + (land_height - params.sea_level) * amp.height;
    height = std::max(static_cast<float>(params.bedrock_height) + 1.0f, height);

    // Oceans are the last step in the generation scheme: any column whose
    // terrain, after the land biome altered it, still sits below sea level
    // becomes an ocean biome — water fills up to sea level and the biome
    // switches to the ocean set (water + sand surfaces). The sea bed keeps
    // the height the land biome gave it, so the floor is shaped by the
    // climate: temperate (plains) basins shelf out shallow, cold/hot (hills)
    // basins drop steeply.
    return ColumnBiome{height < params.sea_level ? BiomeType::Ocean : land_biome, height};
}


BlockID ChunkGenerator::get_surface_block(BiomeType biome, int32_t y, bool has_surface_water, bool near_water) const {
    if (has_surface_water) {
        return biome_config.underwater_surface;
    }
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_surface : s.surface;
}

BlockID ChunkGenerator::get_subsurface_block(BiomeType biome, bool near_water) const {
    const BiomeSurface& s = biome_config.surfaces[static_cast<size_t>(biome)];
    return near_water ? s.near_water_subsurface : s.subsurface;
}

} // namespace VoxelEngine
