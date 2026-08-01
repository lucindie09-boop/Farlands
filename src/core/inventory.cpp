#include "inventory.hpp"
#include <algorithm>

namespace VoxelEngine {

Inventory::Inventory() {
    // Initialize all slots as empty
    for (auto& slot : hotbar_) {
        slot = InventorySlot{};
    }
    for (auto& slot : inventory_) {
        slot = InventorySlot{};
    }
}

bool Inventory::add_block(BlockID block_id, int count) {
    if (block_id == 0 || count <= 0) return false;
    
    int remaining = count;
    
    // First try to add to existing stacks in hotbar
    for (auto& slot : hotbar_) {
        if (slot.block_id == block_id && slot.count < 64) {
            int can_add = 64 - slot.count;
            int to_add = std::min(can_add, remaining);
            slot.count += to_add;
            remaining -= to_add;
            if (remaining <= 0) return true;
        }
    }
    
    // Try empty slots in hotbar
    if (remaining > 0) {
        for (auto& slot : hotbar_) {
            if (slot.is_empty()) {
                slot.block_id = block_id;
                int to_add = std::min(64, remaining);
                slot.count = to_add;
                remaining -= to_add;
                if (remaining <= 0) return true;
            }
        }
    }
    
    // Try existing stacks in main inventory
    if (remaining > 0) {
        for (auto& slot : inventory_) {
            if (slot.block_id == block_id && slot.count < 64) {
                int can_add = 64 - slot.count;
                int to_add = std::min(can_add, remaining);
                slot.count += to_add;
                remaining -= to_add;
                if (remaining <= 0) return true;
            }
        }
    }
    
    // Try empty slots in main inventory
    if (remaining > 0) {
        for (auto& slot : inventory_) {
            if (slot.is_empty()) {
                slot.block_id = block_id;
                int to_add = std::min(64, remaining);
                slot.count = to_add;
                remaining -= to_add;
                if (remaining <= 0) return true;
            }
        }
    }
    
    return remaining == 0; // Return true if all blocks were added
}

bool Inventory::can_add_block(BlockID block_id, int count) const {
    if (block_id == 0 || count <= 0) return false;
    
    int needed = count;
    
    // First check existing stacks in hotbar
    for (const auto& slot : hotbar_) {
        if (slot.block_id == block_id && slot.count < 64) {
            int can_add = 64 - slot.count;
            needed -= can_add;
            if (needed <= 0) return true;
        }
    }
    
    // Check empty slots in hotbar
    if (needed > 0) {
        for (const auto& slot : hotbar_) {
            if (slot.is_empty()) {
                needed -= 64;
                if (needed <= 0) return true;
            }
        }
    }
    
    // Check existing stacks in main inventory
    if (needed > 0) {
        for (const auto& slot : inventory_) {
            if (slot.block_id == block_id && slot.count < 64) {
                int can_add = 64 - slot.count;
                needed -= can_add;
                if (needed <= 0) return true;
            }
        }
    }
    
    // Check empty slots in main inventory
    if (needed > 0) {
        for (const auto& slot : inventory_) {
            if (slot.is_empty()) {
                needed -= 64;
                if (needed <= 0) return true;
            }
        }
    }
    
    return needed <= 0; // Return true if all blocks could be added
}

bool Inventory::consume_block(BlockID block_id, int count) {
    if (block_id == 0 || count <= 0) return false;
    
    int total_available = get_total_count(block_id);
    if (total_available < count) return false;
    
    int remaining = count;
    
    // First try to consume from selected slot
    if (hotbar_[selected_hotbar_slot_].block_id == block_id) {
        int can_take = std::min(hotbar_[selected_hotbar_slot_].count, remaining);
        hotbar_[selected_hotbar_slot_].count -= can_take;
        if (hotbar_[selected_hotbar_slot_].count <= 0) {
            hotbar_[selected_hotbar_slot_] = InventorySlot{};
        }
        remaining -= can_take;
    }
    
    if (remaining > 0) {
        consume_from_slots(hotbar_, block_id, remaining);
    }
    
    if (remaining > 0) {
        consume_from_slots(inventory_, block_id, remaining);
    }
    
    return true;
}

int Inventory::consume_from_slots(std::array<InventorySlot, HOTBAR_SIZE>& slots, BlockID block_id, int count) {
    int consumed = 0;
    
    for (auto& slot : slots) {
        if (slot.block_id == block_id && slot.count > 0) {
            int can_take = std::min(slot.count, count - consumed);
            slot.count -= can_take;
            if (slot.count <= 0) {
                slot = InventorySlot{};
            }
            consumed += can_take;
            if (consumed >= count) break;
        }
    }
    
    return consumed;
}

int Inventory::consume_from_slots(std::array<InventorySlot, INVENTORY_SIZE>& slots, BlockID block_id, int count) {
    int consumed = 0;
    
    for (auto& slot : slots) {
        if (slot.block_id == block_id && slot.count > 0) {
            int can_take = std::min(slot.count, count - consumed);
            slot.count -= can_take;
            if (slot.count <= 0) {
                slot = InventorySlot{};
            }
            consumed += can_take;
            if (consumed >= count) break;
        }
    }
    
    return consumed;
}

int Inventory::get_total_count(BlockID block_id) const {
    int total = 0;
    
    for (const auto& slot : hotbar_) {
        if (slot.block_id == block_id) {
            total += slot.count;
        }
    }
    
    for (const auto& slot : inventory_) {
        if (slot.block_id == block_id) {
            total += slot.count;
        }
    }
    
    return total;
}

BlockID Inventory::get_selected_block() const {
    return hotbar_[selected_hotbar_slot_].block_id;
}

int Inventory::get_selected_count() const {
    return hotbar_[selected_hotbar_slot_].count;
}

void Inventory::select_slot(int slot) {
    if (slot >= 0 && slot < HOTBAR_SIZE) {
        selected_hotbar_slot_ = slot;
    }
}

const InventorySlot& Inventory::get_hotbar_slot(int slot) const {
    if (slot >= 0 && slot < HOTBAR_SIZE) {
        return hotbar_[slot];
    }
    static InventorySlot empty_slot;
    return empty_slot;
}

void Inventory::set_hotbar_slot(int slot, BlockID block_id, int count) {
    if (slot >= 0 && slot < HOTBAR_SIZE) {
        hotbar_[slot].block_id = block_id;
        hotbar_[slot].count = count;
    }
}

const InventorySlot& Inventory::get_inventory_slot(int slot) const {
    if (slot >= 0 && slot < INVENTORY_SIZE) {
        return inventory_[slot];
    }
    static InventorySlot empty_slot;
    return empty_slot;
}

void Inventory::set_inventory_slot(int slot, BlockID block_id, int count) {
    if (slot >= 0 && slot < INVENTORY_SIZE) {
        inventory_[slot].block_id = block_id;
        inventory_[slot].count = count;
    }
}

void Inventory::clear() {
    for (auto& slot : hotbar_) {
        slot = InventorySlot{};
    }
    for (auto& slot : inventory_) {
        slot = InventorySlot{};
    }
    selected_hotbar_slot_ = 0;
}

} // namespace VoxelEngine
