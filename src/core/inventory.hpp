#ifndef FUK_MINECRAFT_INVENTORY_HPP
#define FUK_MINECRAFT_INVENTORY_HPP

#include "core/block_types.hpp"
#include <array>
#include <cstdint>

namespace VoxelEngine {

struct InventorySlot {
    BlockID block_id = 0;
    int count = 0;
    
    bool is_empty() const { return block_id == 0 || count <= 0; }
};

class Inventory {
public:
    static constexpr int HOTBAR_SIZE = 9;
    static constexpr int INVENTORY_SIZE = 27; // 3 rows of 9 slots
    
    Inventory();
    
    // Add blocks to inventory (prefer hotbar, then main inventory)
    bool add_block(BlockID block_id, int count = 1);
    
    // Consume blocks from inventory (prefer selected slot, then hotbar, then main inventory)
    bool consume_block(BlockID block_id, int count = 1);
    
    // Get total count of a block type across all slots
    int get_total_count(BlockID block_id) const;
    
    // Hotbar operations
    BlockID get_selected_block() const;
    int get_selected_count() const;
    void select_slot(int slot);
    int get_selected_slot() const { return selected_hotbar_slot_; }
    const InventorySlot& get_hotbar_slot(int slot) const;
    void set_hotbar_slot(int slot, BlockID block_id, int count);
    
    // Inventory operations
    const InventorySlot& get_inventory_slot(int slot) const;
    
    // Clear all slots
    void clear();
    
private:
    std::array<InventorySlot, HOTBAR_SIZE> hotbar_;
    std::array<InventorySlot, INVENTORY_SIZE> inventory_;
    int selected_hotbar_slot_ = 0;
    
    // Helper: try to consume from a specific slot array
    int consume_from_slots(std::array<InventorySlot, HOTBAR_SIZE>& slots, BlockID block_id, int count);
    int consume_from_slots(std::array<InventorySlot, INVENTORY_SIZE>& slots, BlockID block_id, int count);
};

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_INVENTORY_HPP
