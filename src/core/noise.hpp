#ifndef FARLANDS_NOISE_HPP
#define FARLANDS_NOISE_HPP
#include <cstdint>
#include <cmath>
#include <array>

namespace VoxelEngine {

// -----------------------------------------------------------------------------
// Fast 2D/3D noise (Simplex + Perlin)
// -----------------------------------------------------------------------------
class FastNoise {
private:
    std::array<uint8_t, 512> perm;
    std::array<uint8_t, 512> perm_mod12;

    static constexpr float G2[4][2] = {
        { 1.0f,  1.0f},
        {-1.0f,  1.0f},
        { 1.0f, -1.0f},
        {-1.0f, -1.0f}
    };

    static constexpr float G3[12][3] = {
        { 1.0f,  1.0f,  0.0f}, {-1.0f,  1.0f,  0.0f},
        { 1.0f, -1.0f,  0.0f}, {-1.0f, -1.0f,  0.0f},
        { 1.0f,  0.0f,  1.0f}, {-1.0f,  0.0f,  1.0f},
        { 1.0f,  0.0f, -1.0f}, {-1.0f,  0.0f, -1.0f},
        { 0.0f,  1.0f,  1.0f}, { 0.0f, -1.0f,  1.0f},
        { 0.0f,  1.0f, -1.0f}, { 0.0f, -1.0f, -1.0f}
    };

    static inline int32_t fast_floor(float x) noexcept {
        int32_t ix = static_cast<int32_t>(x);
        return ix - (x < static_cast<float>(ix));
    }

    static inline float fade(float t) noexcept {
        return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
    }

    static inline float lerp(float a, float b, float t) noexcept {
        return a + t * (b - a);
    }

public:
    explicit FastNoise(int32_t seed = 0) noexcept {
        std::array<uint8_t, 256> permutation;
        for (int32_t i = 0; i < 256; ++i) {
            permutation[i] = static_cast<uint8_t>(i);
        }

        for (int32_t i = 255; i > 0; --i) {
            int32_t j = (seed + i) % (i + 1);
            uint8_t tmp = permutation[i];
            permutation[i] = permutation[j];
            permutation[j] = tmp;
        }

        for (int32_t i = 0; i < 512; ++i) {
            uint8_t v = permutation[i & 255];
            perm[i] = v;
            perm_mod12[i] = v % 12;
        }
    }

    [[nodiscard]] float noise_2d(float x, float y) const noexcept {
        int32_t ix = fast_floor(x);
        int32_t iy = fast_floor(y);

        float fx = x - static_cast<float>(ix);
        float fy = y - static_cast<float>(iy);

        int32_t X = ix & 255;
        int32_t Y = iy & 255;

        float u = fade(fx);
        float v = fade(fy);

        int32_t A = perm[X] + Y;
        int32_t B = perm[X + 1] + Y;

        int gi00 = perm[A] & 3;
        int gi10 = perm[B] & 3;
        int gi01 = perm[A + 1] & 3;
        int gi11 = perm[B + 1] & 3;

        float n00 = G2[gi00][0] * fx       + G2[gi00][1] * fy;
        float n10 = G2[gi10][0] * (fx - 1.0f) + G2[gi10][1] * fy;
        float n01 = G2[gi01][0] * fx       + G2[gi01][1] * (fy - 1.0f);
        float n11 = G2[gi11][0] * (fx - 1.0f) + G2[gi11][1] * (fy - 1.0f);

        return lerp(lerp(n00, n10, u), lerp(n01, n11, u), v);
    }

    [[nodiscard]] float noise_3d(float x, float y, float z) const noexcept {
        static constexpr float F3 = 1.0f / 3.0f;
        static constexpr float G3const = 1.0f / 6.0f;

        float s = (x + y + z) * F3;
        int32_t i = fast_floor(x + s);
        int32_t j = fast_floor(y + s);
        int32_t k = fast_floor(z + s);

        float t = static_cast<float>(i + j + k) * G3const;
        float X0 = static_cast<float>(i) - t;
        float Y0 = static_cast<float>(j) - t;
        float Z0 = static_cast<float>(k) - t;

        float x0 = x - X0;
        float y0 = y - Y0;
        float z0 = z - Z0;

        int32_t i1, j1, k1, i2, j2, k2;

        if (x0 >= y0) {
            if (y0 >= z0)      { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
            else if (x0 >= z0) { i1 = 1; j1 = 0; k1 = 0; i2 = 1; j2 = 0; k2 = 1; }
            else               { i1 = 0; j1 = 0; k1 = 1; i2 = 1; j2 = 0; k2 = 1; }
        } else {
            if (y0 < z0)       { i1 = 0; j1 = 0; k1 = 1; i2 = 0; j2 = 1; k2 = 1; }
            else if (x0 < z0)  { i1 = 0; j1 = 1; k1 = 0; i2 = 0; j2 = 1; k2 = 1; }
            else               { i1 = 0; j1 = 1; k1 = 0; i2 = 1; j2 = 1; k2 = 0; }
        }

        float x1 = x0 - static_cast<float>(i1) + G3const;
        float y1 = y0 - static_cast<float>(j1) + G3const;
        float z1 = z0 - static_cast<float>(k1) + G3const;

        float x2 = x0 - static_cast<float>(i2) + 2.0f * G3const;
        float y2 = y0 - static_cast<float>(j2) + 2.0f * G3const;
        float z2 = z0 - static_cast<float>(k2) + 2.0f * G3const;

        float x3 = x0 - 1.0f + 3.0f * G3const;
        float y3 = y0 - 1.0f + 3.0f * G3const;
        float z3 = z0 - 1.0f + 3.0f * G3const;

        int32_t ii = i & 255;
        int32_t jj = j & 255;
        int32_t kk = k & 255;

        float n0 = 0.0f, n1 = 0.0f, n2 = 0.0f, n3 = 0.0f;

        float t0 = 0.6f - x0 * x0 - y0 * y0 - z0 * z0;
        if (t0 >= 0.0f) {
            t0 *= t0;
            int gi0 = perm_mod12[ii + perm_mod12[jj + perm_mod12[kk]]];
            n0 = t0 * t0 * (G3[gi0][0] * x0 + G3[gi0][1] * y0 + G3[gi0][2] * z0);
        }

        float t1 = 0.6f - x1 * x1 - y1 * y1 - z1 * z1;
        if (t1 >= 0.0f) {
            t1 *= t1;
            int gi1 = perm_mod12[ii + i1 + perm_mod12[jj + j1 + perm_mod12[kk + k1]]];
            n1 = t1 * t1 * (G3[gi1][0] * x1 + G3[gi1][1] * y1 + G3[gi1][2] * z1);
        }

        float t2 = 0.6f - x2 * x2 - y2 * y2 - z2 * z2;
        if (t2 >= 0.0f) {
            t2 *= t2;
            int gi2 = perm_mod12[ii + i2 + perm_mod12[jj + j2 + perm_mod12[kk + k2]]];
            n2 = t2 * t2 * (G3[gi2][0] * x2 + G3[gi2][1] * y2 + G3[gi2][2] * z2);
        }

        float t3 = 0.6f - x3 * x3 - y3 * y3 - z3 * z3;
        if (t3 >= 0.0f) {
            t3 *= t3;
            int gi3 = perm_mod12[ii + 1 + perm_mod12[jj + 1 + perm_mod12[kk + 1]]];
            n3 = t3 * t3 * (G3[gi3][0] * x3 + G3[gi3][1] * y3 + G3[gi3][2] * z3);
        }

        return 32.0f * (n0 + n1 + n2 + n3);
    }

    [[nodiscard]] float fbm(float x, float y, int octaves, float persistence, float scale) const noexcept {
        float total = 0.0f;
        float frequency = scale;
        float amplitude = 1.0f;
        float max_value = 0.0f;

        for (int i = 0; i < octaves; ++i) {
            total += noise_2d(x * frequency, y * frequency) * amplitude;
            max_value += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / max_value;
    }

    [[nodiscard]] float fbm_3d(float x, float y, float z, int octaves, float persistence, float scale) const noexcept {
        float total = 0.0f;
        float frequency = scale;
        float amplitude = 1.0f;
        float max_value = 0.0f;

        for (int i = 0; i < octaves; ++i) {
            total += noise_3d(x * frequency, y * frequency, z * frequency) * amplitude;
            max_value += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / max_value;
    }

    [[nodiscard]] float ridged_noise(float x, float y, int octaves, float persistence, float scale) const noexcept {
        float total = 0.0f;
        float frequency = scale;
        float amplitude = 1.0f;
        float max_value = 0.0f;

        for (int i = 0; i < octaves; ++i) {
            float n = 1.0f - std::abs(noise_2d(x * frequency, y * frequency));
            n = n * n;
            total += n * amplitude;
            max_value += amplitude;
            amplitude *= persistence;
            frequency *= 2.0f;
        }

        return total / max_value;
    }
};

} // namespace VoxelEngine

#endif // FARLANDS_NOISE_HPP