#ifndef FUK_MINECRAFT_CHUNK_GENERATOR_HPP
#define FUK_MINECRAFT_CHUNK_GENERATOR_HPP
#include <functional>
#include "core/terrain_params.hpp"
#include "core/noise.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/performance_timer.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/vegetation_config.hpp"
#include <utility>
#include <cstdio>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace VoxelEngine {

// -------------------------------------------------------------------------
// Chunk generator - Minecraft-style procedural terrain generation
// -------------------------------------------------------------------------
class ChunkGenerator {
private:
    // TEMP: set false to disable cave carving (see is_cave).
    static constexpr bool kCavesEnabled = false;
    FastNoise terrain_noise;
    FastNoise cave_noise;
    FastNoise continental_noise;
    FastNoise temp_noise;
    FastNoise humidity_noise;
    FastNoise density_noise;
    FastNoise density_warp_noise;
    FastNoise weirdness_noise;

    TerrainParams params;
    BiomeConfig biome_config;
    VegetationConfig vegetation_config;
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

    // Largest lattice node coordinate <= v (floor division, negative-safe).
    static constexpr int32_t lattice_base(int32_t v) {
        const int32_t q = v / SHAPE_LATTICE_SPACING;
        const int32_t r = v % SHAPE_LATTICE_SPACING;
        return (r < 0 ? q - 1 : q) * SHAPE_LATTICE_SPACING;
    }

    // Trilinear interpolation over the 8 corners of a lattice cell. Corner
    // order and lerp order are fixed so every consumer (chunk lattice, single
    // point queries) computes bit-identical values.
    static float trilinear_interp(
        float v000, float v100, float v010, float v110,
        float v001, float v101, float v011, float v111,
        float fx, float fy, float fz) {
        const float x00 = lerp(v000, v100, fx);
        const float x01 = lerp(v010, v110, fx);
        const float x10 = lerp(v001, v101, fx);
        const float x11 = lerp(v011, v111, fx);
        const float y0 = lerp(x00, x01, fy);
        const float y1 = lerp(x10, x11, fy);
        return lerp(y0, y1, fz);
    }

    // Density from its components (macro delta + 3D shape displacement).
    static float density_from_shape(float delta, float shape_strength, float shape) {
        const float surface_distance = std::abs(delta);
        const float surface_band =
            1.0f - smoothstep(SURFACE_BAND_INNER, SURFACE_BAND_OUTER, surface_distance);
        return delta + shape * shape_strength * surface_band;
    }

    // -------------------------------------------------------------------------
    // Signed 3D density field
    //
    // The macro heightmap stays the base surface (density = surface_y - y);
    // a normalized 3D fBm field deforms only a band around that surface. The
    // "weirdness" mask (very low frequency 2D) decides where the deformation
    // is strong enough to produce overhangs/shelves vs. mostly-plain terrain.
    // -------------------------------------------------------------------------
    static constexpr float DENSITY_MARGIN      = 12.0f; // max 3D displacement + headroom
    static constexpr float SURFACE_BAND_INNER  = 9.0f;
    static constexpr float SURFACE_BAND_OUTER  = 28.0f;
    static constexpr float SHAPE_STRENGTH_MIN  = 1.5f;
    static constexpr float SHAPE_STRENGTH_MAX  = 10.0f;
    static constexpr float SHAPE_FREQUENCY     = 0.026f; // ~38-block horizontal feature scale
    static constexpr float SHAPE_Y_ANISOTROPY  = 1.35f;  // ~0.035 effective vertical scale
    static constexpr float WEIRDNESS_SCALE     = 0.0012f;
    static constexpr float WEIRDNESS_LOW       = -0.20f;
    static constexpr float WEIRDNESS_HIGH      = 0.55f;

    // The 3D shape noise is stored on a 4x4x4 world-aligned lattice and
    // trilinearly interpolated per voxel. SPACING divides the chunk size, so
    // lattice nodes always land on the same world coordinates on both sides of
    // a chunk boundary — the interpolated field is mathematically identical
    // across chunk seams. Only the noise sampling is coarse; the density field
    // and the final block grid stay full resolution.
    static constexpr int32_t SHAPE_LATTICE_SPACING = 4;

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
    // Thresholds live in data/biomes.json (BiomeConfig).
    BiomeType land_biome_from_grid(float temperature, float humidity) const {
        bool hot  = temperature >= biome_config.temp_hot_min;
        bool dry  = humidity < biome_config.hum_dry_max;

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
    // base_off and scale_m per biome center (data/terrain_config.json).
    // Uses fixed base-frequency climate for Voronoi weights so biome boundaries
    // in the height field remain smooth regardless of biome_size.
    float sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const {
        // Sample climate at the base frequency for smooth height blending
        float blend_temp = clamp01((temp_noise.noise_2d(x * params.climate_temp_base_scale,
                                                        z * params.climate_temp_base_scale) + 1.0f) * 0.5f);
        float blend_hum  = clamp01((humidity_noise.noise_2d(x * params.climate_humidity_base_scale,
                                                            z * params.climate_humidity_base_scale) + 1.0f) * 0.5f);

        const size_t num_biomes = params.height_centers.size();
        float w_total = 0.0f, w_base = 0.0f, w_scale = 0.0f;
        // Exactly 3 height centers (order-fixed, see terrain_params.cpp loader).
        float weights[3];
        for (size_t i = 0; i < num_biomes; i++) {
            const HeightCenter& c = params.height_centers[i];
            float dsq = (blend_temp - c.temp) * (blend_temp - c.temp)
                      + (blend_hum  - c.hum)  * (blend_hum  - c.hum);
            float w = 1.0f / (dsq + 0.0001f);
            weights[i] = w;
            w_base  += w * c.base_off;
            w_scale += w * c.scale_m;
            w_total += w;
        }
        float base  = params.height_base_y + w_base / w_total;
        float scale_m = w_scale / w_total;

        // Terrain amplitude control — distinct flat, hilly, and mountainous regions
        float terrain_control = terrain_noise.fbm(x + 7000.0f, z + 7000.0f, 3, 0.50f, 0.0015f);
        float terrain_amplitude = lerp(8.0f, 32.0f, smoothstep(-0.3f, 0.5f, terrain_control)) * scale_m;

        // Anisotropic domain warp — recursive warping for flowing ridges
        float wx1 = terrain_noise.noise_2d(x * 0.002f, z * 0.002f) * 18.0f;
        float wz1 = terrain_noise.noise_2d((x + 5000.0f) * 0.002f, (z + 5000.0f) * 0.002f) * 30.0f;

        float wx2 = terrain_noise.noise_2d((x + wx1) * 0.0018f, (z + wz1) * 0.0018f) * 10.0f;
        float wz2 = terrain_noise.noise_2d((x + wx1 + 5000.0f) * 0.0018f, (z + wz1 + 5000.0f) * 0.0018f) * 16.0f;

        float warp_x = wx1 + wx2;
        float warp_z = wz1 + wz2;

        // Broad low-frequency terrain with light ridged detail
        float per_noise_val = terrain_noise.fbm(x + warp_x, z + warp_z, 4, 0.52f, 0.0064f) * 0.85f
                            + terrain_noise.ridged_noise(x + 4000.0f + warp_x, z + 4000.0f + warp_z, 3, 0.55f, 0.016f) * 0.15f;

        // NEW: fixed-amplitude local detail, independent of the macro shape/amplitude
        float detail = terrain_noise.fbm(x * 1.6f + warp_x * 0.4f, z * 1.6f + warp_z * 0.4f, 3, 0.5f, 0.018f) * 5.0f;

        return base + per_noise_val * terrain_amplitude + detail;
    }

    // Large-region mask deciding where terrain becomes volumetric/unusual.
    // Changes over hundreds of blocks, so the transition feels geological.
    float sample_weirdness(float x, float z) const {
        float raw = weirdness_noise.fbm(
            x + 12000.0f, z - 12000.0f, 3, 0.5f, WEIRDNESS_SCALE);
        return smoothstep(WEIRDNESS_LOW, WEIRDNESS_HIGH, raw);
    }

    // Signed, normalized 3D fBm (FastNoise::fbm_3d already normalizes by the
    // amplitude sum so octave-count changes do not shift overall height).
    // Anisotropic: vertical frequency is higher so the field produces shelves
    // without making the horizontal terrain too busy. This is the LATTICE NODE
    // sampler — call sample_shape_3d_interp for the actual field.
    float sample_shape_3d(float x, float y, float z) const {
        return density_noise.fbm_3d(
            x, y * SHAPE_Y_ANISOTROPY, z, 3, 0.5f, SHAPE_FREQUENCY);
    }

    // Canonical 3D shape field query: trilinear interpolation of the
    // world-aligned 4x4x4 lattice. Chunk generation precomputes the same
    // lattice once per chunk, so single-point queries agree bit-for-bit with
    // the generated block grid (no seam can appear between the two paths).
    float sample_shape_3d_interp(int32_t world_x, int32_t world_y, int32_t world_z) const {
        constexpr int32_t SP = SHAPE_LATTICE_SPACING;
        const int32_t x0 = lattice_base(world_x);
        const int32_t y0 = lattice_base(world_y);
        const int32_t z0 = lattice_base(world_z);
        const float inv_sp = 1.0f / static_cast<float>(SP);
        const float fx = static_cast<float>(world_x - x0) * inv_sp;
        const float fy = static_cast<float>(world_y - y0) * inv_sp;
        const float fz = static_cast<float>(world_z - z0) * inv_sp;
        return trilinear_interp(
            sample_shape_3d(x0,         y0,         z0),
            sample_shape_3d(x0 + SP,    y0,         z0),
            sample_shape_3d(x0,         y0 + SP,    z0),
            sample_shape_3d(x0 + SP,    y0 + SP,    z0),
            sample_shape_3d(x0,         y0,         z0 + SP),
            sample_shape_3d(x0 + SP,    y0,         z0 + SP),
            sample_shape_3d(x0,         y0 + SP,    z0 + SP),
            sample_shape_3d(x0 + SP,    y0 + SP,    z0 + SP),
            fx, fy, fz);
    }

    // Signed density at a world point. >0 solid, <=0 air. `weirdness` is
    // cached per column by the chunk generator (see generate_chunk).
    float sample_terrain_density(int32_t world_x, int32_t world_y, int32_t world_z,
                                 const ColumnSample& column, float weirdness) const {
        // Existing terrain remains the macro surface.
        const float delta = column.height - static_cast<float>(world_y);
        const float shape_strength =
            lerp(SHAPE_STRENGTH_MIN, SHAPE_STRENGTH_MAX, weirdness);
        // Centred (signed) 3D shape noise — NOT a ridged/absolute field, which
        // would shift the average height instead of displacing the boundary.
        const float shape = sample_shape_3d_interp(world_x, world_y, world_z);
        return density_from_shape(delta, shape_strength, shape);
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
        , density_noise(p.seed + 7000)
        , density_warp_noise(p.seed + 8000)
        , weirdness_noise(p.seed + 9000)
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

    // Signed density at a world point (macro surface + 3D deformation).
    // >0 solid, <=0 air. Unlike the cached-weirdness overload used by the
    // chunk generator, this recomputes the weirdness mask per call.
    float sample_terrain_density(int32_t world_x, int32_t world_y, int32_t world_z,
                                 const ColumnSample& column) const {
        return sample_terrain_density(
            world_x, world_y, world_z, column,
            sample_weirdness(static_cast<float>(world_x), static_cast<float>(world_z)));
    }

    // Real topmost air-to-solid transition for a column. The macro heightmap
    // surface is not the actual surface once 3D shaping can push terrain above
    // or below it — used for player spawning and structure placement.
    int32_t find_surface_y(int32_t world_x, int32_t world_z) const;

    // Cheaper than sample_column: only land shape, no biome/lake evaluation.
    float quick_height_estimate(int32_t world_x, int32_t world_z) const {
        float x = static_cast<float>(world_x);
        float z = static_cast<float>(world_z);
        float t = sample_temperature(x, z);
        float h = sample_humidity(x, z);
        return sample_land_shape(x, z, t, h);
    }

    bool is_cave(int32_t x, int32_t y, int32_t z) const {
        // TEMP: set to false to disable cave carving globally.
        if (!kCavesEnabled) return false;
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
        ColumnSample sample{};   // full macro column sample (height/water/temp/...)
        int32_t height = 0;
        BiomeType biome = BiomeType::Plains;
        int32_t water_level = -1;
        bool near_water = false;
        float temperature = 0.0f;
        float humidity = 0.0f;
        float weirdness = 0.0f;  // cached 3D-shaping mask for this column
        int32_t surface_y = -1;  // topmost density surface inside this chunk, -1 if none
    };

    // Cross-chunk block writer callback type
    using CrossChunkWriter = std::function<void(int32_t, int32_t, int32_t, BlockID)>;

    // -------------------------------------------------------------------------
    // Main generation entry point
    // -------------------------------------------------------------------------
    void generate_chunk(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                        const CrossChunkWriter& cross_writer = nullptr, bool vegetation_enabled = true);

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
            density_noise     = FastNoise(p.seed + 7000);
            density_warp_noise = FastNoise(p.seed + 8000);
            weirdness_noise   = FastNoise(p.seed + 9000);

            rng.seed(p.seed);
        }
    }

    const TerrainParams& get_params() const {
        return params;
    }

    void set_biome_config(const BiomeConfig& config) {
        biome_config = config;
    }

    const BiomeConfig& get_biome_config() const {
        return biome_config;
    }

    void set_vegetation_config(const VegetationConfig& config) {
        vegetation_config = config;
    }

    const VegetationConfig& get_vegetation_config() const {
        return vegetation_config;
    }

    static PerformanceTimer& get_perf_timer() {
        return perf_timer;
    }
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_CHUNK_GENERATOR_HPP