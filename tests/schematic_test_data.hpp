#ifndef FARLANDS_TESTS_SCHEMATIC_TEST_DATA_HPP
#define FARLANDS_TESTS_SCHEMATIC_TEST_DATA_HPP

// -----------------------------------------------------------------------------
// Fixtures for the schematic reader tests.
//
// Every compressed fixture here was produced by an independent encoder (python's
// gzip/zlib), and the block files were written by a hand-rolled big-endian NBT
// writer in that same script, so nothing in the reader is ever verified against
// its own output. The stored-block case is built by hand in C++ instead (see
// build_stored_gzip), because a stored block is just a length-prefixed copy and
// writing one here keeps 600 bytes of hex out of the tree.
//
// Regenerate the embedded hex with `python tools/make_schematic_fixtures.py`,
// which rewrites the literals below in place (and checks them with --check).
// -----------------------------------------------------------------------------

#include "core/crc32.hpp"

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace schematic_test {

// Decodes a hex literal (whitespace ignored) into bytes.
inline std::vector<uint8_t> unhex(const char* hex) {
    std::vector<uint8_t> out;
    int high = -1;
    for (const char* p = hex; *p != '\0'; ++p) {
        const char c = *p;
        int value = -1;
        if (c >= '0' && c <= '9') value = c - '0';
        else if (c >= 'a' && c <= 'f') value = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value = c - 'A' + 10;
        else continue;
        if (high < 0) {
            high = value;
        } else {
            out.push_back(static_cast<uint8_t>((high << 4) | value));
            high = -1;
        }
    }
    return out;
}

// "the quick brown fox jumps over the lazy dog. " x12 — the payload of the
// dynamic-Huffman, zlib and raw-DEFLATE fixtures.
inline std::string gzip_plaintext() {
    std::string text;
    for (int i = 0; i < 12; ++i) text += "the quick brown fox jumps over the lazy dog. ";
    return text;
}

// The payload of the hand-built stored-block stream.
inline std::vector<uint8_t> stored_plaintext() {
    std::vector<uint8_t> bytes(600);
    for (size_t i = 0; i < bytes.size(); ++i) bytes[i] = static_cast<uint8_t>((i * 37 + 11) & 0xFF);
    return bytes;
}

// gzip, dynamic Huffman (69 bytes).
inline const char* kGzipDynamicHex =
    "1f8b08000000000002ff2bc94855282c"
    "cd4cce56482aca2fcf5348cbaf50c82a"
    "cd2d2856c82f4b2d5228014ae7245655"
    "2aa4e4a7eb8179a38a478c6200eab875"
    "c11c020000";

// The same stream with one byte in the middle flipped — the CRC32 trailer is
// what catches it.
inline const char* kGzipCorruptHex =
    "1f8b08000000000002ff2bc94855282c"
    "cd4cce56482aca2fcf5348cbaf50c82a"
    "cd2dd756c82f4b2d5228014ae7245655"
    "2aa4e4a7eb8179a38a478c6200eab875"
    "c11c020000";

// zlib with an Adler-32 trailer (59 bytes).
inline const char* kZlibDynamicHex =
    "789c2bc94855282ccd4cce56482aca2f"
    "cf5348cbaf50c82acd2d2856c82f4b2d"
    "5228c94855c849acaa5448c94fd703f3"
    "4615578d14c500f724c355";

// Bare DEFLATE, no container at all (51 bytes).
inline const char* kRawDeflateHex =
    "2bc94855282ccd4cce56482aca2fcf53"
    "48cbaf50c82acd2d2856c82f4b2d5228"
    "014ae72456552aa4e4a7eb8179a38a47"
    "8c6200";

// A 4 x 3 x 5 schematic, gzipped, with Data as one byte per cell.
inline const char* kSchemByteHex =
    "1f8b08000000000002ff15cc470f8240"
    "1086e1a1ec2e4bb520562cd83526fe00"
    "2f1a0f1ef4e4c1f3060910b144f6ffc7"
    "f13679f37c6302bfc659f214328f5520"
    "b7fc2e33d055a0a7244f33091a9ee7e4"
    "9562250ce8a178c78f1200768ace4cc3"
    "72ab5eadd10adabd41381ccf28b73da7"
    "520ffc6627ecf6479368ba5823f19174"
    "91444896f3d566cb403f0a29fe6f4051"
    "354d2794328373d3b26dc7f5540c0483"
    "81c1c2e07aa02061484c240e12f8ef0c"
    "e01721936f2e8a12c8bef864027ef86e"
    "2867d0000000";

// The same build with Data packed two cells per byte.
inline const char* kSchemNibbleHex =
    "1f8b08000000000002ffe362e00c4ece"
    "48cd4d2cc94c6662600dcf4c29c96060"
    "616260f348cd4ccf28616006327d52f3"
    "d281a2acec0c6c4e39f9c9d9c50c0c0c"
    "368c2cec5c1cdc7c82fc42a212629232"
    "f2b20aca6a6c9c3cfcbc02c26222e252"
    "b2d2728a2a4aaa1ada4025224025d240"
    "254a40259aea5aba06ec0c2c2e892589"
    "4063e4048c9c53cb66ac3c7de79f5148"
    "6afb8c5da7dffe1308296b5fb9ebce5b"
    "7e01230e064edfc492d4a2ccc49c6206"
    "56c79c828c440600d0c7f1a8b2000000";

// Ids above 255, carried in the AddBlocks nibbles.
inline const char* kSchemAddHex =
    "1f8b08000000000002ff7d8e394fc340"
    "1046bfd8decb270e382406124e254148"
    "fc011aa31414499522f5ca5ed92bcc21"
    "bcff5f0c14946c35efe9cd6a42a87ddd"
    "9937ed6ced811d6ce33a041ef88bb16d"
    "e7e0d3b835ef2d5926c09ffb8ffa7500"
    "f00491162c3e2ec37cbac82617b72767"
    "d76b9f40122404e3d9e5f2747ef7c009"
    "22822382e2fce6bebc5a3d0a041bedf4"
    "ef3723cff703c6b9904a85511c2769e6"
    "916024248988449a614489a024a424a1"
    "043f7b126aa79df9b2ba1fc0aafeb3d3"
    "02aa6a9abf2be7f9bf0fdf9da48abbfe"
    "000000";

// A Data array of a length that is neither layout.
inline const char* kSchemBadDataHex =
    "1f8b08000000000002ffe362e00c4ece"
    "48cd4d2cc94c6662600dcf4c29c96060"
    "616260f348cd4ccf28616006327d52f3"
    "d281a2acec0c6c4e39f9c9d9c50c0c0c"
    "368c2cec5c1cdc7c82fc42a212629232"
    "f2b20aca6a6c9c3cfcbc02c26222e252"
    "b2d2728a2a4aaa1ada4025224025d240"
    "254a40259aea5aba06ec0c2c2e892589"
    "4063b818189998995958d9d8d8391838"
    "7d134b528b3213738a19581d730a3212"
    "1900c524f68b9e000000";

// Root compound named something else.
inline const char* kSchemWrongRootHex =
    "1f8b08000000000002ff15cc470f8240"
    "1086e1a1ec2e1d0b626fd83526fe002f"
    "180f1ed48b07cf1b2440c412d9ff1f87"
    "dbe4cdf38d01f6f523c25b94c62f2eb2"
    "480672cf1e220555067a8ab32415a0e0"
    "798edf0956c2801ef24ff42c00602fa9"
    "ccd04ca7ead61a2dbfdd1bf6479339d5"
    "2dd7aed47dafd9e97707e369305b6e90"
    "7848ba480224abc57abb63a01eb9e0e5"
    "1b9064455109a54cd375c3b42cdb7165"
    "0c048386c1c4e0b8202161480c243612"
    "28771ae8172ee25fc6f30248987f530e"
    "7f721084efd4000000";

// No dimensions at all.
inline const char* kSchemMissingDimsHex =
    "1f8b08000000000002ffe362e00c4ece"
    "48cd4d2cc94c66676073cac94fce2e66"
    "6060b0616461e7e2e0e613e417129510"
    "9394919755505663e3e4e1e715101613"
    "119792959653545152d5d0062a11012a"
    "91062a51022ad154d7d235600000b185"
    "758c56000000";

// ---------------------------------------------------------------------------
// The geometry the schematic fixtures were written with. The reader is checked
// against these formulas, which is what proves the cell order: an axis swap
// would line the values up differently.
// ---------------------------------------------------------------------------
inline constexpr int32_t kFixtureWidth = 4;
inline constexpr int32_t kFixtureHeight = 3;
inline constexpr int32_t kFixtureLength = 5;
inline constexpr size_t kFixtureCells =
    static_cast<size_t>(kFixtureWidth) * kFixtureHeight * kFixtureLength;

inline uint16_t fixture_block_id(int32_t x, int32_t y, int32_t z) {
    return static_cast<uint16_t>(1 + (x * 3 + y * 5 + z * 7) % 200);
}
inline uint8_t fixture_data_value(int32_t x, int32_t y, int32_t z) {
    return static_cast<uint8_t>((x + y * 2 + z * 3) & 0xF);
}
inline uint16_t fixture_add_block_id(int32_t x, int32_t y, int32_t z) {
    return static_cast<uint16_t>(0x100 + ((x * 7 + y * 3 + z * 5) % 90));
}

// ---------------------------------------------------------------------------
// Hand-built gzip around stored (uncompressed) DEFLATE blocks. Only the header,
// the block framing and the CRC32/ISIZE trailer are needed; the CRC comes from
// the engine's own header-only implementation.
// ---------------------------------------------------------------------------
inline void append_le32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

inline std::vector<uint8_t> build_stored_gzip(const std::vector<uint8_t>& plain, size_t chunk) {
    std::vector<uint8_t> out = {0x1F, 0x8B, 0x08, 0x00, 0, 0, 0, 0, 0x00, 0x03};
    size_t at = 0;
    do {
        const size_t take = (plain.size() - at) < chunk ? (plain.size() - at) : chunk;
        const bool last = (at + take) >= plain.size();
        out.push_back(last ? 1 : 0);
        const uint16_t length = static_cast<uint16_t>(take);
        out.push_back(static_cast<uint8_t>(length & 0xFF));
        out.push_back(static_cast<uint8_t>(length >> 8));
        out.push_back(static_cast<uint8_t>(~length & 0xFF));
        out.push_back(static_cast<uint8_t>((~length >> 8) & 0xFF));
        out.insert(out.end(), plain.begin() + static_cast<long>(at),
                   plain.begin() + static_cast<long>(at + take));
        at += take;
    } while (at < plain.size());
    append_le32(out, VoxelEngine::crc32(plain.data(), plain.size()));
    append_le32(out, static_cast<uint32_t>(plain.size()));
    return out;
}

// ---------------------------------------------------------------------------
// A minimal big-endian NBT writer, for the cursor tests (nested compounds,
// lists, and the little-endian variant of the same tree).
// ---------------------------------------------------------------------------
inline void put_be16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void put_be32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

inline void put_name(std::vector<uint8_t>& out, const std::string& name) {
    put_be16(out, static_cast<uint16_t>(name.size()));
    out.insert(out.end(), name.begin(), name.end());
}

// Starts a tree: the root compound tag plus its name.
inline void nbt_begin(std::vector<uint8_t>& out, const std::string& root_name) {
    out.clear();
    out.push_back(10);  // Compound
    put_name(out, root_name);
}

inline void nbt_tag(std::vector<uint8_t>& out, int tag, const std::string& name) {
    out.push_back(static_cast<uint8_t>(tag));
    put_name(out, name);
}

inline void nbt_byte(std::vector<uint8_t>& out, int8_t value) {
    out.push_back(static_cast<uint8_t>(value));
}

inline void nbt_short(std::vector<uint8_t>& out, int16_t value) {
    put_be16(out, static_cast<uint16_t>(value));
}

inline void nbt_int(std::vector<uint8_t>& out, int32_t value) {
    put_be32(out, static_cast<uint32_t>(value));
}

inline void nbt_long(std::vector<uint8_t>& out, int64_t value) {
    put_be32(out, static_cast<uint32_t>(static_cast<uint64_t>(value) >> 32));
    put_be32(out, static_cast<uint32_t>(static_cast<uint64_t>(value) & 0xFFFFFFFFu));
}

inline void nbt_string(std::vector<uint8_t>& out, const std::string& value) {
    put_name(out, value);
}

inline void nbt_byte_array(std::vector<uint8_t>& out, const std::vector<uint8_t>& values) {
    put_be32(out, static_cast<uint32_t>(values.size()));
    out.insert(out.end(), values.begin(), values.end());
}

inline void nbt_end(std::vector<uint8_t>& out) { out.push_back(0); }

} // namespace schematic_test

#endif // FARLANDS_TESTS_SCHEMATIC_TEST_DATA_HPP
