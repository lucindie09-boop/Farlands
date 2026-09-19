#include "doctest.h"
#include "schematic/mc_palette.hpp"

#include <algorithm>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using VoxelEngine::schematic::McPalette;
using VoxelEngine::schematic::PaletteTarget;
using VoxelEngine::schematic::parse_block_names;
using VoxelEngine::schematic::target_names;

namespace {

// A small table that exercises every feature the file format has: a default
// block, per-data overrides, a named set shared by two rows, an explicit null
// (skip this one data value), a skip row, and a row with no default at all.
constexpr const char* kTable = R"({
  "variant_sets": {
    "stair_data": {
      "0": "{family}_e",
      "2": "{family}_s",
      "3": "{family}",
      "4": "{family}_e_up"
    }
  },
  "blocks": [
    { "id": 0, "block": "air" },
    { "id": 1, "block": "stone", "note": "all plain here" },
    { "id": 5, "block": "oak_planks", "substitute": true, "note": "we have four woods" },
    { "id": 8, "block": "water", "fluid": true },
    { "id": 44, "family": "stone_stairs", "variants": "stair_data" },
    { "id": 50, "skip": true, "note": "torch" },
    { "id": 53, "family": "oak_stairs", "variants": "stair_data" },
    { "id": 90, "variants": { "0": "only_this", "1": null } },
    { "id": 126, "block": "oak_slab", "variants": { "1": "spruce_slab", "2": null, "8": "oak_slab_top" } }
  ]
})";

McPalette must_load(const std::string& text) {
    McPalette palette;
    std::string error;
    const bool ok = palette.load(text, &error);
    CHECK_MESSAGE(ok, "table failed to load: " << error);
    return palette;
}

std::string must_fail(const std::string& text) {
    McPalette palette;
    std::string error;
    const bool ok = palette.load(text, &error);
    CHECK_MESSAGE(!ok, "expected the table to be refused");
    return error;
}

// A row set with one valid entry, so a single broken row can be dropped in.
std::string table_with_rows(const std::string& rows) {
    return std::string("{\"blocks\": [{\"id\": 1, \"block\": \"stone\"}") + rows + "]}";
}

bool read_text(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

} // namespace

TEST_CASE("mc palette: a resolved target says what to place and why") {
    const McPalette palette = must_load(kTable);

    const PaletteTarget stone = palette.resolve(1, 0);
    CHECK(stone.mapped());
    CHECK(stone.block_name == "stone");
    CHECK_FALSE(stone.substitute);
    CHECK_FALSE(stone.fluid);
    CHECK_FALSE(stone.from_variant);
    CHECK(stone.note == "all plain here");

    // A data value with no override belongs to the row's default block.
    CHECK(palette.resolve(1, 15).block_name == "stone");

    const PaletteTarget planks = palette.resolve(5, 3);
    CHECK(planks.mapped());
    CHECK(planks.block_name == "oak_planks");
    CHECK(planks.substitute);
    CHECK(planks.note == "we have four woods");

    const PaletteTarget water = palette.resolve(8, 0);
    CHECK(water.mapped());
    CHECK(water.block_name == "water");
    CHECK(water.fluid);
    CHECK_FALSE(water.substitute);
}

TEST_CASE("mc palette: a named set expands against the row's own family") {
    const McPalette palette = must_load(kTable);

    // The same set, two rows, different blocks: this is the whole reason sets
    // exist, and the failure it prevents is every stair type baking oak in.
    const PaletteTarget oak = palette.resolve(53, 0);
    CHECK(oak.mapped());
    CHECK(oak.block_name == "oak_stairs_e");
    CHECK(oak.from_variant);

    const PaletteTarget stone = palette.resolve(44, 0);
    CHECK(stone.block_name == "stone_stairs_e");

    CHECK(palette.resolve(53, 2).block_name == "oak_stairs_s");
    CHECK(palette.resolve(53, 3).block_name == "oak_stairs");
    CHECK(palette.resolve(53, 4).block_name == "oak_stairs_e_up");

    // A set that does not cover the data value, on a row with no default block,
    // is skipped rather than guessed.
    const PaletteTarget missing = palette.resolve(53, 7);
    CHECK(missing.skipped());
    CHECK(missing.block_name.empty());
    CHECK_FALSE(missing.fluid);

    // No placeholder survives expansion.
    for (const std::string& name : target_names(palette)) {
        CHECK(name.find('{') == std::string::npos);
    }
    CHECK(palette.variant_sets().size() == 1);
    CHECK(palette.variant_sets()[0].name == "stair_data");
    CHECK(palette.variant_sets()[0].variants.size() == 4);
}

TEST_CASE("mc palette: overrides, explicit skips and bare rows") {
    const McPalette palette = must_load(kTable);

    // Inline variants override the default block.
    CHECK(palette.resolve(126, 1).block_name == "spruce_slab");
    CHECK(palette.resolve(126, 8).block_name == "oak_slab_top");
    CHECK(palette.resolve(126, 8).from_variant);
    // ...and an explicit null drops just that one data value.
    CHECK(palette.resolve(126, 2).skipped());
    // ...while an unlisted one keeps the default.
    CHECK(palette.resolve(126, 5).block_name == "oak_slab");

    // A row with variants and no default block: only the listed values exist.
    CHECK(palette.resolve(90, 0).block_name == "only_this");
    CHECK(palette.resolve(90, 1).skipped());
    CHECK(palette.resolve(90, 2).skipped());

    // A skip row is a decision, not a miss, and keeps its note.
    const PaletteTarget torch = palette.resolve(50, 0);
    CHECK(torch.skipped());
    CHECK_FALSE(torch.unknown());
    CHECK(torch.note == "torch");
    CHECK(palette.resolve(50, 9).skipped());

    // An id the table has never heard of is Unknown, never a guess.
    const PaletteTarget alien = palette.resolve(4000, 0);
    CHECK(alien.unknown());
    CHECK_FALSE(alien.mapped());
    CHECK_FALSE(alien.skipped());
    CHECK(alien.block_name.empty());
}

TEST_CASE("mc palette: ids are listed in order and lookup is by id") {
    const McPalette palette = must_load(kTable);
    CHECK(palette.row_count() == 9);

    std::vector<uint16_t> ids = palette.ids();
    CHECK(ids.size() == 9);
    CHECK(std::is_sorted(ids.begin(), ids.end()));
    CHECK(ids.front() == 0);
    CHECK(ids.back() == 126);

    CHECK(palette.row_for(53) != nullptr);
    CHECK(palette.row_for(53)->id == 53);
    CHECK(palette.row_for(53)->block.empty());
    CHECK(palette.row_for(4000) == nullptr);

    // Every target name is reported once, sorted, and only real ones.
    const std::vector<std::string> names = target_names(palette);
    CHECK(std::is_sorted(names.begin(), names.end()));
    CHECK(std::adjacent_find(names.begin(), names.end()) == names.end());
    CHECK(std::find(names.begin(), names.end(), "oak_stairs_e") != names.end());
    CHECK(std::find(names.begin(), names.end(), "stone_stairs_e") != names.end());
    CHECK(std::find(names.begin(), names.end(), "stone") != names.end());
    // Skipped rows contribute nothing.
    CHECK(std::find(names.begin(), names.end(), "torch") == names.end());
}

TEST_CASE("mc palette: loading a second table replaces the first") {
    McPalette palette;
    std::string error;
    CHECK(palette.load(kTable, &error));
    CHECK(palette.row_for(53) != nullptr);

    CHECK(palette.load("{\"blocks\": [{\"id\": 4, \"block\": \"cobblestone\"}]}", &error));
    CHECK(palette.row_count() == 1);
    CHECK(palette.row_for(53) == nullptr);
    CHECK(palette.row_for(4) != nullptr);
    CHECK(palette.variant_sets().empty());
    CHECK(palette.resolve(53, 0).unknown());

    // A failed load must not leave the previous table half-replaced.
    McPalette other;
    CHECK(other.load(kTable, &error));
    CHECK_FALSE(other.load("{\"blocks\": [{\"id\": 1, \"blok\": \"stone\"}]}", &error));
    CHECK(other.resolve(1, 0).unknown());
}

TEST_CASE("mc palette: a malformed table is refused with the reason") {
    // Structural damage.
    CHECK(must_fail("[]").find("root must be an object") != std::string::npos);
    CHECK(must_fail("{}").find("\"blocks\" array") != std::string::npos);
    CHECK(must_fail("{\"blocks\": {}}").find("\"blocks\" array") != std::string::npos);
    CHECK(must_fail("{\"blocks\": []}").find("no rows") != std::string::npos);
    CHECK(must_fail("{\"blocks\": [1]}").find("must be an object") != std::string::npos);
    CHECK(must_fail("{\"blocks\": [{\"block\": \"stone\"}]}").find("numeric \"id\"") !=
          std::string::npos);
    CHECK(must_fail("{\"blocks\": [{\"id\": \"1\", \"block\": \"stone\"}]}").find("numeric \"id\"") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 1, \"block\": \"stone\"}")).find("second row") !=
          std::string::npos);

    // ids outside the legacy range.
    CHECK(must_fail(table_with_rows(", {\"id\": 5000, \"block\": \"stone\"}")).find("legacy range") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": -1, \"block\": \"stone\"}")).find("legacy range") !=
          std::string::npos);

    // Value types.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"block\": 7}")).find("\"block\" must be a string") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"note\": 7}")).find("\"note\" must be a string") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"skip\": \"yes\"}")).find("\"skip\" must be true or false") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"substitute\": 1}")).find("must be true or false") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"fluid\": 1}")).find("must be true or false") !=
          std::string::npos);

    // A typo'd key is refused rather than silently dropping the mapping.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"blok\": \"stone\"}")).find("unknown key \"blok\"") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variant\": {}}")).find("unknown key \"variant\"") !=
          std::string::npos);

    // A row needs a decision in it.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"note\": \"todo\"}")).find("needs \"block\"") !=
          std::string::npos);
}

TEST_CASE("mc palette: ambiguous or impossible rows are refused") {
    // Both a block and skip: the usual cause is a "block": "skip" left behind.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"block\": \"skip\", \"skip\": true}"))
              .find("cannot have both") != std::string::npos);

    // A family is only meaningful with a named set, and a set needs one when it
    // uses the placeholder.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"family\": \"oak_stairs\", \"variants\": {\"0\": \"x\"}}"))
              .find("\"family\" is only meaningful") != std::string::npos);
    CHECK(must_fail("{\"variant_sets\": {\"s\": {\"0\": \"{family}_e\"}},"
                    "\"blocks\": [{\"id\": 2, \"variants\": \"s\"}]}")
              .find("needs a \"family\"") != std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": \"nope\"}"))
              .find("unknown variant set \"nope\"") != std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"family\": \"a{b\", \"variants\": {\"0\": \"x\"}}"))
              .find("must not contain a placeholder") != std::string::npos);

    // Variant keys are data values 0..15, written as numbers.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": {\"16\": \"x\"}}")).find("above 15") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": {\"a\": \"x\"}}")).find("data value 0..15") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": {\"-1\": \"x\"}}")).find("data value 0..15") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": [\"x\"]}")).find("must be an object") !=
          std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"variants\": {\"0\": 5}}")).find("block name or null") !=
          std::string::npos);

    // The variant set itself has to be well formed too.
    CHECK(must_fail("{\"variant_sets\": {\"s\": 5}, \"blocks\": [{\"id\": 1, \"block\": \"stone\"}]}")
              .find("\"variants\" must be an object") != std::string::npos);
    CHECK(must_fail("{\"variant_sets\": 5, \"blocks\": [{\"id\": 1, \"block\": \"stone\"}]}")
              .find("\"variant_sets\" must be an object") != std::string::npos);

    // A still form is the still form OF a liquid: without "fluid" it is a row
    // that forgot the flag, so it is refused rather than resolved to something.
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"block\": \"water\", \"still\": \"surface_water\"}"))
              .find("\"still\" only means something with \"fluid\"") != std::string::npos);
    CHECK(must_fail(table_with_rows(", {\"id\": 2, \"block\": \"water\", \"fluid\": true, \"still\": 5}"))
              .find("\"still\" must be a block name") != std::string::npos);

    // Errors point at the row they came from.
    CHECK(must_fail(table_with_rows(", {\"id\": 77, \"blok\": \"stone\"}")).find("id 77") !=
          std::string::npos);
}

TEST_CASE("mc palette: a liquid carries the still form a paste falls back to") {
    const McPalette palette = must_load(R"({
      "blocks": [
        { "id": 8, "block": "water", "fluid": true },
        { "id": 9, "block": "water", "fluid": true, "still": "surface_water" },
        { "id": 35, "block": "water", "fluid": true, "still": "surface_water", "substitute": true }
      ],
      "names": {
        "minecraft:lava": { "block": "lava", "fluid": true, "still": "surface_lava" }
      }
    })");

    CHECK(palette.resolve(9, 0).still_name == "surface_water");
    CHECK(palette.resolve(9, 0).fluid);
    CHECK(palette.resolve(35, 0).still_name == "surface_water");
    // A liquid with no still form says so by leaving it empty, which is what the
    // planner reads as "leave this cell out when fluids are off".
    CHECK(palette.resolve(8, 0).still_name.empty());

    VoxelEngine::schematic::BlockState named;
    named.id = 0;
    named.name = "minecraft:lava";
    CHECK(palette.resolve(named).still_name == "surface_lava");

    // The still form is a target like any other, so it is enumerated for the
    // caller that checks every name against the block list.
    const std::vector<std::string> names = target_names(palette);
    CHECK(std::find(names.begin(), names.end(), "surface_water") != names.end());
    CHECK(std::find(names.begin(), names.end(), "surface_lava") != names.end());
}

TEST_CASE("mc palette: comment keys are ignored everywhere") {
    const McPalette palette = must_load(R"({
      "_comment": "a note to the reader",
      "variant_sets": {
        "_comment": ["sets below"],
        "s": { "0": "{family}_e" }
      },
      "blocks": [
        { "id": 1, "block": "stone", "_comment": "the commonest block" },
        { "id": 2, "family": "oak_stairs", "variants": "s", "_todo": true }
      ]
    })");
    CHECK(palette.row_count() == 2);
    CHECK(palette.resolve(2, 0).block_name == "oak_stairs_e");
}

TEST_CASE("mc palette: block names are read from a definitions array") {
    std::vector<std::string> names;
    std::string error;
    CHECK(parse_block_names("[{\"name\": \"stone\"}, {\"name\": \"oak_stairs\", \"hidden\": true}]",
                             names, &error));
    CHECK(names.size() == 2);
    CHECK(names[0] == "stone");
    CHECK(names[1] == "oak_stairs");

    // Position is the block id, so an out-of-order or unnamed entry is a fault.
    CHECK_FALSE(parse_block_names("{}", names, &error));
    CHECK(error.find("must be an array") != std::string::npos);
    CHECK_FALSE(parse_block_names("[1]", names, &error));
    CHECK_FALSE(parse_block_names("[{}]", names, &error));
    CHECK(error.find("no \"name\"") != std::string::npos);
    CHECK_FALSE(parse_block_names("[{\"name\": \"\"}]", names, &error));
    CHECK_FALSE(parse_block_names("[{\"name\": 5}]", names, &error));
}

// The table that actually ships has to keep working: a typo in a block name is
// invisible until something is pasted, so it is checked here instead. Missing
// data files (a caller running the binary from elsewhere) skip rather than fail.
TEST_CASE("mc palette: the shipped table loads and names only real blocks") {
    std::string table_text;
    if (read_text("data/minecraft_blocks.json", table_text) ||
        read_text("../data/minecraft_blocks.json", table_text)) {
        McPalette palette;
        std::string error;
        CHECK_MESSAGE(palette.load(table_text, &error), error);
        CHECK(palette.row_count() >= 100);

        std::string names_text;
        if (read_text("data/block_definitions.json", names_text) ||
            read_text("../data/block_definitions.json", names_text)) {
            std::vector<std::string> names;
            CHECK_MESSAGE(parse_block_names(names_text, names, &error), error);
            std::set<std::string> known(names.begin(), names.end());
            CHECK(known.count("stone") == 1);

            std::vector<std::string> missing;
            for (const std::string& name : target_names(palette)) {
                if (known.count(name) == 0) missing.push_back(name);
            }
            CHECK_MESSAGE(missing.empty(),
                          missing.size() << " table target(s) are not blocks here, first: "
                                         << (missing.empty() ? "" : missing.front()));
        } else {
            MESSAGE("data/block_definitions.json not found; target names were not checked");
        }

        // Ground truth from the file the reader was built against.
        CHECK(palette.resolve(0, 0).block_name == "air");
        CHECK(palette.resolve(1, 0).block_name == "stone");
        CHECK(palette.resolve(1, 0).note.empty() == false);
        CHECK(palette.resolve(8, 0).fluid);
        CHECK(palette.resolve(9, 0).fluid);
        CHECK(palette.resolve(4, 0).block_name == "cobblestone");
        CHECK(palette.resolve(50, 1).skipped());          // torch: no block here
        CHECK(palette.resolve(50, 1).note.empty() == false);
        CHECK(palette.resolve(160, 3).skipped());         // stained glass pane
        CHECK(palette.resolve(4000, 0).unknown());

        // Stairs and slabs: the data layout is decided per value, so the checks
        // are that all eight orientations exist, are distinct, and that the plain
        // name is exactly one of them (this game's default facing).
        std::set<std::string> orientations;
        size_t plain = 0;
        for (uint8_t data = 0; data < 8; ++data) {
            const PaletteTarget target = palette.resolve(53, data);
            CHECK(target.mapped());
            CHECK(target.block_name.compare(0, 10, "oak_stairs") == 0);
            orientations.insert(target.block_name);
            if (target.block_name == "oak_stairs") ++plain;
        }
        CHECK(orientations.size() == 8);
        CHECK(plain == 1);

        // The top half of a slab is a different block, and the material bits are
        // handled separately from the half bit.
        CHECK(palette.resolve(126, 8).block_name == "oak_slab_top");
        CHECK(palette.resolve(126, 1).block_name == "spruce_slab");
        CHECK(palette.resolve(126, 5).block_name == "pine_slab");
        CHECK(palette.resolve(126, 0).block_name == "oak_slab");

        // Every stair row in the table resolves all eight orientations, so a new
        // stair family added without its data layout is caught here.
        for (const uint16_t id : palette.ids()) {
            const auto* row = palette.row_for(id);
            CHECK(row != nullptr);
            if (row->variants.empty()) continue;
            if (row->variants.size() != 8) continue;
            std::set<std::string> names;
            for (const auto& variant : row->variants) names.insert(variant.second);
            CHECK_MESSAGE(names.size() == 8, "id " << id << " maps two data values to one block");
        }
    } else {
        MESSAGE("data/minecraft_blocks.json not found; the shipped table was not checked");
    }
}
