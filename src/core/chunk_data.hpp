#ifndef FARLANDS_CHUNK_DATA_HPP
#define FARLANDS_CHUNK_DATA_HPP
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include "core/light_packing.hpp"
#include <cstdint>
#include <memory>
#include <cstring>
#include <algorithm>
#include <vector>

namespace VoxelEngine {

class ChunkData;
void propagate_chunk_block_light_additive(ChunkData& chunk);

// -------------------------------------------------------------------------
// Generic 16^3 paletted section — used for both blocks and light.
// Uniform (single value) costs just the palette entry.
// Non-uniform uses 4/8/16-bit indices into a value palette.
// -------------------------------------------------------------------------
struct PalSection {
    std::vector<uint16_t> palette;
    uint8_t bpi = 0; // 0=uniform, 4, 8, 16
    std::vector<uint8_t> indices;

    [[nodiscard]] uint16_t uniform_val() const { return palette.empty() ? 0 : palette[0]; }
    [[nodiscard]] bool is_uniform() const { return palette.size() <= 1; }
    [[nodiscard]] size_t memory_usage() const { return palette.capacity() * sizeof(uint16_t) + indices.capacity(); }
};

// -------------------------------------------------------------------------
// Palette storage: 8 × 16^3 sections for blocks, 8 for light.
// -------------------------------------------------------------------------
class PaletteStorage {
public:
    static constexpr int SEC_SIZE = 16;
    static constexpr int SECS_PER_DIM = CHUNK_WIDTH / SEC_SIZE; // 2
    static constexpr int NUM_SECTIONS = SECS_PER_DIM * SECS_PER_DIM * SECS_PER_DIM; // 8
    static constexpr int SEC_VOLUME = SEC_SIZE * SEC_SIZE * SEC_SIZE; // 4096

    PalSection block_secs[NUM_SECTIONS];
    PalSection light_secs[NUM_SECTIONS];

    PaletteStorage() {
        for (auto& s : block_secs) s.palette = {0}; // AIR
        for (auto& s : light_secs) s.palette = {0}; // light=0
    }

    // -- Coordinate helpers --

    static int sec_index(int x, int y, int z) {
        return (x / SEC_SIZE) + (y / SEC_SIZE) * SECS_PER_DIM + (z / SEC_SIZE) * SECS_PER_DIM * SECS_PER_DIM;
    }

    static int sec_local(int x, int y, int z) {
        return (x & (SEC_SIZE - 1)) + ((y & (SEC_SIZE - 1)) * SEC_SIZE) + ((z & (SEC_SIZE - 1)) * SEC_SIZE * SEC_SIZE);
    }

    // -- Generic section helpers --

    static uint16_t section_get(const PalSection& s, int local) {
        if (s.is_uniform()) return s.uniform_val();
        return s.palette[static_cast<size_t>(read_index(s.indices.data(), local, s.bpi))];
    }

    static void section_set(PalSection& s, int local, uint16_t val) {
        if (s.is_uniform()) {
            uint16_t cur = s.uniform_val();
            if (cur == val) return;
            // uniform -> 2-entry palette
            s.palette = {cur, val};
            s.bpi = 4;
            s.indices.assign(SEC_VOLUME * 4 / 8, 0);
            write_index(s.indices.data(), local, 1, 4);
            return;
        }
        auto it = std::find(s.palette.begin(), s.palette.end(), val);
        int pal_idx;
        if (it != s.palette.end()) {
            pal_idx = static_cast<int>(it - s.palette.begin());
        } else {
            pal_idx = static_cast<int>(s.palette.size());
            s.palette.push_back(val);
            if (static_cast<size_t>(pal_idx) >= (size_t{1} << s.bpi))
                upgrade(s);
        }
        write_index(s.indices.data(), local, pal_idx, s.bpi);
    }

    // -- Block access --

    BlockID get_block(int x, int y, int z) const {
        return static_cast<BlockID>(section_get(block_secs[sec_index(x, y, z)], sec_local(x, y, z)));
    }

    void set_block(int x, int y, int z, BlockID id) {
        section_set(block_secs[sec_index(x, y, z)], sec_local(x, y, z), static_cast<uint16_t>(id));
    }

    void fill_blocks_uniform(BlockID id) {
        for (auto& s : block_secs) {
            s.palette = {static_cast<uint16_t>(id)};
            s.indices.clear();
            s.bpi = 0;
        }
    }

    // -- Light access (packed uint16_t) --

    uint16_t get_light_word(int x, int y, int z) const {
        return section_get(light_secs[sec_index(x, y, z)], sec_local(x, y, z));
    }

    void set_light_word(int x, int y, int z, uint16_t packed) {
        section_set(light_secs[sec_index(x, y, z)], sec_local(x, y, z), packed);
    }

    void fill_light_uniform(uint16_t packed) {
        for (auto& s : light_secs) {
            s.palette = {packed};
            s.indices.clear();
            s.bpi = 0;
        }
    }

    // -- Bulk construction from dense data --

    void build_from_dense(const BlockID* dense) {
        for (int i = 0; i < NUM_SECTIONS; ++i) {
            int sx = (i % SECS_PER_DIM) * SEC_SIZE;
            int sy = ((i / SECS_PER_DIM) % SECS_PER_DIM) * SEC_SIZE;
            int sz = (i / (SECS_PER_DIM * SECS_PER_DIM)) * SEC_SIZE;

            uint16_t seen[256];
            int n_seen = 0;
            for (int dz = 0; dz < SEC_SIZE; ++dz)
                for (int dy = 0; dy < SEC_SIZE; ++dy)
                    for (int dx = 0; dx < SEC_SIZE; ++dx) {
                        int wx = sx + dx, wy = sy + dy, wz = sz + dz;
                        uint16_t id = static_cast<uint16_t>(dense[wx + wy * CHUNK_WIDTH + wz * CHUNK_WIDTH * CHUNK_HEIGHT]);
                        bool found = false;
                        for (int k = 0; k < n_seen; ++k) { if (seen[k] == id) { found = true; break; } }
                        if (!found && n_seen < 256) seen[n_seen++] = id;
                    }

            auto& s = block_secs[i];
            if (n_seen == 0 || (n_seen == 1 && seen[0] == 0)) {
                s.palette = {0}; s.indices.clear(); s.bpi = 0;
            } else {
                s.palette.assign(seen, seen + n_seen);
                s.bpi = n_seen <= 16 ? 4 : (n_seen <= 256 ? 8 : 16);
                s.indices.assign(SEC_VOLUME * s.bpi / 8, 0);
                for (int dz = 0; dz < SEC_SIZE; ++dz)
                    for (int dy = 0; dy < SEC_SIZE; ++dy)
                        for (int dx = 0; dx < SEC_SIZE; ++dx) {
                            int wx = sx + dx, wy = sy + dy, wz = sz + dz;
                            int local = dx + dy * SEC_SIZE + dz * SEC_SIZE * SEC_SIZE;
                            uint16_t id = static_cast<uint16_t>(dense[wx + wy * CHUNK_WIDTH + wz * CHUNK_WIDTH * CHUNK_HEIGHT]);
                            int idx = 0;
                            for (int k = 0; k < n_seen; ++k) { if (seen[k] == id) { idx = k; break; } }
                            write_index(s.indices.data(), local, idx, s.bpi);
                        }
            }
        }
    }

    // -- Metadata helpers --

    bool all_air() const {
        for (auto& s : block_secs)
            if (!s.is_uniform() || s.uniform_val() != 0) return false;
        return true;
    }

    int count_non_air() const {
        int c = 0;
        for (auto& s : block_secs) {
            if (s.is_uniform()) {
                if (s.uniform_val() != 0) c += SEC_VOLUME;
            } else {
                for (int i = 0; i < SEC_VOLUME; ++i) {
                    int idx = read_index(s.indices.data(), i, s.bpi);
                    if (static_cast<BlockID>(s.palette[static_cast<size_t>(idx)]) != BlockIDs::AIR) ++c;
                }
            }
        }
        return c;
    }

    int count_emissive() const {
        int c = 0;
        for (auto& s : block_secs) {
            if (s.is_uniform()) {
                BlockID id = static_cast<BlockID>(s.uniform_val());
                if (id != BlockIDs::AIR && is_emissive_fast(id)) c += SEC_VOLUME;
            } else {
                for (size_t pi = 0; pi < s.palette.size(); ++pi) {
                    BlockID id = static_cast<BlockID>(s.palette[pi]);
                    if (id != BlockIDs::AIR && is_emissive_fast(id)) {
                        int n = 0;
                        for (int i = 0; i < SEC_VOLUME; ++i)
                            if (read_index(s.indices.data(), i, s.bpi) == static_cast<int>(pi)) ++n;
                        c += n;
                    }
                }
            }
        }
        return c;
    }

    void count_section_blocks(uint32_t out[CHUNK_SECTIONS]) const {
        std::memset(out, 0, CHUNK_SECTIONS * sizeof(uint32_t));
        for (int si = 0; si < NUM_SECTIONS; ++si) {
            int sy = (si / SECS_PER_DIM) % SECS_PER_DIM;
            const auto& s = block_secs[si];
            if (s.is_uniform()) {
                if (s.uniform_val() != 0) out[sy] += SEC_VOLUME;
            } else {
                for (int i = 0; i < SEC_VOLUME; ++i) {
                    int idx = read_index(s.indices.data(), i, s.bpi);
                    if (static_cast<BlockID>(s.palette[static_cast<size_t>(idx)]) != BlockIDs::AIR) ++out[sy];
                }
            }
        }
    }

    bool check_fully_solid() const {
        const auto& registry = BlockRegistry::get_instance();
        for (auto& s : block_secs) {
            if (s.is_uniform()) {
                BlockID id = static_cast<BlockID>(s.uniform_val());
                if (id == BlockIDs::AIR) return false;
                const auto& bt = registry.get_block_fast(id);
                if (!HasProperty(bt.properties, BlockProperty::Solid) || !HasProperty(bt.properties, BlockProperty::Opaque)) return false;
            } else {
                for (int i = 0; i < SEC_VOLUME; ++i) {
                    int idx = read_index(s.indices.data(), i, s.bpi);
                    BlockID id = static_cast<BlockID>(s.palette[static_cast<size_t>(idx)]);
                    if (id == BlockIDs::AIR) return false;
                    const auto& bt = registry.get_block_fast(id);
                    if (!HasProperty(bt.properties, BlockProperty::Solid) || !HasProperty(bt.properties, BlockProperty::Opaque)) return false;
                }
            }
        }
        return true;
    }

    template<typename F>
    void for_each_block(int si, F&& f) const {
        const auto& s = block_secs[si];
        int sx = (si % SECS_PER_DIM) * SEC_SIZE;
        int sy = ((si / SECS_PER_DIM) % SECS_PER_DIM) * SEC_SIZE;
        int sz = (si / (SECS_PER_DIM * SECS_PER_DIM)) * SEC_SIZE;
        if (s.is_uniform()) {
            BlockID id = static_cast<BlockID>(s.uniform_val());
            if (id == BlockIDs::AIR) return;
            for (int dz = 0; dz < SEC_SIZE; ++dz)
                for (int dy = 0; dy < SEC_SIZE; ++dy)
                    for (int dx = 0; dx < SEC_SIZE; ++dx)
                        f(sx + dx, sy + dy, sz + dz, id);
            return;
        }
        for (int dz = 0; dz < SEC_SIZE; ++dz)
            for (int dy = 0; dy < SEC_SIZE; ++dy)
                for (int dx = 0; dx < SEC_SIZE; ++dx) {
                    int local = dx + dy * SEC_SIZE + dz * SEC_SIZE * SEC_SIZE;
                    int idx = read_index(s.indices.data(), local, s.bpi);
                    BlockID id = static_cast<BlockID>(s.palette[static_cast<size_t>(idx)]);
                    if (id != BlockIDs::AIR) f(sx + dx, sy + dy, sz + dz, id);
                }
    }

    [[nodiscard]] size_t memory_usage() const {
        size_t total = 0;
        for (int i = 0; i < NUM_SECTIONS; i++) {
            total += block_secs[i].memory_usage();
            total += light_secs[i].memory_usage();
        }
        return total;
    }

private:
    static bool is_emissive_fast(BlockID id) {
        return HasProperty(BlockRegistry::get_instance().get_block_fast(id).properties, BlockProperty::Emissive);
    }

    static int read_index(const uint8_t* data, int local, int bpi) {
        switch (bpi) {
            case 4:  { int b = local / 2, n = local & 1; return (data[b] >> (n * 4)) & 0xF; }
            case 8:  return data[local];
            case 16: return data[static_cast<size_t>(local) * 2] | (data[static_cast<size_t>(local) * 2 + 1] << 8);
            default: return 0;
        }
    }

    static void write_index(uint8_t* data, int local, int val, int bpi) {
        switch (bpi) {
            case 4: {
                int b = local / 2, n = local & 1;
                uint8_t mask = static_cast<uint8_t>(0xF0 >> (n * 4));
                data[b] = (data[b] & mask) | static_cast<uint8_t>((val & 0xF) << (n * 4));
                break;
            }
            case 8:  data[local] = static_cast<uint8_t>(val); break;
            case 16: data[static_cast<size_t>(local) * 2] = static_cast<uint8_t>(val & 0xFF); data[static_cast<size_t>(local) * 2 + 1] = static_cast<uint8_t>((val >> 8) & 0xFF); break;
            default: break;
        }
    }

    static void upgrade(PalSection& s) {
        int new_bpi = s.bpi == 4 ? 8 : 16;
        std::vector<uint8_t> ni(SEC_VOLUME * new_bpi / 8, 0);
        for (int i = 0; i < SEC_VOLUME; ++i)
            write_index(ni.data(), i, read_index(s.indices.data(), i, s.bpi), new_bpi);
        s.indices = std::move(ni);
        s.bpi = static_cast<uint8_t>(new_bpi);
    }
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
    uint32_t section_block_count[CHUNK_SECTIONS];

    [[nodiscard]] static inline bool is_emissive_block(BlockID id) noexcept {
        return HasProperty(BlockRegistry::get_instance().get_block_fast(id).properties, BlockProperty::Emissive);
    }

public:
    ChunkData();
    ChunkData(const ChunkData& other);
    ChunkData& operator=(const ChunkData& other);
    ChunkData(ChunkData&& other) noexcept;
    ChunkData& operator=(ChunkData&& other) noexcept;

    // Bulk Clear
    void clear_block_light() noexcept;
    void clear_sky_light() noexcept;
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
        clear_block_light();
        if (is_empty || emissive_count == 0) return;
        propagate_chunk_block_light_additive(*this);
    }

    void propagate_sky_light(const ChunkData* chunk_above = nullptr);
    void propagate_sky_light_column(int32_t x, int32_t z, const ChunkData* chunk_above = nullptr);

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
