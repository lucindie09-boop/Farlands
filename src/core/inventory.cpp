#include "inventory.hpp"
#include "core/crc32.hpp"
#include "core/block_types.hpp"
#include "core/item_registry.hpp"
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
    
    // Then try existing stacks in main inventory (moved up, ahead of empty slots)
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
    
    // Only now fall back to empty slots: hotbar first, then main inventory
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
        remaining -= consume_from_slots(hotbar_, block_id, remaining);
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

static void put_inventory_body(std::vector<uint8_t>& out, const Inventory& inventory) {
    auto put32 = [&out](uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };

    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        const InventorySlot& slot = inventory.get_hotbar_slot(i);
        put32(static_cast<uint32_t>(slot.block_id));
        put32(static_cast<uint32_t>(slot.count));
    }
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        const InventorySlot& slot = inventory.get_inventory_slot(i);
        put32(static_cast<uint32_t>(slot.block_id));
        put32(static_cast<uint32_t>(slot.count));
    }

    put32(static_cast<uint32_t>(inventory.get_selected_slot()));
}

void serialize_inventory(const Inventory& inventory, std::vector<uint8_t>& out) {
    auto put32 = [&out](uint32_t v) {
        out.push_back(v & 0xFF);
        out.push_back((v >> 8) & 0xFF);
        out.push_back((v >> 16) & 0xFF);
        out.push_back((v >> 24) & 0xFF);
    };

    out.clear();
    // Header (magic + version) + body + trailing CRC32 over everything before it.
    out.reserve(8 + (Inventory::HOTBAR_SIZE + Inventory::INVENTORY_SIZE + 1) * 4 + 4);

    put32(INVENTORY_MAGIC);
    put32(INVENTORY_VERSION);
    put_inventory_body(out, inventory);

    put32(crc32(out.data(), out.size()));
}

bool deserialize_inventory(const uint8_t* data, size_t size, Inventory& out_inventory) {
    auto get32 = [](const uint8_t* d, size_t offset) -> uint32_t {
        return static_cast<uint32_t>(d[offset])
             | (static_cast<uint32_t>(d[offset + 1]) << 8)
             | (static_cast<uint32_t>(d[offset + 2]) << 16)
             | (static_cast<uint32_t>(d[offset + 3]) << 24);
    };

    // v1 has no trailer; v2 adds the 4-byte CRC32. Each slot is TWO u32s (id
    // and count), so the body is 2×4 bytes per slot plus the one selection u32.
    constexpr size_t body_size =
        (Inventory::HOTBAR_SIZE + Inventory::INVENTORY_SIZE) * 2 * 4 + 4;
    constexpr size_t v1_size = 8 + body_size;
    constexpr size_t v2_size = v1_size + 4;
    if (data == nullptr || size < v1_size) return false;
    if (get32(data, 0) != INVENTORY_MAGIC) return false;

    const uint32_t version = get32(data, 4);
    if (version == INVENTORY_VERSION) {
        if (size < v2_size) return false;
        const uint32_t stored_crc = get32(data, v1_size);
        const uint32_t actual_crc = crc32(data, v1_size);
        if (stored_crc != actual_crc) return false;
    } else if (version != INVENTORY_VERSION_V1) {
        // A version from a build newer than this one: refused, like an edit map
        // from the future, rather than read as whatever its bytes happen to mean.
        return false;
    }

    // The registries are the range check, and BOTH of them: an id is valid if it
    // names a block this session registered OR an item it registered. Items are
    // not an edge case here - the torch, the buckets, the ingot, the hammers and
    // the wand all live in hotbar slots, in the id space above FIRST_ITEM_ID, and
    // the loader in player_controller has always accepted them. Checking against
    // the block count alone would refuse the entire file for a player carrying
    // one torch, which is the ordinary save, not a corrupt one.
    //
    // An id outside both is a save from a different build, and the whole file is
    // refused (the edit-map decoder set the precedent) rather than handing an
    // out-of-range id to the hotbar. Counts outside 0..64 become an empty slot,
    // the same choice: a corrupt count has no honest reading, and inventing a
    // full stack of something is worse than dropping it.
    const BlockRegistry& blocks = BlockRegistry::get_instance();
    const ItemRegistry& items = ItemRegistry::get_instance();
    const size_t registered_count = blocks.get_count();
    auto is_known_id = [&](uint32_t id) {
        return id < registered_count || items.is_item(static_cast<BlockID>(id));
    };
    auto sanitize_count = [](uint32_t raw) -> int {
        return raw > 64 ? 0 : static_cast<int>(raw);
    };

    out_inventory.clear();
    size_t pos = 8;
    for (int i = 0; i < Inventory::HOTBAR_SIZE; i++) {
        const uint32_t id = get32(data, pos);
        if (!is_known_id(id)) return false;
        out_inventory.set_hotbar_slot(i, static_cast<BlockID>(id), sanitize_count(get32(data, pos + 4)));
        pos += 8;
    }
    for (int i = 0; i < Inventory::INVENTORY_SIZE; i++) {
        const uint32_t id = get32(data, pos);
        if (!is_known_id(id)) return false;
        out_inventory.set_inventory_slot(i, static_cast<BlockID>(id), sanitize_count(get32(data, pos + 4)));
        pos += 8;
    }
    const uint32_t selection = get32(data, pos);
    if (selection >= Inventory::HOTBAR_SIZE) {
        // Out of range here means the file is not something this build wrote;
        // silently keeping the default selection would report success for a
        // half-applied load. clear() already ran, so returning false leaves
        // the caller an empty inventory — the same shape the edit-map
        // recovery path leaves.
        return false;
    }
    out_inventory.select_slot(static_cast<int>(selection));
    return true;
}

} // namespace VoxelEngine


