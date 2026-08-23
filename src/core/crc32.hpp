#ifndef FARLANDS_CRC32_HPP
#define FARLANDS_CRC32_HPP
#include <cstdint>
#include <cstddef>

namespace VoxelEngine {

// CRC32 (IEEE 802.3) — header-only, no dependencies.
inline uint32_t crc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++)
            crc = (crc >> 1) ^ (0xEDB88320 & (-(crc & 1)));
    }
    return crc ^ 0xFFFFFFFF;
}

} // namespace VoxelEngine
#endif
