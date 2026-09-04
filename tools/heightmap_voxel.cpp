#include "worldgen/chunk_generator.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <cstring>
#include <cmath>

// Render a 2D heightmap of a block region as a grid of colored squares, one
// per block column, colored by the column's surface height. Reading this image
// directly shows the smooth low-frequency "blobby" noise pattern in the
// terrain because adjacent squares with close heights have close colors.
//
// Usage:  bin\heightmap_voxel [size] [center_x] [center_z] [px_per_block] [output]
//   size          - region edge length in blocks (default 50)
//   center_x      - world X of region center (default 0)
//   center_z      - world Z of region center (default 0)
//   px_per_block  - image pixels per block, each block drawn as a solid square (default 8)
//   output        - output .bmp path (default "bin/heightmap_voxel.bmp")
//
// A legend of the height range is printed to stdout.

namespace {

void write_bmp_rgb24(const std::string& filename, int w, int h, const std::vector<uint8_t>& pixels) {
    const int row_size = ((w * 3 + 3) / 4) * 4;
    const int data_size = row_size * h;
    const int file_size = 14 + 40 + data_size;
    std::vector<uint8_t> buf(static_cast<size_t>(file_size));
    int ofs = 0;
    auto put_u32 = [&](uint32_t v) {
        buf[ofs++] = static_cast<uint8_t>(v & 0xFF);
        buf[ofs++] = static_cast<uint8_t>((v >> 8) & 0xFF);
        buf[ofs++] = static_cast<uint8_t>((v >> 16) & 0xFF);
        buf[ofs++] = static_cast<uint8_t>((v >> 24) & 0xFF);
    };
    auto put_u16 = [&](uint16_t v) {
        buf[ofs++] = static_cast<uint8_t>(v & 0xFF);
        buf[ofs++] = static_cast<uint8_t>((v >> 8) & 0xFF);
    };
    buf[ofs++] = 'B'; buf[ofs++] = 'M';
    put_u32(static_cast<uint32_t>(file_size));
    put_u32(0);
    put_u32(14 + 40);
    put_u32(40);
    put_u32(static_cast<uint32_t>(w));
    put_u32(static_cast<uint32_t>(h));
    put_u16(1);
    put_u16(24);
    put_u32(0);
    put_u32(static_cast<uint32_t>(data_size));
    put_u32(2835);
    put_u32(2835);
    put_u32(0);
    put_u32(0);
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &pixels[(static_cast<size_t>(y) * w + x) * 3];
            buf[ofs++] = p[2];
            buf[ofs++] = p[1];
            buf[ofs++] = p[0];
        }
        for (int p = w * 3; p < row_size; p++) buf[ofs++] = 0;
    }
    FILE* f = fopen(filename.c_str(), "wb");
    if (f) { fwrite(buf.data(), 1, static_cast<size_t>(file_size), f); fclose(f); }
    else std::fprintf(stderr, "Failed to write %s\n", filename.c_str());
}

// Height -> color. Deep blues for water, greens for low land, browns/greys as
// height climbs. t in [0,1] mapped across [min,max].
void color_for_height(float t, uint8_t& r, uint8_t& g, uint8_t& b) {
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    // Palette stops: (60,120,40) low green -> (90,150,50) -> (140,130,60) hill
    // -> (180,160,120) -> (200,200,200) high rock.
    const float stops[5][3] = {
        {  60, 130,  45 },
        {  90, 155,  55 },
        { 140, 130,  62 },
        { 180, 165, 130 },
        { 210, 210, 215 },
    };
    float x = t * 4.0f;
    int i = (int)x;
    if (i >= 4) i = 4;
    float f = x - (float)i;
    int j = (i < 4) ? i + 1 : i;
    r = (uint8_t)(stops[i][0] + (stops[j][0] - stops[i][0]) * f);
    g = (uint8_t)(stops[i][1] + (stops[j][1] - stops[i][1]) * f);
    b = (uint8_t)(stops[i][2] + (stops[j][2] - stops[i][2]) * f);
}

} // namespace

int main(int argc, char** argv) {
    int size = 50;
    int cx = 0, cz = 0;
    int ppb = 8;
    std::string out = "bin/heightmap_voxel.bmp";
    if (argc > 1) size = std::atoi(argv[1]);
    if (argc > 2) cx = std::atoi(argv[2]);
    if (argc > 3) cz = std::atoi(argv[3]);
    if (argc > 4) ppb = std::atoi(argv[4]) > 0 ? std::atoi(argv[4]) : 8;
    if (argc > 5) out = argv[5];
    size = std::max(1, size);

    VoxelEngine::TerrainParams params;
    VoxelEngine::ChunkGenerator gen(params);

    const int lo = -size / 2;
    int min_ht = 1 << 30, max_ht = -1;
    std::vector<int> height(static_cast<size_t>(size) * size);
    for (int lz = 0; lz < size; lz++) {
        for (int lx = 0; lx < size; lx++) {
            const int wx = cx + lo + lx;
            const int wz = cz + lo + lz;
            auto col = gen.sample_column_debug(wx, wz);
            int top = static_cast<int>(std::ceil(col.height));
            top = std::max(0, std::min(1023, top));
            height[static_cast<size_t>(lz) * size + lx] = top;
            min_ht = std::min(min_ht, top);
            max_ht = std::max(max_ht, top);
        }
    }

    const int w = size * ppb, h = size * ppb;
    std::vector<uint8_t> img(static_cast<size_t>(w) * h * 3);
    const float range = std::max(1.0f, (float)(max_ht - min_ht));
    for (int lz = 0; lz < size; lz++) {
        for (int lx = 0; lx < size; lx++) {
            const int top = height[static_cast<size_t>(lz) * size + lx];
            float t = (float)(top - min_ht) / range;
            uint8_t r, g, b;
            color_for_height(t, r, g, b);
            for (int py = 0; py < ppb; py++) {
                for (int px = 0; px < ppb; px++) {
                    const int img_x = lx * ppb + px;
                    const int img_y = lz * ppb + py;
                    size_t idx = (static_cast<size_t>(img_y) * w + img_x) * 3;
                    img[idx] = r; img[idx + 1] = g; img[idx + 2] = b;
                }
            }
        }
    }

    write_bmp_rgb24(out, w, h, img);

    // Dump raw per-column heights as JSON (strip the .bmp suffix for the grid
    // name) so the exact relief can be inspected numerically.
    {
        std::string base = out;
        if (base.size() > 4 && base.substr(base.size() - 4) == ".bmp")
            base = base.substr(0, base.size() - 4);
        const std::string path = base + "_grid.json";
        std::string js = "{\n  \"size\": " + std::to_string(size) + ",\n";
        js += "  \"center_x\": " + std::to_string(cx) + ",\n";
        js += "  \"center_z\": " + std::to_string(cz) + ",\n";
        js += "  \"min_height\": " + std::to_string(min_ht) + ",\n";
        js += "  \"max_height\": " + std::to_string(max_ht) + ",\n";
        js += "  \"heights\": [\n";
        for (int lz = 0; lz < size; lz++) {
            js += "    [";
            for (int lx = 0; lx < size; lx++) {
                js += std::to_string(height[static_cast<size_t>(lz) * size + lx]);
                if (lx < size - 1) js += ", ";
            }
            js += (lz < size - 1) ? "],\n" : "]\n";
        }
        js += "  ]\n}\n";
        FILE* f = fopen(path.c_str(), "wb");
        if (f) { fwrite(js.data(), 1, js.size(), f); fclose(f); }
    }

    std::printf("size=%d center=(%d,%d) px/block=%d min=%d max=%d  wrote %s\n",
                size, cx, cz, ppb, min_ht, max_ht, out.c_str());
    return 0;
}