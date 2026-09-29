// Block files from other tools, decode side: the Minecraft palette, the plan a
// decoded file turns into, and the inspect/preview answers a caller reads before
// pasting. The paste itself (and the job that waits for its chunks) is in
// engine/voxel_engine_paste.cpp.

#include "engine/voxel_engine_controller.hpp"

#include "render/multimesh_instance_layout.hpp"
#include "schematic/paste_plan.hpp"
#include "schematic/schematic_reader.hpp"
#include "world/block_editor.hpp"

#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/vector3i.hpp>

namespace VoxelEngine {
using namespace godot;

// -------------------------------------------------------------------------
// Block files from other tools
// -------------------------------------------------------------------------

bool VoxelEngineController::ensure_minecraft_palette() {
    if (minecraft_palette_loaded_) return true;

    Ref<FileAccess> file = FileAccess::open("res://data/minecraft_blocks.json", FileAccess::READ);
    if (file.is_null()) {
        minecraft_palette_error_ =
            "cannot open res://data/minecraft_blocks.json (no translation table)";
        return false;
    }
    const String text = file->get_as_text();
    std::string table_error;
    if (!minecraft_palette_.load(std::string(text.utf8().get_data()), &table_error)) {
        minecraft_palette_error_ = table_error;
        return false;
    }

    // Resolve every name the table uses to a block id once, by scanning the
    // registry once. Doing it per cell would be a linear scan over the block
    // table for every block in a build.
    std::unordered_map<std::string, BlockID> by_name;
    const BlockRegistry& registry = BlockRegistry::get_instance();
    for (size_t i = 0; i < registry.get_count(); ++i) {
        const BlockType& block = registry.get_block_fast(static_cast<BlockID>(i));
        if (block.name == nullptr) continue;
        by_name.emplace(block.name, static_cast<BlockID>(i));
    }

    std::string missing;
    for (const std::string& name : schematic::target_names(minecraft_palette_)) {
        const auto found = by_name.find(name);
        if (found == by_name.end()) {
            if (missing.empty()) missing = name;
            continue;
        }
        minecraft_name_ids_.emplace(name, found->second);
    }
    if (!missing.empty()) {
        // A table naming a block this build does not have would paste holes, so
        // the paste is refused with the name in the message rather than
        // silently dropping that state (the report tool catches this offline).
        minecraft_palette_error_ =
            "data/minecraft_blocks.json maps a state to \"" + missing +
            "\", which is not a block in this build";
        return false;
    }

    minecraft_palette_loaded_ = true;
    return true;
}

Dictionary VoxelEngineController::plan_counters(const schematic::PastePlan& plan) {
    Dictionary out;
    out["planned"] = static_cast<int64_t>(plan.cells.size());
    out["placed"] = static_cast<int64_t>(plan.stats.placed);
    out["substituted"] = static_cast<int64_t>(plan.stats.substituted);
    out["stilled"] = static_cast<int64_t>(plan.stats.stilled);
    out["declined_fluid"] = static_cast<int64_t>(plan.stats.declined_fluid);
    out["declined_substitute"] = static_cast<int64_t>(plan.stats.declined_substitute);
    out["skipped"] = static_cast<int64_t>(plan.stats.skipped);
    out["unknown"] = static_cast<int64_t>(plan.stats.unknown);
    out["unresolved"] = static_cast<int64_t>(plan.stats.unresolved);
    out["air_ignored"] = static_cast<int64_t>(plan.stats.air_ignored);
    return out;
}

bool VoxelEngineController::decode_and_plan(const PackedByteArray& bytes,
                                            const Dictionary& options,
                                            int32_t origin_x, int32_t origin_y, int32_t origin_z,
                                            schematic::PasteOptions& out_options,
                                            const schematic::SchematicData*& out_file,
                                            schematic::PastePlan& out_plan, std::string& error) {
    if (!ensure_minecraft_palette()) {
        error = minecraft_palette_error_;
        return false;
    }
    if (bytes.size() == 0) {
        error = "the file is empty";
        return false;
    }

    // The decode cache: the same bytes decode to the same build, and a preview is
    // re-planned at a new origin every time you move the crosshair while the file
    // stays the same. FNV-1a is enough here — this catches "is it still that file"
    // for a few hundred KB of compressed bytes, and a collision would need two
    // different builds to hash alike, not an attacker.
    uint64_t fingerprint = 1469598103934665603ull;
    {
        const uint8_t* raw = bytes.ptr();
        const int32_t n = bytes.size();
        for (int32_t i = 0; i < n; ++i) {
            fingerprint ^= raw[i];
            fingerprint *= 1099511628211ull;
        }
    }
    if (!have_decoded_build_ || decoded_build_.fingerprint != fingerprint) {
        schematic::SchematicData fresh;
        if (!schematic::load_schematic_bytes(reinterpret_cast<const uint8_t*>(bytes.ptr()),
                                             static_cast<size_t>(bytes.size()), fresh, &error)) {
            return false;
        }
        decoded_build_.file = std::move(fresh);
        decoded_build_.fingerprint = fingerprint;
        have_decoded_build_ = true;
    }
    out_file = &decoded_build_.file;

    if (options.has("fluids")) out_options.fluids = static_cast<bool>(options["fluids"]);
    if (options.has("substitutes")) {
        out_options.substitutes = static_cast<bool>(options["substitutes"]);
    }
    if (options.has("replace_solid")) {
        out_options.replace_solid = static_cast<bool>(options["replace_solid"]);
    }
    if (options.has("write_air")) out_options.write_air = static_cast<bool>(options["write_air"]);
    if (options.has("max_cells")) {
        const int64_t cap = static_cast<int64_t>(options["max_cells"]);
        out_options.max_cells = cap > 0 ? static_cast<size_t>(cap) : 0;
    }

    const auto resolve = [this](const std::string& name, BlockID& out) {
        const auto found = minecraft_name_ids_.find(name);
        if (found == minecraft_name_ids_.end()) return false;
        out = found->second;
        return true;
    };
    if (!schematic::plan_paste(*out_file, minecraft_palette_, origin_x, origin_y, origin_z,
                               out_options, resolve, out_plan, &error)) {
        return false;
    }
    // The cell the caller aimed at is where the BUILDING goes, not where the file's
    // box corner goes. A build saved from a region selection carries the selection's
    // empty margin inside its declared box, so planting the corner at the crosshair is
    // what put a big schematic a hundred blocks from where it was right-clicked. Both
    // the ghost and the paste come through here, so this is also what keeps them on
    // the same spot.
    (void)schematic::anchor_plan_on_content(out_plan, origin_x, origin_y, origin_z);
    return true;
}

Dictionary VoxelEngineController::inspect_schematic(const PackedByteArray& bytes,
                                                    const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    const schematic::SchematicData* file = nullptr;
    schematic::PastePlan plan;
    std::string error;
    // The origin does not matter for inspection (only for the coordinates the
    // plan carries), so the plan is built from (0, 0, 0).
    if (!decode_and_plan(bytes, options, 0, 0, 0, paste_options, file, plan, error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    result["ok"] = true;
    result["file_width"] = file->width;
    result["file_height"] = file->height;
    result["file_length"] = file->length;
    result["format"] = String(schematic::block_file_format_name(file->format));
    result["format_version"] = file->format_version;
    result["data_version"] = static_cast<int64_t>(file->data_version);
    result["container"] = String(schematic::container_kind_name(file->container));
    result["data_layout"] = String(schematic::data_layout_name(file->data_layout));
    if (file->has_offset) result["offset"] = Vector3i(file->offset[0], file->offset[1], file->offset[2]);
    result["palette_states"] = static_cast<int64_t>(file->palette.size());
    result["non_air_cells"] = static_cast<int64_t>(file->non_air_cells);
    result["tile_entities"] = static_cast<int64_t>(file->tile_entity_count);
    result["entities"] = static_cast<int64_t>(file->entity_count);
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    if (!plan.empty()) {
        // Local to the file, since inspection has no anchor.
        result["min"] = Vector3i(plan.min_x, plan.min_y, plan.min_z);
        result["max"] = Vector3i(plan.max_x, plan.max_y, plan.max_z);
    }
    return result;
}

Dictionary VoxelEngineController::preview_schematic(const PackedByteArray& bytes, int32_t origin_x,
                                                    int32_t origin_y, int32_t origin_z,
                                                    const Dictionary& options) {
    Dictionary result;
    result["ok"] = false;

    schematic::PasteOptions paste_options;
    const schematic::SchematicData* file = nullptr;
    schematic::PastePlan plan;
    std::string error;
    if (!decode_and_plan(bytes, options, origin_x, origin_y, origin_z, paste_options, file, plan,
                         error)) {
        result["error"] = String(error.c_str());
        return result;
    }

    // How many cells the caller wants to draw, and how many it gets. The cap is
    // generous rather than tight: one MultiMesh can hold a few hundred thousand
    // cube instances, and a preview that silently stops at some number would be
    // worse than one that draws a strided ghost.
    int64_t wanted = 20000;
    if (options.has("preview_cells")) {
        wanted = static_cast<int64_t>(options.get("preview_cells", 20000));
    }
    if (wanted < 0) wanted = 0;
    if (wanted > 400000) wanted = 400000;
    const size_t cap = static_cast<size_t>(wanted);

    const size_t total = plan.cells.size();
    const size_t stride = (cap == 0 || total <= cap) ? 1 : (total + cap - 1) / cap;
    size_t returned = 0;
    for (size_t i = 0; i < total; i += stride) ++returned;

    PackedByteArray cells;
    // The same cells again, as the instance buffer a MultiMesh wants: one unit
    // transform per cell with the origin at the cell's CENTRE. Built here rather
    // than in GDScript because a script loop over a hundred thousand cells is a
    // frame hitch at every re-aim, and one `buffer` assignment is a single upload.
    //
    // The packing lives in one place (render/multimesh_instance_layout.hpp) because
    // getting the float order wrong is silent, and that file carries the measured
    // evidence for it. `verify_multimesh_instance_layout` holds it against the engine
    // at startup.
    PackedFloat32Array transforms;
    if (returned > 0) {
        cells.resize(static_cast<int32_t>(returned * 4 * sizeof(int32_t)));
        transforms.resize(static_cast<int32_t>(returned * 12));
        uint8_t* out = cells.ptrw();
        float* xform = transforms.ptrw();
        size_t at = 0;
        int32_t slot = 0;
        for (size_t i = 0; i < total; i += stride) {
            const schematic::PastePlan::Cell& cell = plan.cells[i];
            const int32_t values[4] = {cell.x, cell.y, cell.z, static_cast<int32_t>(cell.block)};
            std::memcpy(out + at, values, sizeof(values));
            at += sizeof(values);
            render::pack_unit_instance_transform(xform + slot, static_cast<float>(cell.x) + 0.5f,
                                                static_cast<float>(cell.y) + 0.5f,
                                                static_cast<float>(cell.z) + 0.5f);
            slot += 12;
        }
    }

    result["ok"] = true;
    result["format"] = String(schematic::block_file_format_name(file->format));
    result["file_width"] = file->width;
    result["file_height"] = file->height;
    result["file_length"] = file->length;
    const Dictionary counters = plan_counters(plan);
    for (const Variant& key : counters.keys()) result[key] = counters[key];
    result["cells"] = cells;
    result["transforms"] = transforms;
    result["cells_returned"] = static_cast<int64_t>(returned);
    result["cells_sampled"] = stride > 1;
    if (!plan.empty()) {
        // World coordinates of the whole volume, so a preview can box it.
        result["min"] = Vector3i(plan.min_x, plan.min_y, plan.min_z);
        result["max"] = Vector3i(plan.max_x, plan.max_y, plan.max_z);
    }
    return result;
}

} // namespace VoxelEngine
