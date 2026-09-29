#ifndef FARLANDS_PALETTE_STORAGE_HPP
#define FARLANDS_PALETTE_STORAGE_HPP
#include "core/block_types.hpp"
#include "core/chunk_coords.hpp"
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace VoxelEngine {

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

            // Pass 1: the highest block id the section holds. It sizes the table
            // the palette is collected into below, and it is the uniform test as
            // well — a section of nothing but air is all zeroes, and needs no
            // palette of its own.
            uint16_t max_id = 0;
            for (int dz = 0; dz < SEC_SIZE; ++dz)
                for (int dy = 0; dy < SEC_SIZE; ++dy)
                    for (int dx = 0; dx < SEC_SIZE; ++dx) {
                        const int wx = sx + dx, wy = sy + dy, wz = sz + dz;
                        const uint16_t id = static_cast<uint16_t>(dense[wx + wy * CHUNK_WIDTH + wz * CHUNK_WIDTH * CHUNK_HEIGHT]);
                        if (id > max_id) max_id = id;
                    }

            auto& s = block_secs[i];
            if (max_id == 0) {
                s.palette = {0}; s.indices.clear(); s.bpi = 0;
                continue;
            }

            // Pass 2: the distinct ids, in first-seen order — which IS the palette
            // order every saved section and every light-state comparison is written
            // against, so it must not change. `slot` maps an id straight to its
            // palette index, which is what bounds a section only by its own 4096
            // cells. The previous form collected into `uint16_t seen[256]` and
            // dropped any id past the 256th; pass 3 then matched nothing for such a
            // cell and wrote palette index 0, silently turning that cell into
            // whatever the section's first id happened to be. Nothing shipped got
            // near 257 distinct ids (the registry holds fewer ids than that), but the
            // failure was silent and the 16-bit palette it was guarding was
            // unreachable, so the guard protected nothing.
            std::vector<int32_t> slot(static_cast<size_t>(max_id) + 1, -1);
            s.palette.clear();
            for (int dz = 0; dz < SEC_SIZE; ++dz)
                for (int dy = 0; dy < SEC_SIZE; ++dy)
                    for (int dx = 0; dx < SEC_SIZE; ++dx) {
                        const int wx = sx + dx, wy = sy + dy, wz = sz + dz;
                        const uint16_t id = static_cast<uint16_t>(dense[wx + wy * CHUNK_WIDTH + wz * CHUNK_WIDTH * CHUNK_HEIGHT]);
                        if (slot[id] < 0) {
                            slot[id] = static_cast<int32_t>(s.palette.size());
                            s.palette.push_back(id);
                        }
                    }

            const size_t distinct = s.palette.size();
            s.bpi = distinct <= 16 ? 4 : (distinct <= 256 ? 8 : 16);
            s.indices.assign(SEC_VOLUME * s.bpi / 8, 0);
            for (int dz = 0; dz < SEC_SIZE; ++dz)
                for (int dy = 0; dy < SEC_SIZE; ++dy)
                    for (int dx = 0; dx < SEC_SIZE; ++dx) {
                        const int wx = sx + dx, wy = sy + dy, wz = sz + dz;
                        const int local = dx + dy * SEC_SIZE + dz * SEC_SIZE * SEC_SIZE;
                        const uint16_t id = static_cast<uint16_t>(dense[wx + wy * CHUNK_WIDTH + wz * CHUNK_WIDTH * CHUNK_HEIGHT]);
                        write_index(s.indices.data(), local, slot[id], s.bpi);
                    }
        }
    }

    // -- Metadata helpers --

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

} // namespace VoxelEngine

#endif // FARLANDS_PALETTE_STORAGE_HPP
