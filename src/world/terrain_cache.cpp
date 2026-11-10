#include "world/terrain_cache.hpp"

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
#include <string>
#include <vector>

namespace VoxelEngine {

namespace {

using godot::DirAccess;
using godot::FileAccess;
using godot::PackedByteArray;
using godot::PackedStringArray;
using godot::String;

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

    const uint64_t key = terrain_cache_key(params, terrain_config, biomes,
                                           vegetation, block_definitions);
    const std::string root = "user://chunks/cache/" + key_directory(key);

    godot::Ref<DirAccess> dir = DirAccess::open(String(root.c_str()));
    if (!dir.is_valid()) return false;

    std::vector<std::string> pack_paths;
    const PackedStringArray names = dir->get_files();
    for (int64_t i = 0; i < names.size(); ++i) {
        const String& name = names[i];
        if (!name.ends_with(".ftc")) continue;
        pack_paths.push_back(root + "/" + std::string(name.utf8().get_data()));
    }
    std::sort(pack_paths.begin(), pack_paths.end());

    return reader_.open(root, key, pack_paths, read_pack_bytes);
}

void TerrainCache::close() {
    reader_.close();
    reader_.reset_stats();
    attempted_ = false;
    attempted_params_key_ = 0;
    env_enabled_ = false;
}

} // namespace VoxelEngine
