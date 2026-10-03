// The inventory side of the node: the hotbar and inventory accessors, crafting
// (match/craft against a grid), texture pack switching, the menu-open flags the
// UI drives, and the inventory save/load pair. Kept apart from
// godot_bindings/player_controller.cpp, which never touches the inventory except
// through these.

#include "godot_bindings/player_controller.hpp"

#include "godot_bindings/chunk_manager.hpp"
#include "core/item_registry.hpp"
#include "engine/voxel_engine_controller.hpp"
#include "render/texture_array_generator.hpp"
#include "render/texture_pack_manager.hpp"

#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/input.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

using namespace godot;
using namespace VoxelEngine;
int PlayerController::get_selected_block() const { return inventory_.get_selected_block(); }
int PlayerController::get_block_edit_counter() const { return block_edit_counter_; }
void PlayerController::set_selected_block(int block_id) { 
    // Legacy method - set the selected hotbar slot to this block type
    // This is for backwards compatibility, but does NOT grant free blocks
    for (int i = 0; i < VoxelEngine::Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory_.get_hotbar_slot(i);
        if (slot.block_id == block_id) {
            inventory_.select_slot(i);
            return;
        }
    }
    // If not found in hotbar, select the first empty slot (without granting blocks)
    for (int i = 0; i < VoxelEngine::Inventory::HOTBAR_SIZE; i++) {
        const auto& slot = inventory_.get_hotbar_slot(i);
        if (slot.count == 0) {
            inventory_.select_slot(i);
            break;
        }
    }
}

// Inventory API
int PlayerController::get_hotbar_slot_count(int slot) const {
    return inventory_.get_hotbar_slot(slot).count;
}

int PlayerController::get_hotbar_slot_block_id(int slot) const {
    return inventory_.get_hotbar_slot(slot).block_id;
}

int PlayerController::get_selected_hotbar_slot() const {
    return inventory_.get_selected_slot();
}

void PlayerController::select_hotbar_slot(int slot) {
    inventory_.select_slot(slot);
}

void PlayerController::set_hotbar_slot(int slot, int block_id, int count) {
    inventory_.set_hotbar_slot(slot, block_id, count);
}

bool PlayerController::can_add_block(int block_id, int count) const {
    if (block_id <= 0 || count <= 0) return false;
    return inventory_.can_add_block(static_cast<BlockID>(block_id), count);
}

int PlayerController::get_inventory_slot_count(int slot) const {
    return inventory_.get_inventory_slot(slot).count;
}

int PlayerController::get_inventory_slot_block_id(int slot) const {
    return inventory_.get_inventory_slot(slot).block_id;
}

void PlayerController::set_inventory_slot(int slot, int block_id, int count) {
    inventory_.set_inventory_slot(slot, block_id, count);
}

bool PlayerController::give_block(int block_id, int count) {
    if (block_id <= 0 || count <= 0) return false;
    return inventory_.add_block(block_id, count);
}

void PlayerController::clear_inventory() {
    inventory_.clear();
}

// Crafting API
Dictionary PlayerController::match_recipe(const PackedInt32Array& grid_ids,
                                          const PackedInt32Array& grid_counts) {
    Dictionary out;
    out["ok"] = false;
    const int cell_count = grid_ids.size();
    if (!chunk_manager_ || cell_count != grid_counts.size()) {
        return out;
    }
    int dim;
    switch (cell_count) {
        case 4:  dim = 2; break;   // inventory menu grid
        case 9:  dim = 3; break;   // crafting table grid
        default: return out;
    }
    VoxelEngineController* controller = chunk_manager_->get_controller();
    if (!controller) {
        return out;
    }
    BlockID cells[9];
    int counts[9];
    for (int i = 0; i < cell_count; ++i) {
        const int c = grid_counts[i] > 0 ? grid_counts[i] : 0;
        const int v = grid_ids[i];
        // A cell drained to 0 by a previous craft can keep its stale id on
        // the GUI side; treat it as empty so matching never sees phantom
        // ingredients. Ids may address blocks or items (>=1024 id space).
        cells[i] = (v > 0 && v < VoxelEngine::ItemRegistry::FIRST_ITEM_ID + 256 && c > 0)
                       ? static_cast<BlockID>(v) : BlockIDs::AIR;
        counts[i] = c;
    }
    const CraftingRecipe* recipe = controller->get_recipe_book().match(cells, dim);
    if (!recipe) {
        return out;
    }
    // Gate the preview on availability: ids alone would keep showing a result
    // after craft_recipe consumed the last ingredient (the ghost-icon bug).
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int available = 0;
        for (int i = 0; i < cell_count; ++i) {
            if (cells[i] == need.block_id) available += counts[i];
        }
        if (available < need.count) {
            return out;
        }
    }
    out["ok"] = true;
    out["block_id"] = static_cast<int>(recipe->result.block_id);
    out["count"] = recipe->result.count;
    return out;
}

Dictionary PlayerController::craft_recipe(const PackedInt32Array& grid_ids,
                                          const PackedInt32Array& grid_counts) {
    Dictionary out;
    out["ok"] = false;
    const int cell_count = grid_ids.size();
    if (!chunk_manager_ || cell_count != grid_counts.size()) {
        return out;
    }
    int dim;
    switch (cell_count) {
        case 4:  dim = 2; break;
        case 9:  dim = 3; break;
        default: return out;
    }
    VoxelEngineController* controller = chunk_manager_->get_controller();
    if (!controller) {
        return out;
    }
    BlockID cells[9];
    int counts[9];
    for (int i = 0; i < cell_count; ++i) {
        const int c = grid_counts[i] > 0 ? grid_counts[i] : 0;
        const int v = grid_ids[i];
        // Same stale-drained-cell guard as match_recipe; ids may address
        // blocks or items (>=1024 id space).
        cells[i] = (v > 0 && v < VoxelEngine::ItemRegistry::FIRST_ITEM_ID + 256 && c > 0)
                       ? static_cast<BlockID>(v) : BlockIDs::AIR;
        counts[i] = c;
    }
    const CraftingRecipe* recipe = controller->get_recipe_book().match(cells, dim);
    if (!recipe) {
        return out;
    }

    // Verify every ingredient total is coverable by the grid before touching
    // any cell, then deduct greedily per total (totals have unique ids, so the
    // deductions never interact).
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int available = 0;
        for (int i = 0; i < cell_count; ++i) {
            if (cells[i] == need.block_id) available += counts[i];
        }
        if (available < need.count) {
            return out;
        }
    }
    PackedInt32Array new_counts = grid_counts;
    for (const InventorySlot& need : recipe->ingredient_totals) {
        int remaining = need.count;
        for (int i = 0; i < cell_count && remaining > 0; ++i) {
            if (cells[i] != need.block_id) continue;
            const int take = counts[i] < remaining ? counts[i] : remaining;
            counts[i] -= take;
            remaining -= take;
            new_counts[i] = counts[i];
        }
    }
    out["ok"] = true;
    out["block_id"] = static_cast<int>(recipe->result.block_id);
    out["count"] = recipe->result.count;
    out["new_counts"] = new_counts;
    return out;
}

bool PlayerController::set_active_texture_pack(const String& pack_name) {
    TexturePackManager& tpm = TexturePackManager::get_instance();
    if (pack_name.is_empty()) {
        tpm.clear_active_pack();
    } else if (!tpm.set_active_pack(pack_name)) {
        return false;  // unknown pack name
    }

    // Re-resolve every texture name through the new pack, rebuild the albedo
    // and emissive arrays, and push them into the shared chunk materials.
    // Layer indices are keyed by name (not pack), so existing meshes remain
    // valid — only the pixels change.
    TextureArrayGenerator::get_instance().invalidate_texture_path_cache();
    TextureArrayGenerator::get_instance().force_regenerate();
    if (chunk_manager_) {
        if (VoxelEngineController* controller = chunk_manager_->get_controller()) {
            controller->get_environment_controller().get_material_manager().reload_textures();
        }
    }
    return true;
}

godot::PackedStringArray PlayerController::get_installed_pack_names() const {
    godot::PackedStringArray names;
    for (const VoxelEngine::TexturePack& pack : TexturePackManager::get_instance().packs()) {
        names.push_back(godot::String(pack.name.c_str()));
    }
    return names;
}

godot::String PlayerController::resolve_texture_path(const String& texture_name) const {
    return TexturePackManager::get_instance().resolve(texture_name);
}

void PlayerController::set_inventory_open(bool open) {
    inventory_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_inventory_open() const {
    return inventory_open_;
}

void PlayerController::set_table_menu_open(bool open) {
    table_menu_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_table_menu_open() const {
    return table_menu_open_;
}

void PlayerController::set_chat_open(bool open) {
    chat_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_chat_open() const {
    return chat_open_;
}

void PlayerController::set_wand_menu_open(bool open) {
    wand_menu_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_wand_menu_open() const {
    return wand_menu_open_;
}

void PlayerController::set_settings_open(bool open) {
    settings_open_ = open;
    update_mouse_mode();
}

bool PlayerController::is_settings_open() const {
    return settings_open_;
}

void PlayerController::save_inventory() {
    if (!chunk_manager_) return;
    auto* controller = chunk_manager_->get_controller();
    if (!controller) return;
    controller->save_inventory(inventory_);
}

bool PlayerController::load_inventory() {
    if (!chunk_manager_) return false;
    auto* controller = chunk_manager_->get_controller();
    if (!controller) return false;
    return controller->load_inventory(inventory_);
}
