// Render the elevation field at several domain-warp strengths to separate BMPs
// so the circular-vs-directional shape change can be judged visually.
//
// Usage:  bin\elevation_warp_sweep [size] [center_x] [center_z] [out_prefix]
//
// Emits <out_prefix>_{warp00..NN}.bmp where NN is the warp amplitude in blocks
// per block of noise. Each is the SAME location with a different warp strength.

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
    // land: green valley -> tan -> brown -> gray peak; below 0 (water) -> blue
    if (t < 0.0f) {
        r = 20; g = 40; b = 120;
        return;
    }
    if (t < 0.25f)      { r = 76; g = 140; b = 60; }
    else if (t < 0.5f)  { r = 160; g = 140; b = 80; }
    else if (t < 0.75f) { r = 130; g = 110; b = 80; }
    else                { r = 190; g = 190; b = 195; }
}

} // namespace

int main(int argc, char** argv) {
    int size = 64;
    int cx = 0, cz = 0;
    std::string prefix = "bin/elev_warp";
    if (argc > 1) size = std::atoi(argv[1]);
    if (argc > 2) cx = std::atoi(argv[2]);
    if (argc > 3) cz = std::atoi(argv[3]);
    if (argc > 4) prefix = argv[4];
    size = std::max(4, size);

    // Matches VoxelEngine::ChunkGenerator's elevation config (see terrain_params).
    const float elevation_scale = 0.001f, elevation_bias = 0.35f;
    const int seed = 10000;
    VoxelEngine::FastNoise elevation_noise(seed);
    const float amp = 60.0f; // elevation_amplitude for absolute height scaling

    // Warp strengths to compare: 0 = raw blob baseline; then smears.
    const std::vector<float> warps = {0.0f, 30.0f, 80.0f, 160.0f, 300.0f};

    const int lo = -size / 2;
    for (float wamp : warps) {
        int min_ht = 1 << 30, max_ht = -1;
        std::vector<int> height(static_cast<size_t>(size) * size);
        for (int lz = 0; lz < size; lz++) {
            for (int lx = 0; lx < size; lx++) {
                const int wx = cx + lo + lx;
                const int wz = cz + lo + lz;
                float fx = static_cast<float>(wx);
                float fz = static_cast<float>(wz);
                if (wamp > 0.0f) {
                    float wx_ = elevation_noise.fbm(fx, fz, 3, 0.5f, elevation_scale * 2.0f);
                    float wz_ = elevation_noise.fbm(fx + 3000.0f, fz - 3000.0f, 3, 0.5f, elevation_scale * 2.0f);
                    fx += wx_ * wamp;
                    fz += wz_ * wamp;
                }
                float elev = elevation_noise.fbm(fx, fz, 2, 0.5f, elevation_scale);
                elev = std::clamp(elev + elevation_bias, -1.0f, 1.0f) * amp + 64.0f;
                int top = static_cast<int>(std::ceil(elev));
                height[static_cast<size_t>(lz) * size + lx] = top;
                min_ht = std::min(min_ht, top);
                max_ht = std::max(max_ht, top);
            }
        }

        const int ppb = 8, w = size * ppb, h = size * ppb;
        std::vector<uint8_t> img(static_cast<size_t>(w) * h * 3);
        const float range = std::max(1.0f, (float)(max_ht - min_ht));
        for (int lz = 0; lz < size; lz++) {
            for (int lx = 0; lx < size; lx++) {
                const int top = height[static_cast<size_t>(lz) * size + lx];
                float t = (float)(top - min_ht) / range;
                uint8_t r, g, b;
                color_for_height(t, r, g, b);
                for (int py = 0; py < ppb; py++)
                    for (int px = 0; px < ppb; px++) {
                        size_t idx = ((size_t)(lz * ppb + py) * w + (lx * ppb + px)) * 3;
                        img[idx] = r; img[idx + 1] = g; img[idx + 2] = b;
                    }
            }
        }

        char fname[256];
        std::snprintf(fname, sizeof(fname), "%s_warp%03d.bmp", prefix.c_str(), (int)wamp);
        write_bmp_rgb24(fname, w, h, img);
        std::printf("warp=%5.0f range=%d..%d wrote %s\n", wamp, min_ht, max_ht, fname);
    }
    return 0;
}