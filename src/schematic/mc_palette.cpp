// Loading the Minecraft block table: the parse helpers for its rows, species and
// name patterns, and the McPalette::load walk that joins them. Turning a legacy
// id or a state name into a target is in mc_palette_resolve.cpp.

#include "schematic/mc_palette.hpp"
#include "schematic/mc_palette_internal.hpp"

#include "schematic/minimal_json.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// The {wood} placeholder is needed on both sides of this split, so it and the
// question about it live in schematic/mc_palette_internal.hpp.
using namespace mc_palette_detail;

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

// Builds a message from its parts in one string. Chaining `+` allocates a temporary per
// operator, and a load-time message that quotes a key it rejected is all parts.
std::string joined(std::initializer_list<std::string_view> parts) {
    size_t total = 0;
    for (const std::string_view part : parts) total += part.size();
    std::string out;
    out.reserve(total);
    for (const std::string_view part : parts) out.append(part.data(), part.size());
    return out;
}

bool fail_row(std::string* error, uint16_t id, const std::string& message) {
    return fail(error, "minecraft_blocks.json: id " + std::to_string(id) + ": " + message);
}

bool fail_name(std::string* error, const std::string& key, const std::string& message) {
    return fail(error, "minecraft_blocks.json: name \"" + key + "\": " + message);
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
            return fail(error, joined({where, ": variant key \"", data_key, "\" must be a data value 0..15"}));
        }
        const long data_value = std::strtol(data_key.c_str(), nullptr, 10);
        if (data_value > 15) {
            return fail(error, joined({where, ": variant key \"", data_key, "\" is above 15"}));
        }
        std::string target;
        if (!variant.second.is_null()) {
            if (!variant.second.is_string()) {
                return fail(error, joined({where, ": variant ", data_key, " must be a block name or null"}));
            }
            target = variant.second.as_string();
        }
        out.emplace_back(static_cast<uint8_t>(data_value), std::move(target));
    }
    return true;
}

// A species entry: an object, or the bare block name it becomes. Every species
// this game has is written out, so the mapping never has to be guessed at.
bool parse_species(const JsonValue& object,
                   std::vector<std::pair<std::string, SpeciesRule>>& out, std::string* error) {
    if (!object.is_object()) {
        return fail(error, "minecraft_blocks.json: \"species\" must be an object");
    }
    for (const auto& entry : object.members()) {
        if (is_comment_key(entry.first)) continue;
        SpeciesRule rule;
        if (entry.second.is_string()) {
            rule.block = entry.second.as_string();
            // Written bare, the entry is the plain reading: the same block.
        } else if (entry.second.is_object()) {
            for (const auto& member : entry.second.members()) {
                const std::string& key = member.first;
                const JsonValue& value = member.second;
                if (is_comment_key(key)) continue;
                if (key == "block") {
                    if (!value.is_string()) {
                        return fail(error, "species \"" + entry.first + "\": \"block\" must be a string");
                    }
                    rule.block = value.as_string();
                } else if (key == "substitute") {
                    if (!value.is_bool()) {
                        return fail(error, "species \"" + entry.first + "\": \"substitute\" must be true or false");
                    }
                    rule.substitute = value.as_bool();
                } else if (key == "note") {
                    if (!value.is_string()) {
                        return fail(error, "species \"" + entry.first + "\": \"note\" must be a string");
                    }
                    rule.note = value.as_string();
                } else {
                    return fail(error, "species \"" + entry.first + "\": unknown key \"" + key + "\"");
                }
            }
        } else {
            return fail(error, "species \"" + entry.first + "\": expected a block name or an object");
        }
        if (rule.block.empty()) return fail(error, "species \"" + entry.first + "\" has no block");
        if (rule.block.find('{') != std::string::npos) {
            return fail(error, "species \"" + entry.first + "\": \"block\" must not contain a placeholder");
        }
        if (entry.first.empty() || entry.first.find(':') != std::string::npos ||
            entry.first.find('*') != std::string::npos) {
            return fail(error, "species \"" + entry.first + "\" must be a bare species word");
        }
        out.emplace_back(entry.first, std::move(rule));
    }
    return true;
}

// One "names" row: a whole name, or a pattern with a single `*`, mapped to a
// block, a shape's family, or a deliberate skip.
bool parse_name_row(const std::string& key, const JsonValue& value, NameRow& row, std::string* error) {
    row.key = key;
    if (key.empty()) return fail_name(error, key, "the key is empty");
    if (key.find(':') == std::string::npos) {
        return fail_name(error, key, "a state name needs a namespace (\"minecraft:stone\")");
    }
    if (key.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ") != std::string::npos) {
        return fail_name(error, key, "state names are lowercase");
    }
    // Two placeholder forms, and a key may hold only one of them: `*` captures
    // any text, `{wood}` captures exactly a species word. Splitting them is what
    // keeps a row like "{wood}_stairs" from matching "stone_bricks_stairs", and
    // it is also what makes `{wood}` safe to use in a target.
    const size_t star = key.find('*');
    const size_t wood = key.find(kWoodPlaceholder);
    if (star != std::string::npos && wood != std::string::npos) {
        return fail_name(error, key, "a key cannot hold both * and {wood}");
    }
    const size_t placeholder = star != std::string::npos ? star : wood;
    if (placeholder != std::string::npos) {
        const size_t length = star != std::string::npos ? 1 : std::strlen(kWoodPlaceholder);
        if (key.find(star != std::string::npos ? '*' : '{', placeholder + length) != std::string::npos) {
            return fail_name(error, key, "a pattern can hold only one placeholder");
        }
        row.wildcard = true;
        row.species = star == std::string::npos;
        row.prefix = key.substr(0, placeholder);
        row.suffix = key.substr(placeholder + length);
    } else if (key.find('{') != std::string::npos || key.find('}') != std::string::npos) {
        return fail_name(error, key, "a placeholder must be written exactly as {wood}");
    }

    const std::string where = "minecraft_blocks.json: name \"" + key + "\": ";
    if (value.is_string()) {
        row.block = value.as_string();
    } else if (value.is_object()) {
        std::string shape_text;
        for (const auto& member : value.members()) {
            const std::string& member_key = member.first;
            const JsonValue& member_value = member.second;
            if (is_comment_key(member_key)) continue;
            if (member_key == "block") {
                if (!member_value.is_string()) return fail(error, where + "\"block\" must be a string");
                row.block = member_value.as_string();
            } else if (member_key == "family") {
                if (!member_value.is_string()) return fail(error, where + "\"family\" must be a string");
                row.family = member_value.as_string();
            } else if (member_key == "shape") {
                if (!member_value.is_string()) return fail(error, where + "\"shape\" must be a string");
                shape_text = member_value.as_string();
            } else if (member_key == "skip") {
                if (!member_value.is_bool()) return fail(error, where + "\"skip\" must be true or false");
                row.skip = member_value.as_bool();
            } else if (member_key == "substitute") {
                if (!member_value.is_bool()) {
                    return fail(error, where + "\"substitute\" must be true or false");
                }
                row.substitute = member_value.as_bool();
            } else if (member_key == "fluid") {
                if (!member_value.is_bool()) return fail(error, where + "\"fluid\" must be true or false");
                row.fluid = member_value.as_bool();
            } else if (member_key == "still") {
                if (!member_value.is_string()) return fail(error, where + "\"still\" must be a block name");
                row.still = member_value.as_string();
            } else if (member_key == "note") {
                if (!member_value.is_string()) return fail(error, where + "\"note\" must be a string");
                row.note = member_value.as_string();
            } else {
                return fail(error, joined({where, "unknown key \"", member_key, "\""}));
            }
        }
        if (!shape_text.empty()) {
            if (shape_text == "stairs") {
                row.shape = ShapeKind::Stairs;
            } else if (shape_text == "slab") {
                row.shape = ShapeKind::Slab;
            } else {
                return fail(error, where + "unknown shape \"" + shape_text + "\" (stairs, slab)");
            }
        }
    } else {
        return fail(error, where + "expected a block name or an object");
    }        if (row.shape != ShapeKind::None) {
        if (!row.block.empty()) {
            return fail(error, where + "\"block\" and \"shape\" say the same thing twice");
        }
        if (row.family.empty()) return fail(error, where + "a shape row needs a \"family\"");
        if (has_placeholder(row.family) && !row.species) {
            return fail(error, where + "\"{wood}\" in a target needs \"{wood}\" in the key");
        }
    } else if (!row.block.empty()) {
        if (has_placeholder(row.block) && !row.species) {
            return fail(error, where + "\"{wood}\" in a target needs \"{wood}\" in the key");
        }
    } else if (!row.skip) {
        return fail(error, where + "a row needs \"block\", \"shape\" or \"skip\"");
    }
    if (row.skip && (!row.block.empty() || row.shape != ShapeKind::None)) {
        return fail(error, where + "a row cannot both skip and name a target");
    }
    // A still form is the still form OF a liquid, so it is meaningless (and
    // almost certainly a row whose "fluid" was forgotten) without one.
    if (!row.still.empty() && !row.fluid) {
        return fail(error, where + "\"still\" only means something with \"fluid\"");
    }
    return true;
}

} // namespace

bool McPalette::load(const std::string& json_text, std::string* error) {
    rows_.clear();
    by_id_.clear();
    sets_.clear();
    names_.clear();
    names_by_key_.clear();
    species_.clear();
    species_by_key_.clear();

    JsonValue document;
    if (!parse_json(json_text, document, error)) return false;
    if (!document.is_object()) return fail(error, "minecraft_blocks.json: root must be an object");

    // The species map first: the name rows are written in terms of it.
    if (const JsonValue* species = document.member("species")) {
        if (!parse_species(*species, species_, error)) return false;
        for (size_t i = 0; i < species_.size(); ++i) species_by_key_.emplace(species_[i].first, i);
    }

    // Shared variant sets next: rows name them, so they have to exist before the
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

    // The name rows: a pattern is only allowed to bring in a species, so an
    // unresolvable one is a table fault rather than a runtime surprise.
    if (const JsonValue* names = document.member("names")) {
        if (!names->is_object()) return fail(error, "minecraft_blocks.json: \"names\" must be an object");
        for (const auto& entry : names->members()) {
            if (is_comment_key(entry.first)) continue;
            NameRow row;
            if (!parse_name_row(entry.first, entry.second, row, error)) return false;
            if (row.wildcard && (has_placeholder(row.block) || has_placeholder(row.family)) &&
                species_.empty()) {
                return fail_name(error, entry.first, "\"{wood}\" needs a \"species\" map");
            }
            // Every key is unique. A specific pattern and a general fallback for
            // the same shape are written as two different keys ("{wood}_stairs"
            // and "*_stairs"), so nothing here has to lean on the order two
            // identical keys happened to be read in.
            if (names_by_key_.find(row.key) != names_by_key_.end()) {
                return fail_name(error, row.key, "a second row for the same name");
            }
            names_by_key_.emplace(row.key, names_.size());
            names_.push_back(std::move(row));
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
    }

    if (rows_.empty() && names_.empty()) {
        return fail(error, "minecraft_blocks.json: no rows");
    }
    return true;
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