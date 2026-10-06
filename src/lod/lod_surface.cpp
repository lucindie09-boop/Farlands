#include "lod/lod_surface.hpp"

#include <algorithm>
#include <cmath>

namespace VoxelEngine {
namespace lod {

namespace {

void push_vertex(TileMesh& mesh, float x, float y, float z, uint8_t layer, float water,
                 float shade) {
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
    v.water = water;
    v.shade = shade;
    mesh.vertices.push_back(v);
}

} // namespace

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
                         float floor_skip_depth, const std::array<int32_t, 4>& neighbour_spacing,
                         const NodeShade& node_shade) {
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

            // The water body this cell belongs to is the deepest of its corners,
            // so a shoreline cell is covered rather than half covered.
            const float water = std::max(std::max(s00.water, s10.water),
                                         std::max(s01.water, s11.water));
            const float floor_min = std::min(std::min(s00.height, s10.height),
                                             std::min(s01.height, s11.height));
            const float floor_max = std::max(std::max(s00.height, s10.height),
                                             std::max(s01.height, s11.height));
            const bool submerged = water > floor_min;
            // The floor is skipped only when the WHOLE cell is under the water: a
            // cell that rises out of it is a shoreline, and its terrain is the
            // beach the water meets -- the one surface a player looks at directly.
            // Skipping it on the strength of the deepest corner alone used to take
            // the beach with it, leaving the water sheet ending at the cell edge
            // while the land behind it rose above that sheet: the two surfaces stop
            // at the same line at different heights, so nothing bridges the step,
            // and the frame shows the inside of the hill through the gap. The
            // remaining skips are strictly below a sheet that is opaque in
            // shaders/lod_grid.gdshader, so nothing can be seen through them.
            const bool deep = submerged && (water - floor_min) > floor_skip_depth &&
                              floor_max <= water;

            if (!deep) {
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
                const float ao00 = node_shade ? node_shade(n0x, n0z) : 1.0f;
                const float ao10 = node_shade ? node_shade(n1x, n0z) : 1.0f;
                const float ao01 = node_shade ? node_shade(n0x, n1z) : 1.0f;
                const float ao11 = node_shade ? node_shade(n1x, n1z) : 1.0f;
                const uint8_t layer = s00.layer;
                // The world's own top-face order (see MeshBuilder::kFaceVertices):
                // (x0,z0), (x1,z0), (x1,z1), (x0,z1), triangles 0-1-2 and 0-2-3.
                push_vertex(mesh, x0, s00.height, z0, layer, 0.0f, cell_shade * ao00);
                push_vertex(mesh, x1, s10.height, z0, layer, 0.0f, cell_shade * ao10);
                push_vertex(mesh, x1, s11.height, z1, layer, 0.0f, cell_shade * ao11);
                push_vertex(mesh, x0, s00.height, z0, layer, 0.0f, cell_shade * ao00);
                push_vertex(mesh, x1, s11.height, z1, layer, 0.0f, cell_shade * ao11);
                push_vertex(mesh, x0, s01.height, z1, layer, 0.0f, cell_shade * ao01);
                ++mesh.terrain_quads;
            }

            if (submerged) {
                // Flat and level: a liquid surface has no slope to shade, so it
                // takes the top constant and the water layer.
                push_vertex(mesh, x0, water, z0, water_layer, 1.0f, kShadeTop);
                push_vertex(mesh, x1, water, z0, water_layer, 1.0f, kShadeTop);
                push_vertex(mesh, x1, water, z1, water_layer, 1.0f, kShadeTop);
                push_vertex(mesh, x0, water, z0, water_layer, 1.0f, kShadeTop);
                push_vertex(mesh, x1, water, z1, water_layer, 1.0f, kShadeTop);
                push_vertex(mesh, x0, water, z1, water_layer, 1.0f, kShadeTop);
                ++mesh.water_quads;
            }
        }
    }
    return mesh;
}

} // namespace lod
} // namespace VoxelEngine
