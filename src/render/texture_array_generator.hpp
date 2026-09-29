#ifndef FARLANDS_TEXTURE_ARRAY_GENERATOR_HPP
#define FARLANDS_TEXTURE_ARRAY_GENERATOR_HPP
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <map>
#include <set>
#include <array>
#include <optional>
#include <cstddef>
#include "core/block_types.hpp"
#include "render/texture_pack_manager.hpp"

namespace VoxelEngine {

class TextureArrayGenerator {
private:
    std::map<godot::String, godot::String> texture_path_cache;
    size_t last_registry_count = 0;
    bool mipmaps_enabled_ = true;
    bool textures_enabled_ = true;
    bool compression_enabled_ = false;

    // Albedo texture state
    static inline godot::Ref<godot::Texture2DArray> s_global_texture_array;
    static inline std::map<godot::String, int> s_global_texture_name_to_index;
    static inline bool s_global_texture_initialized = false;

    // Emissive texture state
    static inline godot::Ref<godot::Texture2DArray> s_global_emissive_array;
    static inline std::map<godot::String, int> s_global_emissive_name_to_index;
    static inline bool s_global_emissive_initialized = false;

    [[nodiscard]] godot::String get_safe_texture_path(const godot::String& texture_name);

public:
    TextureArrayGenerator() = default;
    ~TextureArrayGenerator() = default;

    TextureArrayGenerator(const TextureArrayGenerator&) = delete;
    TextureArrayGenerator& operator=(const TextureArrayGenerator&) = delete;

    [[nodiscard]] static TextureArrayGenerator& get_instance();

    godot::Ref<godot::Texture2DArray> generate_texture_array();
    godot::Ref<godot::Texture2DArray> generate_emissive_texture_array();
    void populate_block_registry();
    void force_regenerate();
    // Drops the per-name resolved-path memo so a newly activated texture pack
    // takes effect on the next generation. force_regenerate() does NOT do this.
    void invalidate_texture_path_cache() { texture_path_cache.clear(); }
    [[nodiscard]] godot::Ref<godot::Texture2DArray> get_texture_array();
    [[nodiscard]] godot::Ref<godot::Texture2DArray> get_emissive_texture_array();
    [[nodiscard]] int get_texture_index(const godot::String& texture_name);
    [[nodiscard]] int get_emissive_texture_index(const godot::String& texture_name);
    // Layer index for a texture name, or -1 when the array holds no such layer.
    // This is NOT get_texture_index(): that one answers 0 for "unknown", which
    // is indistinguishable from the fallback layer, so a caller that WRITES
    // into the array (the Liquid Texture Lab) must use this one or it repaints
    [[nodiscard]] static int find_texture_layer(const godot::String& texture_name);
    [[nodiscard]] int get_block_texture_index(const godot::String& block_name, const godot::String& face);

    // Whether generated arrays call Image::generate_mipmaps(). Toggling after
    // first use regenerates the arrays, so materials must re-assign them
    // (MaterialManager::reload_textures()).
    static void set_mipmaps_enabled(bool enabled);
    [[nodiscard]] static bool is_mipmaps_enabled();

    // Whether generated arrays load the real block textures. When disabled,
    // every layer is a magenta/black checker placeholder. Toggling after first
    // use regenerates the arrays (MaterialManager::reload_textures()).
    static void set_textures_enabled(bool enabled);
    [[nodiscard]] static bool is_textures_enabled();

    // Whether generated arrays use GPU compression (BC7/S3TC/ETC2). When enabled,
    // textures are compressed using Image::compress() before array creation,
    // reducing VRAM by ~4-6x at the cost of lossy compression. Toggling after
    // first use regenerates the arrays (MaterialManager::reload_textures()).
    static void set_compression_enabled(bool enabled);
    [[nodiscard]] static bool is_compression_enabled();

    static void cleanup() {
        s_global_texture_array.unref();
        s_global_texture_name_to_index.clear();
        s_global_texture_initialized = false;
        s_global_emissive_array.unref();
        s_global_emissive_name_to_index.clear();
        s_global_emissive_initialized = false;
        get_instance().last_registry_count = 0;
        get_instance().texture_path_cache.clear();
    }
};

// -----------------------------------------------------------------------------
// Inline Implementation
// -----------------------------------------------------------------------------

// Only the singleton accessor stays inline: two lines, and every consumer calls
// it. Every other body — the array build, the normalisers and the layer lookups
// — is in render/texture_array_generator.cpp.
inline TextureArrayGenerator& TextureArrayGenerator::get_instance() {
    static TextureArrayGenerator instance;
    return instance;
}

} // namespace VoxelEngine

#endif // FARLANDS_TEXTURE_ARRAY_GENERATOR_HPP
