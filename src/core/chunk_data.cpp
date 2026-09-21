#include "core/chunk_data.hpp"

#include <algorithm>

namespace VoxelEngine {

ChunkData::ChunkData()
    : storage(std::make_unique<PaletteStorage>()) {
    clear();
}

ChunkData::ChunkData(const ChunkData& other)
    : storage(std::make_unique<PaletteStorage>(*other.storage)),
      is_empty(other.is_empty),
      is_fully_solid(other.is_fully_solid),
      block_count(other.block_count),
      emissive_count(other.emissive_count),
      liquid_cells(other.liquid_cells) {
    std::memcpy(section_block_count, other.section_block_count, sizeof(section_block_count));
}

ChunkData& ChunkData::operator=(const ChunkData& other) {
    if (this != &other) {
        *storage = *other.storage;
        is_empty       = other.is_empty;
        is_fully_solid = other.is_fully_solid;
        block_count    = other.block_count;
        emissive_count = other.emissive_count;
        liquid_cells   = other.liquid_cells;
        std::memcpy(section_block_count, other.section_block_count, sizeof(section_block_count));
    }
    return *this;
}

ChunkData::ChunkData(ChunkData&& other) noexcept
    : storage(std::move(other.storage)),
      is_empty(other.is_empty),
      is_fully_solid(other.is_fully_solid),
      block_count(other.block_count),
      emissive_count(other.emissive_count),
      liquid_cells(other.liquid_cells) {
    std::memcpy(section_block_count, other.section_block_count, sizeof(section_block_count));
    other.is_empty = true;
    other.is_fully_solid = false;
    other.block_count = 0;
    other.emissive_count = 0;
    other.liquid_cells = 0;
    std::memset(other.section_block_count, 0, sizeof(other.section_block_count));
}

ChunkData& ChunkData::operator=(ChunkData&& other) noexcept {
    if (this != &other) {
        storage        = std::move(other.storage);
        is_empty       = other.is_empty;
        is_fully_solid = other.is_fully_solid;
        block_count    = other.block_count;
        emissive_count = other.emissive_count;
        liquid_cells   = other.liquid_cells;
        std::memcpy(section_block_count, other.section_block_count, sizeof(section_block_count));
        other.is_empty = true;
        other.is_fully_solid = false;
        other.block_count = 0;
        other.emissive_count = 0;
        other.liquid_cells = 0;
        std::memset(other.section_block_count, 0, sizeof(other.section_block_count));
    }
    return *this;
}

bool ChunkData::clear_block_light() noexcept {
    // Returns whether anything was actually there to clear, which is what tells a
    // caller its light did not change: a region pass over daylight terrain has no
    // block light at all, and dirtying 27 chunks for a remesh after wiping nothing
    // is how a paste (or a newly installed chunk) cost far more than it changed.
    bool changed = false;
    for (auto& s : storage->light_secs) {
        if (s.is_uniform()) {
            const uint16_t v = s.uniform_val();
            const uint16_t nv = v & 0x000F;
            if (nv != v) {
                s.palette[0] = nv;
                changed = true;
            }
            continue;
        }
        // Fast path: when no palette entry carries block light there is nothing to
        // clear, and the palette is hundreds of times smaller than the section. A
        // sky-lit terrain chunk lands here for all 8 sections, so the clear costs
        // a few palette reads instead of 4096 read-modify-writes each.
        bool any_block_light = false;
        for (const uint16_t v : s.palette) {
            if ((v & 0xFFF0u) != 0) {
                any_block_light = true;
                break;
            }
        }
        if (!any_block_light) continue;
        for (int i = 0; i < PaletteStorage::SEC_VOLUME; ++i) {
            const uint16_t v = PaletteStorage::section_get(s, i);
            const uint16_t nv = v & 0x000F;
            if (nv != v) {
                PaletteStorage::section_set(s, i, nv);
                changed = true;
            }
        }
    }
    return changed;
}

namespace {

void append_u32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFFu));
}

bool read_u32(const std::vector<uint8_t>& in, size_t& at, uint32_t& out) {
    if (at + 4 > in.size()) return false;
    out = static_cast<uint32_t>(in[at]) | (static_cast<uint32_t>(in[at + 1]) << 8) |
          (static_cast<uint32_t>(in[at + 2]) << 16) | (static_cast<uint32_t>(in[at + 3]) << 24);
    at += 4;
    return true;
}

} // namespace

void ChunkData::append_light_state(std::vector<uint8_t>& out) const {
    for (const PalSection& s : storage->light_secs) {
        out.push_back(s.bpi);
        append_u32(out, static_cast<uint32_t>(s.palette.size()));
        for (const uint16_t v : s.palette) {
            out.push_back(static_cast<uint8_t>(v & 0xFFu));
            out.push_back(static_cast<uint8_t>(v >> 8));
        }
        append_u32(out, static_cast<uint32_t>(s.indices.size()));
        out.insert(out.end(), s.indices.begin(), s.indices.end());
    }
}

bool ChunkData::light_state_equals(const std::vector<uint8_t>& prior) const {
    size_t at = 0;
    for (const PalSection& s : storage->light_secs) {
        if (at >= prior.size()) return false;
        if (prior[at++] != s.bpi) return false;

        uint32_t pal_n = 0;
        if (!read_u32(prior, at, pal_n)) return false;
        if (pal_n != s.palette.size()) return false;
        if (at + static_cast<size_t>(pal_n) * 2 > prior.size()) return false;
        for (uint32_t i = 0; i < pal_n; ++i) {
            const uint16_t v = static_cast<uint16_t>(prior[at] | (static_cast<uint16_t>(prior[at + 1]) << 8));
            if (v != s.palette[i]) return false;
            at += 2;
        }

        uint32_t idx_n = 0;
        if (!read_u32(prior, at, idx_n)) return false;
        if (idx_n != s.indices.size()) return false;
        if (at + idx_n > prior.size()) return false;
        if (idx_n > 0 && std::memcmp(prior.data() + at, s.indices.data(), idx_n) != 0) return false;
        at += idx_n;
    }
    return at == prior.size();
}

void ChunkData::clear_sky_light() noexcept {
    for (auto& s : storage->light_secs) {
        if (s.is_uniform()) {
            s.palette[0] = s.uniform_val() & 0xFFF0;
        } else {
            for (int i = 0; i < PaletteStorage::SEC_VOLUME; ++i) {
                uint16_t v = PaletteStorage::section_get(s, i);
                uint16_t nv = v & 0xFFF0;
                if (nv != v) PaletteStorage::section_set(s, i, nv);
            }
        }
    }
}

void ChunkData::clear_light() noexcept {
    storage->fill_light_uniform(0);
}

void ChunkData::clear() noexcept {
    storage->fill_blocks_uniform(BlockIDs::AIR);
    storage->fill_light_uniform(0);
    is_empty       = true;
    is_fully_solid = false;
    block_count    = 0;
    emissive_count = 0;
    liquid_cells   = 0;
    std::memset(section_block_count, 0, sizeof(section_block_count));
}

void ChunkData::fill_blocks(BlockID block_id) noexcept {
    storage->fill_blocks_uniform(block_id);

    if (block_id == BlockIDs::AIR) {
        is_empty = true;
        is_fully_solid = false;
        block_count = 0;
        emissive_count = 0;
        liquid_cells = 0;
        std::memset(section_block_count, 0, sizeof(section_block_count));
        return;
    }

    is_empty = false;
    block_count = CHUNK_VOLUME;
    emissive_count = is_emissive_block(block_id) ? CHUNK_VOLUME : 0;

    const auto& registry = BlockRegistry::get_instance();
    const auto& block_type = registry.get_block(block_id);
    liquid_cells = block_type.draws_fluid_surface() ? CHUNK_VOLUME : 0;
    is_fully_solid = HasProperty(block_type.properties, BlockProperty::Solid) &&
                     HasProperty(block_type.properties, BlockProperty::Opaque);

    const uint32_t blocks_per_section = CHUNK_VOLUME / CHUNK_SECTIONS;
    for (int32_t s = 0; s < CHUNK_SECTIONS; ++s)
        section_block_count[s] = blocks_per_section;
}

void ChunkData::set_data(const BlockID* data, uint32_t /*count*/) {
    storage->build_from_dense(data);

    block_count    = 0;
    emissive_count = 0;
    liquid_cells   = 0;
    std::memset(section_block_count, 0, sizeof(section_block_count));

    // Count blocks, emissive and liquid from sections
    for (int si = 0; si < PaletteStorage::NUM_SECTIONS; ++si) {
        storage->for_each_block(si, [&](int x, int y, int z, BlockID id) {
            ++block_count;
            if (is_emissive_block(id)) ++emissive_count;
            if (is_liquid_surface_block(id)) ++liquid_cells;
            ++section_block_count[y / SECTION_HEIGHT];
        });
    }

    is_empty = (block_count == 0);
    compute_fully_solid();
}

void ChunkData::compute_fully_solid() {
    if (is_empty) { is_fully_solid = false; return; }
    is_fully_solid = storage->check_fully_solid();
}

void ChunkData::compute_section_flags() {
    storage->count_section_blocks(section_block_count);
}

void ChunkData::propagate_sky_light(const ChunkData* chunk_above) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    for (int32_t x = 0; x < CHUNK_WIDTH; ++x) {
        for (int32_t z = 0; z < CHUNK_DEPTH; ++z) {
            uint8_t current_sky_light = chunk_above ? chunk_above->get_sky_light(x, 0, z) : 15;
            for (int32_t y = CHUNK_HEIGHT - 1; y >= 0; --y) {
                const BlockID block_id = get_block_unsafe(x, y, z);
                if (block_id != BlockIDs::AIR) {
                    const BlockType& block_type = registry.get_block(block_id);
                    if (HasProperty(block_type.properties, BlockProperty::Opaque)) {
                        current_sky_light = 0;
                    } else if (block_type.light_opacity > 0) {
                        // Light crossing this block pays its opacity. A pool
                        // darkens with depth (3/block) instead of staying at
                        // full sky light all the way to the seabed.
                        current_sky_light = static_cast<uint8_t>(std::max(0, static_cast<int>(current_sky_light) - block_type.light_opacity));
                    }
                }
                set_sky_light_unsafe(x, y, z, current_sky_light);
            }
        }
    }
}

void ChunkData::propagate_sky_light_column(int32_t x, int32_t z, const ChunkData* chunk_above) {
    const BlockRegistry& registry = BlockRegistry::get_instance();
    uint8_t current_sky_light = chunk_above ? chunk_above->get_sky_light(x, 0, z) : 15;
    for (int32_t y = CHUNK_HEIGHT - 1; y >= 0; --y) {
        const BlockID block_id = get_block_unsafe(x, y, z);
        if (block_id != BlockIDs::AIR) {
            const BlockType& block_type = registry.get_block(block_id);
            if (HasProperty(block_type.properties, BlockProperty::Opaque)) {
                current_sky_light = 0;
            } else if (block_type.light_opacity > 0) {
                current_sky_light = static_cast<uint8_t>(std::max(0, static_cast<int>(current_sky_light) - block_type.light_opacity));
            }
        }
        set_sky_light_unsafe(x, y, z, current_sky_light);
    }
}

} // namespace VoxelEngine
