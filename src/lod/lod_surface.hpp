#ifndef FARLANDS_LOD_SURFACE_HPP
#define FARLANDS_LOD_SURFACE_HPP
#include <array>
#include <cstdint>
#include <functional>
#include <vector>

namespace VoxelEngine {
namespace lod {

// The far-field surface: no blocks, no chunks, no light. One sample per grid node
// is the whole input and one textured quad per cell the whole output, which is
// what lets terrain at eight kilometres cost a function call instead of a
// generated chunk (docs/lod-modes.md).
//
// This file is the arithmetic only — it names no Godot type and allocates nothing
// but its own vertex list, so the suite can drive it with an analytic surface and
// the engine layer can pack the result however the renderer wants it.

// A column with no water: far above any world height, so `water > height` is
// false everywhere and no comparison needs a sentinel check.
inline constexpr float kNoWater = -1.0e9f;

struct SurfaceSample {
    float height = 0.0f;
    float water = kNoWater;
    // Texture-array layer for the surface block. The engine resolves the
    // biome's surface block to its top-face layer, so the far field wears the
    // same textures the world does (and a far quad's minified sample is that
    // block's average colour for free).
    uint8_t layer = 0;
    // False when the column could not be sampled (outside the world, or a
    // generator that answered nothing). The cell is skipped rather than filled,
    // so a failure shows as a hole and never as invented terrain.
    bool valid = false;
};

// One corner of one quad. `u`/`v` are world-aligned texture coordinates in
// BLOCKS — one block of world is one texture repeat, the same mapping a chunk
// face uses — so a quad several blocks wide advances the coordinate by its own
// width, the sampler takes the matching mip level, and the far surface shows each
// block's average colour for free. (Putting the coordinates on the lattice
// instead — fract of the world position — would hand the sampler a zero
// derivative and stretch one texel across the whole quad.) `layer` is the
// texture array layer to sample, `water` 1 for a liquid quad and 0 for terrain,
// `shade` the baked face constant described below.
struct LodVertex {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    // World block coordinates, not normalised: see above.
    float u = 0.0f;
    float v = 0.0f;
    float layer = 0.0f;
    // The layer this vertex's own corner blends TOWARD, and how much of it: a
    // boundary between two biomes is then a ramp across a cell instead of a line
    // along its edge (see layer_mix). Both are the cell's pair, so they are constant
    // across the quad -- a layer index interpolated between two different layers
    // would sample a third texture that is neither of them.
    float layer2 = 0.0f;
    float mix = 0.0f;
    float water = 0.0f;
    float shade = 1.0f;
};

struct TileMesh {
    // Six vertices per quad (two triangles, deindexed): the far field has no
    // vertex sharing worth an index buffer, and a deindexed layout is what the
    // engine layer hands to ArrayMesh without a remap.
    std::vector<LodVertex> vertices;
    // The mesh's own bounds, in the same world coordinates as the vertices. The
    // engine layer builds the instance's cull box from these rather than from the
    // tile's index: the two cannot then disagree, which is the difference between
    // a far tile that is culled away and one that is merely far away.
    float min_x = 0.0f;
    float max_x = 0.0f;
    float min_z = 0.0f;
    float max_z = 0.0f;
    float min_y = 0.0f;
    float max_y = 0.0f;
    // One per cell that emitted that surface, so a cell whose land came back as two
    // triangles and a cell that filled the cell with its sheet both count once.
    int32_t terrain_quads = 0;
    int32_t water_quads = 0;
};

using SurfaceSampler = std::function<SurfaceSample(int32_t x, int32_t z)>;

// The far field's biome boundaries, one node at a time. The near world's biome
// blending happens in worldgen, where amplification and surface materials are
// already mixed; out here a cell is one flat quad wearing one layer, so a boundary
// between two biomes arrived as a straight line as many blocks long as the cell is
// wide -- sand against grass with a razor edge on it.
//
// The neighbourhood is the node and the four nodes one spacing away from it, which
// the mesh already reads for its occlusion, and the answer is a SHARE rather than a
// flag: a fraction is what makes the blend a ramp, and reading it from the same
// neighbourhood in every cell that touches the node is what keeps the ramp free of
// seams.
struct NodeLayerMix {
    // The most common layer among the ring that is not the node's own. Equal to the
    // node's own when the whole neighbourhood is one biome.
    uint8_t other = 0;
    // How much of the five-node neighbourhood -- the node itself and the four ring
    // nodes -- wears `other`. Zero when there is no other.
    float share = 0.0f;
};

// `north`/`south` are -/+z and `east`/`west` are +/-x, the same axes face_shade and
// concavity_shade are named for. Ties go to the first of the ring in that order, so
// the answer never depends on which cell is asking.
NodeLayerMix layer_mix(uint8_t own, uint8_t north, uint8_t south, uint8_t east, uint8_t west);

// The two layers a cell blends between: `base` is what most of its corners wear and
// `other` the layer a boundary crossing the cell brings in. `other == base` on a cell
// that is one biome through and through, and then nothing blends at all.
struct LayerPair {
    uint8_t base = 0;
    uint8_t other = 0;
};

// `l00`..`l01` are the four corners' own layers in the world's own corner order and
// `others` the four corners' NodeLayerMix::other, in the same order. Ties go to the
// first in that order, so base is the cell's own first corner when nothing is
// commoner than it.
LayerPair layer_pair(uint8_t l00, uint8_t l10, uint8_t l11, uint8_t l01,
                     const std::array<uint8_t, 4>& others);

// The far field's face constants: top 1.0, north/south 0.8, east/west 0.6 -- the
// same numbers `shaders/voxel_shader.gdshader` applies to a chunk face, so the far
// field is lit like the world it continues. Read the rule over all three
// (face_shade) rather than picking one.
inline constexpr float kShadeTop = 1.0f;
inline constexpr float kShadeNorthSouth = 0.8f;
inline constexpr float kShadeEastWest = 0.6f;

// The baked face constant for a cell whose surface rises by `hx` over `spacing`
// blocks along +x and by `hz` along +z. The constants above are reached exactly on
// the axes and blended CONTINUOUSLY between them by the cell's own normal (the same
// arithmetic `shaders/item_lighting.gdshaderinc` uses for a turning body), so two
// cells that differ by a hair are shaded within a hair of each other.
float face_shade(float hx, float hz, int32_t spacing);

// The far field's occlusion, for one node: how much LOWER that node sits than the
// four nodes one spacing away from it (north/south are -/+z and east/west are +/-x,
// the same axes the face constants above are named for). A node standing level with
// its neighbours is open and gets nothing; one sitting in a dip is darkened by how
// deep the dip is, measured against the node's own spacing -- a quarter of a cell of
// fall around it is a full one -- so the term means the same thing at the innermost
// spacing and out at the horizon's.
//
// This is the far field's AO, and it is the only occlusion a body of geometry with
// no blocks can have: the near world's comes from the cells around a face, and there
// are no cells out here. It is baked per CORNER (so a valley darkens across a cell
// instead of stepping at its edge) and multiplied by the cell's face constant -- the
// same order the terrain shader applies them in.
inline constexpr float kAoStrength = 0.35f;
float concavity_shade(float height, float north, float south, float east, float west,
                      int32_t spacing);

// A node's occlusion and its biome mix, as the tile builder reads them. The engine
// layer supplies both from its shared node table (lod_node_cache.hpp), which is why
// this is a lookup rather than arithmetic over samples: the four ring nodes these
// need are its neighbours' own, already wanted by the tiles that own them, and the
// two questions are asked of the same four.
struct NodeSurface {
    float ao = 1.0f;
    NodeLayerMix mix;
};

// An empty callback means "no occlusion and no blending", which is what the tests
// that only care about geometry use.
using NodeSurfaceFn = std::function<NodeSurface(int32_t x, int32_t z)>;

// Builds one tile. `origin_x`/`origin_z` are world block coordinates and are
// expected to be multiples of `tile_size`, which in turn is a multiple of
// `spacing`: nodes then land on global multiples of `spacing`, so two tiles at
// the same spacing share their edge nodes exactly and no crack opens between
// them.
//
// EVERY CORNER WET, OR NO WATER AT ALL. A cell draws its sheet only when all four of
// its samples are under their own water; a cell with a land sample anywhere on it is
// drawn as land, whole, and draws no water at all.
//
// The samples are a cell apart and the cell's corners are joined by straight lines, so
// a cell that straddles a coast has an underwater corner and a dry one and its
// interpolated surface crosses the water level somewhere inside it — but WHERE it
// crosses is a guess, and the real shoreline between two samples that far apart lies
// anywhere in the cell. A sheet drawn from that guess is water over ground the samples
// call land, which is what a far coastline's light blue interleaved with its own grass
// was. Drawing the land AND the sheet over the whole cell instead is the same guess
// made worse: the two then cover the same ground pixel for pixel.
//
// What the rule costs is the coast's last cell — the sea ends at the last wholly wet
// cell, up to one cell short of where it should, and the strip beyond reads as beach.
// What it buys is that a far cell can only ever be one surface, so nothing the sampler
// answered is contradicted, and an ocean cell is one quad with no corner work at all
// (four heights under one water level cannot cross it in between, so the sheet alone is
// that cell's surface and the floor under a sheet is never drawn).
//
// `neighbour_spacing` is the spacing of the tile across each edge, in the order
// -x, +x, -z, +z, with 0 for "no tile there" (the world inside the grid, or off
// the outer ring). Where a neighbour is COARSER than this tile, the shared edge is
// snapped onto that neighbour's chord: two height fields that agree only at the
// shared nodes fan apart between them, and the wedge between the two surfaces is
// the crack visible along every spacing-level boundary. Snapping costs the fine
// tile a little detail along one row of cells and closes the crack exactly, where
// a skirt below the edge would only hide it from most angles.
// `node_surface` is read once per cell corner: it answers how much that corner is
// occluded and which layer it blends toward. Both are quantities of the NODE rather
// than of the cell, so two cells sharing a corner agree there and neither the
// occlusion nor the blend has a seam along the edge between them.
TileMesh build_tile_mesh(int32_t origin_x, int32_t origin_z, int32_t tile_size, int32_t spacing,
                         const SurfaceSampler& sample, uint8_t water_layer,
                         const std::array<int32_t, 4>& neighbour_spacing = {},
                         const NodeSurfaceFn& node_surface = {});

} // namespace lod
} // namespace VoxelEngine

#endif // FARLANDS_LOD_SURFACE_HPP
