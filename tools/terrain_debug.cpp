#include "worldgen/chunk_generator.hpp"
#include <cstdlib>
#include <cstdio>
#include <cstdint>
#include <vector>

// Write a 24-bit RGB buffer as a BMP file
static void write_bmp_rgb24(const char* filename, int w, int h, const uint8_t* pixels_rgb) {
    int row_size = ((w * 3 + 3) / 4) * 4; // padded to 4 bytes
    int data_size = row_size * h;
    int file_size = 14 + 40 + data_size;

    std::vector<uint8_t> buf(file_size);
    int ofs = 0;

    // BMP file header
    buf[ofs++] = 'B'; buf[ofs++] = 'M';
    uint32_t fsize = static_cast<uint32_t>(file_size);
    buf[ofs++] = static_cast<uint8_t>(fsize & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((fsize >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((fsize >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((fsize >> 24) & 0xFF);
    ofs += 4; // reserved
    uint32_t pix_offset = 14 + 40;
    buf[ofs++] = static_cast<uint8_t>(pix_offset & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 24) & 0xFF);

    // DIB header (40 bytes)
    uint32_t bi_size = 40;
    buf[ofs++] = static_cast<uint8_t>(bi_size & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 24) & 0xFF);
    uint32_t iw = static_cast<uint32_t>(w);
    buf[ofs++] = static_cast<uint8_t>(iw & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 24) & 0xFF);
    uint32_t ih = static_cast<uint32_t>(h);
    buf[ofs++] = static_cast<uint8_t>(ih & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 24) & 0xFF);
    uint16_t planes = 1;
    buf[ofs++] = static_cast<uint8_t>(planes & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((planes >> 8) & 0xFF);
    uint16_t bpp = 24;
    buf[ofs++] = static_cast<uint8_t>(bpp & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bpp >> 8) & 0xFF);
    ofs += 4; // compression
    ofs += 4; // image size
    uint32_t ppm = 2835;
    buf[ofs++] = static_cast<uint8_t>(ppm & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 24) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>(ppm & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 24) & 0xFF);
    ofs += 8; // colors used + important colors

    // Pixel data (bottom-up rows, BGR format)
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            int src_idx = (y * w + x) * 3;
            buf[ofs++] = pixels_rgb[src_idx + 2]; // B
            buf[ofs++] = pixels_rgb[src_idx + 1]; // G
            buf[ofs++] = pixels_rgb[src_idx + 0]; // R
        }
        for (int p = w * 3; p < row_size; p++) {
            buf[ofs++] = 0;
        }
    }

    FILE* f = fopen(filename, "wb");
    if (f) {
        fwrite(buf.data(), 1, file_size, f);
        fclose(f);
    }
}

// Write an 8-bit grayscale buffer as a BMP file
static void write_bmp(const char* filename, int w, int h, const uint8_t* pixels) {
    int row_size = ((w + 3) / 4) * 4; // padded to 4 bytes
    int data_size = row_size * h;
    int palette_size = 256 * 4;
    int file_size = 14 + 40 + palette_size + data_size;

    std::vector<uint8_t> buf(file_size);
    int ofs = 0;

    // BMP file header (14 bytes)
    buf[ofs++] = 'B'; buf[ofs++] = 'M';
    ofs += 4; // skip size for now; we set it later
    uint32_t fsize = static_cast<uint32_t>(file_size);
    buf[ofs - 4] = static_cast<uint8_t>(fsize & 0xFF);
    buf[ofs - 3] = static_cast<uint8_t>((fsize >> 8) & 0xFF);
    buf[ofs - 2] = static_cast<uint8_t>((fsize >> 16) & 0xFF);
    buf[ofs - 1] = static_cast<uint8_t>((fsize >> 24) & 0xFF);
    ofs += 4; // reserved
    uint32_t pix_offset = 14 + 40 + palette_size;
    buf[ofs++] = static_cast<uint8_t>(pix_offset & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((pix_offset >> 24) & 0xFF);

    // DIB header (40 bytes)
    uint32_t bi_size = 40;
    buf[ofs++] = static_cast<uint8_t>(bi_size & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bi_size >> 24) & 0xFF);
    uint32_t iw = static_cast<uint32_t>(w);
    buf[ofs++] = static_cast<uint8_t>(iw & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((iw >> 24) & 0xFF);
    uint32_t ih = static_cast<uint32_t>(h);
    buf[ofs++] = static_cast<uint8_t>(ih & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ih >> 24) & 0xFF);
    uint16_t planes = 1;
    buf[ofs++] = static_cast<uint8_t>(planes & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((planes >> 8) & 0xFF);
    uint16_t bpp = 8;
    buf[ofs++] = static_cast<uint8_t>(bpp & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((bpp >> 8) & 0xFF);
    ofs += 4; // compression = 0 (none)
    ofs += 4; // image size (can be 0 for uncompressed)
    uint32_t ppm = 2835; // 72 DPI
    buf[ofs++] = static_cast<uint8_t>(ppm & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 24) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>(ppm & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 8) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 16) & 0xFF);
    buf[ofs++] = static_cast<uint8_t>((ppm >> 24) & 0xFF);
    ofs += 4; // colors used
    ofs += 4; // important colors

    // Grayscale palette (256 entries, 4 bytes each)
    for (int i = 0; i < 256; i++) {
        buf[ofs++] = static_cast<uint8_t>(i); // B
        buf[ofs++] = static_cast<uint8_t>(i); // G
        buf[ofs++] = static_cast<uint8_t>(i); // R
        buf[ofs++] = 0;                        // reserved
    }

    // Pixel data (bottom-up rows)
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            buf[ofs++] = pixels[y * w + x];
        }
        // row padding
        for (int p = w; p < row_size; p++) {
            buf[ofs++] = 0;
        }
    }

    FILE* f = fopen(filename, "wb");
    if (f) {
        fwrite(buf.data(), 1, file_size, f);
        fclose(f);
    }
}

int main() {
    VoxelEngine::TerrainParams params;
    VoxelEngine::ChunkGenerator gen(params);

    // Simple noise-based continentalness, no plates to print
    printf("Noise-based continentalness (no tectonic plates)\n");

    const int W = 2000, H = 2000;
    const float STEP = 8.0f;

    // Continentalness map (16k blocks)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H);
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                float wx = -8000.0f + static_cast<float>(px) * STEP;
                float wz = -8000.0f + static_cast<float>(py) * STEP;
                float cont = gen.sample_continentalness_debug(wx, wz);
                pixels[static_cast<size_t>(py) * W + px] = static_cast<uint8_t>(std::round(cont * 255.0f));
            }
        }
        write_bmp("bin/continentalness.bmp", W, H, pixels.data());
    }

    // Biome map (16k blocks) — 24-bit color
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3);
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                float wx = -8000.0f + static_cast<float>(px) * STEP;
                float wz = -8000.0f + static_cast<float>(py) * STEP;
                auto col = gen.sample_column_debug(
                    static_cast<int32_t>(wx), static_cast<int32_t>(wz));
                size_t idx = (static_cast<size_t>(py) * W + px) * 3;
                uint8_t r, g, b;
                switch (col.biome) {
                    case VoxelEngine::BiomeType::Ocean:         r=40;  g=100; b=180;  break;
                    case VoxelEngine::BiomeType::Beach:         r=220; g=200; b=140;  break;

                    case VoxelEngine::BiomeType::Plains:        r=60;  g=160; b=60;   break;
                    case VoxelEngine::BiomeType::Forest:        r=30;  g=120; b=40;   break;
                    case VoxelEngine::BiomeType::Desert:        r=200; g=180; b=100;  break;

                    default:                                    r=255; g=0;   b=255;  break;
                }
                pixels[idx] = r; pixels[idx+1] = g; pixels[idx+2] = b;
            }
        }
        write_bmp_rgb24("bin/biome.bmp", W, H, pixels.data());
    }

    // Wider view: 200k blocks at 100-block resolution (2000x2000 pixels)
    const float WIDE_STEP = 100.0f;
    const float WIDE_RANGE = 100000.0f;
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H);
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                float wx = -WIDE_RANGE + static_cast<float>(px) * WIDE_STEP;
                float wz = -WIDE_RANGE + static_cast<float>(py) * WIDE_STEP;
                float cont = gen.sample_continentalness_debug(wx, wz);
                pixels[static_cast<size_t>(py) * W + px] = static_cast<uint8_t>(std::round(cont * 255.0f));
            }
        }
        write_bmp("bin/continentalness_wide.bmp", W, H, pixels.data());
    }
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3);
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                float wx = -WIDE_RANGE + static_cast<float>(px) * WIDE_STEP;
                float wz = -WIDE_RANGE + static_cast<float>(py) * WIDE_STEP;
                auto col = gen.sample_column_debug(
                    static_cast<int32_t>(wx), static_cast<int32_t>(wz));
                size_t idx = (static_cast<size_t>(py) * W + px) * 3;
                uint8_t r, g, b;
                switch (col.biome) {
                    case VoxelEngine::BiomeType::Ocean:         r=40;  g=100; b=180;  break;
                    case VoxelEngine::BiomeType::Beach:         r=220; g=200; b=140;  break;

                    case VoxelEngine::BiomeType::Plains:        r=60;  g=160; b=60;   break;
                    case VoxelEngine::BiomeType::Forest:        r=30;  g=120; b=40;   break;
                    case VoxelEngine::BiomeType::Desert:        r=200; g=180; b=100;  break;

                    default:                                    r=255; g=0;   b=255;  break;
                }
                pixels[idx] = r; pixels[idx+1] = g; pixels[idx+2] = b;
            }
        }
        write_bmp_rgb24("bin/biome_wide.bmp", W, H, pixels.data());
    }

    // 1M-block view at 500-block resolution (2000x2000 pixels)
    const float MEGA_STEP = 500.0f;
    const float MEGA_RANGE = 500000.0f;
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 3);
        for (int py = 0; py < H; py++) {
            for (int px = 0; px < W; px++) {
                float wx = -MEGA_RANGE + static_cast<float>(px) * MEGA_STEP;
                float wz = -MEGA_RANGE + static_cast<float>(py) * MEGA_STEP;
                auto col = gen.sample_column_debug(
                    static_cast<int32_t>(wx), static_cast<int32_t>(wz));
                size_t idx = (static_cast<size_t>(py) * W + px) * 3;
                uint8_t r, g, b;
                switch (col.biome) {
                    case VoxelEngine::BiomeType::Ocean:         r=40;  g=100; b=180;  break;
                    case VoxelEngine::BiomeType::Beach:         r=220; g=200; b=140;  break;

                    case VoxelEngine::BiomeType::Plains:        r=60;  g=160; b=60;   break;
                    case VoxelEngine::BiomeType::Forest:        r=30;  g=120; b=40;   break;
                    case VoxelEngine::BiomeType::Desert:        r=200; g=180; b=100;  break;

                    default:                                    r=255; g=0;   b=255;  break;
                }
                pixels[idx] = r; pixels[idx+1] = g; pixels[idx+2] = b;
            }
        }
        write_bmp_rgb24("bin/biome_mega.bmp", W, H, pixels.data());
    }

    printf("Done. Wrote .bmp files to bin/\n");
    return 0;
}
