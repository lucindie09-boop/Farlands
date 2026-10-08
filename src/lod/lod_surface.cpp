#include "lod/lod_surface.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {
namespace lod {

namespace {

void push_vertex(TileMesh& mesh, float x, float y, float z, uint8_t layer, uint8_t layer2,
                 float water, float shade, float mix) {
    LodVertex v;
    v.x = x;
    v.y = y;
    v.z = z;
    // World-aligned, in blocks: one block of world is one repeat of the texture,
    // exactly as a chunk face maps it, so the far field's texels line up with the
    // near world's and a quad several blocks wide takes the mip level that
    // averages that many texels -- which is the block's average colour.
    v.u = x;
    v.v = z;
    v.layer = static_cast<float>(layer);
    v.layer2 = static_cast<float>(layer2);
    v.mix = mix;
    v.water = water;
    v.shade = shade;
    mesh.vertices.push_back(v);
}

// How many of `ring` wear `candidate`, for the two blend questions below.
int32_t count_in(const uint8_t* ring, size_t n, uint8_t candidate) {
    int32_t count = 0;
    for (size_t i = 0; i < n; ++i) {
        if (ring[i] == candidate) ++count;
    }
    return count;
}

// The most common of `ring` among the ones that are not `skip`; ties go to the first
// occurrence in the ring's own order, so the answer is the same wherever it is asked
// from. `best` comes back as the count, 0 when everything is `skip`.
uint8_t most_common_other(const uint8_t* ring, size_t n, uint8_t skip, int32_t* best) {
    uint8_t out = skip;
    *best = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t candidate = ring[i];
        if (candidate == skip) continue;
        bool first = true;
        for (size_t j = 0; j < i; ++j) {
            if (ring[j] == candidate) first = false;
        }
        if (!first) continue;
        const int32_t count = count_in(ring, n, candidate);
        if (count > *best) {
            *best = count;
            out = candidate;
        }
    }
    return out;
}

} // namespace

NodeLayerMix layer_mix(uint8_t own, uint8_t north, uint8_t south, uint8_t east, uint8_t west) {
    const uint8_t ring[4] = {north, south, east, west};
    NodeLayerMix out;
    int32_t count = 0;
    out.other = most_common_other(ring, 4, own, &count);
    // Over the node as well as its ring: the node is never `other`, so this is the
    // ring's own count out of five. A lone node of another biome is a fifth of its own
    // neighbourhood and reads as a fifth of it, which is what softens the boundary
    // instead of moving it.
    out.share = static_cast<float>(count) / 5.0f;
    return out;
}

LayerPair layer_pair(uint8_t l00, uint8_t l10, uint8_t l11, uint8_t l01,
                     const std::array<uint8_t, 4>& others) {
    const uint8_t corners[4] = {l00, l10, l11, l01};
    LayerPair out;
    int32_t count = 0;
    // `skip` is a layer no cell can have (`uint8_t` max), so nothing is skipped and the
    // first corner is the base unless another layer is strictly commoner.
    out.base = most_common_other(corners, 4, static_cast<uint8_t>(255), &count);
    out.other = most_common_other(others.data(), 4, out.base, &count);
    return out;
}

float face_shade(float hx, float hz, int32_t spacing) {
    // The cell's own upward normal, from its two height gradients. `hx`/`hz` are
    // height differences over the cell, so the gradients are those over `spacing`.
    const float s = static_cast<float>(std::max(spacing, 1));
    const float gx = hx / s;
    const float gz = hz / s;
    const float inv = 1.0f / std::sqrt(1.0f + gx * gx + gz * gz);
    // The sign of the horizontal components is irrelevant below: every weight is a
    // square, so this is the same number for a slope rising east and one rising west.
    const float nx = -gx * inv;
    const float nz = -gz * inv;
    const float ny = inv;
    // The SAME arithmetic shaders/item_lighting.gdshaderinc applies to a turning
    // body, and for the same reason. The near world's table (top 1.0, north/south
    // 0.8, east/west 0.6) is exact on a terrain face because a terrain face never
    // moves -- but a far cell's normal DOES, cell to cell, and a table with
    // thresholds in it steps a whole shade the moment a gradient crosses one. On a
    // 256-block cell a near-tie between the two diagonals flipped the cell between
    // 0.6 and 1.0, so the horizon arrived as a quilt of 40%-darker squares rather
    // than as terrain, and a 45-degree slope -- where the top test ties -- flickered
    // cell by cell. The squared components are the weights that keep the table exact
    // on the six axes and slide continuously between them, which is what makes a
    // slope one gradient of shading instead of a patchwork.
    return kShadeEastWest * nx * nx + kShadeNorthSouth * nz * nz +
           (0.75f + 0.25f * ny) * ny * ny;
}

float concavity_shade(float height, float north, float south, float east, float west,
                      int32_t spacing) {
    const float around = (north + south + east + west) * 0.25f;
    // Measured against the node's own spacing: a quarter of a cell of fall around a
    // node is a hollow, whether the cell is 8 blocks across or 256.
    const float scale = std::max(1.0f, static_cast<float>(spacing) * 0.25f);
    const float dip = (around - height) / scale;
    return 1.0f - kAoStrength * std::clamp(dip, 0.0f, 1.0f);
}

TileMesh build_tile_mesh(int32_t origin_x, int32_t origin_z, int32_t tile_size, int32_t spacing,
                         const SurfaceSampler& sample, uint8_t water_layer,
                         const std::array<int32_t, 4>& neighbour_spacing,
                         const NodeSurfaceFn& node_surface) {
    TileMesh mesh;
    if (tile_size <= 0 || spacing <= 0 || tile_size % spacing != 0) return mesh;

    const int32_t cells = tile_size / spacing;
    const int32_t nodes = cells + 1;
    // Every node is shared by up to four cells, so the samples are taken once
    // and the cells read their four corners: a tile costs (cells+1)^2 sampler
    // calls, not 4 cells^2.
    std::vector<SurfaceSample> samples(static_cast<size_t>(nodes) * nodes);
    float min_y = 1.0e30f;
    float max_y = -1.0e30f;
    float min_x = 1.0e30f;
    float max_x = -1.0e30f;
    float min_z = 1.0e30f;
    float max_z = -1.0e30f;
    for (int32_t j = 0; j < nodes; ++j) {
        for (int32_t i = 0; i < nodes; ++i) {
            const int32_t x = origin_x + i * spacing;
            const int32_t z = origin_z + j * spacing;
            SurfaceSample s = sample(x, z);
            if (s.valid) {
                min_y = std::min(min_y, s.height);
                max_y = std::max(max_y, s.height);
                if (s.water > s.height) max_y = std::max(max_y, s.water);
                const float fx = static_cast<float>(x);
                const float fz = static_cast<float>(z);
                min_x = std::min(min_x, fx);
                max_x = std::max(max_x, fx);
                min_z = std::min(min_z, fz);
                max_z = std::max(max_z, fz);
            }
            samples[static_cast<size_t>(j) * nodes + i] = s;
        }
    }
    if (min_y > max_y) {
        mesh.min_y = 0.0f;
        mesh.max_y = 0.0f;
        return mesh;
    }

    // The edges this tile shares with a COARSER neighbour take that neighbour's
    // chord. Two surfaces sampled at different spacings agree exactly at the nodes
    // they have in common and fan apart everywhere between them, and the wedge
    // between the fine surface and the coarse chord is the crack that shows along
    // every spacing-level boundary. Giving up the fine detail along that one row of
    // cells is what closes it; a skirt hanging below the edge would only hide it.
    //
    // Each edge's anchors are the nodes the coarse neighbour also has (every
    // `ratio`th one), and snapping only writes the nodes in between, so the anchors
    // are read before anything is written and the pass order cannot matter.
    auto edge_node = [nodes](int32_t edge, int32_t k) -> size_t {
        switch (edge) {
        case 0: return static_cast<size_t>(k) * static_cast<size_t>(nodes);
        case 1: return static_cast<size_t>(k) * static_cast<size_t>(nodes) + static_cast<size_t>(nodes - 1);
        case 2: return static_cast<size_t>(k);
        default: return static_cast<size_t>(nodes - 1) * static_cast<size_t>(nodes) + static_cast<size_t>(k);
        }
    };
    for (int32_t edge = 0; edge < 4; ++edge) {
        const int32_t coarse = neighbour_spacing[static_cast<size_t>(edge)];
        if (coarse <= spacing || coarse % spacing != 0) continue;
        const int32_t ratio = coarse / spacing;
        for (int32_t k = 0; k + ratio < nodes; k += ratio) {
            const SurfaceSample& a = samples[edge_node(edge, k)];
            const SurfaceSample& b = samples[edge_node(edge, k + ratio)];
            if (!a.valid || !b.valid) continue;
            const float h0 = a.height;
            const float span = b.height - a.height;
            for (int32_t m = 1; m < ratio; ++m) {
                SurfaceSample& mid = samples[edge_node(edge, k + m)];
                if (!mid.valid) continue;
                mid.height = h0 + span * (static_cast<float>(m) / static_cast<float>(ratio));
            }
        }
    }
    // Snapping moves heights, so the vertical bounds are taken after it.
    for (const SurfaceSample& s : samples) {
        if (!s.valid) continue;
        min_y = std::min(min_y, s.height);
        max_y = std::max(max_y, s.water > s.height ? s.water : s.height);
    }
    mesh.min_x = min_x;
    mesh.max_x = max_x;
    mesh.min_z = min_z;
    mesh.max_z = max_z;
    mesh.min_y = min_y;
    mesh.max_y = max_y;

    mesh.vertices.reserve(static_cast<size_t>(cells) * cells * 6);
    for (int32_t j = 0; j < cells; ++j) {
        for (int32_t i = 0; i < cells; ++i) {
            const SurfaceSample& s00 = samples[static_cast<size_t>(j) * nodes + i];
            const SurfaceSample& s10 = samples[static_cast<size_t>(j) * nodes + i + 1];
            const SurfaceSample& s01 = samples[static_cast<size_t>(j + 1) * nodes + i];
            const SurfaceSample& s11 = samples[static_cast<size_t>(j + 1) * nodes + i + 1];
            if (!s00.valid || !s10.valid || !s01.valid || !s11.valid) continue;

            const float x0 = static_cast<float>(origin_x + i * spacing);
            const float x1 = static_cast<float>(origin_x + (i + 1) * spacing);
            const float z0 = static_cast<float>(origin_z + j * spacing);
            const float z1 = static_cast<float>(origin_z + (j + 1) * spacing);

            // EVERY CORNER WET, OR NO WATER AT ALL. A cell with a land sample anywhere on
            // it is drawn as land, whole, and draws no water at all.
            //
            // The sampler reads the world seed on a lattice a cell apart and joins the
            // samples with straight lines, so a cell that straddles a coast has an
            // underwater corner and a dry one and its interpolated surface crosses the water
            // level somewhere inside it. WHERE it crosses is a guess: the real shoreline
            // between two samples a cell apart lies anywhere in the cell, and on gentle
            // ground the guess is wrong by most of a cell's width -- so a sheet drawn from
            // that crossing is water over ground the samples call land. (Cutting the cell at
            // the crossing narrows the guess; it does not stop it being a guess, which is
            // why the report survived the cut.)
            //
            // "Every corner wet" is the sampler's own answer rather than a guess built on
            // top of it, and it is all the far field can know about a cell without inventing
            // detail. What it costs is the coast's last cell: the sea ends at the last
            // wholly wet cell, up to one cell short of where it should, and the strip beyond
            // reads as beach (docs/lod-modes.md).
            //
            // The old shape of this cell drew the land AND a sheet over the whole cell, so
            // the two covered the same ground: the far field's coast came back as the
            // sheet's light blue and the terrain's grass interleaved one pixel at a time all
            // along the shore, which is the report (probe_lod_grid_overlap.gd measured
            // 0.26-0.33 of the far field's drawn pixels covered by both surfaces).
            //
            // It also makes the whole of an ocean cell one quad and no corner work: four
            // heights below one water level cannot cross it in between -- the world has one
            // water level (src/worldgen/chunk_generator_columns.cpp) and every wet sample
            // carries it -- so the sheet alone is that cell's surface, and the floor under a
            // sheet is never drawn.
            const bool wet = s00.water > s00.height && s10.water > s10.height &&
                             s01.water > s01.height && s11.water > s11.height;
            if (wet) {
                // The shallowest of the four, so the sheet never stands above a corner's own
                // water. (One water level in this world, so they are the same number.)
                const float water = std::min(std::min(s00.water, s10.water),
                                             std::min(s01.water, s11.water));
                // Flat and level: a liquid surface has no slope to shade, so it takes the
                // top constant and the water layer, and only its height comes from the cell.
                push_vertex(mesh, x0, water, z0, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                push_vertex(mesh, x1, water, z0, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                push_vertex(mesh, x1, water, z1, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                push_vertex(mesh, x0, water, z0, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                push_vertex(mesh, x1, water, z1, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                push_vertex(mesh, x0, water, z1, water_layer, water_layer, 1.0f, kShadeTop, 0.0f);
                ++mesh.water_quads;
            } else {
                // The cell's slope, as the mean of its two diagonals rather than the
                // s00 corner's own edges: the corners of a saddle disagree about
                // which way the cell leans, and reading one of them decided the whole
                // cell's shading off a quarter of its surface (see face_shade).
                const float hx = ((s10.height - s00.height) + (s11.height - s01.height)) * 0.5f;
                const float hz = ((s01.height - s00.height) + (s11.height - s10.height)) * 0.5f;
                const float cell_shade = face_shade(hx, hz, spacing);
                // ...and each corner wears the cell's face constant times ITS OWN
                // occlusion: a dip darkens across the cell instead of taking the
                // whole cell down a step, which is the difference between a shaded
                // hollow and a patch.
                const int32_t n0x = origin_x + i * spacing;
                const int32_t n1x = origin_x + (i + 1) * spacing;
                const int32_t n0z = origin_z + j * spacing;
                const int32_t n1z = origin_z + (j + 1) * spacing;
                const NodeSurface corner00 = node_surface ? node_surface(n0x, n0z) : NodeSurface{};
                const NodeSurface corner10 = node_surface ? node_surface(n1x, n0z) : NodeSurface{};
                const NodeSurface corner01 = node_surface ? node_surface(n0x, n1z) : NodeSurface{};
                const NodeSurface corner11 = node_surface ? node_surface(n1x, n1z) : NodeSurface{};
                const std::array<uint8_t, 4> others{corner00.mix.other, corner10.mix.other,
                                                   corner11.mix.other, corner01.mix.other};
                const LayerPair pair = layer_pair(s00.layer, s10.layer, s11.layer, s01.layer, others);
                // How much of the pair's OTHER layer a corner wears. A corner of that
                // layer wears everything its own share leaves -- its share counts the
                // layers that are not its own -- and a corner of the base layer wears
                // its own neighbourhood's share of the other. Both come out of the same
                // five-node neighbourhood, so two cells sharing the corner agree there
                // and the ramp has no step where they meet.
                auto corner_mix = [&pair](const NodeSurface& n, uint8_t own) -> float {
                    if (pair.other == pair.base) return 0.0f;
                    if (own == pair.other) return std::clamp(1.0f - n.mix.share, 0.0f, 1.0f);
                    return n.mix.other == pair.other ? n.mix.share : 0.0f;
                };
                const float mix00 = corner_mix(corner00, s00.layer);
                const float mix10 = corner_mix(corner10, s10.layer);
                const float mix11 = corner_mix(corner11, s11.layer);
                const float mix01 = corner_mix(corner01, s01.layer);
                // The world's own top-face order (see MeshBuilder::kFaceVertices):
                // (x0,z0), (x1,z0), (x1,z1), (x0,z1), triangles 0-1-2 and 0-2-3.
                constexpr int32_t kTriangles[6] = {0, 1, 2, 0, 2, 3};
                const float cx[4] = {x0, x1, x1, x0};
                const float cz[4] = {z0, z0, z1, z1};
                const float cy[4] = {s00.height, s10.height, s11.height, s01.height};
                const float cshade[4] = {cell_shade * corner00.ao, cell_shade * corner10.ao,
                                         cell_shade * corner11.ao, cell_shade * corner01.ao};
                const float cmix[4] = {mix00, mix10, mix11, mix01};
                const uint8_t layer = pair.base;
                const uint8_t blend_layer = pair.other;
                for (int32_t k = 0; k < 6; ++k) {
                    const size_t c = static_cast<size_t>(kTriangles[k]);
                    push_vertex(mesh, cx[c], cy[c], cz[c], layer, blend_layer, 0.0f, cshade[c],
                                cmix[c]);
                }
                ++mesh.terrain_quads;
            }
        }
    }
    return mesh;
}

} // namespace lod
} // namespace VoxelEngine
