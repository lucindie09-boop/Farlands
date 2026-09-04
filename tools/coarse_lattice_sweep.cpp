// Render the elevation field as a COARSE WORLD-ALIGNED LATTICE at several
// spacings / smoothing modes, to judge whether it breaks the circular-blob
// look. Lattice is world-aligned (lattice_base helper) so chunk generation
// would sample the same global grid — no seams.
//
// Usage:  bin\coarse_lattice_sweep [size] [center_x] [center_z] [range_per_color]
//   size            - render span in blocks (e.g. 512)
//   center_x/z      - world coords of center
//   range_per_color - blocks of relief stretched to full color ramp (auto if 0)
//
// Emits bin\cl_{near|flat33|flat100|bilin33|bilin100|billow33|billow100}*.bmp

#include "core/noise.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

void write_bmp_rgb24(const std::string& out, int w, int h, const std::vector<uint8_t>& img) {
    const int row_pad = (4 - (w * 3) % 4) % 4;
    const int data_size = (w * 3 + row_pad) * h;
    uint32_t file_size = 54 + data_size;
    std::vector<uint8_t> bmp(file_size);
    bmp[0] = 'B'; bmp[1] = 'M';
    bmp[2] = file_size & 0xff; bmp[3] = (file_size >> 8) & 0xff;
    bmp[4] = (file_size >> 16) & 0xff; bmp[5] = (file_size >> 24) & 0xff;
    bmp[10] = 54;
    bmp[14] = 40;
    bmp[18] = w & 0xff; bmp[19] = (w >> 8) & 0xff; bmp[20] = (w >> 16) & 0xff; bmp[21] = (w >> 24) & 0xff;
    bmp[22] = h & 0xff; bmp[23] = (h >> 8) & 0xff; bmp[24] = (h >> 16) & 0xff; bmp[25] = (h >> 24) & 0xff;
    bmp[26] = 1; bmp[28] = 24;
    size_t src = 0;
    for (int y = h - 1; y >= 0; y--) {
        size_t dst = 54 + (size_t)y * (w * 3 + row_pad);
        std::memcpy(&bmp[dst], &img[src], (size_t)w * 3);
        src += (size_t)w * 3;
    }
    FILE* f = fopen(out.c_str(), "wb");
    if (f) { fwrite(bmp.data(), 1, bmp.size(), f); fclose(f); }
}

void color_for_height(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (t < 0.0f) { r = 20; g = 40; b = 120; return; }
    if (t < 0.25f)      { r = 76; g = 140; b = 60; }
    else if (t < 0.5f)  { r = 160; g = 140; b = 80; }
    else if (t < 0.75f) { r = 130; g = 110; b = 80; }
    else                { r = 190; g = 190; b = 195; }
}

int32_t lattice_base(int32_t v, int32_t spacing) {
    const int32_t q = v / spacing;
    const int32_t r = v % spacing;
    return (r < 0 ? q - 1 : q) * spacing;
}

float lerp(float a, float b, float t) { return a + t * (b - a); }

} // namespace

// field_floor: nearest-sample x, z clamped to [0,1] so it can bolt onto a
// coarse noise lattice used for frequency scaling. Not used for lattice.
int main(int argc, char** argv) {
    int size = 512;
    int cx = 0, cz = 0;
    float range_for_color = 0.0f;
    if (argc > 1) size = std::atoi(argv[1]);
    if (argc > 2) cx = std::atoi(argv[2]);
    if (argc > 3) cz = std::atoi(argv[3]);
    if (argc > 4) range_for_color = (float)std::atof(argv[4]);
    size = std::max(4, size);

    const float elevation_scale = 0.001f, elevation_bias = 0.35f, amplitude = 60.0f;
    VoxelEngine::FastNoise elevation_noise(10000);

    // Configs: spacing (blocks) and smoothing (0=none/hard walls, 1=full bilinear).
    struct Cfg { const char* tag; int spacing; float smooth; };
    const std::vector<Cfg> cfgs = {
        {"near",   8, 0.0f},   // ~per-chunk scale, hard walls
        {"flat100", 100, 0.0f}, // coarse plateaus, no smoothing
        {"bilin100",100, 1.0f}, // coarse, smooth interpolation between nodes
        {"flat48", 48, 0.0f},
        {"bilin48",48, 1.0f},
    };

    // Self-contained elevation sample on a coarse world-aligned lattice.
    auto sample_elev = [&](float wx, float wz, int spacing, float smooth) -> float {
        const float SP = (float)spacing;
        int32_t ix = lattice_base((int32_t)std::floor(wx), spacing);
        int32_t iz = lattice_base((int32_t)std::floor(wz), spacing);
        float fx = (wx - (float)ix) / SP;
        float fz = (wz - (float)iz) / SP;

        auto val = [&](int32_t gx, int32_t gz) -> float {
            return elevation_noise.fbm((float)gx, (float)gz, 2, 0.5f, elevation_scale);
        };
        float h00 = val(ix, iz),       h10 = val(ix + spacing, iz);
        float h01 = val(ix, iz + spacing), h11 = val(ix + spacing, iz + spacing);

        float raw;
        if (smooth >= 0.5f) {
            raw = lerp(lerp(h00, h10, fx), lerp(h01, h11, fx), fz);
        } else {
            // nearest corner (piecewise-flat cells -> hard walls between)
            int32_t gx = fx < 0.5f ? ix : ix + spacing;
            int32_t gz = fz < 0.5f ? iz : iz + spacing;
            raw = val(gx, gz);
        }
        return std::atan(raw) * 1.3f + elevation_bias; // soft tanh-ish contour
    };

    for (const Cfg& c : cfgs) {
        int min_ht = 1 << 30, max_ht = -1;
        std::vector<int> height(static_cast<size_t>(size) * size);
        const int lo = -size / 2;
        for (int lz = 0; lz < size; lz++)
            for (int lx = 0; lx < size; lx++) {
                int wx = cx + lo + lx, wz = cz + lo + lz;
                float elev = sample_elev((float)wx, (float)wz, c.spacing, c.smooth);
                // map elevation(-) ~[-1,1] to height around 60-100
                int top = (int)std::ceil((elev + 1.0f) * 0.5f * amplitude) + 30;
                top = std::max(0, std::min(1023, top));
                height[(size_t)lz * size + lx] = top;
                min_ht = std::min(min_ht, top);
                max_ht = std::max(max_ht, top);
            }

        const int ppb = 4, w = size * ppb, h = size * ppb;
        std::vector<uint8_t> img((size_t)w * h * 3);
        float range = range_for_color > 0.0f ? range_for_color : (float)(max_ht - min_ht);
        for (int lz = 0; lz < size; lz++)
            for (int lx = 0; lx < size; lx++) {
                int top = height[(size_t)lz * size + lx];
                float t = (float)(top - min_ht) / range;
                uint8_t r, g, b;
                color_for_height(t, r, g, b);
                for (int py = 0; py < ppb; py++)
                    for (int px = 0; px < ppb; px++) {
                        size_t idx = ((size_t)(lz * ppb + py) * w + (lx * ppb + px)) * 3;
                        img[idx] = r; img[idx + 1] = g; img[idx + 2] = b;
                    }
            }

        char fname[256];
        std::snprintf(fname, sizeof(fname), "bin/cl_%s.bmp", c.tag);
        write_bmp_rgb24(fname, w, h, img);
        std::printf("%-9s sp=%4d smooth=%.0f range=%d..%d wrote %s\n",
                    c.tag, c.spacing, c.smooth, min_ht, max_ht, fname);
    }
    return 0;
}