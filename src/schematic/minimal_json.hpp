#ifndef FARLANDS_MINIMAL_JSON_HPP
#define FARLANDS_MINIMAL_JSON_HPP

// -----------------------------------------------------------------------------
// Enough JSON for a data file, and no more.
//
// The engine's own config loaders go through `godot::JSON`, which only works
// once the extension is loaded inside the game. The schematic translation table
// has to be read by `bin/schematic_report` and by the test binary as well, and
// those have no engine runtime — so the table is parsed here instead, by the
// same code in both places.
//
// Scope is a complete reader for the JSON subset a hand-written data file uses:
// objects, arrays, strings with escapes, numbers, booleans, null. It is strict
// on purpose, because a table is data someone edits by hand and the failure
// worth catching is a typo, not a missing feature:
//   - trailing commas, comments and bare keys are rejected (not JSON),
//   - duplicate keys in one object are rejected rather than silently letting the
//     last one win,
//   - nesting is capped so a malformed file cannot recurse without bound.
// Every error names the byte offset it gave up at.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace VoxelEngine {
namespace schematic {

namespace detail {
// The recursive-descent reader, declared here so it can be a friend and fill a
// JsonValue in place without exposing setters on the value type.
struct JsonParser;
} // namespace detail

class JsonValue {
public:
    enum class Type : uint8_t { Null = 0, Bool, Number, String, Array, Object };

    [[nodiscard]] Type type() const noexcept { return type_; }
    [[nodiscard]] bool is_null() const noexcept { return type_ == Type::Null; }
    [[nodiscard]] bool is_bool() const noexcept { return type_ == Type::Bool; }
    [[nodiscard]] bool is_number() const noexcept { return type_ == Type::Number; }
    [[nodiscard]] bool is_string() const noexcept { return type_ == Type::String; }
    [[nodiscard]] bool is_array() const noexcept { return type_ == Type::Array; }
    [[nodiscard]] bool is_object() const noexcept { return type_ == Type::Object; }

    [[nodiscard]] bool as_bool() const noexcept { return bool_; }
    [[nodiscard]] double as_number() const noexcept { return number_; }
    [[nodiscard]] int64_t as_integer() const noexcept { return static_cast<int64_t>(number_); }
    [[nodiscard]] const std::string& as_string() const noexcept { return text_; }
    [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
    [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
        return members_;
    }
    // Null when the key is absent. Objects here are small, so this is a scan.
    [[nodiscard]] const JsonValue* member(const std::string& key) const noexcept;

    [[nodiscard]] const char* type_name() const noexcept;

private:
    friend bool parse_json(const char* text, size_t length, JsonValue& out, std::string* error);
    friend struct detail::JsonParser;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0.0;
    std::string text_;
    std::vector<JsonValue> items_;
    std::vector<std::pair<std::string, JsonValue>> members_;
};

// Parses a whole document; trailing content other than whitespace is an error.
bool parse_json(const char* text, size_t length, JsonValue& out, std::string* error = nullptr);
inline bool parse_json(const std::string& text, JsonValue& out, std::string* error = nullptr) {
    return parse_json(text.data(), text.size(), out, error);
}

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_MINIMAL_JSON_HPP
