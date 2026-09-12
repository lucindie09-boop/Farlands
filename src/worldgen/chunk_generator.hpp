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
    // the zone alters nothing.
    struct ShapeEnvelope {
        float strength   = 0.0f;
        float band_inner = 0.0f;
        float band_outer = 0.0f;
    };

    [[nodiscard]] ShapeEnvelope shape_envelope(float weirdness,
                                              float weirdness_size) const {
        const float size = std::max(weirdness_size, 0.0f);
        ShapeEnvelope e;
        e.strength = lerp(params.shape_strength_min, params.shape_strength_max,
                          clamp01(weirdness)) * size;
        e.band_inner = SURFACE_BAND_INNER * size;
        e.band_outer = SURFACE_BAND_OUTER * size;
        return e;
    }

    // -------------------------------------------------------------------------
    // Noise sampling
    // -------------------------------------------------------------------------
    // Continentalness: how "inland vs basin" a column is, [0,1]. This is the
    // 12000-block base layer of the macro height stack — the layer that sets
    // the broad elevation, so continents sit high and ocean basins low. The
    // raw base noise is read directly (no domain warp: displacement is <1% of
    // a 12000-block feature) and normalized around 0.5; the current sea-level
    // margin puts the coast near cont ~0.39. Generation does not gate on it
    // (land biomes come from the climate grid and oceans are the final
    // below-sea stage) — it is carried in ColumnSample so biome selection can
    // compare a column against each biome's preferred_continentalness.
    float sample_continentalness(float x, float z) const {
        const float raw = terrain_noise.noise_2d(x * 0.0000833f, z * 0.0000833f);
        return clamp01(raw * 0.5f + 0.5f);
    }

    // Recursive climate domain warp, mirroring the macro height warp: two
    // octaves, the second read through the first's displacement, with x and z
    // displaced by different amplitudes (anisotropic) so biome shapes get a
    // directional grain instead of isotropic blobs. Frequencies are fixed
    // (~25-block meander plus a ~10-block fine octave). Returns the
    // displaced sample point.
    void warp_climate_point(float x, float z, float& out_x, float& out_z) const {
        float wx1 = climate_warp_noise.noise_2d(x * 0.04f, z * 0.04f) * params.climate_warp_amp_x1;
        float wz1 = climate_warp_noise.noise_2d((x + 5000.0f) * 0.04f, (z + 5000.0f) * 0.04f) * params.climate_warp_amp_z1;
        float wx2 = climate_warp_noise.noise_2d((x + wx1) * 0.1f, (z + wz1) * 0.1f) * params.climate_warp_amp_x2;
        float wz2 = climate_warp_noise.noise_2d((x + wx1 + 5000.0f) * 0.1f, (z + wz1 + 5000.0f) * 0.1f) * params.climate_warp_amp_z2;
        out_x = x + wx1 + wx2;
        out_z = z + wz1 + wz2;
    }

    // Raw climate value at a world point; used as the lattice corner samples
    // below. The coarse field is read at a domain-warped position so its
    // contours flow and meander instead of drawing smooth lines.
    float sample_temperature_raw(float x, float z) const {
        // One coarse feature spans ~1/scale blocks (0.000125 -> ~8000).
        float wx, wz;
        warp_climate_point(x, z, wx, wz);
        return clamp01((temp_noise.noise_2d(wx * params.climate_temp_scale,
                                            wz * params.climate_temp_scale) + 1.0f) * 0.5f);
    }

    float sample_humidity_raw(float x, float z) const {
        float wx, wz;
        warp_climate_point(x, z, wx, wz);
        return clamp01((humidity_noise.noise_2d(wx * params.climate_humidity_scale,
                                                wz * params.climate_humidity_scale) + 1.0f) * 0.5f);
    }

    // The climate fields are sampled on a 4-block world-aligned lattice (one
    // evaluation per 4 blocks) and bilinearly interpolated between nodes, so
    // every chunk reads the same global lattice nodes (no seams) and biome
    // borders follow piecewise-linear contours.
    float sample_temperature(float x, float z) const {
        constexpr int32_t SPACING = 4;
        const int32_t cix = lattice_base(static_cast<int32_t>(std::floor(x)), SPACING);
        const int32_t ciz = lattice_base(static_cast<int32_t>(std::floor(z)), SPACING);
        const float fx = (x - static_cast<float>(cix)) / static_cast<float>(SPACING);
        const float fz = (z - static_cast<float>(ciz)) / static_cast<float>(SPACING);
        const float x0 = static_cast<float>(cix),        x1 = static_cast<float>(cix + SPACING);
        const float z0 = static_cast<float>(ciz),        z1 = static_cast<float>(ciz + SPACING);
        const float v00 = sample_temperature_raw(x0, z0), v10 = sample_temperature_raw(x1, z0);
        const float v01 = sample_temperature_raw(x0, z1), v11 = sample_temperature_raw(x1, z1);
        return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
    }

    float sample_humidity(float x, float z) const {
        constexpr int32_t SPACING = 4;
        const int32_t cix = lattice_base(static_cast<int32_t>(std::floor(x)), SPACING);
        const int32_t ciz = lattice_base(static_cast<int32_t>(std::floor(z)), SPACING);
        const float fx = (x - static_cast<float>(cix)) / static_cast<float>(SPACING);
        const float fz = (z - static_cast<float>(ciz)) / static_cast<float>(SPACING);
        const float x0 = static_cast<float>(cix),        x1 = static_cast<float>(cix + SPACING);
        const float z0 = static_cast<float>(ciz),        z1 = static_cast<float>(ciz + SPACING);
        const float v00 = sample_humidity_raw(x0, z0), v10 = sample_humidity_raw(x1, z0);
        const float v01 = sample_humidity_raw(x0, z1), v11 = sample_humidity_raw(x1, z1);
        return lerp(lerp(v00, v10, fx), lerp(v01, v11, fx), fz);
    }

    // Chunk-level climate lattice: the raw fields are evaluated once per
    // 4-block node (9x9 per 32-block chunk) and every column interpolates
    // from the lattice, so a full chunk costs ~160 raw evaluations instead
    // of one per-call lattice per column. Node values and interpolation are
    // bit-identical to the per-call samplers above.
    static constexpr int32_t CLIMATE_LATTICE_SPACING = 4;
    static constexpr int32_t CLIMATE_LATTICE_NODES = CHUNK_WIDTH / CLIMATE_LATTICE_SPACING + 1;
    // Blend windows are clamped to this half-extent (in nodes). Each node of
    // radius is ~4 blocks of transition on each side of a border (full
    // plateau-to-plateau band of 2R*4 blocks), so 20 = a ~160-block ramp.
    // The per-node window means are computed with a separable 2D prefix pass
    // (O(1) per node); the remaining cost is classifying the (2R+5)^2-biome
    // grid the windows read from once per chunk (~2000 raw climate samples
    // at R=20). The single-point path shares the same clamp so both paths
    // always agree.
    static constexpr int32_t CLIMATE_BLEND_MAX_RADIUS = 20;
    void build_climate_lattice(int32_t chunk_x, int32_t chunk_z,
                               float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                               float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const;
    // Chunk-level land-shape lattice: the raw macro height is evaluated once
    // per 4-block node (9x9 = 81 evaluations per chunk instead of 4 per
    // column) and every column interpolates from it, mirroring the climate
    // lattice. Node values and interpolation are bit-identical to the
    // per-call sample_land_shape.
    void build_land_shape_lattice(int32_t chunk_x, int32_t chunk_z,
                                  float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const;
    static float interp_climate_lattice(const float lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                                        int32_t wx, int32_t wz, int32_t wx0, int32_t wz0);
    // Amplification blend lattice for a chunk: blended knobs at every 4-block
    // node, windows fed from the cached climate lattice inside the chunk and
    // the raw samplers just outside it (identical node values either way).
    // Shared by generate_chunk and the debug accessor so the chunk path and
    // single-point queries can never drift.
    void build_amp_lattice(int32_t chunk_x, int32_t chunk_z,
                           const float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                           const float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
                           BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES]) const;
    static BiomeAmplification interp_amp_lattice(
        const BiomeAmplification lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES],
        int32_t wx, int32_t wz, int32_t wx0, int32_t wz0);

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

    BiomeType land_biome_from_grid(float temperature, float humidity) const {
        const int ti = temperature <= biome_config.temp_cold_max ? 0
                     : temperature >= biome_config.temp_hot_min ? 2 : 1;
        const int hi = humidity <= biome_config.hum_dry_max ? 0
                     : humidity >= biome_config.hum_humid_min ? 2 : 1;
        return kLandBiomeGrid[ti][hi];
    }

    BiomeType biome_from_climate(float temperature, float humidity, float cont) const {
        // Land-biome selection: the 3x3 temperature x humidity grid. Ocean
        // never appears here — ocean biomes are the final below-sea stage in
        // sample_column_with_climate, after this land biome has shaped the
        // terrain. cont is sampled and carried (real value, from the 12000-
        // block base layer) so selection can later compare against each
        // biome's preferred_continentalness.
        return land_biome_from_grid(temperature, humidity);
    }

    // Raw macro surface height at a world point: the full noise stack
    // (12000-block base + ~1000-block detail + ridged flow + the two relief
    // fields), read through the recursive anisotropic domain warp. This is
    // the per-lattice-node evaluation; sample_land_shape interpolates it on
    // the 4-block lattice, and the chunk generator caches it on a per-chunk
    // lattice (81 evaluations instead of 4 per column).
    float sample_land_shape_raw(float x, float z) const {
        // Domain warp: displace the sample point with a low-frequency noise
        // field before reading the terrain, so contour lines and ridges flow
        // instead of reading as isotropic noise blobs. Two warp octaves, the
        // second offset by the first (recursive warping); x/z warped by
        // different amplitudes (anisotropic) so landforms get a directional
        // grain. Amplitudes are in blocks (~500-block warp field wavelength).
        float wx1 = terrain_noise.noise_2d(x * 0.002f, z * 0.002f) * params.macro_warp_amp_x1;
        float wz1 = terrain_noise.noise_2d((x + 5000.0f) * 0.002f, (z + 5000.0f) * 0.002f) * params.macro_warp_amp_z1;
        float wx2 = terrain_noise.noise_2d((x + wx1) * 0.0018f, (z + wz1) * 0.0018f) * params.macro_warp_amp_x2;
        float wz2 = terrain_noise.noise_2d((x + wx1 + 5000.0f) * 0.0018f, (z + wz1 + 5000.0f) * 0.0018f) * params.macro_warp_amp_z2;
        const float sx = x + wx1 + wx2;
        const float sz = z + wz1 + wz2;

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
             + sample_mid_relief(x, z) + sample_small_relief(x, z);
    }

    // Single noise layer controlling height - minimal terrain. Sampled on
    // the 4-block lattice (temperature/humidity are unused inputs), the same
    // lattice idiom as the climate fields, so the chunk-cached lattice path
    // and the per-call path are bit-identical.
    float sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const {
        constexpr int32_t SPACING = 4;
        const int32_t cix = lattice_base(static_cast<int32_t>(std::floor(x)), SPACING);
        const int32_t ciz = lattice_base(static_cast<int32_t>(std::floor(z)), SPACING);
        const float fx = (x - static_cast<float>(cix)) / static_cast<float>(SPACING);
        const float fz = (z - static_cast<float>(ciz)) / static_cast<float>(SPACING);
        const float x0 = static_cast<float>(cix),        x1 = static_cast<float>(cix + SPACING);
        const float z0 = static_cast<float>(ciz),        z1 = static_cast<float>(ciz + SPACING);
        const float h00 = sample_land_shape_raw(x0, z0), h10 = sample_land_shape_raw(x1, z0);
        const float h01 = sample_land_shape_raw(x0, z1), h11 = sample_land_shape_raw(x1, z1);
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

    // Climate biome at a 4-block lattice node (world coords, multiples of
    // CLIMATE_LATTICE_SPACING), evaluated from the raw samplers. This is the
    // node value the chunk path caches, so both paths agree bit-for-bit.
    BiomeType biome_at_node_raw(int32_t nx, int32_t nz) const {
        return biome_from_climate(
            sample_temperature_raw(static_cast<float>(nx), static_cast<float>(nz)),
            sample_humidity_raw(static_cast<float>(nx), static_cast<float>(nz)),
            sample_continentalness(static_cast<float>(nx), static_cast<float>(nz)));
    }

    // Blended knobs at a single lattice node (single-point path).
    BiomeAmplification blend_amplification_node(int32_t nx, int32_t nz) const {
        return blend_amplitudes([&](int32_t di, int32_t dj) {
            return biome_at_node_raw(nx + di * CLIMATE_LATTICE_SPACING,
                                     nz + dj * CLIMATE_LATTICE_SPACING);
        });
    }

    // Blended knobs at an arbitrary column: the four surrounding lattice
    // nodes are blended and bilinearly interpolated, mirroring the climate
    // samplers' lattice structure.
    BiomeAmplification blend_amplification_at(int32_t world_x, int32_t world_z) const {
        constexpr int32_t SP = CLIMATE_LATTICE_SPACING;
        const int32_t x0 = lattice_base(world_x, SP);
        const int32_t z0 = lattice_base(world_z, SP);
        const float fx = static_cast<float>(world_x - x0) / static_cast<float>(SP);
        const float fz = static_cast<float>(world_z - z0) / static_cast<float>(SP);
        const BiomeAmplification v00 = blend_amplification_node(x0, z0);
        const BiomeAmplification v10 = blend_amplification_node(x0 + SP, z0);
        const BiomeAmplification v01 = blend_amplification_node(x0, z0 + SP);
        const BiomeAmplification v11 = blend_amplification_node(x0 + SP, z0 + SP);
        BiomeAmplification out;
        out.height = lerp(lerp(v00.height, v10.height, fx), lerp(v01.height, v11.height, fx), fz);
        out.weirdness = lerp(lerp(v00.weirdness, v10.weirdness, fx),
                             lerp(v01.weirdness, v11.weirdness, fx), fz);
        out.min_weirdness = lerp(lerp(v00.min_weirdness, v10.min_weirdness, fx),
                                 lerp(v01.min_weirdness, v11.min_weirdness, fx), fz);
        out.weirdness_size = lerp(lerp(v00.weirdness_size, v10.weirdness_size, fx),
                                  lerp(v01.weirdness_size, v11.weirdness_size, fx), fz);
        return out;
    }

    // Effective knobs for a column: with blending disabled (radius 0) every
    // column uses its own biome's knobs exactly — the interpolated blend
    // field is ignored so a border is a clean step, not a 4-block lerp. With
    // blending enabled, land biomes use the blended field (ramps across
    // borders). Ocean columns still take the ocean biome's own knobs for the
    // 3D shaping (the blend field is climate-derived and never contains
    // ocean); the ocean's macro seabed height is no longer set here — it
    // keeps the height the land biome gave it before the ocean override.
    const BiomeAmplification& amplification_for(BiomeType biome,
                                                const BiomeAmplification& blended) const {
        const size_t ix = static_cast<size_t>(biome);
        if (params.climate_blend_radius_nodes <= 0) {
            return biome_config.amplification[ix];
        }
        return (biome == BiomeType::Ocean)
            ? biome_config.amplification[static_cast<size_t>(BiomeType::Ocean)]
            : blended;
    }

    // Weirdness mask for a column: the raw 2D mask scaled by the column's
    // amplification knobs (own biome at radius 0, blended field on land
    // otherwise), never allowed
    // below the minimum mask floor, then clamped to [0, 1] so the shaping
    // strength stays within [shape_strength_min, max]. The floor is
    // expressed as an offset above neutral: 1.0 = no floor, 1.1 floors the
    // mask at 0.1.
    float amplified_weirdness(float raw_mask, const BiomeAmplification& a) const {
        const float floor_mask = std::max(0.0f, a.min_weirdness - 1.0f);
        return clamp01(std::max(raw_mask * a.weirdness, floor_mask));
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
    // weirdness_size = 1.0) gives SURFACE_BAND_OUTER + slack = 30.
    [[nodiscard]] float density_margin() const {
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

    // Debug accessors (expose private members for standalone tools)
    float sample_continentalness_debug(float x, float z) const {
        return sample_continentalness(x, z);
    }
    ColumnSample sample_column_debug(int32_t world_x, int32_t world_z) const {
        return sample_column(world_x, world_z);
    }
    // Debug: climate values read through the chunk-cached lattice path (what
    // generate_chunk uses), for cross-checking against the per-call samplers.
    float sample_temperature_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                           int32_t wx, int32_t wz) const {
        float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        build_climate_lattice(chunk_x, chunk_z, temp_lat, hum_lat);
        return interp_climate_lattice(temp_lat, wx, wz, chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);
    }
    float sample_weirdness_debug(float x, float z) const {
        return sample_weirdness(x, z);
    }
    float sample_temperature_debug(float x, float z) const {
        return sample_temperature(x, z);
    }
    float sample_humidity_debug(float x, float z) const {
        return sample_humidity(x, z);
    }
    float sample_land_shape_debug(float x, float z) const {
        return sample_land_shape(x, z, 0.0f, 0.0f);  // temp/humidity are unused
    }
    // Debug: macro land height read through the chunk-cached lattice path
    // (what generate_chunk uses), for cross-checking against the per-call
    // sampler.
    float sample_land_shape_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                          int32_t wx, int32_t wz) const {
        float land_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        build_land_shape_lattice(chunk_x, chunk_z, land_lat);
        return interp_climate_lattice(land_lat, wx, wz,
                                      chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);
    }
    // Debug: blended amplification knobs at a column (per-call path).
    BiomeAmplification blend_amplification_debug(int32_t world_x, int32_t world_z) const {
        return blend_amplification_at(world_x, world_z);
    }
    // Debug: blended amplification read through the chunk-cached lattice path
    // (what generate_chunk uses), for cross-checking against the per-call
    // sampler.
    BiomeAmplification blend_amplification_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                                         int32_t wx, int32_t wz) const {
        float temp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        float hum_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        build_climate_lattice(chunk_x, chunk_z, temp_lat, hum_lat);
        BiomeAmplification amp_lat[CLIMATE_LATTICE_NODES][CLIMATE_LATTICE_NODES];
        build_amp_lattice(chunk_x, chunk_z, temp_lat, hum_lat, amp_lat);
        return interp_amp_lattice(amp_lat, wx, wz,
                                  chunk_x * CHUNK_WIDTH, chunk_z * CHUNK_DEPTH);
    }
    BiomeType biome_from_climate_debug(float temperature, float humidity, float cont) const {
        return biome_from_climate(temperature, humidity, cont);
    }

    ChunkGenerator(const TerrainParams& p = TerrainParams())
        : terrain_noise(p.seed)
        , cave_noise(p.seed + 2000)
        , density_noise(p.seed + 7000)
        , weirdness_noise(p.seed + 9000)
        , temp_noise(p.seed + 3000)
        , humidity_noise(p.seed + 4000)
        , climate_warp_noise(p.seed + 5000)
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
        const BiomeAmplification& amp = amplification_for(column.biome, blended);
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

    // Cheaper than sample_column: only land shape, no biome/lake evaluation.
    // Mirrors sample_column's effective height amplification (own biome at
    // radius 0, the blended field on land otherwise) so the scheduler's
    // surface estimate tracks the generated surface when it is tuned.
    float quick_height_estimate(int32_t world_x, int32_t world_z) const {
        float x = static_cast<float>(world_x);
        float z = static_cast<float>(world_z);
        float t = sample_temperature(x, z);
        float h = sample_humidity(x, z);
        const float cont = sample_continentalness(x, z);
        const float raw = sample_land_shape(x, z, t, h);
        const BiomeType biome = (raw >= params.sea_level)
            ? biome_from_climate(t, h, cont)
            : BiomeType::Ocean;
        const BiomeAmplification& amp = amplification_for(
            biome, params.climate_blend_radius_nodes > 0
                       ? blend_amplification_at(world_x, world_z)
                       : BiomeAmplification{});
        return params.sea_level + (raw - params.sea_level) * amp.height;
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
            temp_noise        = FastNoise(p.seed + 3000);
            humidity_noise    = FastNoise(p.seed + 4000);
            climate_warp_noise = FastNoise(p.seed + 5000);
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