#include "doctest.h"
#include "schematic/nbt_reader.hpp"
#include "schematic_test_data.hpp"

#include <string>
#include <vector>

using schematic_test::nbt_begin;
using schematic_test::nbt_byte;
using schematic_test::nbt_byte_array;
using schematic_test::nbt_end;
using schematic_test::nbt_int;
using schematic_test::nbt_long;
using schematic_test::nbt_short;
using schematic_test::nbt_string;
using schematic_test::nbt_tag;
using schematic_test::put_be32;
using VoxelEngine::schematic::NbtByteOrder;
using VoxelEngine::schematic::NbtReader;
using VoxelEngine::schematic::NbtTag;

namespace {

// A tree with one field of every shape this reader has to handle: scalars, a
// string, a byte array, a nested compound, a list of compounds, and an 8-byte
// double that is never looked at (to prove skipping does not disturb the walk).
std::vector<uint8_t> sample_tree() {
    std::vector<uint8_t> out;
    nbt_begin(out, "Root");
    nbt_tag(out, 2, "Short");
    nbt_short(out, -7);
    nbt_tag(out, 3, "Int");
    nbt_int(out, 123456);
    nbt_tag(out, 4, "Long");
    nbt_long(out, -2);
    nbt_tag(out, 1, "Byte");
    nbt_byte(out, 42);
    nbt_tag(out, 8, "Text");
    nbt_string(out, "hello");
    nbt_tag(out, 7, "Blob");
    nbt_byte_array(out, {9, 8, 7});
    nbt_tag(out, 6, "Ignored");
    for (int i = 0; i < 8; ++i) out.push_back(0);
    nbt_tag(out, 10, "Inner");
    nbt_tag(out, 3, "Deep");
    nbt_int(out, 5);
    nbt_end(out);
    nbt_tag(out, 9, "Points");
    out.push_back(10);  // element type: compound
    put_be32(out, 2);
    nbt_tag(out, 3, "x");
    nbt_int(out, 1);
    nbt_end(out);
    nbt_tag(out, 3, "x");
    nbt_int(out, 2);
    nbt_end(out);
    nbt_end(out);  // end of the root compound
    return out;
}

} // namespace

TEST_CASE("nbt: root must be a compound and carries its name") {
    std::vector<uint8_t> bytes;
    nbt_begin(bytes, "Schematic");
    nbt_end(bytes);

    NbtReader reader(bytes.data(), bytes.size());
    std::string name;
    CHECK(reader.read_root(name));
    CHECK(name == "Schematic");
    CHECK(reader.ok());

    // A non-compound root is refused rather than read as one.
    const std::vector<uint8_t> scalar_root = {2, 0, 1, 'x', 0, 5};
    NbtReader other(scalar_root.data(), scalar_root.size());
    std::string ignored;
    CHECK_FALSE(other.read_root(ignored));
    CHECK(other.error().find("expected Compound") != std::string::npos);
}

TEST_CASE("nbt: every scalar and array type reads back exactly") {
    const std::vector<uint8_t> bytes = sample_tree();
    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));
    CHECK(root == "Root");

    int fields = 0;
    bool saw_inner = false;
    int32_t points_total = 0;
    NbtReader::Entry entry;
    while (reader.next_entry(entry)) {
        ++fields;
        if (entry.name == "Short") {
            int16_t value = 0;
            CHECK(reader.read_i16(value));
            CHECK(value == -7);
        } else if (entry.name == "Int") {
            int32_t value = 0;
            CHECK(reader.read_i32(value));
            CHECK(value == 123456);
        } else if (entry.name == "Long") {
            int64_t value = 0;
            CHECK(reader.read_i64(value));
            CHECK(value == -2);
        } else if (entry.name == "Byte") {
            int8_t value = 0;
            CHECK(reader.read_i8(value));
            CHECK(value == 42);
        } else if (entry.name == "Text") {
            std::string value;
            CHECK(reader.read_string(value));
            CHECK(value == "hello");
        } else if (entry.name == "Blob") {
            const uint8_t* data = nullptr;
            size_t length = 0;
            CHECK(reader.read_byte_array(data, length));
            CHECK(length == 3);
            CHECK(data != nullptr);
            if (data != nullptr) {
                CHECK(data[0] == 9);
                CHECK(data[1] == 8);
                CHECK(data[2] == 7);
            }
        } else if (entry.name == "Inner") {
            saw_inner = true;
            CHECK(reader.push_compound());
            NbtReader::Entry inner;
            while (reader.next_entry(inner)) {
                int32_t value = 0;
                CHECK(inner.name == "Deep");
                CHECK(reader.read_i32(value));
                CHECK(value == 5);
            }
            CHECK(reader.ok());
            CHECK(reader.pop_compound());
        } else if (entry.name == "Points") {
            const bool walked = reader.for_each_compound_element([&reader, &points_total]() {
                NbtReader::Entry element;
                while (reader.next_entry(element)) {
                    int32_t value = 0;
                    CHECK(reader.read_i32(value));
                    points_total += value;
                }
                return reader.ok();
            });
            CHECK(walked);
        } else {
            // "Ignored" (a double) and anything else must skip cleanly.
            CHECK(reader.skip_value());
        }
    }
    CHECK(reader.ok());
    CHECK(fields == 9);
    CHECK(saw_inner);
    CHECK(points_total == 3);

    // The cursor reached the root's End tag under its own steam.
    CHECK(reader.depth() == 0);
}

TEST_CASE("nbt: the reader refuses to walk past an unread payload") {
    const std::vector<uint8_t> bytes = sample_tree();
    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));

    NbtReader::Entry entry;
    CHECK(reader.next_entry(entry));  // "Short", payload untouched
    CHECK_FALSE(reader.next_entry(entry));
    CHECK(reader.error().find("payload") != std::string::npos);
}

TEST_CASE("nbt: a field read as the wrong type is an error, not a coercion") {
    const std::vector<uint8_t> bytes = sample_tree();
    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));

    NbtReader::Entry entry;
    CHECK(reader.next_entry(entry));
    CHECK(entry.name == "Short");
    int32_t as_int = 0;
    CHECK_FALSE(reader.read_i32(as_int));
    CHECK(reader.error().find("expected Int") != std::string::npos);
}

TEST_CASE("nbt: read_any_int takes short, int and long alike") {
    std::vector<uint8_t> bytes;
    nbt_begin(bytes, "Root");
    nbt_tag(bytes, 2, "A");
    nbt_short(bytes, 300);
    nbt_tag(bytes, 3, "B");
    nbt_int(bytes, 70000);
    nbt_tag(bytes, 4, "C");
    nbt_long(bytes, 5000000000LL);
    nbt_end(bytes);

    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));
    NbtReader::Entry entry;
    int64_t a = 0, b = 0, c = 0;
    while (reader.next_entry(entry)) {
        int64_t* target = entry.name == "A" ? &a : (entry.name == "B" ? &b : &c);
        CHECK(reader.read_any_int(*target));
    }
    CHECK(reader.ok());
    CHECK(a == 300);
    CHECK(b == 70000);
    CHECK(c == 5000000000LL);
}

TEST_CASE("nbt: malformed input fails instead of reading past the buffer") {
    const std::vector<uint8_t> bytes = sample_tree();

    // Cut mid-field.
    NbtReader truncated(bytes.data(), bytes.size() / 2);
    std::string root;
    CHECK(truncated.read_root(root));
    NbtReader::Entry entry;
    int guard = 0;
    while (truncated.next_entry(entry)) {
        if (!truncated.skip_value()) break;
        if (++guard > 100) break;  // never loops: it must fail on its own
    }
    CHECK_FALSE(truncated.ok());

    // An unknown tag type is refused by name.
    std::vector<uint8_t> bad;
    nbt_begin(bad, "Root");
    bad.push_back(13);  // past LongArray
    bad.push_back(0);
    bad.push_back(1);
    bad.push_back('x');
    NbtReader unknown(bad.data(), bad.size());
    CHECK(unknown.read_root(root));
    CHECK_FALSE(unknown.next_entry(entry));
    CHECK(unknown.error().find("unknown tag type") != std::string::npos);
}

TEST_CASE("nbt: a compound nested past the cap is refused, not recursed into") {
    std::vector<uint8_t> bytes;
    nbt_begin(bytes, "Root");
    for (int i = 0; i < 100; ++i) nbt_tag(bytes, 10, "c");
    nbt_tag(bytes, 3, "v");
    nbt_int(bytes, 1);
    for (int i = 0; i < 100; ++i) nbt_end(bytes);
    nbt_end(bytes);

    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));
    NbtReader::Entry entry;
    CHECK(reader.next_entry(entry));
    CHECK(entry.type == NbtTag::Compound);
    CHECK_FALSE(reader.skip_value());
    CHECK(reader.error().find("deeper than the cap") != std::string::npos);
}

TEST_CASE("nbt: skipping a compound lands the cursor on the next sibling") {
    std::vector<uint8_t> bytes;
    nbt_begin(bytes, "Root");
    nbt_tag(bytes, 10, "SkipMe");
    nbt_tag(bytes, 3, "junk");
    nbt_int(bytes, 1);
    nbt_tag(bytes, 9, "junklist");
    bytes.push_back(3);  // element type: int
    put_be32(bytes, 2);
    put_be32(bytes, 7);
    put_be32(bytes, 8);
    nbt_end(bytes);
    nbt_tag(bytes, 3, "Kept");
    nbt_int(bytes, 99);
    nbt_end(bytes);

    NbtReader reader(bytes.data(), bytes.size());
    std::string root;
    CHECK(reader.read_root(root));
    NbtReader::Entry entry;
    CHECK(reader.next_entry(entry));
    CHECK(entry.name == "SkipMe");
    CHECK(reader.skip_value());

    CHECK(reader.next_entry(entry));
    CHECK(entry.name == "Kept");
    int32_t value = 0;
    CHECK(reader.read_i32(value));
    CHECK(value == 99);
}

// -----------------------------------------------------------------------------
// Byte order. The tree below is hand-built little-endian — name lengths too,
// which is the detail that makes reading it as big-endian fail rather than
// quietly produce different numbers.
// -----------------------------------------------------------------------------

TEST_CASE("nbt: little-endian trees read, and the same bytes as big-endian do not") {
    std::vector<uint8_t> bytes;
    bytes.push_back(10);  // root compound
    bytes.push_back(4);
    bytes.push_back(0);
    for (char c : std::string("Root")) bytes.push_back(static_cast<uint8_t>(c));

    bytes.push_back(2);  // Short "Value" = 4660
    bytes.push_back(5);
    bytes.push_back(0);
    for (char c : std::string("Value")) bytes.push_back(static_cast<uint8_t>(c));
    bytes.push_back(0x34);
    bytes.push_back(0x12);

    bytes.push_back(3);  // Int "Count" = 5
    bytes.push_back(5);
    bytes.push_back(0);
    for (char c : std::string("Count")) bytes.push_back(static_cast<uint8_t>(c));
    bytes.push_back(5);
    bytes.push_back(0);
    bytes.push_back(0);
    bytes.push_back(0);

    bytes.push_back(0);  // end

    NbtReader little(bytes.data(), bytes.size(), NbtByteOrder::LittleEndian);
    std::string root;
    CHECK(little.read_root(root));
    CHECK(root == "Root");
    NbtReader::Entry entry;
    CHECK(little.next_entry(entry));
    CHECK(entry.name == "Value");
    int16_t short_value = 0;
    CHECK(little.read_i16(short_value));
    CHECK(short_value == 4660);
    CHECK(little.next_entry(entry));
    CHECK(entry.name == "Count");
    int32_t count = 0;
    CHECK(little.read_i32(count));
    CHECK(count == 5);
    CHECK(little.ok());

    // The same buffer read as big-endian: the name length is 0x0400, so the walk
    // runs off the end instead of returning garbled fields.
    NbtReader big(bytes.data(), bytes.size(), NbtByteOrder::BigEndian);
    CHECK_FALSE(big.read_root(root));
}
