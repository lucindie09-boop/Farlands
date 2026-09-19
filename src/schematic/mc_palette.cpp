#include "schematic/mc_palette.hpp"

#include "schematic/minimal_json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <set>

namespace VoxelEngine {
namespace schematic {
namespace {

// Legacy ids were a byte, plus the AddBlocks nibble for later additions. The cap
// is generous rather than exact: it exists to catch a typo like 5300, not to
// model one particular version's id space.
constexpr int64_t kMaxLegacyId = 4095;
// The placeholder a named variant set uses for the block it is applied to.
constexpr const char* kFamilyPlaceholder = "{family}";

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

bool fail_row(std::string* error, uint16_t id, const std::string& message) {
    return fail(error, "minecraft_blocks.json: id " + std::to_string(id) + ": " + message);
}

// Keys starting with '_' are notes to the reader, everywhere in the file.
[[nodiscard]] bool is_comment_key(const std::string& key) noexcept {
    return !key.empty() && key[0] == '_';
}

// One "variants" object: data value → block name (or null to skip that value).
bool parse_variants(const JsonValue& object, std::vector<std::pair<uint8_t, std::string>>& out,
                    const std::string& where, std::string* error) {
    if (!object.is_object()) return fail(error, where + ": \"variants\" must be an object");
    for (const auto& variant : object.members()) {
        const std::string& data_key = variant.first;
        // Keys are data values, written as JSON numbers; a quoted number would
        // never match at resolve time, so it is refused here.
        if (data_key.empty() || data_key.find_first_not_of("0123456789") != std::string::npos) {
            return fail(error, where + ": variant key \"" + data_key + "\" must be a data value 0..15");
        }
        const long data_value = std::strtol(data_key.c_str(), nullptr, 10);
        if (data_value > 15) {
            return fail(error, where + ": variant key \"" + data_key + "\" is above 15");
        }
        std::string target;
        if (!variant.second.is_null()) {
            if (!variant.second.is_string()) {
                return fail(error, where + ": variant " + data_key + " must be a block name or null");
            }
            target = variant.second.as_string();
        }
        out.emplace_back(static_cast<uint8_t>(data_value), std::move(target));
    }
    return true;
}

} // namespace

bool McPalette::load(const std::string& json_text, std::string* error) {
    rows_.clear();
    by_id_.clear();
    sets_.clear();

    JsonValue document;
    if (!parse_json(json_text, document, error)) return false;
    if (!document.is_object()) return fail(error, "minecraft_blocks.json: root must be an object");

    // Shared variant sets first: rows name them, so they have to exist before the
    // rows are read.
    if (const JsonValue* sets = document.member("variant_sets")) {
        if (!sets->is_object()) return fail(error, "minecraft_blocks.json: \"variant_sets\" must be an object");
        for (const auto& entry : sets->members()) {
            if (is_comment_key(entry.first)) continue;
            VariantSet set;
            set.name = entry.first;
            if (!parse_variants(entry.second, set.variants, "variant set \"" + entry.first + "\"", error)) {
                return false;
            }
            sets_.push_back(std::move(set));
        }
    }

    const JsonValue* blocks = document.member("blocks");
    if (blocks == nullptr || !blocks->is_array()) {
        return fail(error, "minecraft_blocks.json: root needs a \"blocks\" array");
    }

    for (const JsonValue& entry : blocks->items()) {
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
                if (value.is_string()) {
                    // A named set, with this row's family substituted in.
                    variants_from_set = true;
                    const std::string name = value.as_string();
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
                } else if (!parse_variants(value, row.variants, "id " + std::to_string(row.id), error)) {
                    return false;
                }
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
        by_id_.emplace(row.id, rows_.size());
        rows_.push_back(std::move(row));
    }

    if (rows_.empty()) return fail(error, "minecraft_blocks.json: no rows");
    return true;
}

const PaletteRow* McPalette::row_for(uint16_t id) const {
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return nullptr;
    return &rows_[found->second];
}

PaletteTarget McPalette::resolve(uint16_t id, uint8_t data) const {
    PaletteTarget target;
    const PaletteRow* row = row_for(id);
    if (row == nullptr) return target;  // Unknown

    target.note = row->note;
    target.substitute = row->substitute;
    target.fluid = row->fluid;

    for (const auto& variant : row->variants) {
        if (variant.first != data) continue;
        target.from_variant = true;
        if (variant.second.empty()) {
            target.kind = PaletteTarget::Kind::Skipped;
            target.fluid = false;
            return target;
        }
        target.kind = PaletteTarget::Kind::Mapped;
        target.block_name = variant.second;
        return target;
    }

    if (!row->block.empty()) {
        target.kind = PaletteTarget::Kind::Mapped;
        target.block_name = row->block;
        return target;
    }

    target.kind = PaletteTarget::Kind::Skipped;
    target.fluid = false;
    return target;
}

std::vector<uint16_t> McPalette::ids() const {
    std::vector<uint16_t> out;
    out.reserve(rows_.size());
    for (const PaletteRow& row : rows_) out.push_back(row.id);
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<std::string> target_names(const McPalette& palette) {
    std::set<std::string> unique;
    for (const uint16_t id : palette.ids()) {
        const PaletteRow* row = palette.row_for(id);
        if (row == nullptr) continue;
        if (!row->block.empty()) unique.insert(row->block);
        for (const auto& variant : row->variants) {
            if (!variant.second.empty()) unique.insert(variant.second);
        }
    }
    return std::vector<std::string>(unique.begin(), unique.end());
}

bool parse_block_names(const std::string& json_text, std::vector<std::string>& out,
                       std::string* error) {
    JsonValue root;
    if (!parse_json(json_text, root, error)) return false;
    if (!root.is_array()) return fail(error, "block_definitions.json: the root must be an array");

    out.clear();
    for (const JsonValue& entry : root.items()) {
        if (!entry.is_object()) {
            return fail(error, "block_definitions.json: entry " + std::to_string(out.size()) +
                                    " is not an object");
        }
        const JsonValue* name = entry.member("name");
        if (name == nullptr || !name->is_string() || name->as_string().empty()) {
            return fail(error, "block_definitions.json: entry " + std::to_string(out.size()) +
                                    " has no \"name\" string");
        }
        out.push_back(name->as_string());
    }
    return true;
}

} // namespace schematic
} // namespace VoxelEngine
