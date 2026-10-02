// The generator's sampling layer and its set-up: the noise fields and their
// construction, the climate fields and their domain warp, the macro land shape with
// its two relief lattices, the weirdness mask, the amplification blend that carries a
// column between two biomes' knobs, and the debug accessors the tools cross-check the
// chunk-cached path against. These are the per-chunk and per-point queries; the
// per-cell density read that runs inside the generation loop stays inline in
// worldgen/chunk_generator.hpp so that loop can still inline it.

#include "worldgen/chunk_generator.hpp"

namespace VoxelEngine {

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
float ChunkGenerator::sample_continentalness(float x, float z) const {
    const float raw = terrain_noise.noise_2d(x * 0.0000833f, z * 0.0000833f);
    return clamp01(raw * 0.5f + 0.5f);
}

// Recursive climate domain warp, mirroring the macro height warp: two
// octaves, the second read through the first's displacement, with x and z
// displaced by different amplitudes (anisotropic) so biome shapes get a
// directional grain instead of isotropic blobs. Frequencies are fixed
// (~25-block meander plus a ~10-block fine octave). Returns the
// displaced sample point.
void ChunkGenerator::warp_climate_point(float x, float z, float& out_x, float& out_z) const {
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
float ChunkGenerator::sample_temperature_raw(float x, float z) const {
    // One coarse feature spans ~1/scale blocks (0.000125 -> ~8000).
    float wx, wz;
    warp_climate_point(x, z, wx, wz);
    return clamp01((temp_noise.noise_2d(wx * params.climate_temp_scale,
                                        wz * params.climate_temp_scale) + 1.0f) * 0.5f);
}

float ChunkGenerator::sample_humidity_raw(float x, float z) const {
    float wx, wz;
    warp_climate_point(x, z, wx, wz);
    return clamp01((humidity_noise.noise_2d(wx * params.climate_humidity_scale,
                                            wz * params.climate_humidity_scale) + 1.0f) * 0.5f);
}

// The climate fields are sampled on a 4-block world-aligned lattice (one
// evaluation per 4 blocks) and bilinearly interpolated between nodes, so
// every chunk reads the same global lattice nodes (no seams) and biome
// borders follow piecewise-linear contours.
float ChunkGenerator::sample_temperature(float x, float z) const {
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

float ChunkGenerator::sample_humidity(float x, float z) const {
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

BiomeType ChunkGenerator::land_biome_from_grid(float temperature, float humidity) const {
    const int ti = temperature <= biome_config.temp_cold_max ? 0
                 : temperature >= biome_config.temp_hot_min ? 2 : 1;
    const int hi = humidity <= biome_config.hum_dry_max ? 0
                 : humidity >= biome_config.hum_humid_min ? 2 : 1;
    return kLandBiomeGrid[ti][hi];
}

BiomeType ChunkGenerator::biome_from_climate(float temperature, float humidity, float cont) const {
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
float ChunkGenerator::sample_land_shape_raw(float x, float z) const {
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
float ChunkGenerator::sample_land_shape(float x, float z, float /*temperature*/, float /*humidity*/) const {
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
float ChunkGenerator::sample_small_relief(float x, float z) const {
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
float ChunkGenerator::sample_mid_relief(float x, float z) const {
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
float ChunkGenerator::sample_weirdness(float x, float z) const {
    float raw = weirdness_noise.fbm(
        x + 12000.0f, z - 12000.0f, 3, 0.5f, params.weirdness_scale);
    return smoothstep(params.weirdness_low, params.weirdness_high, raw);
}

// Climate biome at a 4-block lattice node (world coords, multiples of
// ChunkGeneratorLattice::SPACING), evaluated from the raw samplers. This is the
// node value the chunk path caches, so both paths agree bit-for-bit.
BiomeType ChunkGenerator::biome_at_node_raw(int32_t nx, int32_t nz) const {
    return biome_from_climate(
        sample_temperature_raw(static_cast<float>(nx), static_cast<float>(nz)),
        sample_humidity_raw(static_cast<float>(nx), static_cast<float>(nz)),
        sample_continentalness(static_cast<float>(nx), static_cast<float>(nz)));
}

// Blended knobs at a single lattice node (single-point path).
BiomeAmplification ChunkGenerator::blend_amplification_node(int32_t nx, int32_t nz) const {
    return blend_amplitudes([&](int32_t di, int32_t dj) {
        return biome_at_node_raw(nx + di * ChunkGeneratorLattice::SPACING,
                                 nz + dj * ChunkGeneratorLattice::SPACING);
    });
}

// Blended knobs at an arbitrary column: the four surrounding lattice
// nodes are blended and bilinearly interpolated, mirroring the climate
// samplers' lattice structure.
BiomeAmplification ChunkGenerator::blend_amplification_at(int32_t world_x, int32_t world_z) const {
    constexpr int32_t SP = ChunkGeneratorLattice::SPACING;
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

// Per-column 3D-shaping envelope (declared in chunk_generator.hpp). The squish
// branch replaces the biome's envelope with one scaled to the slice: strength
// and reach both shrink by kBandOuter / SURFACE_BAND_OUTER, so a squished
// surface keeps the proportions of the real one (how far the displacement ramps
// in, how far it can reach) at about a ninth of the size. The reach is what
// matters for the scheduler: at kBandOuter no surface can leave its slice, so
// the band filter's one-slice window is exact rather than a guess.
ChunkGenerator::ShapeEnvelope ChunkGenerator::shape_envelope(float weirdness,
                                                            float weirdness_size) const {
    const float size = std::max(weirdness_size, 0.0f);
    ShapeEnvelope e;
    e.strength = lerp(params.shape_strength_min, params.shape_strength_max,
                      clamp01(weirdness)) * size;
    if (params.squish_enabled) {
        e.strength *= squish::kBandOuter / SURFACE_BAND_OUTER;
        e.band_inner = squish::kBandInner;
        e.band_outer = squish::kBandOuter;
        return e;
    }
    e.band_inner = SURFACE_BAND_INNER * size;
    e.band_outer = SURFACE_BAND_OUTER * size;
    return e;
}

// Effective knobs for a column: with blending disabled (radius 0) every
// column uses its own biome's knobs exactly — the interpolated blend
// field is ignored so a border is a clean step, not a 4-block lerp. With
// blending enabled, land biomes use the blended field (ramps across
// borders). Ocean columns still take the ocean biome's own knobs for the
// 3D shaping (the blend field is climate-derived and never contains
// ocean); the ocean's macro seabed height is no longer set here — it
// keeps the height the land biome gave it before the ocean override.
//
// Returned by value: `blended` is a temporary at several call sites, so a
// reference to it would dangle as soon as the call expression ended.
BiomeAmplification ChunkGenerator::amplification_for(BiomeType biome,
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
float ChunkGenerator::amplified_weirdness(float raw_mask, const BiomeAmplification& a) const {
    const float floor_mask = std::max(0.0f, a.min_weirdness - 1.0f);
    return clamp01(std::max(raw_mask * a.weirdness, floor_mask));
}

// Cheaper than sample_column: only land shape, no biome/lake evaluation.
// Mirrors sample_column's effective height amplification (own biome at
// radius 0, the blended field on land otherwise) so the scheduler's
// surface estimate tracks the generated surface when it is tuned.
float ChunkGenerator::quick_height_estimate(int32_t world_x, int32_t world_z) const {
    float x = static_cast<float>(world_x);
    float z = static_cast<float>(world_z);
    float t = sample_temperature(x, z);
    float h = sample_humidity(x, z);
    const float cont = sample_continentalness(x, z);
    const float raw = sample_land_shape(x, z, t, h);
    const BiomeType biome = (raw >= params.sea_level)
        ? biome_from_climate(t, h, cont)
        : BiomeType::Ocean;
    const BiomeAmplification amp = amplification_for(
        biome, params.climate_blend_radius_nodes > 0
                   ? blend_amplification_at(world_x, world_z)
                   : BiomeAmplification{});
    // Routed through the squish map so this estimate keeps tracking the
    // generated surface when the toggle is on (identity when it is off).
    return squish::height(params,
                          params.sea_level + (raw - params.sea_level) * amp.height);
}

bool ChunkGenerator::is_cave(int32_t x, int32_t y, int32_t z) const {
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
// Construction and configuration
// -------------------------------------------------------------------------
// The noise fields the samplers read are built here, and rebuilt when set_params
// changes the seed.

ChunkGenerator::ChunkGenerator(const TerrainParams& p)
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

BiomeType ChunkGenerator::get_biome(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z).biome;
}

float ChunkGenerator::get_terrain_height(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z).height;
}

// -------------------------------------------------------------------------
// Parameter management
// -------------------------------------------------------------------------
void ChunkGenerator::set_params(const TerrainParams& p) {
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

const TerrainParams& ChunkGenerator::get_params() const {
    return params;
}

void ChunkGenerator::set_biome_config(const BiomeConfig& config) {
    biome_config = config;
}

const BiomeConfig& ChunkGenerator::get_biome_config() const {
    return biome_config;
}

void ChunkGenerator::set_vegetation_config(const VegetationConfig& config) {
    vegetation_config = config;
}

const VegetationConfig& ChunkGenerator::get_vegetation_config() const {
    return vegetation_config;
}

// -------------------------------------------------------------------------
// Debug accessors
// -------------------------------------------------------------------------
// Each one runs the same builders and samplers the chunk path runs, so a
// disagreement means the chunk path drifted rather than that these forwarders did.

float ChunkGenerator::sample_continentalness_debug(float x, float z) const {
    return sample_continentalness(x, z);
}

ChunkGenerator::ColumnSample ChunkGenerator::sample_column_debug(int32_t world_x, int32_t world_z) const {
    return sample_column(world_x, world_z);
}

// Debug: climate values read through the chunk-cached lattice path (what
// generate_chunk uses), for cross-checking against the per-call samplers.
float ChunkGenerator::sample_temperature_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                       int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_climate(*this, chunk_x, chunk_z);
    return lattice.temperature(wx, wz);
}

float ChunkGenerator::sample_weirdness_debug(float x, float z) const {
    return sample_weirdness(x, z);
}

float ChunkGenerator::sample_temperature_debug(float x, float z) const {
    return sample_temperature(x, z);
}

float ChunkGenerator::sample_humidity_debug(float x, float z) const {
    return sample_humidity(x, z);
}

float ChunkGenerator::sample_land_shape_debug(float x, float z) const {
    return sample_land_shape(x, z, 0.0f, 0.0f);  // temp/humidity are unused
}

// Debug: macro land height read through the chunk-cached lattice path
// (what generate_chunk uses), for cross-checking against the per-call
// sampler.
float ChunkGenerator::sample_land_shape_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                      int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_land_shape(*this, chunk_x, chunk_z);
    return lattice.land_shape(wx, wz);
}

// Debug: blended amplification knobs at a column (per-call path).
BiomeAmplification ChunkGenerator::blend_amplification_debug(int32_t world_x, int32_t world_z) const {
    return blend_amplification_at(world_x, world_z);
}

// Debug: blended amplification read through the chunk-cached lattice path
// (what generate_chunk uses), for cross-checking against the per-call
// sampler.
BiomeAmplification ChunkGenerator::blend_amplification_lattice_debug(int32_t chunk_x, int32_t chunk_z,
                                                     int32_t wx, int32_t wz) const {
    ChunkGeneratorLattice lattice;
    lattice.build_climate(*this, chunk_x, chunk_z);
    lattice.build_amplification(*this, chunk_x, chunk_z);
    return lattice.amplification(wx, wz);
}

BiomeType ChunkGenerator::biome_from_climate_debug(float temperature, float humidity, float cont) const {
    return biome_from_climate(temperature, humidity, cont);
}

} // namespace VoxelEngine
