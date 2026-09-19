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
// The placeholder a "names" row uses for the species its pattern captured.
constexpr const char* kWoodPlaceholder = "{wood}";

bool fail(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
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

[[nodiscard]] bool has_placeholder(const std::string& text) noexcept {
    return text.find(kWoodPlaceholder) != std::string::npos;
}

// Puts `block` in place of every `{wood}` in `text`.
void substitute_wood(std::string& text, const std::string& block) {
    for (size_t at = text.find(kWoodPlaceholder); at != std::string::npos;
         at = text.find(kWoodPlaceholder, at + block.size())) {
        text.replace(at, std::strlen(kWoodPlaceholder), block);
    }
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
                return fail(error, where + "unknown key \"" + member_key + "\"");
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

// Applies a shape rule to a state's properties. The rule reads the properties it
// knows and ignores the rest (a stair's `shape=inner_left` says nothing here),
// but a property it does read has to be one it understands: a missing or
// unrecognised value means this state is not placed rather than guessed at,
// because a silently wrong orientation is worse than a reported miss.
bool shape_target(ShapeKind shape, const std::string& family, const BlockState& state,
                  std::string& out, std::string& why) {
    switch (shape) {
        case ShapeKind::Stairs: {
            const std::string* facing = state.property("facing");
            const std::string* half = state.property("half");
            if (facing == nullptr || half == nullptr) {
                why = "a stair needs \"facing\" and \"half\"";
                return false;
            }
            const bool top = *half == "top";
            if (!top && *half != "bottom") {
                why = "half=\"" + *half + "\" is not a stair half";
                return false;
            }
            std::string suffix;
            if (*facing == "north") {
                suffix = top ? "_n_up" : "";
            } else if (*facing == "east") {
                suffix = top ? "_e_up" : "_e";
            } else if (*facing == "south") {
                suffix = top ? "_s_up" : "_s";
            } else if (*facing == "west") {
                suffix = top ? "_w_up" : "_w";
            } else {
                why = "facing=\"" + *facing + "\" is not a compass direction";
                return false;
            }
            out = family + suffix;
            return true;
        }
        case ShapeKind::Slab: {
            // A slab family here is the *material*, not a block: this game names
            // the three states <material>_slab, _slab_top and _double_slab, and
            // the rule spells them out. Stairs go the other way — their family is
            // the plain stair block itself (oak_stairs) — so each shape says what
            // its suffixes attach to.
            const std::string* type = state.property("type");
            if (type == nullptr) {
                why = "a slab needs \"type\"";
                return false;
            }
            if (*type == "bottom") {
                out = family + "_slab";
                return true;
            }
            if (*type == "top") {
                out = family + "_slab_top";
                return true;
            }
            if (*type == "double") {
                out = family + "_double_slab";
                return true;
            }
            why = "type=\"" + *type + "\" is not a slab type";
            return false;
        }
        case ShapeKind::None:
            break;
    }
    why = "no shape rule";
    return false;
}

// Every block a shape's family can be asked for, for the name list a caller
// validates against.
void shape_family_names(ShapeKind shape, const std::string& family, std::vector<std::string>& out) {
    switch (shape) {
        case ShapeKind::Stairs:
            for (const char* suffix : {"", "_e", "_s", "_w", "_n_up", "_e_up", "_s_up", "_w_up"}) {
                out.push_back(family + suffix);
            }
            return;
        case ShapeKind::Slab:
            out.push_back(family + "_slab");
            out.push_back(family + "_slab_top");
            out.push_back(family + "_double_slab");
            return;
        case ShapeKind::None:
            return;
    }
}

// A row's target text with `{wood}` resolved. False means the row does not apply
// to this capture at all — the captured word is not a species the file knows.
bool expand_template(const std::string& templ, const std::string& capture, const McPalette& palette,
                     std::string& out, bool& substitute, std::string& note) {
    out = templ;
    if (!has_placeholder(out)) return true;
    const auto& species = palette.species();
    const auto found = std::find_if(species.begin(), species.end(),
                                    [&capture](const auto& entry) { return entry.first == capture; });
    if (found == species.end()) return false;
    substitute = found->second.substitute;
    note = found->second.note;
    substitute_wood(out, found->second.block);
    return true;
}

} // namespace

const char* shape_kind_name(ShapeKind shape) noexcept {
    switch (shape) {
        case ShapeKind::None: return "none";
        case ShapeKind::Stairs: return "stairs";
        case ShapeKind::Slab: return "slab";
    }
    return "none";
}

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

const PaletteRow* McPalette::row_for(uint16_t id) const {
    const auto found = by_id_.find(id);
    if (found == by_id_.end()) return nullptr;
    return &rows_[found->second];
}

std::vector<std::string> McPalette::species_keys() const {
    std::vector<std::string> out;
    out.reserve(species_.size());
    for (const auto& entry : species_) out.push_back(entry.first);
    return out;
}

PaletteTarget McPalette::resolve(uint16_t id, uint8_t data) const {
    PaletteTarget target;
    const PaletteRow* row = row_for(id);
    if (row == nullptr) return target;  // Unknown

    target.note = row->note;
    target.substitute = row->substitute;
    target.fluid = row->fluid;
    target.still_name = row->still;
    target.source = std::to_string(id);

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

PaletteTarget McPalette::resolve_named(const BlockState& state) const {
    PaletteTarget target;
    const auto exact = names_by_key_.find(state.name);
    if (exact != names_by_key_.end()) {
        return apply_name_row(names_[exact->second], std::string(), state);
    }
    // Patterns are tried in the file's order, so the first one that fits wins —
    // which is how a species row sits above a general fallback for the same
    // shape. An exact key was already answered above, so it beats both.
    for (const NameRow& row : names_) {
        if (!row.wildcard) continue;
        if (state.name.size() < row.prefix.size() + row.suffix.size()) continue;
        if (state.name.compare(0, row.prefix.size(), row.prefix) != 0) continue;
        if (state.name.compare(state.name.size() - row.suffix.size(), row.suffix.size(), row.suffix) != 0) {
            continue;
        }
        const std::string capture =
            state.name.substr(row.prefix.size(), state.name.size() - row.prefix.size() - row.suffix.size());
        // A `{wood}` key matches a species and nothing else, which is what keeps
        // it off every other word that happens to sit in the same place.
        if (row.species && species_by_key_.find(capture) == species_by_key_.end()) continue;
        target = apply_name_row(row, capture, state);
        if (!target.unknown()) return target;
    }
    // Nothing in the table speaks about this state. That is reported, never
    // guessed at, because a wrong block is harder to notice than a gap.
    return PaletteTarget{};
}

PaletteTarget McPalette::apply_name_row(const NameRow& row, const std::string& capture,
                                        const BlockState& state) const {
    PaletteTarget target;
    target.note = row.note;
    target.source = row.key;
    if (row.skip) {
        target.kind = PaletteTarget::Kind::Skipped;
        return target;
    }

    std::string expanded;
    bool species_substitute = false;
    std::string species_note;
    if (row.shape == ShapeKind::None) {
        if (!expand_template(row.block, capture, *this, expanded, species_substitute, species_note)) {
            return target;  // Unknown, and the caller can try the next pattern
        }
        target.kind = PaletteTarget::Kind::Mapped;
        target.block_name = std::move(expanded);
    } else {
        if (!expand_template(row.family, capture, *this, expanded, species_substitute, species_note)) {
            return target;
        }
        std::string why;
        if (!shape_target(row.shape, expanded, state, target.block_name, why)) {
            // The reason a state is not placed is worth as much as the note: it
            // says which property the table (or the file) got wrong.
            target.note = row.note.empty() ? why : (row.note + " (" + why + ")");
            target.block_name.clear();
            return target;
        }
        target.kind = PaletteTarget::Kind::Mapped;
    }
    target.substitute = row.substitute || species_substitute;
    target.fluid = row.fluid;
    target.still_name = row.still;
    if (!species_note.empty()) {
        target.note = row.note.empty() ? species_note : (row.note + "; " + species_note);
    }
    return target;
}

PaletteTarget McPalette::resolve(const BlockState& state) const {
    if (state.is_legacy()) return resolve(state.id, state.data);
    return resolve_named(state);
}

std::vector<uint16_t> McPalette::ids() const {
    std::vector<uint16_t> out;
    out.reserve(rows_.size());
    for (const PaletteRow& row : rows_) out.push_back(row.id);
    std::sort(out.begin(), out.end());
    return out;
}

PaletteTarget unknown_target(const BlockState& state) {
    PaletteTarget target;
    target.note = "no row for " + state.describe();
    target.source = state.describe();
    return target;
}

std::vector<std::string> target_names(const McPalette& palette) {
    std::set<std::string> unique;
    for (const uint16_t id : palette.ids()) {
        const PaletteRow* row = palette.row_for(id);
        if (row == nullptr) continue;
        if (!row->block.empty()) unique.insert(row->block);
        // The still form is a target like any other: a paste places it, so a
        // name typo in it has to be caught the same way.
        if (!row->still.empty()) unique.insert(row->still);
        for (const auto& variant : row->variants) {
            if (!variant.second.empty()) unique.insert(variant.second);
        }
    }

    // Name rows: a pattern can only bring a species in, so the names a row can
    // produce are enumerable here too — which is what makes "every target is a
    // block this build has" a check the report can still make.
    const std::vector<std::string> captures = palette.species_keys();
    for (const NameRow& row : palette.name_rows()) {
        if (row.skip) continue;
        const std::string empty_capture;
        // A `{wood}` row can be handed any species; a free `*` row cannot be
        // handed anything, because a target may not use its capture at all.
        const std::vector<std::string> values = row.species ? captures
                                                           : std::vector<std::string>{empty_capture};
        for (const std::string& capture : values) {
            std::string expanded;
            bool substitute = false;
            std::string note;
            if (!expand_template(row.shape == ShapeKind::None ? row.block : row.family, capture, palette,
                                 expanded, substitute, note)) {
                continue;
            }
            if (!row.still.empty()) {
                // Expanded like any other target, so a `{wood}` liquid row (there
                // is none yet) works the same way its block does.
                std::string still;
                bool still_substitute = false;
                std::string still_note;
                if (expand_template(row.still, capture, palette, still, still_substitute, still_note)) {
                    unique.insert(std::move(still));
                }
            }
            if (row.shape == ShapeKind::None) {
                unique.insert(std::move(expanded));
                continue;
            }
            std::vector<std::string> members;
            shape_family_names(row.shape, expanded, members);
            for (std::string& member : members) unique.insert(std::move(member));
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
