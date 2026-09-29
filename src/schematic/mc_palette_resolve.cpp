// Resolving a state to what it becomes: the shape families a family expands to,
// the template substitution a "names" row goes through, the legacy-id and
// named-state lookups, and the accessors the name list is built from. Loading the
// table itself is in mc_palette.cpp.

#include "schematic/mc_palette.hpp"
#include "schematic/mc_palette_internal.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <set>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {

namespace mc_palette_detail {

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

} // namespace mc_palette_detail

// The two names above are what the helpers below call.
using namespace mc_palette_detail;

namespace {

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

} // namespace schematic
} // namespace VoxelEngine
