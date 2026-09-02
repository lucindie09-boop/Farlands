#include "core/skin_pixel_math.hpp"

#include <cmath>

namespace VoxelEngine {

constexpr uint64_t kPcgMultiplier = 6364136223846793005ULL;

void GodotRng::seed(uint64_t p_seed) {
    // pcg32_srandom_r as invoked by RandomPCG::seed() with Godot's default
    // increment: two advances are consumed during initialisation.
    state_ = 0;
    inc_ = (kPcgDefaultInc << 1u) | 1u;
    advance();
    state_ += p_seed;
    advance();
}

uint32_t GodotRng::advance() {
    const uint64_t old = state_;
    state_ = old * kPcgMultiplier + (inc_ | 1u);
    const uint32_t xorshifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
    const uint32_t rot = static_cast<uint32_t>(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32u - rot) & 31u));
}

uint32_t GodotRng::next_u32() {
    return advance();
}

namespace {
// Count leading zero bits (same result as __builtin_clz / _BitScanReverse).
int clz32(uint32_t x) {
    int n = 0;
    if ((x & 0xFFFF0000u) == 0) {
        n += 16;
        x <<= 16;
    }
    if ((x & 0xFF000000u) == 0) {
        n += 8;
        x <<= 8;
    }
    if ((x & 0xF0000000u) == 0) {
        n += 4;
        x <<= 4;
    }
    if ((x & 0xC0000000u) == 0) {
        n += 2;
        x <<= 2;
    }
    if ((x & 0x80000000u) == 0) {
        n += 1;
    }
    return n;
}
} // namespace

double GodotRng::randf() {
    // RandomPCG::randf(): the raw output picks the exponent via CLZ, the next
    // output provides the significand (rounded to float first, like Godot).
    const uint32_t proto_exp_offset = advance();
    if (proto_exp_offset == 0) {
        return 0.0;
    }
    const float significand = static_cast<float>(advance() | 0x80000001u);
    return std::ldexp(significand, -32 - clz32(proto_exp_offset));
}

TexelRange uv_texel_range(double lo_x, double lo_y, double hi_x, double hi_y, int dim) {
    if (dim <= 0) {
        return TexelRange();
    }
    const int min_idx = 0;
    const int max_idx = dim - 1;
    auto clamp_idx = [min_idx, max_idx](long long v) {
        return v < min_idx ? min_idx : (v > max_idx ? max_idx : static_cast<int>(v));
    };
    TexelRange r;
    r.x0 = clamp_idx(static_cast<long long>(std::ceil(lo_x * dim - 0.5)));
    r.y0 = clamp_idx(static_cast<long long>(std::ceil(lo_y * dim - 0.5)));
    r.x1 = clamp_idx(static_cast<long long>(std::ceil(hi_x * dim - 0.5)) - 1);
    r.y1 = clamp_idx(static_cast<long long>(std::ceil(hi_y * dim - 0.5)) - 1);
    return r;
}

void apply_gray_noise_8(uint8_t* rgba, int texels, const uint8_t* noise_r, double amount) {
    for (int i = 0; i < texels; ++i) {
        // `Image.get_pixel().r` returns float(byte / 255.0); GDScript then
        // widens to double for `(r - 0.5) * 2.0 * amount`.
        const double noise_v = static_cast<double>(static_cast<float>(noise_r[i] / 255.0f));
        const double delta = (noise_v - 0.5) * 2.0 * amount;
        for (int ch = 0; ch < 3; ++ch) {
            uint8_t& out = rgba[i * 4 + ch];
            // base colour read back as float(byte / 255.0), widened to double.
            const double base_v = static_cast<double>(static_cast<float>(out / 255.0f));
            double v = base_v + delta;
            v = v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v);
            // Color(float) narrowing, then uint8_t(CLAMP(c * 255.0, 0, 255)).
            const double scaled = static_cast<double>(static_cast<float>(v)) * 255.0;
            out = scaled <= 0.0 ? 0u : (scaled >= 255.0 ? 255u : static_cast<uint8_t>(scaled));
        }
    }
}

} // namespace VoxelEngine