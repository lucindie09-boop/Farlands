#ifndef FARLANDS_CORE_SKIN_PIXEL_MATH_HPP
#define FARLANDS_CORE_SKIN_PIXEL_MATH_HPP

#include <cstdint>

// Pure, Godot-free pixel/noise math ported out of skin_manager.gd. The thin
// GDScript autoload keeps all Godot state (image, texture, noise base,
// severities, save timer) and feeds raw byte buffers into these routines so
// every trick is doctest-covered without an engine runtime.

namespace VoxelEngine {

// Faithful port of Godot's RandomNumberGenerator, which wraps a PCG32
// generator. It matches the engine bit-for-bit so regenerated noise maps keep
// the exact grain the original GDScript produced from `rng.seed = N`;
// `pcg32_srandom_r` + `RandomPCG::randf()` (LDEXP over a float-rounded 32-bit
// significand with a CLZ exponent offset) are reproduced exactly.
class GodotRng {
public:
    static constexpr uint64_t kPcgDefaultInc = 1442695040888963407ULL;

    GodotRng() = default;

    // Equivalent to GDScript `rng.seed = p_seed`.
    void seed(uint64_t p_seed);

    // raw PCG32 output (one `rng.rand()` advance).
    uint32_t next_u32();

    // Equivalent to GDScript `rng.randf()`, a double-widened float in [0, 1).
    double randf();

private:
    uint32_t advance();
    uint64_t state_ = 0;
    uint64_t inc_ = 0;
};

// Inclusive texel rectangle for a UV-space half-open box. Mirrors the
// GDScript `fill_uv_rect` clamping: `ceil(uv * dim - 0.5)` picks texels by
// centre position so clamping to a face edge never bleeds a column into a
// neighbouring island. `valid()` is false when the rect is empty (the returned
// bounds are clamped to the image, so callers must test validity, not rays).
struct TexelRange {
    int x0 = 0;
    int y0 = 0;
    int x1 = -1;
    int y1 = -1;

    bool valid() const { return x1 >= x0 && y1 >= y0; }
};

TexelRange uv_texel_range(double lo_x, double lo_y, double hi_x, double hi_y, int dim);

// Applies monochromatic grain in-place to an RGBA8 (`texels * 4` bytes) image.
// `noise_r` holds one red-channel byte per texel; the same delta
// `(noise/255 - 0.5) * 2 * amount` is added (clamped) to every RGB channel,
// alpha is left untouched. The float/double choreography mirrors the original
// GDScript (`Image.get_pixel`/`set_pixel` round-trips), so byte-for-byte
// output matches what the script produced.
void apply_gray_noise_8(uint8_t* rgba, int texels, const uint8_t* noise_r, double amount);

} // namespace VoxelEngine

#endif // FARLANDS_CORE_SKIN_PIXEL_MATH_HPP