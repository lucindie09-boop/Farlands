// The CLASSIC rows: the "blocks" array of minecraft_blocks.json, keyed by the older
// game's numeric id. Split from schematic/mc_palette.cpp, which owns the loader that
// walks them (McPalette::load) and the name-row readers; the readers both halves use
// are declared in schematic/mc_palette_internal.hpp.
//
// A row validates itself as it is read and the caller only appends what came back
// whole, which is why these return false with a reason rather than filling in a row
// the table would then have to reject.

#include "schematic/mc_palette.hpp"
#include "schematic/mc_palette_internal.hpp"

#include "schematic/minimal_json.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {

using namespace mc_palette_detail;

// One entry of the classic "blocks" array: its id, every key it carries, and the
// checks that need the whole entry read. The caller appends the row it produces.
bool McPalette::load_classic_row(const JsonValue& entry, std::string* error) {
    if (!entry.is_object()) return fail(error, "minecraft_blocks.json: every row must be an object");

    const JsonValue* id_value = entry.member("id");
    if (id_value == nullptr || !id_value->is_number()) {
        return fail(error, "minecraft_blocks.json: a row is missing a numeric \"id\"");
    }
    const int64_t raw_id = id_value->as_integer();
    if (raw_id < 0 || raw_id > kMaxLegacyId) {
        return fail(error, "minecraft_blocks.json: id " + std::to_string(raw_id) +
                               " is outside the legacy range 0.." + std::to_string(kMaxLegacyId));
    }
    PaletteRow row;
    row.id = static_cast<uint16_t>(raw_id);
    if (by_id_.find(row.id) != by_id_.end()) {
        return fail_row(error, row.id, "a second row for the same id");
    }

    std::string family;
    // "family" only has an effect on the named-set path; on an inline
    // "variants" object it would be read and dropped, which is exactly the
    // silent no-op the unknown-key check exists to prevent.
    bool variants_from_set = false;
    for (const auto& member : entry.members()) {
        const std::string& key = member.first;
        const JsonValue& value = member.second;
        if (is_comment_key(key) || key == "id") continue;
        if (key == "note") {
            if (!value.is_string()) return fail_row(error, row.id, "\"note\" must be a string");
            row.note = value.as_string();
            continue;
        }
        if (key == "block") {
            if (!value.is_string()) return fail_row(error, row.id, "\"block\" must be a string");
            row.block = value.as_string();
            continue;
        }
        if (key == "family") {
            if (!value.is_string()) return fail_row(error, row.id, "\"family\" must be a string");
            family = value.as_string();
            if (family.find('{') != std::string::npos) {
                return fail_row(error, row.id, "\"family\" must not contain a placeholder");
            }
            continue;
        }
        if (key == "variants") {
            if (!parse_row_variants(value, row, family, variants_from_set, error)) return false;
            continue;
        }
        if (key == "skip") {
            if (!value.is_bool()) return fail_row(error, row.id, "\"skip\" must be true or false");
            row.skip = value.as_bool();
            continue;
        }
        if (key == "substitute") {
            if (!value.is_bool()) return fail_row(error, row.id, "\"substitute\" must be true or false");
            row.substitute = value.as_bool();
            continue;
        }
        if (key == "fluid") {
            if (!value.is_bool()) return fail_row(error, row.id, "\"fluid\" must be true or false");
            row.fluid = value.as_bool();
            continue;
        }
        if (key == "still") {
            if (!value.is_string()) {
                return fail_row(error, row.id, "\"still\" must be a block name");
            }
            row.still = value.as_string();
            continue;
        }
        // An unrecognised key is nearly always a typo ("blok", "variant"),
        // and silently ignoring it would drop the mapping it was meant to
        // make, so it is an error.
        return fail_row(error, row.id, "unknown key \"" + key + "\"");
    }

    if (!family.empty() && !variants_from_set) {
        return fail_row(error, row.id, "\"family\" is only meaningful with a named variant set");
    }
    // A row that both names a block and declares itself skipped is ambiguous
    // about which one wins, and the usual cause is a "block": "skip" left in
    // as if it were a value, which would then be placed as a block name.
    if (row.skip && !row.block.empty()) {
        return fail_row(error, row.id, "a row cannot have both \"block\" and \"skip\"");
    }
    if (row.block.empty() && row.variants.empty() && !row.skip) {
        return fail_row(error, row.id, "a row needs \"block\", \"variants\" or \"skip\"");
    }
    if (!row.still.empty() && !row.fluid) {
        return fail_row(error, row.id, "\"still\" only means something with \"fluid\"");
    }
    by_id_.emplace(row.id, rows_.size());
    rows_.push_back(std::move(row));
    return true;
}

// The "variants" key of one classic row: a named set, with this row's "family"
// substituted for {family}, or an inline data-value object. `from_set` reports which
// of the two it was, because a "family" only means something for the first.
bool McPalette::parse_row_variants(const JsonValue& value, PaletteRow& row, const std::string& family,
                                   bool& from_set, std::string* error) {
    if (!value.is_string()) {
        return parse_variants(value, row.variants, "id " + std::to_string(row.id), error);
    }
    // A named set, with this row's family substituted in.
    from_set = true;
    const std::string& name = value.as_string();
    const auto found = std::find_if(sets_.begin(), sets_.end(),
                                    [&name](const VariantSet& s) { return s.name == name; });
    if (found == sets_.end()) {
        return fail_row(error, row.id, "unknown variant set \"" + name + "\"");
    }
    for (const auto& variant : found->variants) {
        std::string target = variant.second;
        if (target.find(kFamilyPlaceholder) != std::string::npos && family.empty()) {
            return fail_row(error, row.id,
                            "variant set \"" + name + "\" needs a \"family\"");
        }
        for (size_t at = target.find(kFamilyPlaceholder); at != std::string::npos;
             at = target.find(kFamilyPlaceholder)) {
            target.replace(at, std::strlen(kFamilyPlaceholder), family);
        }
        row.variants.emplace_back(variant.first, std::move(target));
    }
    return true;
}

} // namespace schematic
} // namespace VoxelEngine
