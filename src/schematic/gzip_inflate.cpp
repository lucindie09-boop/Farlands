#include "schematic/gzip_inflate.hpp"

#include "core/crc32.hpp"

#include <array>
#include <cstring>

namespace VoxelEngine {
namespace schematic {
namespace {

// RFC 1951 caps both the literal/length and distance codes at 15 bits.
constexpr int kMaxCodeBits = 15;

bool set_error(std::string* error, const char* message) {
    if (error) *error = message;
    return false;
}

bool set_error(std::string* error, const std::string& message) {
    if (error) *error = message;
    return false;
}

uint32_t read_u32_le(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

uint32_t read_u32_be(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}

uint32_t adler32(const uint8_t* data, size_t len) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; ++i) {
        a = (a + data[i]) % 65521u;
        b = (b + a) % 65521u;
    }
    return (b << 16) | a;
}

// LSB-first bit reader. DEFLATE packs Huffman codes starting at the least
// significant bit of each byte, which is the opposite of how the codes are
// written in the spec, hence the reversal in the bit loop rather than here.
struct BitReader {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t pos = 0;
    uint32_t buffer = 0;
    int count = 0;
    bool failed = false;

    BitReader(const uint8_t* d, size_t n) : data(d), size(n) {}

    uint32_t take(int bits) {
        if (bits <= 0) return 0;
        while (count < bits) {
            if (pos >= size) {
                failed = true;
                return 0;
            }
            buffer |= static_cast<uint32_t>(data[pos++]) << count;
            count += 8;
        }
        const uint32_t value = buffer & ((1u << bits) - 1u);
        buffer >>= bits;
        count -= bits;
        return value;
    }

    // Drops the partial byte, so the next take(8) lands on a byte boundary
    // within the source stream (used by stored blocks and by the trailer).
    void align_to_byte() {
        const int drop = count & 7;
        buffer >>= drop;
        count -= drop;
    }

    // Source bytes the stream has consumed for good. Bits still sitting in the
    // buffer came out of `pos`, so only whole buffered bytes count as consumed.
    size_t consumed() const { return pos - static_cast<size_t>(count / 8); }
};

// Canonical Huffman code: how many symbols sit at each code length, plus those
// symbols ordered by (length, symbol value) — the standard counting-sort form,
// which is what makes decoding a walk down the tree without any table build.
struct Huffman {
    std::array<int16_t, kMaxCodeBits + 1> count{};
    std::vector<int16_t> symbol;
};

bool build_huffman(Huffman& h, const uint8_t* lengths, int n, std::string* error) {
    h.count.fill(0);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] > kMaxCodeBits) return set_error(error, "code length above 15 bits");
        h.count[lengths[i]]++;
    }
    // Walk the implicit tree level by level: each level doubles the number of
    // free slots and then spends the symbols at that length. Running out means
    // two codes share a prefix, which is not a decodable code. Ending with slots
    // left over is legal (the fixed tables and a single distance code both do
    // it): the unused patterns simply decode as a bad symbol.
    int free_slots = 1;
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        free_slots <<= 1;
        free_slots -= h.count[len];
        if (free_slots < 0) return set_error(error, "over-subscribed huffman code");
    }
    std::array<int16_t, kMaxCodeBits + 2> offset{};
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        offset[len + 1] = static_cast<int16_t>(offset[len] + h.count[len]);
    }
    h.symbol.assign(static_cast<size_t>(n), 0);
    for (int i = 0; i < n; ++i) {
        if (lengths[i] != 0) h.symbol[static_cast<size_t>(offset[lengths[i]]++)] = static_cast<int16_t>(i);
    }
    return true;
}

// Returns the symbol the next code decodes to, or -1 on a damaged stream.
int decode_symbol(BitReader& reader, const Huffman& h) {
    int code = 0;
    int first = 0;
    int index = 0;
    for (int len = 1; len <= kMaxCodeBits; ++len) {
        code |= static_cast<int>(reader.take(1));
        if (reader.failed) return -1;
        const int count = h.count[len];
        if (code - count < first) {
            // Both terms are non-negative here (code - first is at least count, and
            // count is a symbol count), so widening them separately indexes the table
            // without an addition that could be read as signed.
            return h.symbol[static_cast<size_t>(index) + static_cast<size_t>(code - first)];
        }
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

const Huffman& fixed_literal_huffman() {
    static const Huffman table = [] {
        std::array<uint8_t, 288> lengths{};
        for (int i = 0; i < 144; ++i) lengths[static_cast<size_t>(i)] = 8;
        for (int i = 144; i < 256; ++i) lengths[static_cast<size_t>(i)] = 9;
        for (int i = 256; i < 280; ++i) lengths[static_cast<size_t>(i)] = 7;
        for (int i = 280; i < 288; ++i) lengths[static_cast<size_t>(i)] = 8;
        Huffman h;
        build_huffman(h, lengths.data(), 288, nullptr);
        return h;
    }();
    return table;
}

const Huffman& fixed_distance_huffman() {
    // All 32 five-bit patterns are decodable; only 0..29 mean anything, and the
    // two extras are rejected at use rather than here.
    static const Huffman table = [] {
        std::array<uint8_t, 32> lengths{};
        lengths.fill(5);
        Huffman h;
        build_huffman(h, lengths.data(), 32, nullptr);
        return h;
    }();
    return table;
}

// RFC 1951 length/distance code tables.
constexpr int kLengthBase[29] = {3,  4,  5,  6,  7,  8,  9,  10,  11,  13,  15,  17,  19,  23, 27,
                                 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr int kLengthExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                  2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr int kDistanceBase[30] = {1,    2,    3,    4,    5,    7,     9,     13,    17,   25,
                                   33,   49,   65,   97,   129,  193,   257,   385,   513,  769,
                                   1025, 1537, 2049, 3073, 4097, 6145,  8193,  12289, 16385, 24577};
constexpr int kDistanceExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2,  3,  3,  4,  4,  5,  5,  6,
                                    6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// The order code-length codes are stored in — shortest codes first, which is why
// it is not simply 0..18.
constexpr int kCodeLengthOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

bool inflate_block_data(BitReader& reader, const Huffman& literals, const Huffman& distances,
                        std::vector<uint8_t>& out, std::string* error,
                        const InflateLimits& limits) {
    for (;;) {
        const int symbol = decode_symbol(reader, literals);
        if (symbol < 0) return set_error(error, "damaged deflate stream");
        if (symbol < 256) {
            if (out.size() >= limits.max_output_bytes) {
                return set_error(error, "decompressed size above limit");
            }
            out.push_back(static_cast<uint8_t>(symbol));
            continue;
        }
        if (symbol == 256) return true;  // end of block

        const int length_index = symbol - 257;
        if (length_index >= 29) return set_error(error, "invalid length code");
        const int length =
            kLengthBase[length_index] + static_cast<int>(reader.take(kLengthExtra[length_index]));
        if (reader.failed) return set_error(error, "truncated deflate stream");

        const int distance_symbol = decode_symbol(reader, distances);
        if (distance_symbol < 0 || distance_symbol >= 30) return set_error(error, "invalid distance code");
        const int distance =
            kDistanceBase[distance_symbol] + static_cast<int>(reader.take(kDistanceExtra[distance_symbol]));
        if (reader.failed) return set_error(error, "truncated deflate stream");
        if (distance > static_cast<int>(out.size())) return set_error(error, "distance before start of output");
        if (out.size() + static_cast<size_t>(length) > limits.max_output_bytes) {
            return set_error(error, "decompressed size above limit");
        }
        // Byte at a time on purpose: a run may overlap its own output (distance
        // below length), which is how DEFLATE spells a repeated pattern.
        const size_t from = out.size() - static_cast<size_t>(distance);
        for (int i = 0; i < length; ++i) out.push_back(out[from + static_cast<size_t>(i)]);
    }
}

bool inflate_deflate(const uint8_t* data, size_t size, size_t& consumed,
                     std::vector<uint8_t>& out, std::string* error,
                     const InflateLimits& limits) {
    BitReader reader(data, size);
    bool last_block = false;
    while (!last_block) {
        last_block = reader.take(1) != 0;
        const uint32_t type = reader.take(2);
        if (reader.failed) return set_error(error, "truncated deflate stream");

        if (type == 0) {
            reader.align_to_byte();
            const uint32_t length = reader.take(16);
            const uint32_t inverse = reader.take(16);
            if (reader.failed) return set_error(error, "truncated stored block");
            if ((length ^ 0xFFFFu) != inverse) return set_error(error, "stored block length mismatch");
            if (out.size() + length > limits.max_output_bytes) {
                return set_error(error, "decompressed size above limit");
            }
            for (uint32_t i = 0; i < length; ++i) out.push_back(static_cast<uint8_t>(reader.take(8)));
            if (reader.failed) return set_error(error, "truncated stored block");
            continue;
        }

        if (type == 1) {
            if (!inflate_block_data(reader, fixed_literal_huffman(), fixed_distance_huffman(),
                                    out, error, limits)) {
                return false;
            }
            continue;
        }

        if (type == 2) {
            const int literal_count = static_cast<int>(reader.take(5)) + 257;
            const int distance_count = static_cast<int>(reader.take(5)) + 1;
            const int code_length_count = static_cast<int>(reader.take(4)) + 4;
            if (reader.failed) return set_error(error, "truncated dynamic block header");
            if (literal_count > 288 || distance_count > 32) {
                return set_error(error, "dynamic block header out of range");
            }

            std::array<uint8_t, 19> code_length_lengths{};
            for (int i = 0; i < code_length_count; ++i) {
                code_length_lengths[static_cast<size_t>(kCodeLengthOrder[i])] =
                    static_cast<uint8_t>(reader.take(3));
            }
            if (reader.failed) return set_error(error, "truncated dynamic block header");

            Huffman code_lengths;
            if (!build_huffman(code_lengths, code_length_lengths.data(), 19, error)) return false;

            std::array<uint8_t, 288 + 32> lengths{};
            const int total = literal_count + distance_count;
            int index = 0;
            while (index < total) {
                const int symbol = decode_symbol(reader, code_lengths);
                if (symbol < 0) return set_error(error, "damaged code length code");
                if (symbol < 16) {
                    lengths[static_cast<size_t>(index++)] = static_cast<uint8_t>(symbol);
                } else if (symbol == 16) {
                    // Repeat the previous length 3-6 times; it needs one to exist.
                    if (index == 0) return set_error(error, "repeat with no previous code length");
                    const uint8_t previous = lengths[static_cast<size_t>(index - 1)];
                    const int repeat = 3 + static_cast<int>(reader.take(2));
                    for (int i = 0; i < repeat && index < total; ++i) {
                        lengths[static_cast<size_t>(index++)] = previous;
                    }
                } else if (symbol == 17 || symbol == 18) {
                    const int extra = symbol == 17 ? 3 : 11;
                    const int repeat = extra + static_cast<int>(reader.take(symbol == 17 ? 3 : 7));
                    index += repeat;
                } else {
                    return set_error(error, "invalid code length code");
                }
                if (reader.failed) return set_error(error, "truncated dynamic block header");
            }
            if (index > total) return set_error(error, "code lengths overrun their table");
            // Without an end-of-block code the block can never terminate, so a
            // stream like this is malformed no matter what follows it.
            if (lengths[256] == 0) return set_error(error, "dynamic block has no end-of-block code");

            Huffman literals;
            Huffman distances;
            if (!build_huffman(literals, lengths.data(), literal_count, error)) return false;
            if (!build_huffman(distances, lengths.data() + literal_count, distance_count, error)) {
                return false;
            }
            if (!inflate_block_data(reader, literals, distances, out, error, limits)) return false;
            continue;
        }

        return set_error(error, "reserved block type");
    }
    reader.align_to_byte();
    consumed = reader.consumed();
    return true;
}

// gzip header (RFC 1952). Fills header_len. Optional fields are skipped, and the
// header's own checksum (FHCRC) is not verified, because the whole-member CRC32
// covers everything the header describes anyway.
bool parse_gzip_header(const uint8_t* data, size_t size, size_t& header_len, std::string* error) {
    if (size < 10) return set_error(error, "truncated gzip header");
    if (data[0] != 0x1F || data[1] != 0x8B) return set_error(error, "not a gzip member");
    if (data[2] != 8) return set_error(error, "unsupported gzip compression method");
    const uint8_t flags = data[3];
    if (flags & 0xE0) return set_error(error, "reserved gzip flags set");
    size_t at = 10;
    if (flags & 0x04) {  // FEXTRA: a length-prefixed blob
        if (at + 2 > size) return set_error(error, "truncated gzip extra field");
        const size_t extra = static_cast<size_t>(data[at]) | (static_cast<size_t>(data[at + 1]) << 8);
        at += 2 + extra;
        if (at > size) return set_error(error, "truncated gzip extra field");
    }
    // FNAME and FCOMMENT are both NUL-terminated strings, and are the only
    // variable-length parts of a header this format still sets.
    for (const uint8_t field : {0x08, 0x10}) {
        if ((flags & field) == 0) continue;
        while (at < size && data[at] != 0) ++at;
        if (at >= size) return set_error(error, "unterminated gzip name or comment");
        ++at;
    }
    if (flags & 0x02) at += 2;  // FHCRC: 16-bit header CRC, unchecked on purpose
    if (at > size) return set_error(error, "truncated gzip header");
    header_len = at;
    return true;
}

} // namespace

bool inflate_wrapped(const uint8_t* data, size_t size, WrapKind kind, std::vector<uint8_t>& out,
                     std::string* error, const InflateLimits& limits) {
    if (data == nullptr || size == 0) return set_error(error, "no input bytes");

    size_t at = 0;
    while (true) {
        size_t header_len = 0;
        if (kind == WrapKind::Gzip) {
            if (!parse_gzip_header(data + at, size - at, header_len, error)) return false;
        } else if (kind == WrapKind::Zlib) {
            if (size - at < 2) return set_error(error, "truncated zlib header");
            const uint8_t cmf = data[at];
            const uint8_t flg = data[at + 1];
            if ((cmf & 0x0F) != 8) return set_error(error, "unsupported zlib compression method");
            if ((static_cast<uint32_t>(cmf) * 256u + flg) % 31u != 0) return set_error(error, "bad zlib header check");
            if (flg & 0x20) return set_error(error, "zlib preset dictionary not supported");
            header_len = 2;
        }

        const size_t body_start = at + header_len;
        if (body_start >= size) return set_error(error, "truncated stream");

        const size_t produced_from = out.size();
        size_t body_len = 0;
        if (!inflate_deflate(data + body_start, size - body_start, body_len, out, error, limits)) {
            return false;
        }
        // Bare DEFLATE has no container framing left to walk, so one member is
        // the whole input.
        if (kind == WrapKind::Raw) return true;

        const size_t produced = out.size() - produced_from;
        at = body_start + body_len;

        if (kind == WrapKind::Gzip) {
            if (at + 8 > size) return set_error(error, "truncated gzip trailer");
            const uint32_t stored_crc = read_u32_le(data + at);
            const uint32_t stored_size = read_u32_le(data + at + 4);
            const uint32_t actual_crc = crc32(out.data() + produced_from, produced);
            if (stored_crc != actual_crc) return set_error(error, "gzip crc mismatch");
            if (stored_size != static_cast<uint32_t>(produced & 0xFFFFFFFFu)) {
                return set_error(error, "gzip length mismatch");
            }
            at += 8;
            // A gzip file may hold several members back to back; keep going while
            // another header follows.
            if (at + 2 <= size && data[at] == 0x1F && data[at + 1] == 0x8B) continue;
            return true;
        }

        if (kind == WrapKind::Zlib) {
            if (at + 4 > size) return set_error(error, "truncated zlib trailer");
            if (read_u32_be(data + at) != adler32(out.data() + produced_from, produced)) {
                return set_error(error, "zlib checksum mismatch");
            }
            return true;
        }

        return true;
    }
}

bool inflate_auto(const uint8_t* data, size_t size, std::vector<uint8_t>& out,
                  WrapKind* detected_kind, std::string* error, const InflateLimits& limits) {
    if (data == nullptr || size == 0) return set_error(error, "no input bytes");
    WrapKind kind = WrapKind::Raw;
    if (size >= 2 && data[0] == 0x1F && data[1] == 0x8B) {
        kind = WrapKind::Gzip;
    } else if (size >= 2 && (data[0] & 0x0F) == 8 &&
               (static_cast<uint32_t>(data[0]) * 256u + data[1]) % 31u == 0) {
        kind = WrapKind::Zlib;
    }
    if (detected_kind) *detected_kind = kind;
    return inflate_wrapped(data, size, kind, out, error, limits);
}

} // namespace schematic
} // namespace VoxelEngine
