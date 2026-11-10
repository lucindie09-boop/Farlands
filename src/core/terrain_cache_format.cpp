#include "core/terrain_cache_format.hpp"

#include "core/crc32.hpp"
#include "core/palette_storage.hpp"

#include <array>
#include <set>

namespace VoxelEngine {

namespace {

void put_u16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
}

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void put_i32(std::vector<uint8_t>& out, int32_t value) {
    put_u32(out, static_cast<uint32_t>(value));
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

class Reader {
public:
    Reader(const uint8_t* data, size_t size) : data_(data), size_(size) {}

    [[nodiscard]] size_t at() const noexcept { return pos_; }
    [[nodiscard]] size_t remaining() const noexcept { return size_ - pos_; }

    bool read_u8(uint8_t& value) {
        if (remaining() < 1) return false;
        value = data_[pos_++];
        return true;
    }

    bool read_u16(uint16_t& value) {
        if (remaining() < 2) return false;
        value = static_cast<uint16_t>(data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return true;
    }

    bool read_u32(uint32_t& value) {
        if (remaining() < 4) return false;
        value = 0;
        for (int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(data_[pos_ + i]) << (i * 8);
        pos_ += 4;
        return true;
    }

    bool read_i32(int32_t& value) {
        uint32_t bits = 0;
        if (!read_u32(bits)) return false;
        value = static_cast<int32_t>(bits);
        return true;
    }

    bool read_u64(uint64_t& value) {
        if (remaining() < 8) return false;
        value = 0;
        for (int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(data_[pos_ + i]) << (i * 8);
        pos_ += 8;
        return true;
    }

    const uint8_t* read_bytes(size_t count) {
        if (remaining() < count) return nullptr;
        const uint8_t* result = data_ + pos_;
        pos_ += count;
        return result;
    }

private:
    const uint8_t* data_;
    size_t size_;
    size_t pos_ = 0;
};

int unpack_index(const uint8_t* data, int local, uint8_t bpi) {
    switch (bpi) {
        case 4: {
            const int byte = local / 2;
            return (data[byte] >> ((local & 1) * 4)) & 0xF;
        }
        case 8:
            return data[local];
        case 16:
            return data[static_cast<size_t>(local) * 2] |
                   (data[static_cast<size_t>(local) * 2 + 1] << 8);
        default:
            return -1;
    }
}

bool encode_section(const PalSection& section, std::vector<uint8_t>& out) {
    if (section.is_uniform()) {
        out.push_back(0);
        put_u16(out, section.uniform_val());
        return true;
    }

    if (section.bpi != 4 && section.bpi != 8 && section.bpi != 16) return false;
    if (section.palette.size() < 2 || section.palette.size() > 65535) return false;
    const size_t index_bytes =
        static_cast<size_t>(PaletteStorage::SEC_VOLUME) * section.bpi / 8;
    if (section.indices.size() != index_bytes) return false;
    const size_t palette_limit = size_t{1} << section.bpi;
    if (section.palette.size() > palette_limit) return false;

    for (int local = 0; local < PaletteStorage::SEC_VOLUME; ++local) {
        const int index = unpack_index(section.indices.data(), local, section.bpi);
        if (index < 0 || static_cast<size_t>(index) >= section.palette.size()) return false;
    }

    out.push_back(1);
    put_u16(out, static_cast<uint16_t>(section.palette.size()));
    out.push_back(section.bpi);
    for (const uint16_t value : section.palette) put_u16(out, value);
    out.insert(out.end(), section.indices.begin(), section.indices.end());
    return true;
}

bool decode_section(Reader& reader, const BlockRegistry& registry, PalSection& out) {
    uint8_t kind = 0;
    if (!reader.read_u8(kind)) return false;

    if (kind == 0) {
        uint16_t id = 0;
        if (!reader.read_u16(id)) return false;
        if (id >= registry.get_count()) return false;
        out.palette = {id};
        out.bpi = 0;
        out.indices.clear();
        return true;
    }
    if (kind != 1) return false;

    uint16_t palette_count = 0;
    uint8_t bpi = 0;
    if (!reader.read_u16(palette_count) || !reader.read_u8(bpi)) return false;
    if (bpi != 4 && bpi != 8 && bpi != 16) return false;
    if (palette_count < 2) return false;
    if (palette_count > (size_t{1} << bpi)) return false;

    std::vector<uint16_t> palette;
    palette.reserve(palette_count);
    for (uint16_t i = 0; i < palette_count; ++i) {
        uint16_t id = 0;
        if (!reader.read_u16(id)) return false;
        if (id >= registry.get_count()) return false;
        palette.push_back(id);
    }

    const size_t index_bytes =
        static_cast<size_t>(PaletteStorage::SEC_VOLUME) * bpi / 8;
    const uint8_t* indices = reader.read_bytes(index_bytes);
    if (!indices) return false;

    for (int local = 0; local < PaletteStorage::SEC_VOLUME; ++local) {
        const int index = unpack_index(indices, local, bpi);
        if (index < 0 || static_cast<size_t>(index) >= palette.size()) return false;
    }

    out.palette = std::move(palette);
    out.bpi = bpi;
    out.indices.assign(indices, indices + index_bytes);
    return true;
}

void put_header(std::vector<uint8_t>& out, const TerrainCachePackHeader& header) {
    out.clear();
    out.reserve(kTerrainCacheHeaderSize);
    put_u32(out, kTerrainCachePackMagic);
    put_u32(out, kTerrainCacheFormatVersion);
    put_u64(out, header.key);
    put_i32(out, header.region_x);
    put_i32(out, header.region_z);
    put_u32(out, header.chunk_count);
    put_u32(out, header.index_offset);
    put_u32(out, header.index_size);
    put_u32(out, header.index_crc);
}

using ChunkCoord = std::array<int32_t, 3>;

bool coordinates_in_region(int32_t x, int32_t y, int32_t z,
                           int32_t region_x, int32_t region_z) {
    const int64_t min_x = static_cast<int64_t>(region_x) * kTerrainCacheRegionChunks;
    const int64_t min_z = static_cast<int64_t>(region_z) * kTerrainCacheRegionChunks;
    const int64_t max_x = min_x + kTerrainCacheRegionChunks;
    const int64_t max_z = min_z + kTerrainCacheRegionChunks;
    const int32_t max_y = WORLD_HEIGHT_Y / CHUNK_HEIGHT;
    return x >= min_x && x < max_x && z >= min_z && z < max_z && y >= 0 && y < max_y;
}

} // namespace

bool encode_terrain_cache_chunk(const ChunkData& chunk,
                                const std::vector<TerrainCacheSpill>& spill,
                                std::vector<uint8_t>& out) {
    out.clear();
    if (spill.size() > kTerrainCacheMaxSpills) return false;

    for (int section = 0; section < ChunkData::NUM_BLOCK_SECTIONS; ++section) {
        if (!encode_section(chunk.block_section(section), out)) return false;
    }

    put_u32(out, static_cast<uint32_t>(spill.size()));
    for (const TerrainCacheSpill& entry : spill) {
        put_i32(out, entry.x);
        put_i32(out, entry.y);
        put_i32(out, entry.z);
        put_u16(out, static_cast<uint16_t>(entry.block));
    }

    if (out.size() > kTerrainCacheMaxChunkBytes) {
        out.clear();
        return false;
    }
    return true;
}

bool decode_terrain_cache_chunk(const uint8_t* data, size_t size,
                                const BlockRegistry& registry,
                                ChunkData& out_chunk,
                                std::vector<TerrainCacheSpill>& out_spill) {
    out_spill.clear();
    if (!data || size == 0 || size > kTerrainCacheMaxChunkBytes) return false;

    Reader reader(data, size);
    std::array<PalSection, ChunkData::NUM_BLOCK_SECTIONS> sections;
    for (PalSection& section : sections) {
        if (!decode_section(reader, registry, section)) return false;
    }

    uint32_t spill_count = 0;
    if (!reader.read_u32(spill_count)) return false;
    if (spill_count > kTerrainCacheMaxSpills) return false;
    const size_t spill_bytes = static_cast<size_t>(spill_count) * 14;
    if (reader.remaining() != spill_bytes) return false;

    std::vector<TerrainCacheSpill> spill;
    spill.reserve(spill_count);
    for (uint32_t i = 0; i < spill_count; ++i) {
        TerrainCacheSpill entry;
        uint16_t block = 0;
        if (!reader.read_i32(entry.x) || !reader.read_i32(entry.y) ||
            !reader.read_i32(entry.z) || !reader.read_u16(block)) {
            return false;
        }
        if (block >= registry.get_count()) return false;
        entry.block = block;
        spill.push_back(entry);
    }

    ChunkData decoded;
    decoded.set_block_sections(sections.data());
    out_chunk = std::move(decoded);
    out_spill = std::move(spill);
    return true;
}

bool encode_terrain_cache_pack(uint64_t key, int32_t region_x, int32_t region_z,
                               const std::vector<TerrainCacheEncodedChunk>& chunks,
                               std::vector<uint8_t>& out) {
    out.clear();
    if (chunks.size() > kTerrainCacheMaxChunksPerPack) return false;

    std::set<ChunkCoord> seen;
    std::vector<uint8_t> payloads;
    std::vector<TerrainCacheChunkRecord> index;
    index.reserve(chunks.size());

    size_t offset = kTerrainCacheHeaderSize;
    for (const TerrainCacheEncodedChunk& chunk : chunks) {
        if (!coordinates_in_region(chunk.x, chunk.y, chunk.z, region_x, region_z)) return false;
        if (chunk.payload.empty() || chunk.payload.size() > kTerrainCacheMaxChunkBytes) return false;
        if (!seen.insert(ChunkCoord{chunk.x, chunk.y, chunk.z}).second) return false;

        if (offset + chunk.payload.size() > kTerrainCacheMaxPackBytes) return false;
        TerrainCacheChunkRecord record;
        record.x = chunk.x;
        record.y = chunk.y;
        record.z = chunk.z;
        record.payload_offset = static_cast<uint32_t>(offset);
        record.payload_size = static_cast<uint32_t>(chunk.payload.size());
        record.payload_crc = crc32(chunk.payload.data(), chunk.payload.size());
        index.push_back(record);
        payloads.insert(payloads.end(), chunk.payload.begin(), chunk.payload.end());
        offset += chunk.payload.size();
    }

    const size_t index_offset = offset;
    const size_t index_size = index.size() * kTerrainCacheIndexEntrySize;
    if (index_offset + index_size > kTerrainCacheMaxPackBytes) return false;

    TerrainCachePackHeader header;
    header.key = key;
    header.region_x = region_x;
    header.region_z = region_z;
    header.chunk_count = static_cast<uint32_t>(index.size());
    header.index_offset = static_cast<uint32_t>(index_offset);
    header.index_size = static_cast<uint32_t>(index_size);

    out.reserve(index_offset + index_size);
    for (const TerrainCacheChunkRecord& record : index) {
        put_i32(out, record.x);
        put_i32(out, record.y);
        put_i32(out, record.z);
        put_u32(out, record.payload_offset);
        put_u32(out, record.payload_size);
        put_u32(out, record.payload_crc);
    }
    header.index_crc = crc32(out.data(), out.size());

    std::vector<uint8_t> full;
    put_header(full, header);
    full.insert(full.end(), payloads.begin(), payloads.end());
    full.insert(full.end(), out.begin(), out.end());
    out = std::move(full);
    return true;
}

bool decode_terrain_cache_pack_header(const uint8_t* data, size_t size,
                                      TerrainCachePackHeader& out_header) {
    if (!data || size < kTerrainCacheHeaderSize) return false;

    Reader reader(data, size);
    uint32_t magic = 0;
    uint32_t version = 0;
    TerrainCachePackHeader header;
    if (!reader.read_u32(magic) || !reader.read_u32(version) ||
        !reader.read_u64(header.key) || !reader.read_i32(header.region_x) ||
        !reader.read_i32(header.region_z) || !reader.read_u32(header.chunk_count) ||
        !reader.read_u32(header.index_offset) || !reader.read_u32(header.index_size) ||
        !reader.read_u32(header.index_crc)) {
        return false;
    }
    if (magic != kTerrainCachePackMagic || version != kTerrainCacheFormatVersion) return false;
    if (header.chunk_count > kTerrainCacheMaxChunksPerPack) return false;
    if (header.index_size != header.chunk_count * kTerrainCacheIndexEntrySize) return false;
    if (header.index_offset < kTerrainCacheHeaderSize) return false;
    if (header.index_offset > kTerrainCacheMaxPackBytes ||
        header.index_size > kTerrainCacheMaxPackBytes - header.index_offset) {
        return false;
    }
    out_header = header;
    return true;
}

bool decode_terrain_cache_pack_index_bytes(const uint8_t* data, size_t size,
                                           const TerrainCachePackHeader& header,
                                           std::vector<TerrainCacheChunkRecord>& out_index) {
    out_index.clear();
    if (!data && size != 0) return false;
    if (size != header.index_size) return false;
    if (crc32(data, size) != header.index_crc) return false;

    Reader index_reader(data, size);
    std::vector<TerrainCacheChunkRecord> index;
    index.reserve(header.chunk_count);
    uint32_t previous_end = static_cast<uint32_t>(kTerrainCacheHeaderSize);
    std::set<ChunkCoord> seen;

    for (uint32_t i = 0; i < header.chunk_count; ++i) {
        TerrainCacheChunkRecord record;
        if (!index_reader.read_i32(record.x) || !index_reader.read_i32(record.y) ||
            !index_reader.read_i32(record.z) || !index_reader.read_u32(record.payload_offset) ||
            !index_reader.read_u32(record.payload_size) || !index_reader.read_u32(record.payload_crc)) {
            return false;
        }
        if (!coordinates_in_region(record.x, record.y, record.z, header.region_x, header.region_z)) {
            return false;
        }
        if (record.payload_offset < previous_end ||
            record.payload_size == 0 || record.payload_size > kTerrainCacheMaxChunkBytes) {
            return false;
        }
        if (record.payload_offset > header.index_offset ||
            record.payload_size > header.index_offset - record.payload_offset) {
            return false;
        }
        if (!seen.insert(ChunkCoord{record.x, record.y, record.z}).second) return false;
        previous_end = record.payload_offset + record.payload_size;
        index.push_back(record);
    }

    out_index = std::move(index);
    return true;
}

bool decode_terrain_cache_pack_index(const uint8_t* data, size_t size,
                                     uint64_t expected_key,
                                     TerrainCachePackHeader& out_header,
                                     std::vector<TerrainCacheChunkRecord>& out_index) {
    out_index.clear();
    if (!data || size < kTerrainCacheHeaderSize) return false;

    TerrainCachePackHeader header;
    if (!decode_terrain_cache_pack_header(data, size, header)) return false;
    if (header.key != expected_key) return false;
    if (header.index_offset > size || header.index_size > size - header.index_offset) return false;
    if (header.index_offset + header.index_size != size) return false;
    if (!decode_terrain_cache_pack_index_bytes(data + header.index_offset,
                                               header.index_size, header, out_index)) {
        return false;
    }
    out_header = header;
    return true;
}

bool decode_terrain_cache_payload(const uint8_t* data, size_t size,
                                  const TerrainCacheChunkRecord& record,
                                  const BlockRegistry& registry,
                                  ChunkData& out_chunk,
                                  std::vector<TerrainCacheSpill>& out_spill) {
    if (!data || size != record.payload_size) return false;
    if (record.payload_size == 0 || record.payload_size > kTerrainCacheMaxChunkBytes) return false;
    if (crc32(data, size) != record.payload_crc) return false;
    return decode_terrain_cache_chunk(data, size, registry, out_chunk, out_spill);
}

bool decode_terrain_cache_pack_chunk(const uint8_t* data, size_t size,
                                     const TerrainCacheChunkRecord& record,
                                     const BlockRegistry& registry,
                                     ChunkData& out_chunk,
                                     std::vector<TerrainCacheSpill>& out_spill) {
    if (!data || record.payload_offset < kTerrainCacheHeaderSize) return false;
    if (record.payload_offset > size || record.payload_size > size - record.payload_offset) {
        return false;
    }
    return decode_terrain_cache_payload(data + record.payload_offset, record.payload_size,
                                        record, registry, out_chunk, out_spill);
}

} // namespace VoxelEngine
