#include "world/terrain_cache.hpp"

#include "world/terrain_cache_internal.hpp"

#include "core/terrain_cache_manifest.hpp"

#include <godot_cpp/classes/dir_access.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace VoxelEngine {

namespace {

using godot::DirAccess;
using godot::FileAccess;
using godot::PackedByteArray;
using godot::PackedStringArray;
using godot::String;

// The cache is disposable, and a second world doubles its size. Eviction only
// ever touches directories directly under user://chunks/cache.
constexpr uint64_t kTerrainCacheCapBytes = 512ull << 20;

bool terrain_cache_environment_enabled() {
    // Read once from the main thread during cache setup; nothing in this process
    // mutates the environment after startup.
    // NOLINTNEXTLINE(concurrency-mt-unsafe) - single main-thread setup read
    const char* raw = std::getenv("FARLANDS_TERRAIN_CACHE");
    if (raw == nullptr || raw[0] == '\0') return false;
    std::string value(raw);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value == "1" || value == "true" || value == "on" || value == "yes";
}

bool read_data_file(const char* path, std::vector<uint8_t>& out) {
    out.clear();
    if (!FileAccess::file_exists(String(path))) return false;
    const PackedByteArray bytes = FileAccess::get_file_as_bytes(String(path));
    if (bytes.size() == 0) return false;
    out.assign(bytes.ptr(), bytes.ptr() + bytes.size());
    return true;
}

std::string key_directory(uint64_t key) {
    char text[32] = {};
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(key));
    return text;
}

// One FileAccess handle per thread. Godot's FileAccess is an instance with its
// own seek position, so sharing one across workers would interleave seeks. The
// function-local thread_local keeps the godot-cpp Ref out of namespace scope,
// where constructing it before gdextension_init would crash the DLL load.
struct ThreadFile {
    std::string path;
    godot::Ref<FileAccess> file;
};

ThreadFile& thread_file() {
    thread_local ThreadFile state;
    return state;
}

bool read_pack_bytes(const std::string& path, uint64_t offset, size_t size,
                     std::vector<uint8_t>& out) {
    ThreadFile& state = thread_file();
    if (state.path != path || !state.file.is_valid()) {
        state.path = path;
        state.file = FileAccess::open(String(path.c_str()), FileAccess::READ);
    }
    if (!state.file.is_valid()) return false;
    state.file->seek(offset);
    out.resize(size);
    if (size == 0) return true;
    return state.file->get_buffer(out.data(), static_cast<uint64_t>(size)) == size;
}

uint64_t file_size_bytes(const std::string& path) {
    const godot::Ref<FileAccess> file = FileAccess::open(String(path.c_str()), FileAccess::READ);
    if (!file.is_valid()) return 0;
    return static_cast<uint64_t>(file->get_length());
}

uint64_t directory_size(const std::string& path) {
    const godot::Ref<DirAccess> dir = DirAccess::open(String(path.c_str()));
    if (!dir.is_valid()) return 0;
    uint64_t total = 0;
    const PackedStringArray files = dir->get_files();
    for (int64_t i = 0; i < files.size(); ++i) {
        total += file_size_bytes(path + "/" + std::string(files[i].utf8().get_data()));
    }
    return total;
}

bool remove_world_cache(const std::string& path, const std::string& name) {
    const godot::Ref<DirAccess> dir = DirAccess::open(String(path.c_str()));
    if (dir.is_valid()) {
        const PackedStringArray files = dir->get_files();
        for (int64_t i = 0; i < files.size(); ++i) dir->remove(files[i]);
    }
    const godot::Ref<DirAccess> parent = DirAccess::open("user://chunks/cache");
    if (!parent.is_valid()) return false;
    return parent->remove(String(name.c_str())) == godot::Error::OK;
}

// Least recently used world cache directories go first. The current world is
// never a candidate, and a directory without a cache.meta sorts oldest. A
// failed delete just means the cap may still be exceeded; it is a cache.
void enforce_cache_cap(const std::string& current_root, uint64_t current_bytes) {
    const godot::Ref<DirAccess> cache_dir = DirAccess::open("user://chunks/cache");
    if (!cache_dir.is_valid()) return;

    struct Candidate {
        std::string name;
        std::string path;
        uint64_t bytes = 0;
        uint64_t mtime = 0;
    };
    std::vector<Candidate> candidates;
    uint64_t total = current_bytes;

    cache_dir->list_dir_begin();
    for (String name = cache_dir->get_next(); !name.is_empty(); name = cache_dir->get_next()) {
        if (!cache_dir->current_is_dir()) continue;
        Candidate candidate;
        candidate.name = name.utf8().get_data();
        candidate.path = "user://chunks/cache/" + candidate.name;
        if (candidate.path == current_root) continue;
        candidate.bytes = directory_size(candidate.path);
        candidate.mtime = FileAccess::get_modified_time(
            String((candidate.path + "/cache.meta").c_str()));
        total += candidate.bytes;
        candidates.push_back(std::move(candidate));
    }
    cache_dir->list_dir_end();
    if (total <= kTerrainCacheCapBytes) return;

    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) { return a.mtime < b.mtime; });
    for (const Candidate& candidate : candidates) {
        if (total <= kTerrainCacheCapBytes) break;
        if (!remove_world_cache(candidate.path, candidate.name)) continue;
        total -= std::min(total, candidate.bytes);
    }
}

} // namespace

bool TerrainCache::ensure_open(const TerrainParams& params) {
    const uint64_t params_key = terrain_cache_key(params, {}, {}, {}, {});
    if (attempted_ && attempted_params_key_ == params_key) return reader_.is_open();

    close();
    attempted_ = true;
    attempted_params_key_ = params_key;
    env_enabled_ = terrain_cache_environment_enabled();
    if (!env_enabled_) return false;

    std::vector<uint8_t> terrain_config;
    std::vector<uint8_t> biomes;
    std::vector<uint8_t> vegetation;
    std::vector<uint8_t> block_definitions;
    if (!read_data_file("res://data/terrain_config.json", terrain_config) ||
        !read_data_file("res://data/biomes.json", biomes) ||
        !read_data_file("res://data/vegetation.json", vegetation) ||
        !read_data_file("res://data/block_definitions.json", block_definitions)) {
        return false;
    }

    key_ = terrain_cache_key(params, terrain_config, biomes, vegetation,
                             block_definitions);
    root_ = "user://chunks/cache/" + key_directory(key_);
    DirAccess::make_dir_recursive_absolute(String(root_.c_str()));

    const godot::Ref<DirAccess> dir = DirAccess::open(String(root_.c_str()));
    if (!dir.is_valid()) {
        key_ = 0;
        root_.clear();
        return false;
    }

    // `cache.meta` is authoritative about what is complete; a pack published
    // after it (a crash, or a reset that stopped the writer mid-publish) is
    // adopted here instead of invalidating the whole directory. Orphan `.tmp`
    // files from an interrupted write are removed for the same reason.
    std::vector<std::string> pack_paths;
    std::unordered_set<uint64_t> pack_regions;
    std::unordered_map<uint64_t, uint64_t> pack_bytes;
    uint64_t total_bytes = 0;
    const PackedStringArray names = dir->get_files();
    for (int64_t i = 0; i < names.size(); ++i) {
        const String& name = names[i];
        if (name.ends_with(".tmp")) {
            dir->remove(name);
            continue;
        }
        const std::string filename = name.utf8().get_data();
        int32_t region_x = 0;
        int32_t region_z = 0;
        if (!parse_terrain_cache_pack_filename(filename, region_x, region_z)) continue;
        const std::string path = root_ + "/" + filename;
        const uint64_t bytes = file_size_bytes(path);
        pack_paths.push_back(path);
        const uint64_t region_key = terrain_cache_region_key(region_x, region_z);
        pack_regions.insert(region_key);
        pack_bytes[region_key] = bytes;
        total_bytes += bytes;
    }
    std::sort(pack_paths.begin(), pack_paths.end());

    bool manifest_ok = false;
    uint64_t created_unix = static_cast<uint64_t>(std::time(nullptr));
    {
        std::vector<uint8_t> manifest_bytes;
        const std::string manifest_path = terrain_cache_manifest_path(root_);
        const godot::Ref<FileAccess> file =
            FileAccess::open(String(manifest_path.c_str()), FileAccess::READ);
        if (file.is_valid() && file->get_length() ==
                                    static_cast<int64_t>(kTerrainCacheManifestSize)) {
            manifest_bytes.resize(kTerrainCacheManifestSize);
            if (file->get_buffer(manifest_bytes.data(), kTerrainCacheManifestSize) ==
                kTerrainCacheManifestSize) {
                TerrainCacheManifest manifest;
                if (decode_terrain_cache_manifest(manifest_bytes.data(),
                                                  manifest_bytes.size(), manifest) &&
                    manifest.key == key_ && manifest.pack_count == pack_paths.size() &&
                    manifest.total_bytes == total_bytes) {
                    manifest_ok = true;
                    created_unix = manifest.created_unix;
                }
            }
        }
    }

    auto state = std::make_shared<TerrainCacheWriterState>();
    state->root = root_;
    state->key = key_;
    state->created_unix = created_unix;
    state->packs = std::move(pack_regions);
    state->pack_bytes = std::move(pack_bytes);
    writer_ = state;
    if (!manifest_ok) terrain_cache_publish_manifest(state);

    enforce_cache_cap(root_, total_bytes);

    return reader_.open(root_, key_, pack_paths, read_pack_bytes);
}

void TerrainCache::close() {
    if (writer_) {
        writer_->stop.store(true, std::memory_order_release);
        writer_.reset();
    }
    reader_.close();
    reader_.reset_stats();
    key_ = 0;
    root_.clear();
    attempted_ = false;
    attempted_params_key_ = 0;
    env_enabled_ = false;
}

} // namespace VoxelEngine
