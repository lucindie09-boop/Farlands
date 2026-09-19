// What is actually inside a block file.
//
// Decodes a `.schematic` (gzip'd or bare NBT) and reports the things that decide
// whether it can be pasted: dimensions, which Data layout the writer used,
// whether ids above 255 are carried in AddBlocks, how much of the box is air,
// and the full palette of block states with cell counts. The palette is the
// interesting part — it is the exact list a translation table has to cover, and
// it is the list to check against when a new file turns up.
//
// Pure C++: no Godot runtime, no zlib. Reads the file itself and inflates it.
//
// Build: scons schematic_report
// Run:   bin/schematic_report <file.schematic> [--top N | --all]

#include "schematic/schematic_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace VoxelEngine::schematic;

namespace {

bool read_file(const char* path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        error = "cannot open " + std::string(path);
        return false;
    }
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size < 0) {
        error = "cannot measure " + std::string(path);
        return false;
    }
    stream.seekg(0, std::ios::beg);
    out.resize(static_cast<size_t>(size));
    if (size > 0) stream.read(reinterpret_cast<char*>(out.data()), size);
    if (!stream) {
        error = "cannot read " + std::string(path);
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    size_t top = 40;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all") == 0) {
            top = 0;
        } else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (argv[i][0] != '-' && path == nullptr) {
            path = argv[i];
        } else {
            std::printf("usage: schematic_report <file.schematic> [--top N | --all]\n");
            return 2;
        }
    }
    if (path == nullptr) {
        std::printf("usage: schematic_report <file.schematic> [--top N | --all]\n");
        return 2;
    }

    std::vector<uint8_t> file;
    std::string error;
    if (!read_file(path, file, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    SchematicData data;
    if (!load_schematic_bytes(file.data(), file.size(), data, &error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    std::printf("file        %s\n", path);
    std::printf("bytes       %zu on disk, %zu decompressed (%s)\n", data.file_bytes, data.nbt_bytes,
                container_kind_name(data.container));
    std::printf("root        \"%s\"", data.root_name.c_str());
    if (!data.materials.empty()) std::printf("  materials \"%s\"", data.materials.c_str());
    std::printf("\n");
    std::printf("size        %d x %d x %d  (%zu cells)\n", data.width, data.height, data.length,
                data.cell_count());
    std::printf("arrays      Blocks %zu bytes, Data %zu bytes (%s)", data.blocks_bytes, data.data_bytes,
                data_layout_name(data.data_layout));
    if (data.has_add_blocks) std::printf(", AddBlocks %zu bytes", data.add_blocks_bytes);
    std::printf("\n");

    const size_t cells = data.cell_count();
    const double fill = cells > 0 ? (100.0 * static_cast<double>(data.non_air_cells) /
                                    static_cast<double>(cells))
                                  : 0.0;
    std::printf("content     %zu non-air cells (%.1f%% of the box), %zu distinct states\n",
                data.non_air_cells, fill, data.palette.size());

    // Bounding box of the non-air cells, so it is obvious whether the build sits
    // against the edges of its box or floats inside it.
    int32_t min_x = data.width, max_x = -1, min_y = data.height, max_y = -1, min_z = data.length,
            max_z = -1;
    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const LegacyBlockState* state = data.state_at(x, y, z);
                if (state == nullptr || state->is_air()) continue;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                if (z < min_z) min_z = z;
                if (z > max_z) max_z = z;
            }
        }
    }
    if (max_x >= 0) {
        std::printf("bbox        x %d..%d  y %d..%d  z %d..%d\n", min_x, max_x, min_y, max_y, min_z,
                    max_z);
    } else {
        std::printf("bbox        empty (every cell is air)\n");
    }

    if (data.has_origin) {
        std::printf("origin      %d, %d, %d (where the writer copied it from)\n", data.origin[0],
                    data.origin[1], data.origin[2]);
    }
    std::printf("ignored     %zu tile entities, %zu entities (not blocks; neither is pasted)\n",
                data.tile_entity_count, data.entity_count);
    if (data.tile_entities_truncated) {
        std::printf("            (tile entity positions were truncated while reading)\n");
    }
    for (size_t i = 0; i < data.tile_entity_positions.size() && i < 4; ++i) {
        std::printf("              at %d, %d, %d\n", data.tile_entity_positions[i][0],
                    data.tile_entity_positions[i][1], data.tile_entity_positions[i][2]);
    }

    // Palette, most-used first: the order a translation table wants to be written
    // in, and the order a "what did this file need" review reads in.
    std::vector<std::pair<size_t, size_t>> counted;  // (count, palette slot)
    counted.reserve(data.palette.size());
    for (size_t slot = 0; slot < data.palette.size(); ++slot) {
        size_t count = 0;
        for (const uint32_t cell : data.cells) {
            if (cell == slot) ++count;
        }
        counted.emplace_back(count, slot);
    }
    std::sort(counted.begin(), counted.end(), [](const auto& a, const auto& b) {
        if (a.first != b.first) return a.first > b.first;
        return a.second < b.second;
    });

    const size_t shown = (top == 0 || top > counted.size()) ? counted.size() : top;
    std::printf("\npalette     id      data    count   share   cell\n");
    for (size_t i = 0; i < shown; ++i) {
        const size_t slot = counted[i].second;
        const LegacyBlockState& state = data.palette[slot];
        const double share = cells > 0 ? 100.0 * static_cast<double>(counted[i].first) /
                                             static_cast<double>(cells)
                                       : 0.0;
        std::printf("            %-7u %-7u %-7zu %5.2f%%%s\n", static_cast<unsigned>(state.id),
                    static_cast<unsigned>(state.data), counted[i].first, share,
                    state.is_air() ? "   (air)" : "");
    }
    if (shown < counted.size()) {
        std::printf("            ... %zu more states, use --all\n", counted.size() - shown);
    }

    return 0;
}
