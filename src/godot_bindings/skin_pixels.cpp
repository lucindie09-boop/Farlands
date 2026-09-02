#include "godot_bindings/skin_pixels.hpp"

#include "core/skin_pixel_math.hpp"

#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>

#include <cstring>
#include <vector>

using namespace godot;
using namespace VoxelEngine;

namespace {
// uint8_t(CLAMP(color * 255.0, 0, 255)) — the same byte a Godot RGBA8
// Image::set_pixel writes for a float channel.
uint8_t u8_clamped(float channel) {
    const double v = static_cast<double>(channel) * 255.0;
    return v <= 0.0 ? 0u : (v >= 255.0 ? 255u : static_cast<uint8_t>(v));
}
} // namespace

Ref<Image> SkinPixels::make_noise_map(int32_t dim, int64_t seed) {
    if (dim <= 0) {
        return Ref<Image>();
    }
    GodotRng rng;
    rng.seed(static_cast<uint64_t>(static_cast<uint32_t>(seed)));

    const int64_t total = static_cast<int64_t>(dim) * dim * 4;
    PackedByteArray bytes;
    bytes.resize(total);
    uint8_t* p = bytes.ptrw();
    for (int64_t i = 0; i < total; i += 4) {
        const double q = rng.randf();
        // Color(float(q), 0, 0, 1) -> uint8_t(CLAMP(r * 255.0, 0, 255)).
        const double rv = static_cast<double>(static_cast<float>(q)) * 255.0;
        p[i] = rv <= 0.0 ? 0u : (rv >= 255.0 ? 255u : static_cast<uint8_t>(rv));
        p[i + 1] = 0;
        p[i + 2] = 0;
        p[i + 3] = 255;
    }
    return Image::create_from_data(dim, dim, false, Image::FORMAT_RGBA8, bytes);
}

Ref<Image> SkinPixels::apply_gray_noise(const Ref<Image>& base, const Ref<Image>& noise_map, double amount) {
    if (base.is_null() || base->is_empty() || noise_map.is_null() || noise_map->is_empty()) {
        return Ref<Image>();
    }
    const int32_t w = base->get_width();
    const int32_t h = base->get_height();
    if (w <= 0 || h <= 0) {
        return Ref<Image>();
    }
    const int32_t nw = noise_map->get_width();
    const int32_t nh = noise_map->get_height();
    if (nw <= 0 || nh <= 0) {
        return Ref<Image>();
    }
    const int64_t texels = static_cast<int64_t>(w) * h;
    const int64_t total = texels * 4;

    // Snap the source to an RGBA8 level-0 byte buffer (mirrors the colours
    // Image.get_pixel would return, byte-writes use the same truncation).
    std::vector<uint8_t> rgba(static_cast<size_t>(total));
    const PackedByteArray raw = base->get_data();
    if (base->get_format() == Image::FORMAT_RGBA8 && !base->has_mipmaps() && raw.size() == total) {
        std::memcpy(rgba.data(), raw.ptr(), static_cast<size_t>(total));
    } else {
        for (int64_t t = 0; t < texels; ++t) {
            const Color c = base->get_pixel(static_cast<int>(t % w), static_cast<int>(t / w));
            rgba[t * 4 + 0] = u8_clamped(c.r);
            rgba[t * 4 + 1] = u8_clamped(c.g);
            rgba[t * 4 + 2] = u8_clamped(c.b);
            rgba[t * 4 + 3] = u8_clamped(c.a);
        }
    }

    // Per-texel red-channel noise bytes (wrapped so either image can be a
    // different size; the noise field was always 64x64 in the original code).
    std::vector<uint8_t> noise_r(static_cast<size_t>(texels));
    const PackedByteArray noise_raw = noise_map->get_data();
    const int64_t noise_texels = static_cast<int64_t>(nw) * nh;
    const bool noise_is_byte_ok =
            noise_map->get_format() == Image::FORMAT_RGBA8 && !noise_map->has_mipmaps() && noise_raw.size() >= noise_texels * 4;
    for (int64_t t = 0; t < texels; ++t) {
        const int64_t tx = t % w;
        const int64_t ty = t / w;
        const int64_t nt = (tx % nw) + (ty % nh) * nw;
        if (noise_is_byte_ok) {
            noise_r[static_cast<size_t>(t)] = noise_raw[nt * 4];
        } else {
            const Color c = noise_map->get_pixel(static_cast<int>(tx % nw), static_cast<int>(ty % nh));
            noise_r[static_cast<size_t>(t)] = u8_clamped(c.r);
        }
    }

    apply_gray_noise_8(rgba.data(), static_cast<int>(texels), noise_r.data(), amount);

    PackedByteArray out_bytes;
    out_bytes.resize(total);
    std::memcpy(out_bytes.ptrw(), rgba.data(), static_cast<size_t>(total));
    return Image::create_from_data(w, h, false, Image::FORMAT_RGBA8, out_bytes);
}

PackedInt32Array SkinPixels::uv_texel_bounds(double lo_x, double lo_y, double hi_x, double hi_y, int32_t dim) {
    const TexelRange r = uv_texel_range(lo_x, lo_y, hi_x, hi_y, dim);
    PackedInt32Array arr;
    arr.resize(4);
    arr.set(0, r.x0);
    arr.set(1, r.y0);
    arr.set(2, r.x1);
    arr.set(3, r.y1);
    return arr;
}

void SkinPixels::_bind_methods() {
    ClassDB::bind_static_method("SkinPixels", D_METHOD("make_noise_map", "dim", "seed"), &SkinPixels::make_noise_map);
    ClassDB::bind_static_method("SkinPixels", D_METHOD("apply_gray_noise", "base", "noise_map", "amount"), &SkinPixels::apply_gray_noise);
    ClassDB::bind_static_method("SkinPixels", D_METHOD("uv_texel_bounds", "lo_x", "lo_y", "hi_x", "hi_y", "dim"), &SkinPixels::uv_texel_bounds);
}