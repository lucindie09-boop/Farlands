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

// The same build wrapped in an unnamed root, which writers do and which the
// reader has to see through: the build compound is found by shape, not by the
// root's own name.
inline const char* kSchemWrappedHex =
    "1f8b08000000000002ff15ccb96ec240"
    "1485e1e365663ce30d02615fc2960450"
    "241e80064441011505f5c858d8c22c0a"
    "f3fee2d25dfdface5580823c24597ad5"
    "264f6cb0637e32195c1b7c9be6e7ccc0"
    "a17397dece5499005f17f7e4f204b0b4"
    "5ca13c3f2ac71f9f8d5ab3d3ef7e8dbe"
    "b90ce2b054a955ebad6ebb37180f27bf"
    "732255226d224322d39fd9df42c0dd68"
    "a3df6f60d98ee332ce8527a5f283208c"
    "629b02a3e051f02944312c2282882212"
    "12c17be741eeb549ff735d3cc156c523"
    "d3c00be9b1723ed4000000";

// The classic build again, with AddBlocks one byte longer than the packed size.
// Some writers round the length up a whole byte; the extra half-byte is ignored
// and the file still has to decode to exactly the same build.
inline const char* kSchemAddPaddedHex =
    "1f8b08000000000002ff7d8e3b6fc230"
    "14463f92d88ef304caab694bca4b4085"
    "c41fe842d5a1034c1d3a5b8995448487"
    "8affbfb865e888a77b8eceb5ae07f99d"
    "95faa04c9559603f556e4a3816f897ae"
    "8ad2c0a671ab8f055926c03fea53b6bf"
    "00788788ba2c7848bcd6208d7b2fd3ce"
    "d3786913b8042141fbf175de1fce569c"
    "c0276812749f276fc968b116703e9551"
    "b76f1a966d3b8c73e14ae9f9411046b1"
    "4582917049f824a2180d4a04251e2521"
    "25f8db732177cae8df4ad517b04d7d2e"
    "9580dce4f9ff9569ebee03ae231af56a"
    "ff000000";

// The nibble-packed build with Data one byte longer than the packed size, which
// is the same slip on the other array.
inline const char* kSchemNibblePaddedHex =
    "1f8b08000000000002ffe362e00c4ece"
    "48cd4d2cc94c6662600dcf4c29c96060"
    "616260f348cd4ccf28616006327d52f3"
    "d281a2acec0c6c4e39f9c9d9c50c0c0c"
    "368c2cec5c1cdc7c82fc42a212629232"
    "f2b20aca6a6c9c3cfcbc02c26222e252"
    "b2d2728a2a4aaa1ada4025224025d240"
    "254a40259aea5aba06ec0c2c2e892589"
    "4063e4058c9c53cb66ac3c7de79f5148"
    "6afb8c5da7dffe1308296b5fb9ebce5b"
    "7e0123060e064edfc492d4a2ccc49c62"
    "0656c79c828c4406004d9418b2b30000"
    "00";

// A build whose skipped sections come FIRST and are dense with floats and
// doubles (an icon, a block entity, and an entity, as real files have). Skipping
// a float at the wrong width does not fail there: the cursor desynchronises and
// the file dies later, so a reader that gets this wrong loses the whole build.
inline const char* kSchemFloatFieldsHex =
    "1f8b08000000000002ff5d8ec94e1b41"
    "10866bc6d3b379ec61314b4220ecab90"
    "10c708c9061c0ba418a180c5854babdd"
    "785ab4bb91a7918097e0157806c4811b"
    "11af803871e6311054931b75a9aa5fdf"
    "ff57c5101db08c77a9112c066f976945"
    "80fcc555576b003e90034625af79f0bf"
    "22480e85e4bf951146f03c46c921906e"
    "9fe74677f7689737a4a6a6d667add17e"
    "8f33910badaacfadcf2a8073611dd82f"
    "6d1af62bbb636cf825d2dfe1549a6cf3"
    "c626f94d8d0fa987bb01f2787dbc4420"
    "695029eb223754315e45870761abb55b"
    "6feadcdcbf3ead575e92db105cd186fe"
    "ae509cf5e889f9c532c14eb98a216ae8"
    "5e87d7a9a178678fe786b7abff005cf0"
    "0e85ba7c7fb32339126d9381e7da4f44"
    "273350c0f10f571d544900fe96d4ec34"
    "c7d31b8e17c461b1dc9f0e0c8d0e7ffb"
    "f1737c7266de8f92b4d437385c19f93e"
    "3e3631353b3db7b8824805913144a611"
    "595a585e5d0bc0b37fd81870dc42c123"
    "be1f8451141793a4544e5d14080a210a"
    "4514ca2938880488c488941001eb0b21"
    "6a52c37b82ca1cc8a63ccb287c001e1a"
    "3e3ddb010000";

inline const char* kSchemSpongeV2Hex =
    "1f8b08000000000002ff4d90414ec340"
    "0c457f6642934ca9380217e80910d920"
    "9058504042a28baaaa4ce224a3269928"
    "63041c8153c384228a57d6d3fff6b70d"
    "60903d150d7724b6d0489e79f4d6f500"
    "94c6fc9a84fec8e253e1646d4b69a015"
    "66b76ceb46a0427bc77d1da89a63f650"
    "559e25d8f557281c2a8c328fd4b208af"
    "e83d80d820f9051a8bcef65c8c54c905"
    "d971d26b9c1d9917d7738091c6e5913a"
    "daefbc04bddf5454d8bece99bc2c1b6a"
    "ab5cdcb07c23e1b17575cd655e51eb79"
    "7bc871fe6ff030be16bcf32dbd6ce463"
    "e0c9b7fdd99e20bb6a5db19fce0fe014"
    "918a940e6e8d28437ad38b15cbde4c59"
    "0dd2150b95419a22bea78e110bfbe905"
    "df40dab4765e010000";

// The same idea at v3, where the palette and the cell data are nested under a
// "Blocks" compound, next to a block-entity list and a biome section to skip.
inline const char* kSchemSpongeV3Hex =
    "1f8b08000000000002ff65505d4bc440"
    "0c9c76ebf5cb4345f0c7f8280a0a9e1e"
    "08e7a3ac6dda866bb7d2cd833fd2dfe4"
    "995e4f7b60603761320933c990be140d"
    "7556b83088373478ee1d006390df5ab1"
    "7fc8c5578893572ea58109b1b827ae1b"
    "41a8e523b95ad130c7e2b9aa3cc938ae"
    "2fdaed76df9ae30c8b9bb62fb6de205b"
    "db964468653f475686f800185c76eca8"
    "186c25d7debad24bef4839a1c172ee58"
    "1eb05f7f3563ef3c14cddb476bddd66b"
    "2f408c6894aef5290255184c7f8ae55e"
    "c69d1316269f8de404e14389b3799b5e"
    "c34b0eb3eefdc1478029348f4eb8efe8"
    "9f93e0d8c9f9bc4d55b1f3fbf1595582"
    "df48911cabd1fdc98ac496ca4b103dd9"
    "8e1089ea317ada816b76d3dc0f776953"
    "cbb6010000";

// A palette whose single entry sits at index 128, so the cell data is a
// multi-byte varint and the palette index is sparse.
inline const char* kSchemSpongeVarintHex =
    "1f8b08000000000002ff3d8e410b8240"
    "1484675d4b5d89fe4fc728e890150475"
    "5ef4a94bba82fb0e1dbdf6abeb49d19c"
    "866fe60dcf20bb962df5965da991dc68"
    "0c6ef000228d7c67d9fec9ea15617177"
    "15b788222c0fe49a96a1c41ec9374255"
    "8ee5b9ae03b19cebb7085fc994b9d88e"
    "98a9b04f01ca20f9018d75ef3c95a3ad"
    "791378f024f98404d9b61bcac7fc8180"
    "785293ca90ee3d3b7614cc3c6b9016c4"
    "b6924a8af8647b42cc1418f80033c702"
    "a4d5000000";

// A palette file whose cell data stops one cell short of the volume: refused,
// because the rest of the build would silently be air.
inline const char* kSchemSpongeShortHex =
    "1f8b08000000000002ff4d90db4ac340"
    "10867fb3b139517c045fa04f20e64614"
    "bcb02a087a514a1937b3c9d2cd81ec88"
    "fa083eb56eac34fe57331fff1c732047"
    "f6a41b6e49ac56489e79f4b6ef00440a"
    "c535091dc9f22bc2e98bada4818ab0b8"
    "655b3782288477dcd5814605160fc678"
    "9650aebe83705068953f9263115ed347"
    "00718ee40f282c5bdbb11ec9c805d971"
    "f22b9ccdcc4bdf7180270a9733ed69bf"
    "f312fc7e6348dbae2e99bcac1a72a694"
    "7e58bd93f0e8fabae6aa34e43c6f0f7b"
    "9cff6b3c8c6f9a77ded1eb463e079eea"
    "b6bfd3136457aed7fbe9fc000accca90"
    "de7462c5b2cfa73c47ba66a12a3853c4"
    "f7d43262613f7de0072a96497b5d0100"
    "00";

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
// The palette fixtures, and the states they were written with. The index each
// cell holds is a formula rather than a table, so a cell read out of order is a
// failure instead of a coincidence.
// ---------------------------------------------------------------------------
inline constexpr int32_t kSpongeWidth = 3;
inline constexpr int32_t kSpongeHeight = 2;
inline constexpr int32_t kSpongeLength = 2;

inline uint32_t sponge_v2_state_index(int32_t x, int32_t y, int32_t z) {
    return static_cast<uint32_t>((x + 2 * y + z) % 4);
}
inline uint32_t sponge_v3_state_index(int32_t x, int32_t y, int32_t z) {
    return static_cast<uint32_t>((x + 3 * y + 2 * z) % 3);
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
