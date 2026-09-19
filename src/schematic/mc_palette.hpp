#ifndef FARLANDS_MC_PALETTE_HPP
#define FARLANDS_MC_PALETTE_HPP

// -----------------------------------------------------------------------------
// What a legacy block state should become here.
//
// A block file numbers its blocks with an older game's ids, and those ids mean
// nothing to this engine — a large share of them name blocks this game does not
// have at all. The mapping therefore lives in a data file
// (`data/minecraft_blocks.json`) that can be edited without a rebuild, and this
// is its reader and resolver.
//
// A row is keyed by the legacy id and provides:
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
// Named variant sets exist because the legacy data layout for a shape is the
// same whatever it is made of: every stair type spells its four facings and two
// halves identically, so the orientation convention is written once and each
// stair family is one line naming it. That also means the one place to look when
// a pasted staircase climbs the wrong way is the set, not a dozen duplicated
// blocks.
//
// Deliberately absent: any inference from the target's name, any family or
// orientation logic beyond the explicit per-data rows, and any fallback that
// invents a block for an id the table has never heard of. An id with no row
// resolves to Unknown and is REPORTED, because a silent guess is how a pasted
// building turns out subtly wrong with no way to tell.
//
// Name validity is not checked here: this file never touches the block
// registry, so it stays usable without the engine. A caller that has a block
// list checks the returned names against it (see `target_names`).
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace VoxelEngine {
namespace schematic {

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

// A named data-value mapping from the file's "variant_sets", with the raw
// "{family}" placeholder still in it (kept so the sets can be listed).
struct VariantSet {
    std::string name;
    std::vector<std::pair<uint8_t, std::string>> variants;
};

class McPalette {
public:
    // Loads from the table's JSON text. Returns false with a reason (including
    // the offending row's id) on a malformed table.
    bool load(const std::string& json_text, std::string* error = nullptr);

    [[nodiscard]] PaletteTarget resolve(uint16_t id, uint8_t data) const;
    [[nodiscard]] const PaletteRow* row_for(uint16_t id) const;
    [[nodiscard]] size_t row_count() const noexcept { return rows_.size(); }
    // Ids the table knows, ascending; useful for a report or a validation pass.
    [[nodiscard]] std::vector<uint16_t> ids() const;
    [[nodiscard]] const std::vector<VariantSet>& variant_sets() const noexcept { return sets_; }

private:
    std::vector<PaletteRow> rows_;
    std::unordered_map<uint16_t, size_t> by_id_;
    std::vector<VariantSet> sets_;
};

// Every distinct target name in the table, sorted, so a caller can report the
// rows that name a block this game does not have.
[[nodiscard]] std::vector<std::string> target_names(const McPalette& palette);

// Reads the "name" of every entry in a block_definitions.json array, in file
// order — which is also the block id order, since ids are positional. This is
// the list of names a table row is allowed to use, and reading it needs no
// engine runtime, so the report tool and the tests can both validate against it.
bool parse_block_names(const std::string& json_text, std::vector<std::string>& out,
                       std::string* error = nullptr);

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_MC_PALETTE_HPP
