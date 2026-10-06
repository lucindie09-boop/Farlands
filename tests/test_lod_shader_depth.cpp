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
    //      (DETAIL_AVERAGE_MIP), which is also why the term fades itself out: past the
    //      distance where one repeat is a pixel, both samples are that average and the
    //      ratio is 1 -- no aliasing, and no fade to keep in step with anything.
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
