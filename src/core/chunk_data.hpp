#ifndef FARLANDS_CHUNK_DATA_HPP
#define FARLANDS_CHUNK_DATA_HPP
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/light_packing.hpp"
#include "core/palette_storage.hpp"
#include <cstdint>
#include <memory>
#include <cstring>
#include <algorithm>
#include <vector>
#include <cstddef>

namespace VoxelEngine {

class ChunkData;
void propagate_chunk_block_light_additive(ChunkData& chunk);

// One cell of a sky-light column rescan whose level changed. The rescan can only
// say what the column looks like NOW; the edit path needs the old level to
// un-propagate the light the cell used to hand out, because the sky walk only
// ever raises a level and cannot lower one on its own.
struct SkyColumnChange {
    int16_t y = 0;
    uint8_t old_level = 0;
    uint8_t new_level = 0;
};

// -------------------------------------------------------------------------
// Chunk Data
// -------------------------------------------------------------------------
class ChunkData {
private:
    std::unique_ptr<PaletteStorage> storage;

    bool     is_empty = true;
    bool     is_fully_solid = false;
    uint32_t block_count = 0;
    uint32_t emissive_count = 0;
    // Cells the liquid surface pass has to draw (see BlockType::draws_fluid_surface).
    // Maintained beside block_count on every write path, so "does this chunk hold
    // liquid" is an O(1) question: the renderer asks it to check that a chunk with
    // liquid in its data actually has liquid geometry on the GPU.
    uint32_t liquid_cells = 0;
    uint32_t section_block_count[CHUNK_SECTIONS];
    // Whether any cell of this chunk ended up shaded by its own column scan: a
    // cell that is neither opaque nor liquid yet holds less than full sky light.
    // Only ever SET by the scans (cleared by the wipes), never cleared by a
    // partial one, so a stale true costs a walk that finds nothing and a stale
    // false cannot exist for a chunk whose light came from a scan. The install
    // pass reads it to skip its own sweep, which is what keeps a plain or an ocean
    // from paying for this feature at all.
    bool has_sky_shade_ = false;

    [[nodiscard]] static inline bool is_emissive_block(BlockID id) noexcept {
        return HasProperty(BlockRegistry::get_instance().get_block_fast(id).properties, BlockProperty::Emissive);
    }

    [[nodiscard]] static inline bool is_liquid_surface_block(BlockID id) noexcept {
        return BlockRegistry::get_instance().get_block_fast(id).draws_fluid_surface();
    }

public:
    ChunkData();
    ChunkData(const ChunkData& other);
    ChunkData& operator=(const ChunkData& other);
    ChunkData(ChunkData&& other) noexcept;
    ChunkData& operator=(ChunkData&& other) noexcept;

    // Bulk Clear
    // Returns whether there was any block light to clear, so a caller that relights
    // a region can tell whether that region's light changed at all.
    [[nodiscard]] bool clear_block_light() noexcept;
    void clear_sky_light() noexcept;

    // Exact byte image of the light sections (palette and indices included), so a
    // caller can tell whether a relight pass changed anything at all. Equal images
    // mean identical light values, so a mesh built from one matches the other and
    // needs no rebuild. A pristine chunk costs a few dozen bytes here, because its
    // sections are uniform.
    void append_light_state(std::vector<uint8_t>& out) const;
    [[nodiscard]] bool light_state_equals(const std::vector<uint8_t>& prior) const;
    void clear_light() noexcept;
    void clear() noexcept;

    // Memory footprint (bytes of palette + index storage)
    [[nodiscard]] size_t memory_usage() const {
        if (!storage) return 0;
        return sizeof(ChunkData) + sizeof(PaletteStorage) + storage->memory_usage();
    }

    // Block Access
    [[nodiscard]] BlockID get_block(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH)
            return BlockIDs::AIR;
        return storage->get_block(x, y, z);
    }

    [[nodiscard]] BlockID get_block(const BlockPos& pos) const noexcept { return get_block(pos.x, pos.y, pos.z); }

    void set_block(int32_t x, int32_t y, int32_t z, BlockID block_id) noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return;
        const BlockID old = storage->get_block(x, y, z);
        if (old == block_id) return;
        storage->set_block(x, y, z, block_id);
        if (is_emissive_block(old) && emissive_count > 0) --emissive_count;
        if (is_emissive_block(block_id)) ++emissive_count;
        if (is_liquid_surface_block(old) && liquid_cells > 0) --liquid_cells;
        if (is_liquid_surface_block(block_id)) ++liquid_cells;
        if (old == BlockIDs::AIR && block_id != BlockIDs::AIR) ++block_count;
        else if (old != BlockIDs::AIR && block_id == BlockIDs::AIR) --block_count;
        is_empty = (block_count == 0);
        const int s = y / SECTION_HEIGHT;
        if (old == BlockIDs::AIR && block_id != BlockIDs::AIR) ++section_block_count[s];
        else if (old != BlockIDs::AIR && block_id == BlockIDs::AIR) { if (section_block_count[s] > 0) --section_block_count[s]; }
        if (is_fully_solid) {
            if (block_id == BlockIDs::AIR) is_fully_solid = false;
            else {
                const auto& bt = BlockRegistry::get_instance().get_block(block_id);
                if (!HasProperty(bt.properties, BlockProperty::Solid) || !HasProperty(bt.properties, BlockProperty::Opaque))
                    is_fully_solid = false;
            }
        }
    }

    void set_block(const BlockPos& pos, BlockID block_id) noexcept { set_block(pos.x, pos.y, pos.z, block_id); }

    [[nodiscard]] BlockID get_block_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return storage->get_block(x, y, z);
    }

    void fill_blocks(BlockID block_id) noexcept;

    [[nodiscard]] bool is_all_air() const noexcept { return is_empty; }
    [[nodiscard]] bool fully_solid() const noexcept { return is_fully_solid; }
    [[nodiscard]] uint32_t get_block_count() const noexcept { return block_count; }
    [[nodiscard]] uint32_t get_emissive_count() const noexcept { return emissive_count; }
    [[nodiscard]] uint32_t liquid_count() const noexcept { return liquid_cells; }

    // Light Access (all go through paletted light sections)

    [[nodiscard]] uint8_t get_light(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return 0;
        uint16_t v = storage->get_light_word(x, y, z);
        uint8_t r = unpack_r(v), g = unpack_g(v), b = unpack_b(v);
        return r > g ? (r > b ? r : b) : (g > b ? g : b);
    }

    void set_light(int32_t x, int32_t y, int32_t z, uint8_t level) noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return;
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(unpack_sky(v), level, level, level));
    }

    [[nodiscard]] uint8_t get_sky_light(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return 0;
        return unpack_sky(storage->get_light_word(x, y, z));
    }

    void set_sky_light(int32_t x, int32_t y, int32_t z, uint8_t level) noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return;
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(level, unpack_r(v), unpack_g(v), unpack_b(v)));
    }

    [[nodiscard]] uint8_t get_light_r(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return 0;
        return unpack_r(storage->get_light_word(x, y, z));
    }

    [[nodiscard]] uint8_t get_light_g(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return 0;
        return unpack_g(storage->get_light_word(x, y, z));
    }

    [[nodiscard]] uint8_t get_light_b(int32_t x, int32_t y, int32_t z) const noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return 0;
        return unpack_b(storage->get_light_word(x, y, z));
    }

    void set_light_rgb(int32_t x, int32_t y, int32_t z, uint8_t r, uint8_t g, uint8_t b) noexcept {
        if (x < 0 || x >= CHUNK_WIDTH || y < 0 || y >= CHUNK_HEIGHT || z < 0 || z >= CHUNK_DEPTH) return;
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(unpack_sky(v), r, g, b));
    }

    // Unchecked light accessors (hot paths)

    [[nodiscard]] inline uint8_t get_light_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        uint16_t v = storage->get_light_word(x, y, z);
        uint8_t r = unpack_r(v), g = unpack_g(v), b = unpack_b(v);
        return r > g ? (r > b ? r : b) : (g > b ? g : b);
    }

    inline void set_light_unsafe(int32_t x, int32_t y, int32_t z, uint8_t level) noexcept {
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(unpack_sky(v), level, level, level));
    }

    [[nodiscard]] inline uint8_t get_sky_light_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return unpack_sky(storage->get_light_word(x, y, z));
    }

    inline void set_sky_light_unsafe(int32_t x, int32_t y, int32_t z, uint8_t level) noexcept {
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(level, unpack_r(v), unpack_g(v), unpack_b(v)));
    }

    [[nodiscard]] inline uint8_t get_light_r_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return unpack_r(storage->get_light_word(x, y, z));
    }

    [[nodiscard]] inline uint8_t get_light_g_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return unpack_g(storage->get_light_word(x, y, z));
    }

    [[nodiscard]] inline uint8_t get_light_b_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return unpack_b(storage->get_light_word(x, y, z));
    }

    [[nodiscard]] inline uint16_t get_light_packed_word_unsafe(int32_t x, int32_t y, int32_t z) const noexcept {
        return storage->get_light_word(x, y, z);
    }

    inline void set_light_rgb_unsafe(int32_t x, int32_t y, int32_t z, uint8_t r, uint8_t g, uint8_t b) noexcept {
        uint16_t v = storage->get_light_word(x, y, z);
        storage->set_light_word(x, y, z, pack_light(unpack_sky(v), r, g, b));
    }

    // Light Propagation
    void propagate_light() {
        (void)clear_block_light();  // the result matters only to a caller relighting a region
        if (is_empty || emissive_count == 0) return;
        propagate_chunk_block_light_additive(*this);
    }

    void propagate_sky_light(const ChunkData* chunk_above = nullptr);
    // When `changes` is given it receives one entry per cell of this column whose
    // level moved, with the value it held before the rescan overwrote it.
    void propagate_sky_light_column(int32_t x, int32_t z, const ChunkData* chunk_above = nullptr,
                                    std::vector<SkyColumnChange>* changes = nullptr);

    [[nodiscard]] bool has_sky_shade() const noexcept { return has_sky_shade_; }

    // Bulk load from dense array
    void set_data(const BlockID* data, uint32_t count);

    // Section Queries
    void compute_section_flags();
    void compute_fully_solid();

    [[nodiscard]] bool is_section_all_air(int32_t si) const noexcept {
        if (si < 0 || si >= CHUNK_SECTIONS) return false;
        return section_block_count[si] == 0;
    }

    template<typename Func>
    [[nodiscard]] bool iterate_blocks(Func&& callback) const {
        if (is_empty) return true;
        for (int si = 0; si < PaletteStorage::NUM_SECTIONS; ++si)
            storage->for_each_block(si, [&](int x, int y, int z, BlockID id) {
                return callback(BlockPos{x, y, z}, id);
            });
        return true;
    }

    [[nodiscard]] BlockID get_neighbor_block(int32_t x, int32_t y, int32_t z, int32_t dx, int32_t dy, int32_t dz) const noexcept {
        int32_t nx = x + dx, ny = y + dy, nz = z + dz;
        if (nx < 0 || nx >= CHUNK_WIDTH || ny < 0 || ny >= CHUNK_HEIGHT || nz < 0 || nz >= CHUNK_DEPTH) return BlockIDs::AIR;
        return get_block_unsafe(nx, ny, nz);
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_CHUNK_DATA_HPP
