#include "doctest.h"
#include "schematic/gzip_inflate.hpp"
#include "schematic_test_data.hpp"

#include <string>
#include <vector>

using schematic_test::build_stored_gzip;
using schematic_test::gzip_plaintext;
using schematic_test::kGzipCorruptHex;
using schematic_test::kGzipDynamicHex;
using schematic_test::kRawDeflateHex;
using schematic_test::kZlibDynamicHex;
using schematic_test::stored_plaintext;
using schematic_test::unhex;
using VoxelEngine::schematic::InflateLimits;
using VoxelEngine::schematic::inflate_auto;
using VoxelEngine::schematic::inflate_wrapped;
using VoxelEngine::schematic::WrapKind;

// -----------------------------------------------------------------------------
// The inflater, checked against streams produced by an independent encoder.
//
// Every compressed fixture in these cases came out of python's gzip/zlib, so a
// round trip through our own code is never what proves our own code. The stored
// case is the exception: a stored block is a length-prefixed copy, so the test
// builds one by hand rather than embedding 600 bytes of hex.
// -----------------------------------------------------------------------------

TEST_CASE("inflate: gzip with dynamic huffman codes") {
    const std::vector<uint8_t> bytes = unhex(kGzipDynamicHex);
    std::vector<uint8_t> out;
    std::string error;
    WrapKind kind = WrapKind::Raw;
    CHECK(inflate_auto(bytes.data(), bytes.size(), out, &kind, &error));
    CHECK(error.empty());
    CHECK(kind == WrapKind::Gzip);
    CHECK(std::string(out.begin(), out.end()) == gzip_plaintext());
}

TEST_CASE("inflate: gzip stored blocks, several of them, plus a byte-exact trailer") {
    const std::vector<uint8_t> plain = stored_plaintext();
    // 200 bytes a block with a 600-byte payload: three blocks, so the multiple
    // block loop and the LEN/NLEN check are both exercised.
    const std::vector<uint8_t> stream = build_stored_gzip(plain, 200);
    std::vector<uint8_t> out;
    std::string error;
    CHECK(inflate_wrapped(stream.data(), stream.size(), WrapKind::Gzip, out, &error));
    CHECK(out == plain);
    CHECK(error.empty());
}

TEST_CASE("inflate: a payload shorter than one block still round trips") {
    const std::vector<uint8_t> plain = {1, 2, 3, 4, 5};
    const std::vector<uint8_t> stream = build_stored_gzip(plain, 200);
    std::vector<uint8_t> out;
    std::string error;
    CHECK(inflate_wrapped(stream.data(), stream.size(), WrapKind::Gzip, out, &error));
    CHECK(out == plain);
}

TEST_CASE("inflate: zlib and bare deflate are told apart by their headers") {
    const std::vector<uint8_t> zlib_bytes = unhex(kZlibDynamicHex);
    std::vector<uint8_t> out;
    std::string error;
    WrapKind kind = WrapKind::Gzip;
    CHECK(inflate_auto(zlib_bytes.data(), zlib_bytes.size(), out, &kind, &error));
    CHECK(kind == WrapKind::Zlib);
    CHECK(std::string(out.begin(), out.end()) == gzip_plaintext());

    const std::vector<uint8_t> raw_bytes = unhex(kRawDeflateHex);
    std::vector<uint8_t> raw_out;
    error.clear();
    kind = WrapKind::Gzip;
    CHECK(inflate_auto(raw_bytes.data(), raw_bytes.size(), raw_out, &kind, &error));
    CHECK(kind == WrapKind::Raw);
    CHECK(std::string(raw_out.begin(), raw_out.end()) == gzip_plaintext());
}

TEST_CASE("inflate: concatenated gzip members inflate in sequence") {
    const std::vector<uint8_t> one = unhex(kGzipDynamicHex);
    std::vector<uint8_t> two = one;
    two.insert(two.end(), one.begin(), one.end());
    std::vector<uint8_t> out;
    std::string error;
    CHECK(inflate_auto(two.data(), two.size(), out, nullptr, &error));
    const std::string expected = gzip_plaintext() + gzip_plaintext();
    CHECK(std::string(out.begin(), out.end()) == expected);
}

TEST_CASE("inflate: a flipped payload byte fails the crc check, not silently") {
    const std::vector<uint8_t> bytes = unhex(kGzipCorruptHex);
    std::vector<uint8_t> out;
    std::string error;
    CHECK_FALSE(inflate_auto(bytes.data(), bytes.size(), out, nullptr, &error));
    CHECK(error.find("crc") != std::string::npos);
}

TEST_CASE("inflate: truncated and empty inputs fail instead of reading past the end") {
    const std::vector<uint8_t> bytes = unhex(kGzipDynamicHex);
    std::vector<uint8_t> out;
    std::string error;
    CHECK_FALSE(inflate_auto(bytes.data(), bytes.size() / 2, out, nullptr, &error));
    CHECK_FALSE(error.empty());

    out.clear();
    error.clear();
    CHECK_FALSE(inflate_auto(bytes.data(), 0, out, nullptr, &error));
    CHECK_FALSE(error.empty());

    // Header only: the trailer is gone.
    out.clear();
    error.clear();
    CHECK_FALSE(inflate_auto(bytes.data(), 10, out, nullptr, &error));
}

TEST_CASE("inflate: a reserved block type is rejected") {
    // 0xFF sets the final-block bit and then a block type of 3, which the format
    // reserves. Treated as raw deflate so the byte is read as the first block.
    const std::vector<uint8_t> bytes = {0xFF, 0x00, 0x00, 0x00};
    std::vector<uint8_t> out;
    std::string error;
    CHECK_FALSE(inflate_wrapped(bytes.data(), bytes.size(), WrapKind::Raw, out, &error));
    CHECK(error.find("reserved") != std::string::npos);
}

TEST_CASE("inflate: output is capped rather than trusted") {
    const std::vector<uint8_t> plain = stored_plaintext();
    const std::vector<uint8_t> stream = build_stored_gzip(plain, 200);
    InflateLimits limits;
    limits.max_output_bytes = 64;
    std::vector<uint8_t> out;
    std::string error;
    CHECK_FALSE(inflate_wrapped(stream.data(), stream.size(), WrapKind::Gzip, out, &error, limits));
    CHECK(error.find("limit") != std::string::npos);
    CHECK(out.size() <= 64);
}
