#include "doctest.h"
#include "schematic/minimal_json.hpp"

#include <string>

using VoxelEngine::schematic::JsonValue;
using VoxelEngine::schematic::parse_json;

namespace {

// Parses and requires success, so a failure message names what went wrong.
JsonValue must_parse(const std::string& text) {
    JsonValue value;
    std::string error;
    const bool ok = parse_json(text, value, &error);
    CHECK_MESSAGE(ok, "parse failed: " << error << " for: " << text);
    return value;
}

// Parses and requires failure, returning the message.
std::string must_fail(const std::string& text) {
    JsonValue value;
    std::string error;
    const bool ok = parse_json(text, value, &error);
    CHECK_MESSAGE(!ok, "expected a parse failure for: " << text);
    return error;
}

} // namespace

TEST_CASE("json: scalars carry their own type") {
    CHECK(must_parse("null").is_null());
    CHECK(must_parse("true").is_bool());
    CHECK(must_parse("true").as_bool() == true);
    CHECK(must_parse("false").as_bool() == false);
    CHECK(must_parse("42").is_number());
    CHECK(must_parse("42").as_integer() == 42);
    CHECK(must_parse("-7").as_integer() == -7);
    CHECK(must_parse("0").as_integer() == 0);
    CHECK(must_parse("2147483647").as_integer() == 2147483647);
    // A fractional value keeps its number type; an integer read of it truncates
    // toward zero rather than rounding, which is what a data-value field wants.
    CHECK(must_parse("3.5").is_number());
    CHECK(must_parse("-3.5").as_integer() == -3);
    CHECK(must_parse("1e3").as_number() == doctest::Approx(1000.0));
    CHECK(must_parse("2.5E-2").as_number() == doctest::Approx(0.025));

    // A type mismatch must not read as a plausible value.
    CHECK(must_parse("\"true\"").is_string());
    CHECK(must_parse("\"true\"").as_bool() == false);
}

TEST_CASE("json: type names are reported for every kind") {
    CHECK(std::string(must_parse("null").type_name()) == "null");
    CHECK(std::string(must_parse("true").type_name()) == "boolean");
    CHECK(std::string(must_parse("1").type_name()) == "number");
    CHECK(std::string(must_parse("\"x\"").type_name()) == "string");
    CHECK(std::string(must_parse("[]").type_name()) == "array");
    CHECK(std::string(must_parse("{}").type_name()) == "object");
}

TEST_CASE("json: strings handle escapes and unicode") {
    CHECK(must_parse("\"\"").as_string().empty());
    CHECK(must_parse("\"plain\"").as_string() == "plain");
    CHECK(must_parse("\"a\\nb\\tc\\\"q\\\"\\\\\"").as_string() == "a\nb\tc\"q\"\\");
    CHECK(must_parse("\"\\u00e9\"").as_string() == "\xc3\xa9");           // é in UTF-8
    CHECK(must_parse("\"\\u0041\"").as_string() == "A");
    CHECK(must_parse("\"\\/\\b\\f\\r\"").as_string() == "/\b\f\r");
    // A surrogate pair is one code point, encoded as four UTF-8 bytes.
    CHECK(must_parse("\"\\ud83d\\ude00\"").as_string() == "\xf0\x9f\x98\x80");
}

TEST_CASE("json: objects keep their order and refuse duplicate keys") {
    const JsonValue object = must_parse("{\"b\":1,\"a\":2,\"c\":{\"d\":[1,2]}}");
    CHECK(object.is_object());
    CHECK(object.members().size() == 3);
    CHECK(object.members()[0].first == "b");
    CHECK(object.members()[1].first == "a");
    CHECK(object.members()[2].first == "c");

    CHECK(object.member("a") != nullptr);
    CHECK(object.member("a")->as_integer() == 2);
    CHECK(object.member("missing") == nullptr);
    // A missing key and a null value are different answers.
    CHECK(must_parse("{\"a\":null}").member("a") != nullptr);
    CHECK(must_parse("{\"a\":null}").member("a")->is_null());
    // Neither accessor may be trusted on the wrong type.
    CHECK(must_parse("[1]").member("a") == nullptr);
    CHECK(must_parse("1").member("a") == nullptr);

    const std::string error = must_fail("{\"a\":1,\"a\":2}");
    CHECK(error.find("duplicate key \"a\"") != std::string::npos);

    // Nested lookups reach the leaves.
    const JsonValue nested = must_parse("{\"c\":{\"d\":[7]}}");
    const JsonValue* inner = nested.member("c");
    CHECK(inner != nullptr);
    const JsonValue* list = inner->member("d");
    CHECK(list != nullptr);
    CHECK(list->items().size() == 1);
    CHECK(list->items()[0].as_integer() == 7);
}

TEST_CASE("json: arrays keep their order") {
    const JsonValue array = must_parse("[1,\"two\",false,null,[3]]");
    CHECK(array.is_array());
    CHECK(array.items().size() == 5);
    CHECK(array.items()[0].as_integer() == 1);
    CHECK(array.items()[1].as_string() == "two");
    CHECK(array.items()[2].as_bool() == false);
    CHECK(array.items()[3].is_null());
    CHECK(array.items()[4].items()[0].as_integer() == 3);

    CHECK(must_parse("[]").items().empty());
    CHECK(must_parse("{}").members().empty());
    CHECK(must_parse("[\n  1,\n  2\n]").items().size() == 2);
}

TEST_CASE("json: whitespace around the document is fine, anything else is not") {
    CHECK(must_parse("  \n\t{ \"a\" : 1 }\r\n ").member("a")->as_integer() == 1);

    const std::string trailing = must_fail("1 2");
    CHECK(trailing.find("trailing content") != std::string::npos);
    CHECK(trailing.find("offset") != std::string::npos);

    CHECK(!must_fail("").empty());
    CHECK(!must_fail("   ").empty());
}

TEST_CASE("json: the traps a hand-edited file falls into are all refused") {
    // Trailing commas (both positions), comments, and bare keys are not JSON.
    CHECK(!must_fail("{\"a\":1,}").empty());
    CHECK(!must_fail("[1,]").empty());
    CHECK(!must_fail("{\"a\":1 // note\n}").empty());
    CHECK(!must_fail("{\"a\" 1}").empty());
    CHECK(!must_fail("{a:1}").empty());
    CHECK(!must_fail("{'a':1}").empty());
    CHECK(!must_fail("[1;2]").empty());
    // Structural damage.
    CHECK(!must_fail("\"unterminated").empty());
    CHECK(!must_fail("{\"a\":1").empty());
    CHECK(!must_fail("[1,2").empty());
    CHECK(!must_fail("tru").empty());
    CHECK(!must_fail("\"\\q\"").empty());
    CHECK(!must_fail("\"\\u00\"").empty());
    CHECK(!must_fail("\"\\u00zz\"").empty());
    CHECK(!must_fail("\"\\ud83d\"").empty());        // lone high surrogate
    CHECK(!must_fail("\"\\ud83dx\"").empty());       // unpaired surrogate
    CHECK(!must_fail("\"a\nb\"").empty());           // raw newline in a string
    CHECK(!must_fail(".").empty());
    CHECK(!must_fail("+1").empty());
    CHECK(!must_fail("01").empty());
    CHECK(!must_fail("1.").empty());
    CHECK(!must_fail("1e").empty());
}

TEST_CASE("json: every error names the offset it gave up at") {
    CHECK(must_fail("{\"a\":1,}").find("offset 7") != std::string::npos);
    CHECK(must_fail("[1,]").find("offset 3") != std::string::npos);
    CHECK(must_fail("{\"a\" 1}").find("offset 5") != std::string::npos);
    CHECK(must_fail("1 2").find("offset 2") != std::string::npos);
}

TEST_CASE("json: nesting is capped so a malformed file cannot recurse forever") {
    std::string deep;
    for (int i = 0; i < 100; ++i) deep += "[";
    const std::string error = must_fail(deep);
    CHECK(error.find("nested deeper than the cap") != std::string::npos);

    // And a document inside the cap still parses.
    std::string ok;
    for (int i = 0; i < 20; ++i) ok += "[";
    ok += "1";
    for (int i = 0; i < 20; ++i) ok += "]";
    JsonValue value;
    std::string ok_error;
    CHECK_MESSAGE(parse_json(ok, value, &ok_error), ok_error);
}

TEST_CASE("json: a failed parse leaves the output empty rather than half-built") {
    JsonValue value = must_parse("{\"a\":1,\"b\":2}");
    CHECK(value.is_object());

    std::string error;
    CHECK_FALSE(parse_json("{\"a\":1,\"b\":}", value, &error));
    // The caller must not be able to read a stale document after a failure.
    CHECK(value.is_null());

    // Error reporting is optional: the same calls work with no string to fill.
    JsonValue other;
    CHECK_FALSE(parse_json("nope", other, nullptr));
    CHECK(parse_json("{}", other, nullptr));
}
