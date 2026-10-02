#ifndef FARLANDS_CHUNK_GENERATOR_HPP
#define FARLANDS_CHUNK_GENERATOR_HPP
#include <functional>
#include "core/terrain_params.hpp"
#include "core/noise.hpp"
#include "core/block_types.hpp"
#include "core/chunk_data.hpp"
#include "core/performance_timer.hpp"
#include "worldgen/biome_config.hpp"
#include "worldgen/chunk_generator_lattice.hpp"
#include "worldgen/terrain_squish.hpp"
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
    // The lattice cache is filled from the private samplers below, which is what
    // keeps the cached chunk path bit-identical to the per-call one.
    friend class ChunkGeneratorLattice;

private:
    // TEMP: set false to disable cave carving (see is_cave).
    static constexpr bool kCavesEnabled = false;
    FastNoise terrain_noise;
    FastNoise cave_noise;
    FastNoise density_noise;    // seed+7000: signed 3D shape field (see sample_shape_3d)
    FastNoise weirdness_noise;  // seed+9000: very-low-frequency 2D shaping gate
    FastNoise temp_noise;          // seed+3000: low-frequency 2D temperature field (~8000-block features)
    FastNoise humidity_noise;      // seed+4000: low-frequency 2D humidity field (~8000-block features)
    FastNoise climate_warp_noise;  // seed+5000: low-frequency 2D displacement for climate sampling

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
    // The surface band bounds how far the real surface can sit from the macro
    // heightmap. Displacement = shape * strength * surface_band, and the band
    // is exactly zero at distance band_outer — so no matter how large the
    // shape strength grows, the surface never leaves the band of the macro
    // height. Both the strength and the band are scaled per column by the
    // biome's weirdness_size knob (see shape_envelope), so the reach is
    // SURFACE_BAND_OUTER * size; density_margin() is that worst case plus
    // slack and is what chunk scheduling pads every height range by.
    static constexpr float SURFACE_BAND_INNER  = 9.0f;
    static constexpr float SURFACE_BAND_OUTER  = 28.0f;
    static constexpr float DENSITY_MARGIN_SLACK = 2.0f;
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
    // band_inner/band_outer are the caller's per-column envelope (see
    // shape_envelope): the displacement fades to zero by band_outer, so the
    // surface can never leave that band no matter how large the strength is.
    static float density_from_shape(float delta, float shape_strength, float shape,
                                   float band_inner, float band_outer) {
        const float surface_distance = std::abs(delta);
        float surface_band;
        if (band_outer <= band_inner) {
            // Degenerate envelope (weirdness_size 0): a hard cutoff at
            // band_outer, since smoothstep would divide by zero here.
            surface_band = (surface_distance < band_outer) ? 1.0f : 0.0f;
        } else {
            surface_band = 1.0f - smoothstep(band_inner, band_outer, surface_distance);
        }
        return delta + shape * shape_strength * surface_band;
    }

    // Per-column 3D-shaping envelope. weirdness_size scales BOTH halves of
    // the shape: how far terrain is displaced (strength) and how many blocks
    // around the macro surface may be altered (the inner/outer band). 1.0 is
    // the neutral envelope from params + SURFACE_BAND_INNER/OUTER; 0 means
    // the zone alters nothing. Defined in chunk_generator_sampling.cpp, whose
    // squish branch is what keeps a squished world inside its one slice.
    struct ShapeEnvelope {
        float strength   = 0.0f;
        float band_inner = 0.0f;
        float band_outer = 0.0f;
    };

    [[nodiscard]] ShapeEnvelope shape_envelope(float weirdness,
                                              float weirdness_size) const;
    float sample_continentalness(float x, float z) const;
    void warp_climate_point(float x, float z, float& out_x, float& out_z) const;
    float sample_temperature_raw(float x, float z) const;
    float sample_humidity_raw(float x, float z) const;
    float sample_temperature(float x, float z) const;
    float sample_humidity(float x, float z) const;

    // Blend windows are clamped to this half-extent (in nodes). Each node of
    // radius is ~4 blocks of transition on each side of a border (full
    // plateau-to-plateau band of 2R*4 blocks), so 20 = a ~160-block ramp.
    // The per-node window means are computed with a separable 2D prefix pass
    // (O(1) per node); the remaining cost is classifying the (2R+5)^2-biome
    // grid the windows read from once per chunk (~2000 raw climate samples
    // at R=20). The single-point path shares the same clamp so both paths
    // always agree.
    static constexpr int32_t CLIMATE_BLEND_MAX_RADIUS = 20;

    // 3x3 (temperature x humidity) land-biome grid, indexed [temp][hum] with
    // 0 = cold/dry, 1 = neutral, 2 = hot/humid. Cold and hot bands stay
    // Hills; the temperate (neutral-temperature) band is Plains. Adding a
    // biome is a cell entry here plus a BiomeType enum value and a
    // data/biomes.json entry.
    static constexpr BiomeType kLandBiomeGrid[3][3] = {
        //              dry             neutral         humid
        /* cold     */ {BiomeType::Hills,  BiomeType::Hills,  BiomeType::Hills},
        /* neutral  */ {BiomeType::Plains, BiomeType::Plains, BiomeType::Plains},
        /* hot      */ {BiomeType::Hills,  BiomeType::Hills,  BiomeType::Hills},
    };
    BiomeType land_biome_from_grid(float temperature, float humidity) const;
    BiomeType biome_from_climate(float temperature, float humidity, float cont) const;
    float sample_land_shape_raw(float x, float z) const;
    float sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const;
    float sample_small_relief(float x, float z) const;
    float sample_mid_relief(float x, float z) const;
    float sample_weirdness(float x, float z) const;

    // -------------------------------------------------------------------------
    // Amplification blending across biome borders
    //
    // Each 4-block climate-lattice node's effective knobs (height /
    // weirdness / min_weirdness / weirdness_size) are the arithmetic mean over the biome nodes
    // within climate_blend_radius_nodes (each node of radius = ~4 blocks of
    // transition on each side of a border). A uniform average makes the knobs
    // ramp linearly from one biome's plateau to the next across the whole
    // window, so the blend band genuinely widens with the radius — an
    // inverse-distance kernel was tried first and it front-loaded almost the
    // whole transition into the two lattice cells touching the border, which
    // is why large radii still produced a sharp height step there.
    // -------------------------------------------------------------------------
    // Accumulate the blend over a window whose biome at lattice-node offset
    // (di, dj) is returned by `biome_at`. Used by the single-point path
    // (raw samplers). The chunk path computes the same window means with a
    // separable 2D prefix pass over its cached biome grid; both feed the same
    // node values so the two paths agree to within float rounding. A window
    // of radius 0 contains only the center node, so the blended knobs reduce
    // exactly to that node's own biome (no smearing).
    template <typename BiomeAt>
    BiomeAmplification blend_amplitudes(BiomeAt biome_at) const {
        const int32_t R = std::min(std::max(params.climate_blend_radius_nodes, 0),
                                   CLIMATE_BLEND_MAX_RADIUS);
        double h_sum = 0.0, w_sum = 0.0, m_sum = 0.0, s_sum = 0.0;
        double n = 0.0;
        for (int32_t dj = -R; dj <= R; ++dj) {
            for (int32_t di = -R; di <= R; ++di) {
                const BiomeAmplification& a =
                    biome_config.amplification[static_cast<size_t>(biome_at(di, dj))];
                h_sum += static_cast<double>(a.height);
                w_sum += static_cast<double>(a.weirdness);
                m_sum += static_cast<double>(a.min_weirdness);
                s_sum += static_cast<double>(a.weirdness_size);
                n += 1.0;
            }
        }
        BiomeAmplification out;
        out.height = static_cast<float>(h_sum / n);
        out.weirdness = static_cast<float>(w_sum / n);
        out.min_weirdness = static_cast<float>(m_sum / n);
        out.weirdness_size = static_cast<float>(s_sum / n);
        return out;
    }
    BiomeType biome_at_node_raw(int32_t nx, int32_t nz) const;
    BiomeAmplification blend_amplification_node(int32_t nx, int32_t nz) const;
    BiomeAmplification blend_amplification_at(int32_t world_x, int32_t world_z) const;
    // Returned by value, not by reference: the blended field is often built as a
    // temporary at the call site, and handing back a reference to it would dangle
    // the moment the full expression ended. The struct is five floats.
    BiomeAmplification amplification_for(BiomeType biome,
                                        const BiomeAmplification& blended) const;
    float amplified_weirdness(float raw_mask, const BiomeAmplification& a) const;

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
    // clamped to [0,1] here so strength stays in [SHAPE_STRENGTH_MIN, MAX];
    // `weirdness_size` is that column's envelope scale (1.0 = neutral).
    float sample_terrain_density(int32_t world_x, int32_t world_y, int32_t world_z,
                                 const ColumnSample& column, float weirdness,
                                 float weirdness_size) const {
        // Existing terrain remains the macro surface.
        const float delta = column.height - static_cast<float>(world_y);
        const ShapeEnvelope env = shape_envelope(weirdness, weirdness_size);
        // Centred (signed) 3D shape noise — NOT a ridged/absolute field, which
        // would shift the average height instead of displacing the boundary.
        const float shape = sample_shape_3d_interp(world_x, world_y, world_z);
        return density_from_shape(delta, env.strength, shape, env.band_inner, env.band_outer);
    }

    // -------------------------------------------------------------------------
    // Per-column terrain evaluation 
    // -------------------------------------------------------------------------
    ColumnSample sample_column(int32_t world_x, int32_t world_z) const;
    // Column evaluation with pre-supplied climate values, macro land height
    // and blended amplification knobs (all lattice-interpolated by the
    // caller, e.g. generate_chunk's chunk-cached lattices).
    ColumnSample sample_column_with_climate(int32_t world_x, int32_t world_z,
                                            float temperature, float humidity,
                                            float land_height,
                                            const BiomeAmplification& blended) const;

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

    // Bounds how far the 3D shape can push the real surface from the macro
    // heightmap: the widest surface band in the biome config, plus slack.
    // Every height range is padded by this, and the fully-above/below chunk
    // fast paths trust it, so it must never be smaller than the largest
    // per-column shape_envelope().band_outer. Neutral config (every
    // weirdness_size = 1.0) gives SURFACE_BAND_OUTER + slack = 30. A squished
    // world replaces it with the squish band (see terrain_squish.hpp), which is
    // what keeps every height range inside the one slice.
    [[nodiscard]] float density_margin() const {
        if (params.squish_enabled) return squish::margin(params);
        float outer = SURFACE_BAND_OUTER;
        for (const BiomeAmplification& a : biome_config.amplification) {
            outer = std::max(outer, SURFACE_BAND_OUTER * std::max(a.weirdness_size, 0.0f));
        }
        return outer + DENSITY_MARGIN_SLACK;
    }
    BlockID get_chunk_subsurface_block(int32_t chunk_x, int32_t chunk_z) const;

    // -------------------------------------------------------------------------
    // Fast uniform-chunk fill (all air / all bedrock / all solid subsurface)
    // -------------------------------------------------------------------------
    // Uses this generator's configured params / biome config, so callers that
    // keep a configured instance (the worker's thread-local generator) get the
    // fast paths without constructing a fresh generator per chunk. Shared with
    // the generation worker (ChunkWorld) so the fully-solid bookkeeping is
    // identical everywhere. Returns true when the chunk was handled by a fast
    // path; false when full generation is required.
    bool generate_fast_path(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z);
    float sample_continentalness_debug(float x, float z) const;
    ColumnSample sample_column_debug(int32_t world_x, int32_t world_z) const;
    float sample_temperature_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                           int32_t wx, int32_t wz) const;
    float sample_weirdness_debug(float x, float z) const;
    float sample_temperature_debug(float x, float z) const;
    float sample_humidity_debug(float x, float z) const;
    float sample_land_shape_debug(float x, float z) const;
    float sample_land_shape_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                          int32_t wx, int32_t wz) const;
    BiomeAmplification blend_amplification_debug(int32_t world_x, int32_t world_z) const;
    BiomeAmplification blend_amplification_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                                         int32_t wx, int32_t wz) const;
    BiomeType biome_from_climate_debug(float temperature, float humidity, float cont) const;
    ChunkGenerator(const TerrainParams& p = TerrainParams());
    BiomeType get_biome(int32_t world_x, int32_t world_z) const;
    float get_terrain_height(int32_t world_x, int32_t world_z) const;

    // Searches outward from (center_x, center_z) for the nearest column whose
    // biome is `target`, within max_radius_blocks. Phase 1 walks concentric
    // 16-block rings outward (early exit on the first hit); phase 2 refines
    // the hit with a fine 4-block scan of the surrounding area and keeps the
    // closest matching column to the requested center. Returns true and fills
    // out_x / out_z (the found column) and out_height (its macro surface
    // height — sea bed for ocean columns).
    bool find_nearest_biome(BiomeType target, int32_t center_x, int32_t center_z,
                            int32_t max_radius_blocks, int32_t& out_x, int32_t& out_z,
                            float& out_height) const;

    // Signed density at a world point (macro surface + 3D deformation).
    // >0 solid, <=0 air. Unlike the cached-weirdness overload used by the
    // chunk generator, this recomputes the weirdness mask per call (scaled by
    // the column's effective amplification: its own biome's knobs when the
    // blend radius is 0, the blended field on land otherwise).
    float sample_terrain_density(int32_t world_x, int32_t world_y, int32_t world_z,
                                 const ColumnSample& column) const {
        const BiomeAmplification blended = blend_amplification_at(world_x, world_z);
        const BiomeAmplification amp = amplification_for(column.biome, blended);
        return sample_terrain_density(
            world_x, world_y, world_z, column,
            amplified_weirdness(sample_weirdness(static_cast<float>(world_x),
                                                 static_cast<float>(world_z)), amp),
            amp.weirdness_size);
    }

    // Real topmost air-to-solid transition for a column. The macro heightmap
    // surface is not the actual surface once 3D shaping can push terrain above
    // or below it — used for player spawning and structure placement.
    int32_t find_surface_y(int32_t world_x, int32_t world_z) const;
    float quick_height_estimate(int32_t world_x, int32_t world_z) const;
    bool is_cave(int32_t x, int32_t y, int32_t z) const;

    // -------------------------------------------------------------------------
    // Per-column data used during chunk generation (replaces 7 separate arrays)
    // -------------------------------------------------------------------------
    struct ChunkColumn {
        ColumnSample sample{};   // full macro column sample (height/water/temp/...)
        int32_t height = 0;
        BiomeType biome = BiomeType::Hills;
        int32_t water_level = -1;
        bool near_water = false;
        float temperature = 0.0f;
        float humidity = 0.0f;
        float weirdness = 0.0f;  // cached 3D-shaping mask for this column
        float weirdness_size = 1.0f;  // cached shape envelope scale (1.0 = neutral)
        int32_t surface_y = -1;  // topmost density surface inside this chunk, -1 if none
    };

    // Cross-chunk block writer callback type
    using CrossChunkWriter = std::function<void(int32_t, int32_t, int32_t, BlockID)>;

    // -------------------------------------------------------------------------
    // Main generation entry point
    // -------------------------------------------------------------------------
    void generate_chunk(ChunkData& chunk, int32_t chunk_x, int32_t chunk_y, int32_t chunk_z,
                        const CrossChunkWriter& cross_writer = nullptr, bool vegetation_enabled = true);
    void set_params(const TerrainParams& p);
    const TerrainParams& get_params() const;
    void set_biome_config(const BiomeConfig& config);
    const BiomeConfig& get_biome_config() const;
    void set_vegetation_config(const VegetationConfig& config);
    const VegetationConfig& get_vegetation_config() const;

    static PerformanceTimer& get_perf_timer() {
        return perf_timer;
    }
};

// True when the chunk at (cx, cy, cz) would be entirely solid if it were
// generated: the whole chunk sits below every column's lowest possible
// surface (macro height − density margin), so the density field has no air
// inside it — and chunks below the bedrock layer are bedrock, also solid.
// Out-of-world chunks return false so boundary faces at the world edges still
// render. Used by mesh culling to treat an ungenerated underground neighbor
// as opaque instead of rendering a box wall into the void.
bool chunk_would_be_fully_solid(const ChunkGenerator& gen, int32_t cx, int32_t cy, int32_t cz);

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_GENERATOR_HPP
