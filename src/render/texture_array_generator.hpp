#ifndef FUK_MINECRAFT_TEXTURE_ARRAY_GENERATOR_HPP
#define FUK_MINECRAFT_TEXTURE_ARRAY_GENERATOR_HPP
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
#include <cstddef>
#include "core/block_types.hpp"

namespace VoxelEngine {

class TextureArrayGenerator {
private:
    std::map<godot::String, godot::String> texture_path_cache;
    size_t last_registry_count = 0;

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
    [[nodiscard]] godot::Ref<godot::Texture2DArray> get_texture_array();
    [[nodiscard]] godot::Ref<godot::Texture2DArray> get_emissive_texture_array();
    [[nodiscard]] int get_texture_index(const godot::String& texture_name);
    [[nodiscard]] int get_emissive_texture_index(const godot::String& texture_name);
    [[nodiscard]] int get_block_texture_index(const godot::String& block_name, const godot::String& face);

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
inline TextureArrayGenerator& TextureArrayGenerator::get_instance() {
    static TextureArrayGenerator instance;
    return instance;
}

inline godot::String TextureArrayGenerator::get_safe_texture_path(const godot::String& texture_name) {
    auto it = texture_path_cache.find(texture_name);
    if (it != texture_path_cache.end()) {
        return it->second;
    }

    godot::String primary_path = "res://textures/blocks/" + texture_name + ".png";
    godot::String result;
    if (godot::FileAccess::file_exists(primary_path)) {
        result = primary_path;
    } else {
        result = "res://textures/blocks/stone.png";
        WARN_PRINT("Texture not found: " + texture_name + ", falling back to stone.png");
    }
    texture_path_cache.emplace(texture_name, result);
    return result;
}

inline godot::Ref<godot::Texture2DArray> TextureArrayGenerator::generate_texture_array() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    // Collect unique texture names from all registered blocks
    std::set<godot::String> unique_textures;
    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        for (int f = 0; f < 6; ++f) {
            if (!bt.texture_names[f].empty()) {
                unique_textures.insert(godot::String(bt.texture_names[f].c_str()));
            }
        }
    }

    godot::PackedStringArray texture_paths;
    for (const godot::String& texture_name : unique_textures) {
        texture_paths.append(get_safe_texture_path(texture_name));
    }

    s_global_texture_array.instantiate();
    godot::ResourceLoader* loader = godot::ResourceLoader::get_singleton();

    if (texture_paths.size() == 0) {
        ERR_PRINT("No block textures available to load.");
        return s_global_texture_array;
    }

    godot::Ref<godot::Texture2D> base_texture = loader->load(texture_paths[0]);
    if (!base_texture.is_valid()) {
        ERR_PRINT("Failed to load base layout reference texture: " + texture_paths[0]);
        return s_global_texture_array;
    }

    godot::Ref<godot::Image> base_image = base_texture->get_image();
    const int width  = base_image->get_width();
    const int height = base_image->get_height();

    godot::Array textures;
    s_global_texture_name_to_index.clear();

    for (int i = 0; i < static_cast<int>(texture_paths.size()); ++i) {
        godot::Ref<godot::Texture2D> texture = loader->load(texture_paths[i]);
        if (!texture.is_valid()) {
            continue;
        }

        godot::Ref<godot::Image> image = texture->get_image();
        if (!image.is_valid()) {
            continue;
        }

        if (image->get_width() != width || image->get_height() != height) {
            image->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
        }

        image->generate_mipmaps();

        const int layer_index = static_cast<int>(textures.size());
        textures.append(image);

        godot::String file_name = texture_paths[i].get_file().get_basename();
        s_global_texture_name_to_index[file_name] = layer_index;
    }

    if (textures.size() > 0) {
        s_global_texture_array->create_from_images(textures);
        godot::print_line("Generated texture array with " + godot::String::num_int64(s_global_texture_array->get_layers()) + " layers (with mipmaps)");
    }

    return s_global_texture_array;
}

inline godot::Ref<godot::Texture2DArray> TextureArrayGenerator::generate_emissive_texture_array() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    // Collect unique emissive texture names (excluding empty)
    std::set<godot::String> unique_emissive;
    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        for (int f = 0; f < 6; ++f) {
            if (!bt.emissive_texture_names[f].empty()) {
                unique_emissive.insert(godot::String(bt.emissive_texture_names[f].c_str()));
            }
        }
    }

    s_global_emissive_array.instantiate();
    s_global_emissive_name_to_index.clear();

    // Determine target resolution from an albedo texture (reuse width/height)
    int target_width = 16;
    int target_height = 16;
    if (s_global_texture_array.is_valid() && s_global_texture_array->get_layers() > 0) {
        godot::Ref<godot::Image> ref_img = s_global_texture_array->get_layer_data(0);
        if (ref_img.is_valid()) {
            target_width = ref_img->get_width();
            target_height = ref_img->get_height();
        }
    }

    // Layer 0 = solid black (no emissive contribution)
    godot::PackedByteArray black_data;
    black_data.resize(static_cast<int64_t>(target_width) * target_height * 4);
    black_data.fill(0);
    // Alpha = 255 so emissive.rgb * emissive.a doesn't multiply by zero-alpha edge cases
    for (int i = 3; i < static_cast<int>(black_data.size()); i += 4) {
        black_data[i] = 255;
    }
    godot::Ref<godot::Image> black_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, black_data);
    black_image->generate_mipmaps();

    godot::Array images;
    images.append(black_image);

    if (unique_emissive.empty()) {
        s_global_emissive_array->create_from_images(images);
        s_global_emissive_initialized = true;
        return s_global_emissive_array;
    }

    godot::ResourceLoader* loader = godot::ResourceLoader::get_singleton();

    for (const godot::String& tex_name : unique_emissive) {
        godot::String path = "res://textures/blocks/" + tex_name + ".png";
        godot::Ref<godot::Image> emissive_image;

        if (godot::FileAccess::file_exists(path)) {
            godot::Ref<godot::Texture2D> tex = loader->load(path);
            if (tex.is_valid()) {
                emissive_image = tex->get_image();
            }
        }

        if (!emissive_image.is_valid()) {
            // Missing emissive texture → fall back to black (no glow)
            godot::PackedByteArray fb_data;
            fb_data.resize(static_cast<int64_t>(target_width) * target_height * 4);
            fb_data.fill(0);
            for (int i = 3; i < static_cast<int>(fb_data.size()); i += 4) {
                fb_data[i] = 255;
            }
            emissive_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, fb_data);
        } else {
            if (emissive_image->get_width() != target_width || emissive_image->get_height() != target_height) {
                emissive_image->resize(target_width, target_height, godot::Image::INTERPOLATE_NEAREST);
            }
        }

        emissive_image->generate_mipmaps();

        const int layer_index = static_cast<int>(images.size());
        images.append(emissive_image);

        const godot::String& file_name = tex_name;
        s_global_emissive_name_to_index[file_name] = layer_index;
    }

    s_global_emissive_array->create_from_images(images);
    s_global_emissive_initialized = true;
    godot::print_line("Generated emissive texture array with " + godot::String::num_int64(s_global_emissive_array->get_layers()) + " layers");

    return s_global_emissive_array;
}

inline void TextureArrayGenerator::populate_block_registry() {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    if (block_count == last_registry_count) {
        return;
    }
    last_registry_count = block_count;

    for (size_t i = 0; i < block_count; ++i) {
        BlockType* block = registry.get_block_mutable(static_cast<BlockID>(i));
        if (!block || !block->name) {
            continue;
        }

        for (int f = 0; f < 6; ++f) {
            if (block->texture_names[f].empty()) {
                block->texture_indices[f] = 0;
            } else {
                block->texture_indices[f] = get_texture_index(
                    godot::String(block->texture_names[f].c_str()));
            }

            if (block->emissive_texture_names[f].empty()) {
                block->emissive_texture_indices[f] = 0;
            } else {
                block->emissive_texture_indices[f] = get_emissive_texture_index(
                    godot::String(block->emissive_texture_names[f].c_str()));
            }
        }
    }
}

inline void TextureArrayGenerator::force_regenerate() {
    s_global_texture_array.unref();
    s_global_texture_initialized = false;
    s_global_emissive_array.unref();
    s_global_emissive_initialized = false;
    last_registry_count = 0;
    generate_texture_array();
    generate_emissive_texture_array();
    populate_block_registry();
    s_global_texture_initialized = true;
    s_global_emissive_initialized = true;
}

inline godot::Ref<godot::Texture2DArray> TextureArrayGenerator::get_texture_array() {
    if (!s_global_texture_array.is_valid() || !s_global_texture_initialized) {
        generate_texture_array();
        populate_block_registry();
        s_global_texture_initialized = true;
    } else {
        populate_block_registry();
    }
    return s_global_texture_array;
}

inline godot::Ref<godot::Texture2DArray> TextureArrayGenerator::get_emissive_texture_array() {
    if (!s_global_emissive_array.is_valid() || !s_global_emissive_initialized) {
        generate_emissive_texture_array();
    }
    return s_global_emissive_array;
}

inline int TextureArrayGenerator::get_texture_index(const godot::String& texture_name) {
    auto it = s_global_texture_name_to_index.find(texture_name);
    if (it != s_global_texture_name_to_index.end()) {
        return it->second;
    }

    godot::String safe_fallback = get_safe_texture_path(texture_name).get_file().get_basename();
    it = s_global_texture_name_to_index.find(safe_fallback);
    if (it != s_global_texture_name_to_index.end()) {
        return it->second;
    }

    return 0;
}

inline int TextureArrayGenerator::get_emissive_texture_index(const godot::String& texture_name) {
    auto it = s_global_emissive_name_to_index.find(texture_name);
    if (it != s_global_emissive_name_to_index.end()) {
        return it->second;
    }
    return 0;
}

inline int TextureArrayGenerator::get_block_texture_index(const godot::String& block_name, const godot::String& face) {
    BlockRegistry& registry = BlockRegistry::get_instance();
    const size_t block_count = registry.get_count();

    for (size_t i = 0; i < block_count; ++i) {
        const BlockType& bt = registry.get_block_fast(static_cast<BlockID>(i));
        if (bt.name && block_name == bt.name) {
            int face_idx = 2; // default to top
            if (face == "right")  face_idx = 0;
            if (face == "left")   face_idx = 1;
            if (face == "top")    face_idx = 2;
            if (face == "bottom") face_idx = 3;
            if (face == "front")  face_idx = 4;
            if (face == "back")   face_idx = 5;

            if (bt.texture_names[face_idx].empty()) return 0;
            return get_texture_index(godot::String(bt.texture_names[face_idx].c_str()));
        }
    }
    return 0;
}

} // namespace VoxelEngine

#endif // FUK_MINECRAFT_TEXTURE_ARRAY_GENERATOR_HPP
