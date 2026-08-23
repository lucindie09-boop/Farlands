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
inline TextureArrayGenerator& TextureArrayGenerator::get_instance() {
    static TextureArrayGenerator instance;
    return instance;
}

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

    // Array layer resolution comes from the active pack's declared
    // base_resolution (default 16), not from whichever texture sorts first.
    // Pack and built-in layers are all resized to it with nearest-neighbour.
    const int width  = TexturePackManager::get_instance().get_base_resolution();
    const int height = width;

    godot::Array textures;
    s_global_texture_name_to_index.clear();

    for (int i = 0; i < static_cast<int>(texture_paths.size()); ++i) {
        const godot::String& path = texture_paths[i];
        
        godot::Ref<godot::Image> image;
        if (textures_enabled_) {
            if (path.begins_with("user://")) {
                // Pack textures live outside the import system: load the raw
                // PNG directly so we don't hit the ResourceLoader cache or the
                // exported .pck import remap.
                image = godot::Image::load_from_file(path);
            } else {
                godot::Ref<godot::Texture2D> texture = loader->load(path);
                if (texture.is_valid()) {
                    image = texture->get_image();
                }
            }
            
            if (!image.is_valid()) {
                // Corrupt/truncated pack PNG: degrade to the built-in texture
                // instead of dropping the layer (which would map the name to
                // layer 0/stone and look wrong).
                const godot::String builtin = "res://textures/blocks/" + texture_paths[i].get_file().get_basename() + ".png";
                godot::Ref<godot::Texture2D> fallback = loader->load(builtin);
                if (fallback.is_valid()) {
                    image = fallback->get_image();
                }
            }
            if (!image.is_valid()) {
                WARN_PRINT("Failed to load texture: " + texture_paths[i] + ", skipping layer");
                continue;
            }

            // Validate image dimensions before resize
            const int img_width = image->get_width();
            const int img_height = image->get_height();
            
            if (img_width <= 0 || img_height <= 0) {
                WARN_PRINT("Invalid image dimensions for: " + texture_paths[i] + ", skipping layer");
                continue;
            }

            if (img_width != width || img_height != height) {
                image->resize(width, height, godot::Image::INTERPOLATE_NEAREST);
            }
        } else {
            image = build_checker_image(width, height);
        }

        if (!image.is_valid()) {
            WARN_PRINT("Image became invalid during processing: " + texture_paths[i] + ", skipping layer");
            continue;
        }

        // Normalize format to RGBA8
        normalize_format(image);
        
        // Force mipmap consistency: all images must have the same mipmap state
        if (mipmaps_enabled_) {
            if (!image->has_mipmaps()) {
                image->generate_mipmaps();
            }
        } else {
            if (image->has_mipmaps()) {
                // Create a new image without mipmaps
                godot::Ref<godot::Image> new_image = godot::Image::create(image->get_width(), image->get_height(), false, image->get_format());
                new_image->copy_from(image);
                image = new_image;
            }
        }
        
        apply_compression(image, compression_enabled_);

        const int layer_index = static_cast<int>(textures.size());
        textures.append(image);

        godot::String file_name = texture_paths[i].get_file().get_basename();
        s_global_texture_name_to_index[file_name] = layer_index;
    }

    if (textures.size() > 0) {
        s_global_texture_array->create_from_images(textures);
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
    const int64_t black_size = static_cast<int64_t>(target_width) * target_height * 4;
    black_data.resize(black_size);
    if (black_data.size() != black_size) {
        ERR_PRINT("Failed to allocate emissive black layer");
        target_width = 16;
        target_height = 16;
        black_data.resize(static_cast<int64_t>(16) * 16 * 4);
    }
    black_data.fill(0);
    // Alpha = 255 so emissive.rgb * emissive.a doesn't multiply by zero-alpha edge cases
    for (int i = 3; i < static_cast<int>(black_data.size()); i += 4) {
        black_data[i] = 255;
    }
    godot::Ref<godot::Image> black_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, black_data);
    normalize_format(black_image);
    normalize_mipmaps(black_image, mipmaps_enabled_);
    apply_compression(black_image, compression_enabled_);

    godot::Array images;
    images.append(black_image);

    if (unique_emissive.empty()) {
        s_global_emissive_array->create_from_images(images);
        s_global_emissive_initialized = true;
        return s_global_emissive_array;
    }

    godot::ResourceLoader* loader = godot::ResourceLoader::get_singleton();

    for (const godot::String& tex_name : unique_emissive) {
        // Packs may override emissive textures too, but a missing emissive is
        // NOT a stone fallback — it means "no glow" (black below).
        std::optional<godot::String> path = TexturePackManager::get_instance().resolve_optional(tex_name);
        godot::Ref<godot::Image> emissive_image;

        if (textures_enabled_ && path.has_value()) {
            if (path->begins_with("user://")) {
                emissive_image = godot::Image::load_from_file(*path);
            } else {
                godot::Ref<godot::Texture2D> tex = loader->load(*path);
                if (tex.is_valid()) {
                    emissive_image = tex->get_image();
                }
            }
        }

        if (!emissive_image.is_valid()) {
            // Missing emissive texture → fall back to black (no glow)
            godot::PackedByteArray fb_data;
            const int64_t fb_size = static_cast<int64_t>(target_width) * target_height * 4;
            fb_data.resize(fb_size);
            if (fb_data.size() != fb_size) {
                ERR_PRINT("Failed to allocate emissive fallback layer");
                continue;
            }
            fb_data.fill(0);
            for (int i = 3; i < static_cast<int>(fb_data.size()); i += 4) {
                fb_data[i] = 255;
            }
            emissive_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, fb_data);
        } else {
            // Validate image dimensions before resize
            if (emissive_image->get_width() <= 0 || emissive_image->get_height() <= 0) {
                WARN_PRINT("Invalid emissive image dimensions for: " + tex_name + ", using fallback");
                godot::PackedByteArray fb_data;
                const int64_t fb_size = static_cast<int64_t>(target_width) * target_height * 4;
                fb_data.resize(fb_size);
                fb_data.fill(0);
                for (int i = 3; i < static_cast<int>(fb_data.size()); i += 4) {
                    fb_data[i] = 255;
                }
                emissive_image = godot::Image::create_from_data(target_width, target_height, false, godot::Image::FORMAT_RGBA8, fb_data);
            } else if (emissive_image->get_width() != target_width || emissive_image->get_height() != target_height) {
                emissive_image->resize(target_width, target_height, godot::Image::INTERPOLATE_NEAREST);
            }
        }

        if (!emissive_image.is_valid()) {
            WARN_PRINT("Emissive image became invalid during processing: " + tex_name + ", skipping layer");
            continue;
        }

        normalize_format(emissive_image);
        normalize_mipmaps(emissive_image, mipmaps_enabled_);
        apply_compression(emissive_image, compression_enabled_);

        const int layer_index = static_cast<int>(images.size());
        images.append(emissive_image);

        const godot::String& file_name = tex_name;
        s_global_emissive_name_to_index[file_name] = layer_index;
    }

    s_global_emissive_array->create_from_images(images);
    s_global_emissive_initialized = true;

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

inline bool TextureArrayGenerator::is_mipmaps_enabled() {
    return get_instance().mipmaps_enabled_;
}

inline void TextureArrayGenerator::set_mipmaps_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.mipmaps_enabled_ == enabled) return;
    gen.mipmaps_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
}

inline bool TextureArrayGenerator::is_textures_enabled() {
    return get_instance().textures_enabled_;
}

inline void TextureArrayGenerator::set_textures_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.textures_enabled_ == enabled) return;
    gen.textures_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
}

inline bool TextureArrayGenerator::is_compression_enabled() {
    return get_instance().compression_enabled_;
}

inline void TextureArrayGenerator::set_compression_enabled(bool enabled) {
    TextureArrayGenerator& gen = get_instance();
    if (gen.compression_enabled_ == enabled) return;
    gen.compression_enabled_ = enabled;
    if (s_global_texture_initialized || s_global_emissive_initialized) {
        gen.force_regenerate();
    }
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

#endif // FARLANDS_TEXTURE_ARRAY_GENERATOR_HPP
