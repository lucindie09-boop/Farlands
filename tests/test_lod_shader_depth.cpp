// The far mode's material must WRITE DEPTH, and writing ALPHA is what stops it.
//
// The far field is one merged mesh per spacing level, which is what took its cost
// from ~130 draw calls to a handful (docs/lod-modes.md). A mesh per tile had been
// ordered for free -- the transparent pipeline sorts instances back to front -- and
// a mesh that contains everything is sorted against nothing. So if the material
// does not write depth, its triangles land in index order and a ridge drawn after
// the ridge in front of it shows through it: "terrain visible through other
// terrain", on every steep cell, in motion.
//
// Godot decides that from the shader TEXT: a spatial shader that writes ALPHA goes
// down the transparent pipeline, whose contract is no depth write, which is why
// `depth_draw_opaque` ("write depth for opaque materials") had nothing to write.
// The world's terrain shader writes ALPHA = 1.0 too and gets away with it only
// because each chunk is its own instance and is sorted.
//
// So the two things below are asserted against the shader's own source, because
// they are declared nowhere else and nothing in C++ can override them:
//
//   1. the render_mode line forces the depth write unconditionally, and
//   2. no line assigns ALPHA -- an ALPHA write would put the material back in the
//      transparent pipeline whatever the depth mode says.
//
// probes/probe_lod_depth.gd measures the consequence in a rendered frame: two
// overlapping quads in ONE mesh, and which of them wins the pixels. This runs
// everywhere the suite runs, including CI, where no probe can.

#include "doctest.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

bool read_text(const std::string& path, std::string& out) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return false;
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    out = buffer.str();
    return true;
}

// The far mode's shader, wherever the suite's working directory puts it.
bool load_lod_shader(std::string& out) {
    return read_text("shaders/lod_grid.gdshader", out) ||
           read_text("../shaders/lod_grid.gdshader", out);
}

// ...and the half that packs the vertex attributes the shader takes apart. The two
// are one contract with a file boundary in the middle of it, so the test reads both.
bool load_lod_upload(std::string& out) {
    return read_text("src/lod/lod_grid_upload.cpp", out) ||
           read_text("../src/lod/lod_grid_upload.cpp", out);
}

// The value of a `const float NAME = <number>;` in the shader's text, NaN when the
// declaration is not there. NaN compares false against everything, so an assertion
// about a missing constant fails rather than passing on a default.
float named_float(const std::string& text, const std::string& name) {
    const size_t at = text.find(name + " = ");
    if (at == std::string::npos) return std::nanf("");
    const size_t start = at + name.size() + 3;
    const size_t end = text.find(';', start);
    if (end == std::string::npos) return std::nanf("");
    return std::strtof(text.substr(start, end - start).c_str(), nullptr);
}

std::string strip_comment(const std::string& line) {
    const size_t comment = line.find("//");
    return comment == std::string::npos ? line : line.substr(0, comment);
}

bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// True when the code assigns ALPHA (not ALPHA_HASH_SCALE, not a comparison).
bool writes_alpha(const std::string& shader) {
    std::istringstream stream(shader);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string code = strip_comment(line);
        const size_t at = code.find("ALPHA");
        if (at == std::string::npos) continue;
        if (at > 0 && is_ident_char(code[at - 1])) continue;
        size_t next = at + 5;
        if (next < code.size() && is_ident_char(code[next])) continue;
        while (next < code.size() && code[next] == ' ') ++next;
        if (next >= code.size() || code[next] != '=') continue;
        // `==` is a test, and `=` is a write.
        if (next + 1 < code.size() && code[next + 1] == '=') continue;
        return true;
    }
    return false;
}

// The code lines (comments stripped) that mention a token, for the assertions that
// are about how one statement is built rather than about a word appearing anywhere.
std::vector<std::string> code_lines_with(const std::string& shader, const std::string& needle) {
    std::vector<std::string> out;
    std::istringstream stream(shader);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string code = strip_comment(line);
        if (code.find(needle) != std::string::npos) out.push_back(code);
    }
    return out;
}

bool render_mode_has(const std::string& shader, const std::string& mode) {
    std::istringstream stream(shader);
    std::string line;
    while (std::getline(stream, line)) {
        const std::string code = strip_comment(line);
        if (code.find("render_mode") == std::string::npos) continue;
        return code.find(mode) != std::string::npos;
    }
    return false;
}

} // namespace

TEST_CASE("lod shader: the far mode's material writes depth") {
    std::string shader;
    if (!load_lod_shader(shader)) {
        MESSAGE("shaders/lod_grid.gdshader not found; the far mode's material was not checked");
        return;
    }
    // The unconditional depth write. `depth_draw_opaque` is the default and reads
    // like the right answer for an opaque material, which is exactly the trap: the
    // ALPHA write below is what made it mean nothing.
    CHECK(render_mode_has(shader, "depth_draw_always"));
    // Both faces are drawn, because a height field cell whose corners form a saddle
    // can tilt one of its two triangles past the viewer and depth is what settles
    // which one is in front.
    CHECK(render_mode_has(shader, "cull_disabled"));
    // ...and the material must stay OPAQUE: an ALPHA write is the transparent
    // pipeline, and the transparent pipeline is where the depth write goes.
    CHECK_FALSE(writes_alpha(shader));
}

TEST_CASE("lod shader: the detail term cannot move the average colour") {
    // The far field read as one flat colour per biome ("just yellow or green"),
    // because a cell is 32 to 256 blocks wide and the texture coordinate IS the world
    // coordinate: the sampler legitimately lands on the mip that has averaged the
    // whole 16x16 face away. The average is the right colour, so what this shader adds
    // back has to be a RATIO -- the face at a fixed world scale, divided by that same
    // scale's average -- and never an offset. Two consequences, both of them asserted
    // here because neither can be seen from C++:
    //
    //   1. the composition is a multiplication by something whose neutral value is 1,
    //      so the block's own colour is redistributed and its mean cannot drift, and
    //   2. the divisor is a sample at the mip that IS the face's average
    //      (DETAIL_AVERAGE_MIP), which is what keeps the mean: a ratio against a
    //      constant is a redistribution of the face's own colour, not a repaint of it.
    //      (That divisor is NOT a fade, which an earlier version of this comment
    //      claimed: the two samples only meet once a face texel is a pixel, kilometres
    //      past the band where the term is drawn. The fade is its own case below.)
    std::string shader;
    if (!load_lod_shader(shader)) {
        MESSAGE("shaders/lod_grid.gdshader not found; the far mode's material was not checked");
        return;
    }
    CHECK(shader.find("DETAIL_AVERAGE_MIP") != std::string::npos);
    CHECK(shader.find("detail_scale") != std::string::npos);
    CHECK(shader.find("detail_strength") != std::string::npos);

    // The average the ratio is divided by is read with an explicit LOD, not through
    // the sampler's own choice -- that is the whole point of it being the average.
    const std::vector<std::string> average_lines = code_lines_with(shader, "textureLod");
    CHECK(average_lines.size() == 1);
    for (const std::string& line : average_lines) {
        CHECK(line.find("DETAIL_AVERAGE_MIP") != std::string::npos);
        CHECK(line.find('*') == std::string::npos);  // a sample, not a scale of one
    }

    // The tiled coordinate: the detail is read at the world coordinate over the
    // detail scale, so its features are a fixed size in BLOCKS however far away the
    // cell is.
    const std::vector<std::string> uv_lines = code_lines_with(shader, "detail_uv");
    CHECK(uv_lines.size() >= 2);
    CHECK(shader.find("UV / detail_scale") != std::string::npos);

    // ...and the colour is multiplied by a mix whose neutral value is 1.
    const std::vector<std::string> albedo_lines = code_lines_with(shader, "albedo *=");
    CHECK(albedo_lines.size() == 1);
    for (const std::string& line : albedo_lines) {
        CHECK(line.find("mix(vec3(1.0)") != std::string::npos);
        CHECK(line.find("ratio") != std::string::npos);
    }
}

TEST_CASE("lod shader: a biome boundary is a blend, and both halves agree on the packing") {
    // Worldgen mixes the near world's biomes before it picks a surface material, so a
    // boundary there is a gradient of materials before any geometry exists. A far cell
    // is one flat quad wearing one texture array layer, so the same boundary arrived as
    // a straight line as long as the cell is wide -- sand against grass with a razor
    // edge on it. The two layers and the weight now arrive in the vertex
    // (lod/lod_surface.hpp) and the mix happens here.
    //
    // What has to hold, and what these lines pin:
    //   * the second sample goes through the SAME colour function as the first, so the
    //     detail term and its mean-preservation apply to both sides of a blend,
    //   * the pair is skipped where there is nothing to blend with, so a world of one
    //     biome pays no second sample, and
    //   * the layer pair survives the vertex stage: it rides in UV2.y as a small exact
    //     integer, which is what lets the whole thing cost no new vertex attribute.
    std::string shader;
    if (!load_lod_shader(shader)) {
        MESSAGE("shaders/lod_grid.gdshader not found; the far mode's material was not checked");
        return;
    }
    CHECK(shader.find("mix(albedo, far_albedo(layer2, UV, detail_uv), blend)") != std::string::npos);
    CHECK(shader.find("float blend = COLOR.a") != std::string::npos);
    CHECK(shader.find("blend > 0.002 && layer2 != layer") != std::string::npos);

    // The packing, taken apart the way the upload half wrote it: water in the low bit,
    // the second layer above it.
    CHECK(shader.find("mod(UV2.y, 2.0)") != std::string::npos);
    CHECK(shader.find("floor(UV2.y * 0.5)") != std::string::npos);

    // ...and the writer of that packing, which is the other end of the contract: a
    // change on one side without the other is a layer read out of the wrong texture.
    std::string upload;
    if (!load_lod_upload(upload)) {
        MESSAGE("src/lod/lod_grid_upload.cpp not found; the packing was checked on one side only");
        return;
    }
    CHECK(upload.find("v.water + 2.0f * v.layer2") != std::string::npos);
    CHECK(upload.find("Color(v.shade, v.shade, v.shade, v.mix)") != std::string::npos);
}

TEST_CASE("lod shader: the detail term is faded out where a repeat is too narrow to read") {
    // The term is the face's variation at a fixed world scale, and it is put back by
    // dividing a sample of it by the face's own average. The one thing that ratio
    // cannot survive is a repeat narrower than the screen: the fine sample's mip is a
    // choice the sampler makes per 2x2 pixel quad, the divisor is a constant, and the
    // difference between the two is then a speckle that crawls with the camera. Measured
    // in probes/probe_lod_grid_zfight.gd as the far field's own spike rate, on the one
    // heading whose ground is far enough out to be in that band, and it is what came
    // back from the field as "z fighting on every single face".
    //
    // The answer is the world's own answer to the same problem, in the same units: the
    // grain in shaders/block_noise.gdshaderinc is faded by its footprint so a distant
    // hillside does not boil. A repeat of this term is a face's 16 texels, so the two
    // rules differ only in which of those they count.
    std::string shader;
    if (!load_lod_shader(shader)) {
        MESSAGE("shaders/lod_grid.gdshader not found; the far mode's material was not checked");
        return;
    }
    // The footprint is the world-coordinate UV's own derivative: UV is in BLOCKS, so
    // its derivative is blocks per pixel, and one repeat's width is detail_scale over it.
    CHECK(shader.find("fwidth(uv)") != std::string::npos);
    // The wider of the two axes is the one read, which is the axis the sampler's own mip
    // choice is made on: ground seen edge-on has to fade with the rest of what the screen
    // cannot carry rather than go on texturing the axis that happens to survive.
    CHECK(shader.find("repeat_px = detail_scale / max(max(footprint.x, footprint.y)")
          != std::string::npos);
    // ...and the fade rides in the strength the mix is taken at, so a closed fade is the
    // term's neutral value and cannot darken or brighten the face it is drawn on.
    CHECK(shader.find("detail_strength * fade") != std::string::npos);

    const float narrow = named_float(shader, "DETAIL_REPEAT_MIN_PX");
    const float full = named_float(shader, "DETAIL_REPEAT_FULL_PX");
    CHECK(narrow > 0.0f);   // a zero minimum is the fade switched off
    CHECK(full > narrow);   // ...and an inverted pair is a fade that never opens
    CHECK(narrow <= 4.0f);  // narrower than a repeat the eye reads as texture at all
    CHECK(full <= 16.0f);   // ...and the term is back by the width of a whole face
}
