#ifndef FARLANDS_HASH_UTILS_HPP
#define FARLANDS_HASH_UTILS_HPP

#include <cstdint>
#include <cstddef>

namespace VoxelEngine {

inline uint64_t fnv1a_hash_bytes(const void* data, size_t size, uint64_t hash = 0xcbf29ce484222325ULL) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

template <typename T>
inline uint64_t fnv1a_hash_value(const T& value, uint64_t hash = 0xcbf29ce484222325ULL) {
    return fnv1a_hash_bytes(&value, sizeof(value), hash);
}

} // namespace VoxelEngine

#endif // FARLANDS_HASH_UTILS_HPP
