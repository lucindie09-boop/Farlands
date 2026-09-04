#ifndef FARLANDS_CHUNK_GENERATOR_HPP
#define FARLANDS_CHUNK_GENERATOR_HPP
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
    FastNoise density_noise;    // seed+7000: signed 3D shape field (see sample_shape_3d)
    FastNoise weirdness_noise;  // seed+9000: very-low-frequency 2D shaping gate

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
    static constexpr int32_t lattice_base(int32_t v, int32_t spacing) {
        const int32_t q = v / spacing;
        const int32_t r = v % spacing;
        return (r < 0 ? q - 1 : q) * spacing;
    }

    // -------------------------------------------------------------------------
    // Signed 3D density field
    //
    // The macro heightmap stays the base surface (density = surface_y - y);
    // a normalized 3D fBm field deforms only a band around that surface. The
    // "weirdness" mask (very low frequency 2D) decides where the deformation
    // is strong enough to produce overhangs/shelves vs. mostly-plain terrain.
    // -------------------------------------------------------------------------
    // DENSITY_MARGIN bounds how far the real surface can sit from the macro
    // heightmap. Displacement = shape * strength * surface_band, and the band
    // is exactly zero at distance SURFACE_BAND_OUTER — so no matter how large
    // the shape strength grows, the surface never leaves SURFACE_BAND_OUTER
    // of the macro height. 30 = OUTER + small slack.
    static constexpr float DENSITY_MARGIN      = 30.0f;
    static constexpr float SURFACE_BAND_INNER  = 9.0f;
    static constexpr float SURFACE_BAND_OUTER  = 28.0f;
    // Optional: read the 3D shape field through a light 2D domain warp (the
    // same recursive scheme as the macro height warp, at much smaller scale)
    // so the craggy micro-detail curves with the terrain instead of reading
    // as static noise. Vertical shelves are preserved: only x/z are warped.
    static constexpr bool kShapeDomainWarpEnabled = true;
    static constexpr float SHAPE_FREQUENCY     = 0.026f; // ~38-block horizontal feature scale
    static constexpr float SHAPE_Y_ANISOTROPY  = 1.35f;  // ~0.035 effective vertical scale
    // Remaining tuning — macro/base height, domain-warp amplitudes, the
    // medium (~500-block) and small (~150-block) relief fields, the shape
    // strength range and the weirdness mask thresholds — lives in
    // TerrainParams and loads from data/terrain_config.json (see
    // TerrainParams::load_from_json). Only structural invariants stay
    // constexpr here: the density band, lattice spacing, shape field
    // frequency/anisotropy and the domain-warp enable flag.

    // The 3D shape noise is stored on a 4x4x4 world-aligned lattice and
    // trilinearly interpolated per voxel. SPACING divides the chunk size, so
    // lattice nodes always land on the same world coordinates on both sides of
    // a chunk boundary — the interpolated field is mathematically identical
    // across chunk seams. Only the noise sampling is coarse; the density field
    // and the final block grid stay full resolution.
    static constexpr int32_t SHAPE_LATTICE_SPACING = 4;

    // Largest lattice node coordinate <= v for the shape lattice.
    static constexpr int32_t lattice_base(int32_t v) {
        return lattice_base(v, SHAPE_LATTICE_SPACING);
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
    // Noise sampling
    // -------------------------------------------------------------------------
    float sample_continentalness(float x, float z) const {
        return 0.5f; // Disabled - flat continentalness
    }

    float sample_temperature(float x, float z) const {
        return 0.5f; // Disabled - flat temperature
    }

    float sample_humidity(float x, float z) const {
        return 0.5f; // Disabled - flat humidity
    }

    // Simplified biome - single biome only
    BiomeType land_biome_from_grid(float temperature, float humidity) const {
        return BiomeType::Plains;
    }

    BiomeType biome_from_climate(float temperature, float humidity, float cont) const {
        return BiomeType::Plains;
    }

    // Single noise layer controlling height - minimal terrain
    float sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const {
        // Sample on 4-block lattice for performance (3000x3000 grid over 12000x12000 area)
        constexpr int32_t SPACING = 4;
        const int32_t cix = lattice_base(static_cast<int32_t>(std::floor(x)), SPACING);
        const int32_t ciz = lattice_base(static_cast<int32_t>(std::floor(z)), SPACING);
        const float fx = (x - static_cast<float>(cix)) / static_cast<float>(SPACING);
        const float fz = (z - static_cast<float>(ciz)) / static_cast<float>(SPACING);
        const float x0 = static_cast<float>(cix),        x1 = static_cast<float>(cix + SPACING);
        const float z0 = static_cast<float>(ciz),        z1 = static_cast<float>(ciz + SPACING);

        // Domain warp: displace the sample point with a low-frequency noise
        // field before reading the terrain, so contour lines and ridges flow
        // instead of reading as isotropic noise blobs. Two warp octaves, the
        // second offset by the first (recursive warping); x/z warped by
        // different amplitudes (anisotropic) so landforms get a directional
        // grain. Amplitudes are in blocks (~500-block warp field wavelength).
        auto h_at = [&](float px, float pz) -> float {
            float wx1 = terrain_noise.noise_2d(px * 0.002f, pz * 0.002f) * params.macro_warp_amp_x1;
            float wz1 = terrain_noise.noise_2d((px + 5000.0f) * 0.002f, (pz + 5000.0f) * 0.002f) * params.macro_warp_amp_z1;
            float wx2 = terrain_noise.noise_2d((px + wx1) * 0.0018f, (pz + wz1) * 0.0018f) * params.macro_warp_amp_x2;
            float wz2 = terrain_noise.noise_2d((px + wx1 + 5000.0f) * 0.0018f, (pz + wz1 + 5000.0f) * 0.0018f) * params.macro_warp_amp_z2;
            const float sx = px + wx1 + wx2;
            const float sz = pz + wz1 + wz2;

            // Single noise layer (terrain_noise): 12000-block base plus
            // ~1000-block detail, both sampled through the warped domain.
            float base_height = params.height_base_y + terrain_noise.noise_2d(sx * 0.0000833f, sz * 0.0000833f) * 500.0f;
            float detail = terrain_noise.noise_2d(sx * 0.001f, sz * 0.001f) * 100.0f;

            // Light mid-frequency ridged detail, also warped. This is the
            // wavelength band (~300-block, down to ~80) where the +/-50-block
            // warp actually bends contours into flowing ridges instead of
            // smearing features 10x larger than the displacement.
            float flow = terrain_noise.fbm(sx * 0.0032f, sz * 0.0032f, 3, 0.5f, 1.0f) * 16.0f;
            return base_height + detail + flow
                 + sample_mid_relief(px, pz) + sample_small_relief(px, pz);
        };

        const float h00 = h_at(x0, z0), h10 = h_at(x1, z0);
        const float h01 = h_at(x0, z1), h11 = h_at(x1, z1);
        return lerp(lerp(h00, h10, fx), lerp(h01, h11, fx), fz);
    }

    // 150-block-scale relief: coarse lattice + bilinear lerp, world-anchored
    // (lattice_base) so every chunk reads the same global nodes — no seams.
    float sample_small_relief(float x, float z) const {
        const int32_t SP = static_cast<int32_t>(params.small_lattice_spacing);
        const int32_t nx0 = lattice_base(static_cast<int32_t>(std::floor(x)), SP);
        const int32_t nz0 = lattice_base(static_cast<int32_t>(std::floor(z)), SP);
        const float fx = (x - static_cast<float>(nx0)) / static_cast<float>(SP);
        const float fz = (z - static_cast<float>(nz0)) / static_cast<float>(SP);
        const auto n_at = [&](float px, float pz) {
            return terrain_noise.noise_2d(px * params.small_frequency, pz * params.small_frequency);
        };
        const float v00 = n_at(nx0, nz0), v10 = n_at(nx0 + SP, nz0);
        const float v01 = n_at(nx0, nz0 + SP), v11 = n_at(nx0 + SP, nz0 + SP);
        return params.small_amplitude * lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
    }

    // 500-block-scale relief (64x64 lattice samples per ~500-block area,
    // bilinearly lerped between nodes). The lattice is world-anchored
    // (lattice_base) so every chunk reads the same global nodes — no seams.
    float sample_mid_relief(float x, float z) const {
        const int32_t SP = static_cast<int32_t>(params.mid_lattice_spacing);
        const int32_t nx0 = lattice_base(static_cast<int32_t>(std::floor(x)), SP);
        const int32_t nz0 = lattice_base(static_cast<int32_t>(std::floor(z)), SP);
        const float fx = (x - static_cast<float>(nx0)) / static_cast<float>(SP);
        const float fz = (z - static_cast<float>(nz0)) / static_cast<float>(SP);
        const auto n_at = [&](float px, float pz) {
            return terrain_noise.noise_2d(px * params.mid_frequency, pz * params.mid_frequency);
        };
        const float v00 = n_at(nx0, nz0), v10 = n_at(nx0 + SP, nz0);
        const float v01 = n_at(nx0, nz0 + SP), v11 = n_at(nx0 + SP, nz0 + SP);
        return params.mid_amplitude * lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
    }

    // Large-region mask deciding where terrain becomes volumetric/unusual.
    // Changes over hundreds of blocks, so the transition feels geological.
    // Ranges [0,1]: smoothstep over the raw fBm keeps most of the world at
    // low weirdness (gentle shaping) with scattered strong zones. The pre-strip
    // version multiplied by an elevation term that no longer exists here, so
    // the mask is purely the smoothstep field now.
    float sample_weirdness(float x, float z) const {
        float raw = weirdness_noise.fbm(
            x + 12000.0f, z - 12000.0f, 3, 0.5f, params.weirdness_scale);
        return smoothstep(params.weirdness_low, params.weirdness_high, raw);
    }

    // Signed, normalized 3D fBm (FastNoise::fbm_3d already normalizes by the
    // amplitude sum so octave-count changes do not shift overall height).
    // Anisotropic: vertical frequency is higher so the field produces shelves
    // without making the horizontal terrain too busy. This is the LATTICE NODE
    // sampler — call sample_shape_3d_interp for the actual field.
    float sample_shape_3d(float x, float y, float z) const {
        if (kShapeDomainWarpEnabled) {
            // Displace the sample point in the horizontal plane before reading
            // the field. ~250-block warp wavelength with anisotropic amplitudes
            // (params.shape_warp_amp_x/z) vs the ~38-block shape features:
            // small enough to shear and orient the craggy detail without
            // smearing it into blobs.
            float wx1 = density_noise.noise_2d(x * 0.004f, z * 0.004f) * params.shape_warp_amp_x;
            float wz1 = density_noise.noise_2d((x + 5000.0f) * 0.004f, (z + 5000.0f) * 0.004f) * params.shape_warp_amp_z;
            x += wx1;
            z += wz1;
        }
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
    // cached per column by the chunk generator (see generate_chunk) and
    // clamped to [0,1] here so strength stays in [SHAPE_STRENGTH_MIN, MAX].
    float sample_terrain_density(int32_t world_x, int32_t world_y, int32_t world_z,
                                 const ColumnSample& column, float weirdness) const {
        // Existing terrain remains the macro surface.
        const float delta = column.height - static_cast<float>(world_y);
        const float shape_strength =
            lerp(params.shape_strength_min, params.shape_strength_max, clamp01(weirdness));
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
    float sample_weirdness_debug(float x, float z) const {
        return sample_weirdness(x, z);
    }

    ChunkGenerator(const TerrainParams& p = TerrainParams())
        : terrain_noise(p.seed)
        , cave_noise(p.seed + 2000)
        , density_noise(p.seed + 7000)
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
            density_noise     = FastNoise(p.seed + 7000);
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

#endif // FARLANDS_CHUNK_GENERATOR_HPP