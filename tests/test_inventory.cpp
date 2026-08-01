#include "doctest.h"
#include "core/inventory.hpp"
#include "core/block_types.hpp"

using namespace VoxelEngine;

TEST_CASE("inventory basic operations") {
    BlockRegistry::get_instance().initialize_default_blocks();
    Inventory inv;
    
    SUBCASE("empty inventory has no blocks") {
        CHECK(inv.get_total_count(1) == 0);
        CHECK(inv.get_selected_block() == 0);
        CHECK(inv.get_selected_count() == 0);
    }
    
    SUBCASE("add_block adds blocks to inventory") {
        CHECK(inv.add_block(1, 10));
        CHECK(inv.get_total_count(1) == 10);
    }
    
    SUBCASE("add_block returns false when inventory is full") {
        // Fill all slots (36 slots * 64 = 2304 blocks max)
        for (int i = 0; i < 36; i++) {
            inv.add_block(i + 1, 64);
        }
        CHECK_FALSE(inv.add_block(1, 1));
    }
    
    SUBCASE("can_add_block checks space before adding") {
        CHECK(inv.can_add_block(1, 100));
        
        // Fill inventory
        for (int i = 0; i < 36; i++) {
            inv.add_block(i + 1, 64);
        }
        CHECK_FALSE(inv.can_add_block(1, 1));
    }
    
    SUBCASE("consume_block removes blocks from inventory") {
        inv.add_block(1, 10);
        CHECK(inv.consume_block(1, 5));
        CHECK(inv.get_total_count(1) == 5);
    }
    
    SUBCASE("consume_block returns false when not enough blocks") {
        inv.add_block(1, 5);
        CHECK_FALSE(inv.consume_block(1, 10));
    }
    
    SUBCASE("add_block prefers existing stacks") {
        inv.add_block(1, 10);
        inv.add_block(1, 20);
        CHECK(inv.get_total_count(1) == 30);
        
        // Should have added to existing stack, not new slot
        int filled_slots = 0;
        for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
            if (inv.get_hotbar_slot(i).count > 0) filled_slots++;
        }
        CHECK(filled_slots == 1);
    }
    
    SUBCASE("hotbar slot selection") {
        inv.set_hotbar_slot(0, 1, 10);
        inv.set_hotbar_slot(1, 2, 20);
        
        inv.select_slot(0);
        CHECK(inv.get_selected_block() == 1);
        CHECK(inv.get_selected_count() == 10);
        
        inv.select_slot(1);
        CHECK(inv.get_selected_block() == 2);
        CHECK(inv.get_selected_count() == 20);
    }
    
    SUBCASE("set_hotbar_slot overwrites existing content") {
        inv.set_hotbar_slot(0, 1, 10);
        inv.set_hotbar_slot(0, 2, 5);
        CHECK(inv.get_hotbar_slot(0).block_id == 2);
        CHECK(inv.get_hotbar_slot(0).count == 5);
    }
    
    SUBCASE("clear empties all slots") {
        inv.add_block(1, 10);
        inv.add_block(2, 20);
        inv.clear();
        CHECK(inv.get_total_count(1) == 0);
        CHECK(inv.get_total_count(2) == 0);
        CHECK(inv.get_selected_slot() == 0);
    }
    
    SUBCASE("stack limit of 64 per slot") {
        inv.add_block(1, 100);
        // Should use multiple slots, 64 + 36 = 100 total
        CHECK(inv.get_total_count(1) == 100);
        
        // Count how many slots are used
        int filled_slots = 0;
        for (int i = 0; i < Inventory::HOTBAR_SIZE + Inventory::INVENTORY_SIZE; i++) {
            if (i < Inventory::HOTBAR_SIZE) {
                if (inv.get_hotbar_slot(i).count > 0) filled_slots++;
            } else {
                if (inv.get_inventory_slot(i - Inventory::HOTBAR_SIZE).count > 0) filled_slots++;
            }
        }
        CHECK(filled_slots == 2); // Should use 2 slots
        
        // Verify first slot is maxed at 64
        CHECK(inv.get_hotbar_slot(0).count == 64);
    }
    
    SUBCASE("consume_block prefers selected slot") {
        inv.set_hotbar_slot(0, 1, 10);
        inv.set_hotbar_slot(1, 1, 20);
        inv.select_slot(0);
        
        inv.consume_block(1, 5);
        CHECK(inv.get_hotbar_slot(0).count == 5);
        CHECK(inv.get_hotbar_slot(1).count == 20); // Unchanged
    }
    
    SUBCASE("inventory slot operations") {
        inv.set_inventory_slot(0, 1, 30);
        CHECK(inv.get_inventory_slot(0).block_id == 1);
        CHECK(inv.get_inventory_slot(0).count == 30);
    }
}

TEST_CASE("inventory edge cases") {
    BlockRegistry::get_instance().initialize_default_blocks();
    Inventory inv;
    
    SUBCASE("add_block with zero block_id returns false") {
        CHECK_FALSE(inv.add_block(0, 10));
    }
    
    SUBCASE("add_block with zero count returns false") {
        CHECK_FALSE(inv.add_block(1, 0));
    }
    
    SUBCASE("add_block with negative count returns false") {
        CHECK_FALSE(inv.add_block(1, -5));
    }
    
    SUBCASE("consume_block with zero block_id returns false") {
        CHECK_FALSE(inv.consume_block(0, 1));
    }
    
    SUBCASE("select_slot with invalid index does nothing") {
        inv.select_slot(-1);
        CHECK(inv.get_selected_slot() == 0);
        
        inv.select_slot(100);
        CHECK(inv.get_selected_slot() == 0);
    }
}