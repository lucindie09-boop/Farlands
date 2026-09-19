// What is actually inside a block file, and what it would become here.
//
// Decodes a `.schematic` (gzip'd or bare NBT) and reports the things that decide
// whether it can be pasted: dimensions, which Data layout the writer used,
// whether ids above 255 are carried in AddBlocks, how much of the box is air,
// the full palette of block states with cell counts, and — per state — the block
// this game would place, marked when it is a stand-in, a liquid, or nothing.
//
// The palette is the interesting part: it is the exact list a translation table
// has to cover, and the coverage summary is the honest answer to "will this file
// survive a paste, and what will be missing when it does".
//
// Pure C++: no Godot runtime, no zlib. Reads the files itself and inflates the
// block file. The block-name list is read from `data/block_definitions.json`
// next to the table, so a typo in the table is caught here rather than at paste
// time — that check failing is the one thing that exits non-zero on a good file.
//
// Build: scons schematic_report
// Run:   bin/schematic_report <file.schematic> [--top N | --all]
//                                            [--table data/minecraft_blocks.json]

#include "schematic/mc_palette.hpp"
#include "schematic/schematic_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
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

bool read_text(const std::string& path, std::string& out, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_file(path.c_str(), bytes, error)) return false;
    out.assign(bytes.begin(), bytes.end());
    return true;
}

// The directory a path lives in, so the table and the block-name list can be
// found together wherever they are moved to.
std::string parent_dir(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash + 1);
}

struct FailedPastRow {
    unsigned id = 0;
    unsigned data = 0;
    size_t count = 0;
    std::string detail;
};

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    size_t top = 40;
    std::string table_path = "data/minecraft_blocks.json";
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all") == 0) {
            top = 0;
        } else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (argv[i][0] != '-' && path == nullptr) {
            path = argv[i];
        } else {
            std::printf("usage: schematic_report <file.schematic> [--top N | --all] [--table PATH]\n");
            return 2;
        }
    }
    if (path == nullptr) {
        std::printf("usage: schematic_report <file.schematic> [--top N | --all] [--table PATH]\n");
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

    // The table is optional: without it the palette is still a complete report of
    // what the file holds, so a missing table is a note rather than a failure.
    McPalette palette;
    std::set<std::string> known_names;
    bool have_table = false;
    bool table_clean = true;
    std::string table_note;
    {
        std::string text;
        if (!read_text(table_path, text, error)) {
            table_note = error + " (no per-state targets)";
        } else if (!palette.load(text, &error)) {
            table_note = error + " (no per-state targets)";
        } else {
            std::string names_text;
            std::string names_error;
            std::vector<std::string> names;
            const std::string names_path = parent_dir(table_path) + "block_definitions.json";
            if (!read_text(names_path, names_text, names_error)) {
                table_note = names_error + " (names unchecked)";
            } else if (!parse_block_names(names_text, names, &names_error)) {
                table_note = names_error + " (names unchecked)";
            } else {
                known_names.insert(names.begin(), names.end());
            }
            have_table = true;
        }
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

    if (have_table) {
        const size_t sets = palette.variant_sets().size();
        std::printf("table       %s  (%zu ids, %zu variant set%s)\n", table_path.c_str(),
                    palette.row_count(), sets, sets == 1 ? "" : "s");
        if (!known_names.empty()) {
            // A table row naming a block that does not exist would paste as
            // nothing at runtime, so it is a hard error here.
            std::vector<std::string> missing;
            for (const std::string& name : target_names(palette)) {
                if (known_names.count(name) == 0) missing.push_back(name);
            }
            if (missing.empty()) {
                std::printf("            every target name resolves against %s (%zu names)\n",
                            (parent_dir(table_path) + "block_definitions.json").c_str(),
                            known_names.size());
            } else {
                table_clean = false;
                std::printf("            !! %zu target name%s not in the block list: ",
                            missing.size(), missing.size() == 1 ? " is" : "s are");
                for (size_t i = 0; i < missing.size() && i < 8; ++i) {
                    std::printf("%s%s", i == 0 ? "" : ", ", missing[i].c_str());
                }
                if (missing.size() > 8) std::printf(", +%zu more", missing.size() - 8);
                std::printf("\n");
            }
        } else {
            std::printf("            %s\n", table_note.c_str());
        }
    } else {
        std::printf("table       %s\n", table_note.c_str());
    }

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
    // in, and the order a "what did this file need" review reads in. Every state
    // is resolved, not just the ones the dump prints, so the coverage summary is
    // the whole file.
    struct Resolved {
        size_t slot = 0;
        size_t count = 0;
        bool air = false;
        PaletteTarget target;
    };
    std::vector<Resolved> counts;
    counts.reserve(data.palette.size());
    for (size_t slot = 0; slot < data.palette.size(); ++slot) {
        Resolved entry;
        entry.slot = slot;
        entry.air = data.palette[slot].is_air();
        for (const uint32_t cell : data.cells) {
            if (cell == slot) ++entry.count;
        }
        if (have_table && !entry.air) {
            entry.target = palette.resolve(data.palette[slot].id, data.palette[slot].data);
        }
        counts.push_back(std::move(entry));
    }

    size_t mapped_cells = 0, substituted_cells = 0, skipped_cells = 0, unknown_cells = 0,
           fluid_cells = 0;
    size_t mapped_states = 0, substituted_states = 0, skipped_states = 0, unknown_states = 0;
    std::vector<FailedPastRow> not_placed;
    for (const Resolved& entry : counts) {
        if (entry.air || !have_table) continue;
        const LegacyBlockState& state = data.palette[entry.slot];
        FailedPastRow row;
        row.id = state.id;
        row.data = state.data;
        row.count = entry.count;
        row.detail = entry.target.note;
        if (entry.target.unknown()) {
            unknown_cells += entry.count;
            ++unknown_states;
            row.detail = entry.target.note.empty() ? "no row for this id in the table"
                                                   : entry.target.note;
            not_placed.push_back(row);
        } else if (entry.target.skipped()) {
            skipped_cells += entry.count;
            ++skipped_states;
            not_placed.push_back(row);
        } else if (entry.target.substitute) {
            substituted_cells += entry.count;
            ++substituted_states;
            if (row.detail.empty()) row.detail = "stand-in for " + entry.target.block_name;
            not_placed.push_back(row);
        } else {
            mapped_cells += entry.count;
            ++mapped_states;
        }
        if (entry.target.fluid) fluid_cells += entry.count;
    }

    std::sort(counts.begin(), counts.end(), [](const Resolved& a, const Resolved& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.slot < b.slot;
    });

    bool any_marker = !not_placed.empty();
    std::printf("\npalette     id      data    count   share   becomes\n");
    const size_t shown = (top == 0 || top > counts.size()) ? counts.size() : top;
    for (size_t i = 0; i < shown; ++i) {
        const Resolved& entry = counts[i];
        const LegacyBlockState& state = data.palette[entry.slot];
        const double share = cells > 0 ? 100.0 * static_cast<double>(entry.count) /
                                             static_cast<double>(cells)
                                       : 0.0;
        std::printf("            %-7u %-7u %-7zu %5.2f%%", static_cast<unsigned>(state.id),
                    static_cast<unsigned>(state.data), entry.count, share);

        if (entry.air) {
            std::printf("   (air)\n");
        } else if (!have_table) {
            std::printf("\n");
        } else if (!entry.target.mapped()) {
            // Skipped and unknown both paste as nothing.
            std::printf("   -    %s\n",
                        entry.target.skipped() ? "skipped" : "unknown id");
        } else {
            std::printf("   %s%s%s", entry.target.block_name.c_str(),
                        entry.target.substitute ? "*" : "",
                        entry.target.fluid ? "  (liquid)" : "");
            std::printf("\n");
        }
    }
    if (shown < counts.size()) {
        std::printf("            ... %zu more states, use --all\n", counts.size() - shown);
    }

    if (any_marker) {
        std::printf("            * = stand-in: the shape or role is right, the block is not\n");
    }

    if (have_table) {
        // Air is listed in the palette dump but is not a placement decision, so
        // the state tallies leave it out.
        size_t states_total = 0;
        for (const LegacyBlockState& state : data.palette) {
            if (!state.is_air()) ++states_total;
        }
        const size_t placed = data.non_air_cells;
        const double placed_pct = placed > 0 ? 100.0 * static_cast<double>(mapped_cells) /
                                                   static_cast<double>(placed)
                                             : 0.0;
        std::printf("\ncoverage    %zu of %zu non-air cells placed (%.1f%%): %zu substituted (%.1f%%),"
                    " %zu skipped, %zu unknown\n",
                    mapped_cells, placed, placed_pct, substituted_cells,
                    placed > 0 ? 100.0 * static_cast<double>(substituted_cells) /
                                     static_cast<double>(placed)
                               : 0.0,
                    skipped_cells, unknown_cells);
        std::printf("            states (air excluded): %zu mapped, %zu substituted, %zu skipped,"
                    " %zu unknown, of %zu\n",
                    mapped_states, substituted_states, skipped_states, unknown_states, states_total);
        if (fluid_cells > 0) {
            std::printf("            %zu cells are liquid; they only land with fluids enabled\n",
                        fluid_cells);
        }

        if (!not_placed.empty()) {
            std::sort(not_placed.begin(), not_placed.end(),
                      [](const FailedPastRow& a, const FailedPastRow& b) {
                          if (a.count != b.count) return a.count > b.count;
                          if (a.id != b.id) return a.id < b.id;
                          return a.data < b.data;
                      });
            // Everything that is not an exact match: the pasted building will
            // either lose these cells or get a different block in them.
            std::printf("\ngaps        id      data    count   why\n");
            const size_t notes_shown = not_placed.size() < 40 ? not_placed.size() : 40;
            for (size_t i = 0; i < notes_shown; ++i) {
                const FailedPastRow& row = not_placed[i];
                std::printf("            %-7u %-7u %-7zu %s\n", row.id, row.data, row.count,
                            row.detail.empty() ? "no note in the table" : row.detail.c_str());
            }
            if (notes_shown < not_placed.size()) {
                std::printf("            ... %zu more\n", not_placed.size() - notes_shown);
            }
        }
    }

    return table_clean ? 0 : 1;
}
