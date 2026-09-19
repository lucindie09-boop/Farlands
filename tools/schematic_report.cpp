// What is actually inside a build file, and what it would become here.
//
// Decodes what the file turns out to be — a classic schematic (numbered states),
// or a palette one (named states, `.schem`) — and reports the things that decide
// whether it can be pasted: which format it is, how it wrapped and packed its
// arrays, how much of the box is air, the full palette of states with cell
// counts, and, per state, the block this game would place, marked when it is a
// stand-in, a liquid, or nothing.
//
// The palette is the interesting part: it is the exact list a translation table
// has to cover, and the coverage summary is the honest answer to "will this file
// survive a paste, and what will be missing when it does".
//
// Pure C++: no Godot runtime, no zlib. Reads the files itself and inflates the
// build file. The block-name list is read from `data/block_definitions.json`
// next to the table, so a name a row gets wrong is caught here rather than at
// paste time — that check failing is the one thing that exits non-zero on a good
// file.
//
// `--plan` goes one step further and builds the real paste plan (see paste_plan),
// timing it: that is the pass that runs before a paste touches the world, so it
// is the floor on what a paste costs. `--repeat N` repeats it and reports the
// best of N, which is what a per-file budget should be set from.
//
// Build: scons schematic_report
// Run:   bin/schematic_report <file> [--top N | --all]
//                                   [--table data/minecraft_blocks.json]
//                                   [--plan] [--repeat N]

#include "schematic/mc_palette.hpp"
#include "schematic/paste_plan.hpp"
#include "schematic/schematic_reader.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

using VoxelEngine::BlockID;
using namespace VoxelEngine::schematic;

namespace {

constexpr const char* USAGE =
    "usage: schematic_report <file> [--top N | --all] [--table PATH]"
    " [--plan] [--repeat N]\n";

// Wall time since `start`, in milliseconds. Monotonic, so a clock adjustment
// mid-run cannot produce a negative measurement.
double elapsed_ms(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
        .count();
}

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

// A state as one short column: "53:2" for a numbered file, and the name without
// its namespace (keeping the properties, which is what distinguishes the states)
// for a named one. Long names are cut, because a table wants to line up.
std::string short_state(const BlockState& state, size_t width) {
    std::string text;
    if (state.is_legacy()) {
        text = std::to_string(state.id) + ":" + std::to_string(state.data);
    } else {
        text = state.name;
        const size_t colon = text.find(':');
        if (colon != std::string::npos) text = text.substr(colon + 1);
        if (!state.properties.empty()) {
            text.push_back('[');
            for (size_t i = 0; i < state.properties.size(); ++i) {
                if (i != 0) text.push_back(',');
                text += state.properties[i].first;
                text.push_back('=');
                text += state.properties[i].second;
            }
            text.push_back(']');
        }
    }
    if (text.size() > width) {
        if (width <= 3) return text.substr(0, width);
        return text.substr(0, width - 3) + "...";
    }
    return text;
}

struct FailedPastRow {
    std::string state;
    size_t count = 0;
    std::string detail;
};

} // namespace

int main(int argc, char** argv) {
    const char* path = nullptr;
    size_t top = 40;
    std::string table_path = "data/minecraft_blocks.json";
    // Building the paste plan is off by default: it is the same decision the
    // report already counts, done for real, and it is what costs time on a big
    // file — so it is asked for, and repeated when a number is wanted.
    int plan_reps = 0;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--all") == 0) {
            top = 0;
        } else if (std::strcmp(argv[i], "--top") == 0 && i + 1 < argc) {
            top = static_cast<size_t>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--table") == 0 && i + 1 < argc) {
            table_path = argv[++i];
        } else if (std::strcmp(argv[i], "--plan") == 0) {
            plan_reps = plan_reps > 0 ? plan_reps : 1;
        } else if (std::strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            plan_reps = std::atoi(argv[++i]);
            if (plan_reps < 1) plan_reps = 1;
        } else if (argv[i][0] != '-' && path == nullptr) {
            path = argv[i];
        } else {
            std::printf(USAGE);
            return 2;
        }
    }
    if (path == nullptr) {
        std::printf(USAGE);
        return 2;
    }

    std::vector<uint8_t> file;
    std::string error;
    if (!read_file(path, file, error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }

    SchematicData data;
    const auto read_start = std::chrono::steady_clock::now();
    if (!load_schematic_bytes(file.data(), file.size(), data, &error)) {
        std::printf("error: %s\n", error.c_str());
        return 1;
    }
    const double read_ms = elapsed_ms(read_start);
    const bool palette_format = data.format != BlockFileFormat::Classic;

    // The table is optional: without it the palette is still a complete report of
    // what the file holds, so a missing table is a note rather than a failure.
    McPalette palette;
    std::set<std::string> known_names;
    // In file order, which is id order: this doubles as the name-to-id resolver
    // the plan needs, so a planned cell holds the same id the game would.
    std::vector<std::string> block_names;
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
                block_names = names;
                known_names.insert(names.begin(), names.end());
            }
            have_table = true;
        }
    }

    std::printf("file        %s\n", path);
    std::printf("bytes       %zu on disk, %zu decompressed (%s), decoded in %.1f ms\n",
                data.file_bytes, data.nbt_bytes, container_kind_name(data.container), read_ms);
    std::printf("root        \"%s\"\n", data.root_name.c_str());
    std::printf("format      %s", block_file_format_name(data.format));
    if (palette_format) {
        std::printf(", version %d", data.format_version);
        if (data.data_version > 0) std::printf(", data version %lld",
                                              static_cast<long long>(data.data_version));
    } else if (!data.materials.empty()) {
        std::printf(", materials \"%s\"", data.materials.c_str());
    }
    std::printf("\n");
    std::printf("size        %d x %d x %d  (%zu cells)\n", data.width, data.height, data.length,
                data.cell_count());
    if (palette_format) {
        std::printf("arrays      palette %zu states (max index %d), cell data %zu bytes of varint"
                    " indices\n",
                    data.palette.size(), data.palette_max, data.block_data_bytes);
    } else {
        std::printf("arrays      Blocks %zu bytes, Data %zu bytes (%s)", data.blocks_bytes,
                    data.data_bytes, data_layout_name(data.data_layout));
        if (data.has_add_blocks) std::printf(", AddBlocks %zu bytes", data.add_blocks_bytes);
        std::printf("\n");
    }
    if (data.data_padded || data.add_blocks_padded) {
        // Both arrays are packed two cells to a byte, and a writer that rounds up
        // leaves one byte extra. It is harmless, and saying so is the difference
        // between "this file decoded" and "this file decoded under protest".
        std::printf("            %s written one byte longer than the packed size; the trailing"
                    " half-byte is ignored\n",
                    data.data_padded && data.add_blocks_padded
                        ? "Data and AddBlocks are"
                        : (data.data_padded ? "Data is" : "AddBlocks is"));
    }

    if (have_table) {
        const size_t sets = palette.variant_sets().size();
        std::printf("table       %s  (%zu numbered rows, %zu named rows, %zu species, %zu variant"
                    " set%s)\n",
                    table_path.c_str(), palette.row_count(), palette.name_row_count(),
                    palette.species().size(), sets, sets == 1 ? "" : "s");
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
    const double fill =
        cells > 0 ? (100.0 * static_cast<double>(data.non_air_cells) / static_cast<double>(cells)) : 0.0;
    std::printf("content     %zu non-air cells (%.1f%% of the box), %zu distinct states\n",
                data.non_air_cells, fill, data.palette.size());

    // Bounding box of the non-air cells, so it is obvious whether the build sits
    // against the edges of its box or floats inside it.
    int32_t min_x = data.width, max_x = -1, min_y = data.height, max_y = -1, min_z = data.length,
            max_z = -1;
    for (int32_t y = 0; y < data.height; ++y) {
        for (int32_t z = 0; z < data.length; ++z) {
            for (int32_t x = 0; x < data.width; ++x) {
                const BlockState* state = data.state_at(x, y, z);
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
    if (data.has_offset) {
        std::printf("offset      %d, %d, %d (where the build sits in its region)\n", data.offset[0],
                    data.offset[1], data.offset[2]);
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
    // the whole file. The counts come from one pass over the cells, not one pass
    // per state, which is the difference between instant and quadratic on a build
    // with a million cells.
    struct Resolved {
        size_t slot = 0;
        size_t count = 0;
        bool air = false;
        PaletteTarget target;
    };
    const std::vector<size_t> counts = data.counts();
    std::vector<Resolved> resolved;
    resolved.reserve(data.palette.size());
    for (size_t slot = 0; slot < data.palette.size(); ++slot) {
        Resolved entry;
        entry.slot = slot;
        entry.count = slot < counts.size() ? counts[slot] : 0;
        entry.air = data.palette[slot].is_air();
        entry.target = entry.air ? PaletteTarget{}
                                 : (have_table ? palette.resolve(data.palette[slot])
                                               : unknown_target(data.palette[slot]));
        resolved.push_back(std::move(entry));
    }

    size_t mapped_cells = 0, substituted_cells = 0, skipped_cells = 0, unknown_cells = 0,
           fluid_cells = 0;
    size_t mapped_states = 0, substituted_states = 0, skipped_states = 0, unknown_states = 0;
    std::vector<FailedPastRow> not_placed;
    for (const Resolved& entry : resolved) {
        if (entry.air || !have_table) continue;
        const BlockState& state = data.palette[entry.slot];
        FailedPastRow row;
        row.state = short_state(state, 46);
        row.count = entry.count;
        row.detail = entry.target.note;
        if (!entry.target.source.empty()) {
            row.detail = (row.detail.empty() ? std::string() : row.detail + "  ") +
                         "[" + entry.target.source + "]";
        }
        if (entry.target.unknown()) {
            unknown_cells += entry.count;
            ++unknown_states;
            row.detail = "no row for this state";
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

    std::sort(resolved.begin(), resolved.end(), [](const Resolved& a, const Resolved& b) {
        if (a.count != b.count) return a.count > b.count;
        return a.slot < b.slot;
    });

    bool any_marker = !not_placed.empty();
    std::printf("\npalette     %-46s %-8s %6s  becomes\n", "state", "count", "share");
    const size_t shown = (top == 0 || top > resolved.size()) ? resolved.size() : top;
    for (size_t i = 0; i < shown; ++i) {
        const Resolved& entry = resolved[i];
        const BlockState& state = data.palette[entry.slot];
        const double share =
            cells > 0 ? 100.0 * static_cast<double>(entry.count) / static_cast<double>(cells) : 0.0;
        std::printf("            %-46s %-8zu %5.2f%%", short_state(state, 46).c_str(), entry.count,
                    share);

        if (entry.air) {
            std::printf("   (air)\n");
        } else if (!have_table) {
            std::printf("\n");
        } else if (!entry.target.mapped()) {
            // Skipped and unknown both paste as nothing.
            std::printf("   -    %s\n", entry.target.skipped() ? "skipped" : "unknown state");
        } else {
            std::printf("   %s%s%s", entry.target.block_name.c_str(),
                        entry.target.substitute ? "*" : "",
                        entry.target.fluid ? "  (liquid)" : "");
            std::printf("\n");
        }
    }
    if (shown < resolved.size()) {
        std::printf("            ... %zu more states, use --all\n", resolved.size() - shown);
    }

    if (any_marker) {
        std::printf("            * = stand-in: the shape or role is right, the block is not\n");
    }

    if (have_table) {
        // Air is listed in the palette dump but is not a placement decision, so
        // the state tallies leave it out.
        // (Fluid and stand-in cells are counted the same way the plan counts
        // them, so the two agree by construction.)
        size_t states_total = 0;
        for (const BlockState& state : data.palette) {
            if (!state.is_air()) ++states_total;
        }
        const size_t placed = data.non_air_cells;
        const double placed_pct =
            placed > 0 ? 100.0 * static_cast<double>(mapped_cells) / static_cast<double>(placed) : 0.0;
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
                          return a.state < b.state;
                      });
            // Everything that is not an exact match: the pasted building will
            // either lose these cells or get a different block in them.
            std::printf("\ngaps        %-46s %-8s why\n", "state", "count");
            const size_t notes_shown = not_placed.size() < 40 ? not_placed.size() : 40;
            for (size_t i = 0; i < notes_shown; ++i) {
                const FailedPastRow& row = not_placed[i];
                std::printf("            %-46s %-8zu %s\n", row.state.c_str(), row.count,
                            row.detail.empty() ? "no note in the table" : row.detail.c_str());
            }
            if (notes_shown < not_placed.size()) {
                std::printf("            ... %zu more\n", not_placed.size() - notes_shown);
            }
        }
    }

    if (plan_reps > 0 && have_table) {
        // The plan for real, timed: every cell into one bucket, with the table's
        // resolution done per distinct state. This is the pass that runs before a
        // paste touches the world, so its cost is the paste's floor.
        std::unordered_map<std::string, BlockID> ids;
        ids.reserve(block_names.size());
        for (size_t i = 0; i < block_names.size(); ++i) {
            ids.emplace(block_names[i], static_cast<BlockID>(i));
        }
        const auto resolve = [&ids](const std::string& name, BlockID& out) {
            const auto found = ids.find(name);
            if (found == ids.end()) return false;
            out = found->second;
            return true;
        };

        PasteOptions options;
        PastePlan plan;
        double best = 0.0;
        double total = 0.0;
        for (int rep = 0; rep < plan_reps; ++rep) {
            const auto start = std::chrono::steady_clock::now();
            if (!plan_paste(data, palette, 0, 0, 0, options, resolve, plan, &error)) {
                std::printf("\nplan        refused: %s\n", error.c_str());
                return 1;
            }
            const double ms = elapsed_ms(start);
            total += ms;
            if (rep == 0 || ms < best) best = ms;
        }
        const double per_cell_ns =
            plan.stats.file_cells > 0
                ? 1000000.0 * best / static_cast<double>(plan.stats.file_cells)
                : 0.0;
        std::printf("\nplan        %zu cells to change out of %zu file cells in %.1f ms"
                    " (%.0f ns/cell, %s)\n",
                    plan.size(), plan.stats.file_cells, best, per_cell_ns,
                    plan_reps == 1 ? "1 run"
                                   : (std::to_string(plan_reps) + " runs, best of, avg " +
                                      std::to_string(total / plan_reps).substr(0, 5) + " ms")
                                         .c_str());
        std::printf("            placed %zu, substituted %zu, skipped %zu, unknown %zu,"
                    " declined %zu, air ignored %zu\n",
                    plan.stats.placed, plan.stats.substituted, plan.stats.skipped,
                    plan.stats.unknown,
                    plan.stats.declined_fluid + plan.stats.declined_substitute,
                    plan.stats.air_ignored);
    }

    return table_clean ? 0 : 1;
}
