#ifndef FARLANDS_TEXTURE_ARRAY_GENERATOR_BUILD_HPP
#define FARLANDS_TEXTURE_ARRAY_GENERATOR_BUILD_HPP

// Implementation half of texture_array_generator.hpp: the free image normalisers
// every layer goes through, the resolved-path memo, and the two entry points that
// only exist to serve the array build (populate_block_registry / force_regenerate).
// Included at the BOTTOM of that header, so it must not open namespace VoxelEngine.
//
// The helpers are file-local in effect — nothing outside this header uses them —
// and they live here so the class header stays a readable interface.
#include <cstdint>
#include <godot_cpp/classes/file_access.hpp>
#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/classes/texture2d_array.hpp>
#include <godot_cpp/core/error_macros.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include "core/block_types.hpp"
#include "render/texture_pack_manager.hpp"
#include "render/texture_array_generator.hpp"

namespace VoxelEngine {

// Forces an image into the requested mipmap state. Images returned by
// texture->get_image() may already carry mipmaps from the 3D texture import
// (while create_from_data images never do), so every image must be normalized
// explicitly or create_from_images() rejects the array as mixed usage.
inline void normalize_mipmaps(godot::Ref<godot::Image>& image, bool enabled) {
    if (image.is_null()) return;
    if (enabled) {
        if (!image->has_mipmaps()) {
            image->generate_mipmaps();
        }
    } else {
        if (image->has_mipmaps()) {
            image->clear_mipmaps();
        }
    }
}

// Normalizes image format to RGBA8. Texture packs may contain mixed formats
// (RGB8, RGBA8, etc.) which causes create_from_images() to fail. This ensures
// all layers share a consistent format.
inline void normalize_format(godot::Ref<godot::Image>& image) {
    if (image.is_null()) return;
    if (image->get_format() != godot::Image::FORMAT_RGBA8) {
        image->convert(godot::Image::FORMAT_RGBA8);
    }
}

// Applies GPU compression to an image if enabled. Uses S3TC (DXT1/DXT5, BC1/BC3)
// which provides ~4:1–6:1 compression at the cost of lossy artifacts.
inline void apply_compression(godot::Ref<godot::Image>& image, bool enabled) {
    if (image.is_null() || !enabled) return;
    // COMPRESS_S3TC uses DXT1/DXT5 (BC1/BC3), not BC7. For better quality
    // (especially gradients), use COMPRESS_BPTC instead, but S3TC has wider support.
    image->compress(godot::Image::COMPRESS_S3TC);
}

// Magenta/black checker placeholder used when textures are disabled.
inline godot::Ref<godot::Image> build_checker_image(int width, int height) {
    if (width <= 0 || height <= 0) {
        ERR_PRINT("Invalid checker image dimensions");
        width = 16;
        height = 16;
    }
    
    const int cell = 4;
    godot::PackedByteArray data;
    const int64_t expected_size = static_cast<int64_t>(width) * height * 4;
    data.resize(expected_size);
    
    if (data.size() != expected_size) {
        ERR_PRINT("Failed to allocate checker image data");
        return godot::Image::create(width, height, false, godot::Image::FORMAT_RGBA8);
    }
    
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int64_t idx = (static_cast<int64_t>(y) * width + x) * 4;
            if (idx + 3 >= data.size()) continue; // Safety check
            const bool magenta = ((x / cell + y / cell) % 2) == 0;
            if (magenta) {
                data[idx] = 255;
                data[idx + 1] = 0;
                data[idx + 2] = 255;
            } else {
                data[idx] = 0;
                data[idx + 1] = 0;
                data[idx + 2] = 0;
            }
            data[idx + 3] = 255;
        }
    }
    return godot::Image::create_from_data(width, height, false, godot::Image::FORMAT_RGBA8, data);
}

inline godot::String TextureArrayGenerator::get_safe_texture_path(const godot::String& texture_name) {
    auto it = texture_path_cache.find(texture_name);
    if (it != texture_path_cache.end()) {
        return it->second;
    }

    // Resolution stack (see TexturePackManager): active pack textures override
    // the built-in res://textures/blocks set, which itself falls back to
    // stone.png. The manager owns the path decision; the array build below is
    // unchanged.
    godot::String result = TexturePackManager::get_instance().resolve(texture_name);
    if (texture_name != "stone" && result == "res://textures/blocks/stone.png") {
        WARN_PRINT("Texture not found: " + texture_name + ", falling back to stone.png");
    }
    texture_path_cache.emplace(texture_name, result);
    return result;
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

} // namespace VoxelEngine

#endif // FARLANDS_TEXTURE_ARRAY_GENERATOR_BUILD_HPP
