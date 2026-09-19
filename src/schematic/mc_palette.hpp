#ifndef FARLANDS_MC_PALETTE_HPP
#define FARLANDS_MC_PALETTE_HPP

// -----------------------------------------------------------------------------
// What a saved block should become here.
//
// A build file names its blocks in one of two languages, and both are translated
// by one data file (`data/minecraft_blocks.json`) that can be edited without a
// rebuild. This is that file's reader and resolver.
//
// Classic files number their blocks with an older game's ids, which mean nothing
// to this engine. A row is keyed by that id and provides:
//   "block"    — what every data value of that id becomes,
//   "variants" — per-data-value overrides, either written out or naming a set
//                from the file's top-level "variant_sets" (a data value mapped
//                to null means "skip this one"),
//   "family"   — the block a variant set's "{family}" placeholder expands to,
//   "skip"     — the row has no block here and is dropped,
//   "substitute" — the target is a stand-in, not the same block,
//   "fluid"    — the target is a liquid, which a paste has to handle specially,
//   "note"     — free text for whoever reads the file (and the report).
//
// Palette files name their blocks outright — "minecraft:oak_stairs", with
// properties — so they are translated by the file's "names" section instead. A
// name row is keyed by the state's name and may be:
//   "minecraft:stone": "stone"                 — a whole name maps to a block,
//   {"block": "...", "substitute": true, ...}  — with the same flags as above,
//   {"skip": true, "note": "..."}              — a deliberate no,
//   {"shape": "stairs", "family": "..."}       — the shape's properties decide
//                                                which of the family's blocks is
//                                                meant (see ShapeKind).
// The extra possibilities a named file brings:
//   * a key may contain `{wood}`, which matches exactly the word of a species in
//     the file's "species" map, or `*`, which matches any text. The two are not
//     the same thing and a key may hold only one of them: `{wood}_stairs`
//     matches every wood this game can stand in for and nothing else, while
//     `*_stairs` is the fallback for the stair materials it cannot (which is why
//     the specific rows have to be written above it, since the first pattern
//     that fits wins). It also means a free `*` cannot leak into a target: only
//     `{wood}` may appear in one, because only it promises a species.
//   * a species entry carries its own "substitute" flag and note, because
//     whether a mapping is a stand-in is a property of the mapping: oak to oak
//     is the same block, birch to our spruce is not.
//
// Deliberately absent: any inference from a target's name, any family or
// orientation logic beyond the explicit rows and the shapes below, and any
// fallback that invents a block for a state the table has never heard of. A
// state with no row resolves to Unknown and is REPORTED, because a silent guess
// is how a pasted building turns out subtly wrong with no way to tell.
//
// Name validity is not checked here: this file never touches the block registry,
// so it stays usable without the engine. A caller that has a block list checks
// the returned names against it (see `target_names`).
// -----------------------------------------------------------------------------

#include "schematic/schematic_reader.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// The shapes whose properties this game's own blocks encode. A shape row says
// which family to use and the resolver reads the state's properties to pick the
// member: a stair's stack rises toward its `facing`, and the northward one is
// the plain block, which is how this game names them.
enum class ShapeKind : uint8_t { None = 0, Stairs, Slab };

[[nodiscard]] const char* shape_kind_name(ShapeKind shape) noexcept;

struct PaletteTarget {
    enum class Kind : uint8_t { Unknown = 0, Skipped, Mapped };

    Kind kind = Kind::Unknown;
    // The block name to place; empty for Skipped and Unknown.
    std::string block_name;
    // The mapping is a stand-in rather than the block itself.
    bool substitute = false;
    // The target is a liquid.
    bool fluid = false;
    // The mapping came from a per-data-value override rather than the row's
    // default, which is what tells an oriented block from a plain one.
    bool from_variant = false;
    // The table's note for the row, when it has one.
    std::string note;
    // Which row decided this: an id for a classic file, a name or pattern for a
    // palette one. Reported so a surprising placement points at the line to fix.
    std::string source;

    [[nodiscard]] bool mapped() const noexcept { return kind == Kind::Mapped; }
    [[nodiscard]] bool skipped() const noexcept { return kind == Kind::Skipped; }
    [[nodiscard]] bool unknown() const noexcept { return kind == Kind::Unknown; }
};

struct PaletteRow {
    uint16_t id = 0;
    // Data value → block name, already expanded. An empty name means "skip this
    // data value".
    std::vector<std::pair<uint8_t, std::string>> variants;
    // What a data value without an override becomes; empty means skip.
    std::string block;
    bool skip = false;
    bool substitute = false;
    bool fluid = false;
    std::string note;
};

// One "names" row. `prefix`/`suffix` are the literal text either side of a
// pattern's placeholder; a key without one matches whole.
struct NameRow {
    std::string key;
    std::string prefix;
    std::string suffix;
    // The key holds a placeholder at all (either kind).
    bool wildcard = false;
    // The placeholder is `{wood}`, so what it matches has to be a species and the
    // row may use `{wood}` in its target.
    bool species = false;
    // The target: either a literal block name (a `{wood}` template allowed), or,
    // for a shape row, the family the shape's members are named after.
    std::string block;
    std::string family;
    ShapeKind shape = ShapeKind::None;
    bool skip = false;
    bool substitute = false;
    bool fluid = false;
    std::string note;
};

// What one of the other game's wood species becomes here. The stand-in flag and
// note live on the mapping, because the same row is exact for one species and a
// substitute for the next.
struct SpeciesRule {
    std::string block;
    bool substitute = false;
    std::string note;
};

// A named data-value mapping from the file's "variant_sets", with the raw
// "{family}" placeholder still in it (kept so the sets can be listed).
struct VariantSet {
    std::string name;
    std::vector<std::pair<uint8_t, std::string>> variants;
};

class McPalette {
public:
    // Loads from the table's JSON text. Returns false with a reason (including
    // the offending row's id or key) on a malformed table.
    bool load(const std::string& json_text, std::string* error = nullptr);

    // The target for a state, whichever language it is written in.
    [[nodiscard]] PaletteTarget resolve(const BlockState& state) const;
    // The classic path, for a caller that only has the numbers.
    [[nodiscard]] PaletteTarget resolve(uint16_t id, uint8_t data) const;

    [[nodiscard]] const PaletteRow* row_for(uint16_t id) const;
    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }
    [[nodiscard]] size_t name_row_count() const noexcept { return names_.size(); }
    [[nodiscard]] const std::vector<NameRow>& name_rows() const noexcept { return names_; }
    [[nodiscard]] const std::vector<std::pair<std::string, SpeciesRule>>& species() const noexcept {
        return species_;
    }
    // The species a `{wood}` template can be given, in file order.
    [[nodiscard]] std::vector<std::string> species_keys() const;
    // Ids the table knows, ascending; useful for a report or a validation pass.
    [[nodiscard]] std::vector<uint16_t> ids() const;
    [[nodiscard]] const std::vector<VariantSet>& variant_sets() const noexcept { return sets_; }

private:
    [[nodiscard]] PaletteTarget resolve_named(const BlockState& state) const;
    // Applies one row to one state. `capture` is what the row's `*` stood for,
    // empty for a row keyed by a whole name. A row that does not fit the state
    // leaves the target Unknown, which is what lets resolve_named fall through
    // to the next pattern.
    [[nodiscard]] PaletteTarget apply_name_row(const NameRow& row, const std::string& capture,
                                               const BlockState& state) const;

    std::vector<PaletteRow> rows_;
    std::unordered_map<uint16_t, size_t> by_id_;
    std::vector<VariantSet> sets_;
    std::vector<NameRow> names_;
    // Exact name → row index. A pattern is matched by walking `names_`, so a
    // file's order decides between two patterns that both fit.
    std::unordered_map<std::string, size_t> names_by_key_;
    std::vector<std::pair<std::string, SpeciesRule>> species_;
    std::unordered_map<std::string, size_t> species_by_key_;
};

// Every distinct target name in the table, sorted, so a caller can report the
// rows that name a block this game does not have. Shape rows are expanded into
// the members they can produce, and a pattern is expanded over the species it
// can be given, so the list is every name the table can actually ask for.
[[nodiscard]] std::vector<std::string> target_names(const McPalette& palette);

// The target for a state with no table at all, which is how a caller reports
// "this state is not in the table" without inventing one.
[[nodiscard]] PaletteTarget unknown_target(const BlockState& state);

// Reads the "name" of every entry in a block_definitions.json array, in file
// order — which is also the block id order, since ids are positional. This is
// the list of names a table row is allowed to use, and reading it needs no
// engine runtime, so the report tool and the tests can both validate against it.
bool parse_block_names(const std::string& json_text, std::vector<std::string>& out,
                       std::string* error = nullptr);

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_MC_PALETTE_HPP
