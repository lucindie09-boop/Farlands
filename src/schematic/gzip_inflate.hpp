#ifndef FARLANDS_GZIP_INFLATE_HPP
#define FARLANDS_GZIP_INFLATE_HPP

// -----------------------------------------------------------------------------
// DEFLATE decompression, self-contained.
//
// Third-party block files arrive compressed: `.schematic` is a gzip member,
// `.schem` is gzip or zlib, and a bare NBT dump is not compressed at all. The
// game could ask Godot's FileAccess to do this (it wraps gzip natively), but the
// readers have to run in the standalone test binary and in `bin/schematic_report`
// too, where there is no engine runtime and no zlib on the link line. So the
// inflater lives here: no dependencies, no Godot, deterministic, and the same
// code path in-game as out of it.
//
// Scope is deliberately "just enough to open the format": one-shot inflation of
// a whole member into a buffer, with the container's own integrity check
// (gzip CRC32 / zlib Adler-32) verified and decompressed size bounded. No
// streaming, no deflate compression, no dictionary support.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VoxelEngine {
namespace schematic {

// What wrapped the raw DEFLATE stream.
enum class WrapKind : uint8_t {
    Gzip = 0,  // RFC 1952: magic 1f 8b, CRC32 + ISIZE trailer
    Zlib,      // RFC 1950: 2-byte header, Adler-32 trailer
    Raw        // bare DEFLATE, no header or trailer
};

// Bounds on a single inflation. A damaged or hostile header can describe a huge
// output from a tiny input, so the decompressed size is capped rather than
// trusted: a schematic is a build, not a disk image.
struct InflateLimits {
    size_t max_output_bytes = static_cast<size_t>(512) * 1024u * 1024u;
};

// Inflates one member of the given container kind, appending to `out`.
// Returns false and fills `error` (when non-null) on a malformed header, a
// damaged stream, a container checksum mismatch, or output past `limits`.
bool inflate_wrapped(const uint8_t* data, size_t size, WrapKind kind,
                     std::vector<uint8_t>& out, std::string* error = nullptr,
                     const InflateLimits& limits = InflateLimits{});

// Sniffs the container from its first bytes (gzip magic, else a valid zlib
// header, else bare DEFLATE) and inflates it. Reports what it found through
// `detected_kind` when that is non-null. Concatenated gzip members are inflated
// in sequence, which is legal gzip and is what some exporters emit.
bool inflate_auto(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  WrapKind* detected_kind = nullptr, std::string* error = nullptr,
                  const InflateLimits& limits = InflateLimits{});

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_GZIP_INFLATE_HPP
