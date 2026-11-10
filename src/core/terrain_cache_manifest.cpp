#include "core/terrain_cache_manifest.hpp"

#include <cerrno>
#include <cstdlib>
#include <limits>

namespace VoxelEngine {

namespace {

void put_u32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

void put_u64(std::vector<uint8_t>& out, uint64_t value) {
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<uint8_t>((value >> (i * 8)) & 0xFF));
}

bool read_u32(const uint8_t* data, size_t size, size_t& pos, uint32_t& value) {
    if (pos > size || size - pos < 4) return false;
    value = static_cast<uint32_t>(data[pos]) |
            (static_cast<uint32_t>(data[pos + 1]) << 8) |
            (static_cast<uint32_t>(data[pos + 2]) << 16) |
            (static_cast<uint32_t>(data[pos + 3]) << 24);
    pos += 4;
    return true;
}

bool read_u64(const uint8_t* data, size_t size, size_t& pos, uint64_t& value) {
    uint32_t lo = 0;
    uint32_t hi = 0;
    if (!read_u32(data, size, pos, lo)) return false;
    if (!read_u32(data, size, pos, hi)) return false;
    value = static_cast<uint64_t>(lo) | (static_cast<uint64_t>(hi) << 32);
    return true;
}

std::string join_path(const std::string& root, const char* leaf) {
    if (root.empty()) return leaf;
    if (root.back() == '/' || root.back() == '\\') return root + leaf;
    return root + "/" + leaf;
}

bool parse_i32(const std::string& text, int32_t& out_value) {
    if (text.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const long value = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0' ||
        value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    out_value = static_cast<int32_t>(value);
    return true;
}

} // namespace

bool encode_terrain_cache_manifest(const TerrainCacheManifest& manifest,
                                   std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(kTerrainCacheManifestSize);
    put_u32(out, kTerrainCacheManifestMagic);
    put_u32(out, kTerrainCacheManifestVersion);
    put_u64(out, manifest.key);
    put_u32(out, manifest.pack_count);
    put_u64(out, manifest.total_bytes);
    put_u64(out, manifest.created_unix);
    return out.size() == kTerrainCacheManifestSize;
}

bool decode_terrain_cache_manifest(const uint8_t* data, size_t size,
                                   TerrainCacheManifest& out_manifest) {
    if (!data || size != kTerrainCacheManifestSize) return false;
    size_t pos = 0;
    uint32_t magic = 0;
    uint32_t version = 0;
    TerrainCacheManifest manifest;
    if (!read_u32(data, size, pos, magic) || !read_u32(data, size, pos, version) ||
        !read_u64(data, size, pos, manifest.key) ||
        !read_u32(data, size, pos, manifest.pack_count) ||
        !read_u64(data, size, pos, manifest.total_bytes) ||
        !read_u64(data, size, pos, manifest.created_unix)) {
        return false;
    }
    if (magic != kTerrainCacheManifestMagic || version != kTerrainCacheManifestVersion) {
        return false;
    }
    if (manifest.pack_count > kTerrainCacheMaxPacks) return false;
    if (pos != size) return false;
    manifest.format_version = version;
    out_manifest = manifest;
    return true;
}

std::string terrain_cache_manifest_path(const std::string& cache_root) {
    return join_path(cache_root, "cache.meta");
}

std::string terrain_cache_pack_filename(int32_t region_x, int32_t region_z) {
    return "r_" + std::to_string(region_x) + "_" + std::to_string(region_z) + ".ftc";
}

bool parse_terrain_cache_pack_filename(const std::string& filename,
                                       int32_t& out_region_x, int32_t& out_region_z) {
    constexpr const char* kPrefix = "r_";
    constexpr const char* kSuffix = ".ftc";
    if (filename.size() <= 7) return false;
    if (filename.compare(0, 2, kPrefix) != 0) return false;
    if (filename.compare(filename.size() - 4, 4, kSuffix) != 0) return false;

    const std::string body = filename.substr(2, filename.size() - 6);
    const size_t separator = body.find('_');
    if (separator == std::string::npos || separator == 0 || separator + 1 >= body.size()) {
        return false;
    }
    int32_t x = 0;
    int32_t z = 0;
    if (!parse_i32(body.substr(0, separator), x) ||
        !parse_i32(body.substr(separator + 1), z)) {
        return false;
    }
    out_region_x = x;
    out_region_z = z;
    return true;
}

} // namespace VoxelEngine
