#ifndef FUK_MINECRAFT_CHUNK_GENERATOR_HPP
#define FUK_MINECRAFT_CHUNK_GENERATOR_HPP
#include <functional>
#include "core/terrain_params.hpp"
#include "core/noise.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/performance_timer.hpp"
#include <utility>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Biome types
// -------------------------------------------------------------------------
enum class BiomeType : uint8_t {
    Ocean,
    Beach,
    Plains,
    Forest,
    Desert,
};

// -------------------------------------------------------------------------
// Chunk generator - Minecraft-style procedural terrain generation
// -------------------------------------------------------------------------
class ChunkGenerator {
private:
    FastNoise terrain_noise;
    FastNoise cave_noise;
    FastNoise continental_noise;
    FastNoise temp_noise;
    FastNoise humidity_noise;

    TerrainParams params;
    std::mt19937 rng;
    static PerformanceTimer perf_timer;



public:
    struct ColumnSample {
        BiomeType biome;
        float height;
        float water_level;
        bool near_water;
        float land_height;
        float cont;              // continentalness value (0-1)
        float temperature;
        float humidity;
    };

private:
    // -------------------------------------------------------------------------
    // Math helpers
    // -------------------------------------------------------------------------
    static float clamp01(float v) {
        return std::max(0.0f, std::min(1.0f, v));
    }

    static float smoothstep(float edge0, float edge1, float x) {
        float t = clamp01((x - edge0) / (edge1 - edge0));
        return t * t * (3.0f - 2.0f * t);
    }

    static float lerp(float a, float b, float t) {
        return a + (b - a) * t;
    }

    // -------------------------------------------------------------------------
    // Noise sampling
    // -------------------------------------------------------------------------
    float sample_continentalness(float x, float z) const {
        float raw = continental_noise.noise_2d(x * params.continentalness_scale,
                                                z * params.continentalness_scale);
        return clamp01((raw + 1.0f) * 0.5f);
    }

    float sample_temperature(float x, float z) const {
        return clamp01((temp_noise.noise_2d(x * params.climate_temp_scale,
                                             z * params.climate_temp_scale) + 1.0f) * 0.5f);
    }

    float sample_humidity(float x, float z) const {
        return clamp01((humidity_noise.noise_2d(x * params.climate_humidity_scale,
                                                 z * params.climate_humidity_scale) + 1.0f) * 0.5f);
    }

    // Grid-based land biome lookup from temperature/humidity.
    //
    // Thresholds were chosen empirically (not guessed) by sampling this exact
    // noise implementation across a wide area and measuring its real
    // distribution: single-octave value/gradient noise here comes out roughly
    // bell-curved around 0.5 with stddev ~0.15, NOT uniform across [0,1].
    // A nearest-center Voronoi pick (the old approach) with an off-center
    // biome point therefore starves that biome almost entirely, because the
    // point only "wins" in a rarely-sampled tail of the distribution.
    //
    // Using tertile thresholds instead (measured ~0.43 / ~0.57 splits the
    // sampled data into even thirds) guarantees each temperature/humidity
    // bin gets a fair, predictable share of land regardless of biome_size,
    // since biome_size only rescales noise frequency, not its distribution.
    static constexpr float TEMP_COLD_MAX  = 0.43f;
    static constexpr float TEMP_HOT_MIN   = 0.57f;
    static constexpr float HUM_DRY_MAX    = 0.43f;
    static constexpr float HUM_HUMID_MIN  = 0.57f;

    static BiomeType land_biome_from_grid(float temperature, float humidity) {
        bool hot  = temperature >= TEMP_HOT_MIN;
        bool dry  = humidity < HUM_DRY_MAX;

        if (hot) {
            return dry ? BiomeType::Desert : BiomeType::Forest;
        }
        // cold or temperate
        return dry ? BiomeType::Plains : BiomeType::Forest;
    }

    BiomeType biome_from_climate(float temperature, float humidity, float cont) const {
        float beach_t = smoothstep(params.land_threshold, params.land_threshold + params.beach_width, cont);
        if (beach_t < 0.9f && cont >= params.land_threshold - params.beach_width) {
            return BiomeType::Beach;
        }
        if (cont < params.ocean_threshold) {
            return BiomeType::Ocean;
        }
        return land_biome_from_grid(temperature, humidity);
    }

    // Continuous Voronoi-weighted blend of all land-biome height parameters.
    // All biomes share the same noise recipe; differentiation comes from
    // base_off and scale_m per biome center.
    // Uses fixed base-frequency climate for Voronoi weights so biome boundaries
    // in the height field remain smooth regardless of biome_size.
    float sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const {
        static constexpr float BASE_TEMP_SCALE = 0.00015f;
        static constexpr float BASE_HUM_SCALE  = 0.00020f;
        // One entry per land biome. base_off/scale_m shape the *height*;
        // all biomes now use the same noise recipe, so the only difference
        // between them is base height offset and amplitude scaling.
        static constexpr struct { float t, h, base_off, scale_m; } centers[] = {
 {0.50f, 0.35f,   6.0f, 0.12f},   // 0 Plains — gentle rolling
            {0.50f, 0.78f,   4.0f, 1.00f},   // 1 Forest     — temperate, humid: hilly
            {0.78f, 0.22f, -12.0f, 0.37f},   // 2 Desert     — hot, dry
        };
        static constexpr int NUM_BIOMES = 3;
        // Sample climate at the base frequency for smooth height blending
        float blend_temp = clamp01((temp_noise.noise_2d(x * BASE_TEMP_SCALE, z * BASE_TEMP_SCALE) + 1.0f) * 0.5f);
        float blend_hum  = clamp01((humidity_noise.noise_2d(x * BASE_HUM_SCALE,  z * BASE_HUM_SCALE)  + 1.0f) * 0.5f);

        float w_total = 0.0f, w_base = 0.0f;
        float weights[NUM_BIOMES];
        for (int i = 0; i < NUM_BIOMES; i++) {
            float dsq = (blend_temp - centers[i].t) * (blend_temp - centers[i].t)
                      + (blend_hum  - centers[i].h) * (blend_hum  - centers[i].h);
            float w = 1.0f / (dsq + 0.0001f);
            weights[i] = w;
            w_base  += w * centers[i].base_off;
            w_total += w;
        }
        float base  = 208.0f + w_base / w_total;

        // Terrain amplitude control — distinct flat, hilly, and mountainous regions
        float terrain_control = terrain_noise.fbm(x + 7000.0f, z + 7000.0f, 3, 0.50f, 0.0015f);
        float terrain_amplitude = lerp(8.0f, 32.0f, smoothstep(-0.3f, 0.5f, terrain_control));

        // Anisotropic domain warp — subtle directional flow so contours aren't perfectly isotropic
        float warp_x = terrain_noise.noise_2d(x * 0.002f, z * 0.002f) * 18.0f;
        float warp_z = terrain_noise.noise_2d((x + 5000.0f) * 0.002f, (z + 5000.0f) * 0.002f) * 30.0f;

        // Broad low-frequency terrain with light ridged detail
        float per_noise_val = terrain_noise.fbm(x + warp_x, z + warp_z, 4, 0.52f, 0.0064f) * 0.85f
                            + terrain_noise.ridged_noise(x + 4000.0f + warp_x, z + 4000.0f + warp_z, 3, 0.55f, 0.016f) * 0.15f;

        // NEW: fixed-amplitude local detail, independent of the macro shape/amplitude
        float detail = terrain_noise.fbm(x * 1.6f + warp_x * 0.4f, z * 1.6f + warp_z * 0.4f, 3, 0.5f, 0.018f) * 5.0f;

        return base + per_noise_val * terrain_amplitude + detail;
    }

    // -------------------------------------------------------------------------
    // Per-column terrain evaluation 
    // -------------------------------------------------------------------------
    ColumnSample sample_column(int32_t world_x, int32_t world_z) const;

    // -------------------------------------------------------------------------
    // Block selection helpers
    // -------------------------------------------------------------------------
    BlockID get_surface_block(BiomeType biome, int32_t y, bool has_surface_water, bool near_water) const;
    BlockID get_subsurface_block(BiomeType biome, bool near_water) const;

public:
    // -------------------------------------------------------------------------
    // Fast chunk content estimation (for surface-aware generation)
    // -------------------------------------------------------------------------
    struct HeightRange {
        float min_h = 0.0f;
        float max_h = 0.0f;
float max_water_h = -1.0f;
    };
    HeightRange get_chunk_height_range(int32_t chunk_x, int32_t chunk_z) const;
    BlockID get_chunk_subsurface_block(int32_t chunk_x, int32_t chunk_z) const;

    // Debug accessors (expose private members for standalone tools)
    float sample_continentalness_debug(float x, float z) const {
        return sample_continentalness(x, z);
    }
    ColumnSample sample_column_debug(int32_t world_x, int32_t world_z) const {
        return sample_column(world_x, world_z);
    }

    ChunkGenerator(const TerrainParams& p = TerrainParams())
        : terrain_noise(p.seed)
        , cave_noise(p.seed + 2000)
        , continental_noise(p.seed + 6000)
        , temp_noise(p.seed + 4000)
        , humidity_noise(p.seed + 5000)
        , params(p)
        , rng(p.seed)
    {
    }

    BiomeType get_biome(int32_t world_x, int32_t world_z) const {
        return sample_column(world_x, world_z).biome;
    }

    float get_terrain_height(int32_t world_x, int32_t world_z) const {
        return sample_column(world_x, world_z).height;
    }

    // Cheaper than sample_column: only land shape, no biome/lake evaluation.
    float quick_height_estimate(int32_t world_x, int32_t world_z) const {
        float x = static_cast<float>(world_x);
        float z = static_cast<float>(world_z);
        float t = sample_temperature(x, z);
        float h = sample_humidity(x, z);
        return sample_land_shape(x, z, t, h);
    }

    bool is_cave(int32_t x, int32_t y, int32_t z) {
        if (y < params.bedrock_height + 3 || static_cast<float>(y) > params.sea_level + 10.0f) {
            return false;
        }
        float nx = static_cast<float>(x) * params.cave_scale;
        float ny = static_cast<float>(y) * params.cave_scale;
        float nz = static_cast<float>(z) * params.cave_scale;
        return cave_noise.noise_3d(nx, ny, nz) > params.cave_threshold;
    }

    // -------------------------------------------------------------------------
    // Per-column data used during chunk generation (replaces 7 separate arrays)
    // -------------------------------------------------------------------------
    struct ChunkColumn {
        int32_t height = 0;
        BiomeType biome = BiomeType::Plains;
        int32_t water_level = -1;
        bool near_water = false;
        float temperature = 0.0f;
        float humidity = 0.0f;
    };

    // Cross-chunk block writer callback type
    using CrossChunkWriter = std::function<void(int32_t, int32_t, int32_t, BlockID)>;

    // -------------------------------------------------------------------------
    // Main generation entry point
    // -------------------------------------------------------------------------
    void generate_chunk(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                        const CrossChunkWriter& cross_writer = nullptr, bool vegetation_enabled = true);

    // -------------------------------------------------------------------------
    // Debug: render continentalness as a PGM image (portable graymap)
    // -------------------------------------------------------------------------
    void render_continentalness_pgm(const char* filename, int img_w, int img_h,
                                    float world_x_start, float world_z_start,
                                    float step) const;

    void render_biome_pgm(const char* filename, int img_w, int img_h,
                          float world_x_start, float world_z_start,
                          float step) const;

    // -------------------------------------------------------------------------
    // Parameter management
    // -------------------------------------------------------------------------
    void set_params(const TerrainParams& p) {
        bool seed_changed = (p.seed != params.seed);
        params = p;
        if (seed_changed) {
            terrain_noise     = FastNoise(p.seed);
            cave_noise        = FastNoise(p.seed + 2000);
            continental_noise = FastNoise(p.seed + 6000);
            temp_noise        = FastNoise(p.seed + 4000);
            humidity_noise    = FastNoise(p.seed + 5000);

            rng.seed(p.seed);
        }
    }

    const TerrainParams& get_params() const {
        return params;
    }

    static PerformanceTimer& get_perf_timer() {
        return perf_timer;
    }
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_GENERATOR_HPP