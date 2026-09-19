#include "schematic/minimal_json.hpp"

#include <cstdlib>
#include <cstring>

namespace VoxelEngine {
namespace schematic {
namespace {

constexpr int kMaxNesting = 64;

bool fail(std::string* error, size_t offset, const char* what) {
    if (error) {
        *error = "json: " + std::string(what) + " at offset " + std::to_string(offset);
    }
    return false;
}

} // namespace

namespace detail {

struct JsonParser {
    const char* text = nullptr;
    size_t size = 0;
    size_t at = 0;
    std::string* error = nullptr;

    [[nodiscard]] bool done() const { return at >= size; }
    [[nodiscard]] char peek() const { return at < size ? text[at] : '\0'; }

    void skip_space() {
        while (at < size) {
            const char c = text[at];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++at;
            } else {
                break;
            }
        }
    }

    bool expect(char c) {
        if (at >= size) return fail(error, at, "unexpected end of input");
        if (text[at] != c) {
            const std::string what = std::string("expected '") + c + "'";
            return fail(error, at, what.c_str());
        }
        ++at;
        return true;
    }

    bool literal(const char* word) {
        const size_t length = std::strlen(word);
        if (at + length > size || std::memcmp(text + at, word, length) != 0) {
            return fail(error, at, "invalid literal");
        }
        at += length;
        return true;
    }

    // Appends a code point as UTF-8.
    static void append_utf8(std::string& out, uint32_t code) {
        if (code <= 0x7F) {
            out.push_back(static_cast<char>(code));
        } else if (code <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else if (code <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (code >> 18)));
            out.push_back(static_cast<char>(0x80 | ((code >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
    }

    bool hex4(uint32_t& out) {
        if (at + 4 > size) return fail(error, at, "truncated \\u escape");
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = text[at + static_cast<size_t>(i)];
            uint32_t digit = 0;
            if (c >= '0' && c <= '9') digit = static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') digit = static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') digit = static_cast<uint32_t>(c - 'A' + 10);
            else return fail(error, at + static_cast<size_t>(i), "invalid hex digit");
            value = (value << 4) | digit;
        }
        at += 4;
        out = value;
        return true;
    }

    bool parse_string(std::string& out) {
        if (!expect('"')) return false;
        out.clear();
        while (true) {
            if (at >= size) return fail(error, at, "unterminated string");
            const char c = text[at++];
            if (c == '"') return true;
            if (c != '\\') {
                if (static_cast<unsigned char>(c) < 0x20) {
                    return fail(error, at - 1, "raw control character in string");
                }
                out.push_back(c);
                continue;
            }
            if (at >= size) return fail(error, at, "unterminated escape");
            const char escape = text[at++];
            switch (escape) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u': {
                    uint32_t code = 0;
                    if (!hex4(code)) return false;
                    // A surrogate pair is two escapes; anything else is one code point.
                    if (code >= 0xD800 && code <= 0xDBFF) {
                        if (at + 1 < size && text[at] == '\\' && text[at + 1] == 'u') {
                            at += 2;
                            uint32_t low = 0;
                            if (!hex4(low)) return false;
                            if (low >= 0xDC00 && low <= 0xDFFF) {
                                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                            } else {
                                return fail(error, at, "malformed surrogate pair");
                            }
                        } else {
                            return fail(error, at, "lone surrogate escape");
                        }
                    }
                    append_utf8(out, code);
                    break;
                }
                default:
                    return fail(error, at - 1, "invalid escape");
            }
        }
    }

    bool parse_number(JsonValue& out) {
        const size_t start = at;
        if (peek() == '-') ++at;
        if (at >= size || text[at] < '0' || text[at] > '9') return fail(error, at, "invalid number");
        if (text[at] == '0') {
            ++at;
        } else {
            while (at < size && text[at] >= '0' && text[at] <= '9') ++at;
        }
        if (at < size && text[at] == '.') {
            ++at;
            if (at >= size || text[at] < '0' || text[at] > '9') {
                return fail(error, at, "digit expected after the decimal point");
            }
            while (at < size && text[at] >= '0' && text[at] <= '9') ++at;
        }
        if (at < size && (text[at] == 'e' || text[at] == 'E')) {
            ++at;
            if (at < size && (text[at] == '+' || text[at] == '-')) ++at;
            if (at >= size || text[at] < '0' || text[at] > '9') {
                return fail(error, at, "digit expected in the exponent");
            }
            while (at < size && text[at] >= '0' && text[at] <= '9') ++at;
        }
        const std::string literal(text + start, at - start);
        out.type_ = JsonValue::Type::Number;
        out.number_ = std::strtod(literal.c_str(), nullptr);
        out.text_ = literal;
        return true;
    }

    bool parse_value(JsonValue& out, int depth) {
        if (depth > kMaxNesting) return fail(error, at, "nested deeper than the cap");
        skip_space();
        if (at >= size) return fail(error, at, "unexpected end of input");
        switch (peek()) {
            case '{': return parse_object(out, depth);
            case '[': return parse_array(out, depth);
            case '"':
                out.type_ = JsonValue::Type::String;
                return parse_string(out.text_);
            case 't':
                if (!literal("true")) return false;
                out.type_ = JsonValue::Type::Bool;
                out.bool_ = true;
                return true;
            case 'f':
                if (!literal("false")) return false;
                out.type_ = JsonValue::Type::Bool;
                out.bool_ = false;
                return true;
            case 'n':
                if (!literal("null")) return false;
                out.type_ = JsonValue::Type::Null;
                return true;
            default:
                return parse_number(out);
        }
    }

    bool parse_array(JsonValue& out, int depth) {
        if (!expect('[')) return false;
        out.type_ = JsonValue::Type::Array;
        out.items_.clear();
        skip_space();
        if (peek() == ']') {
            ++at;
            return true;
        }
        while (true) {
            JsonValue item;
            if (!parse_value(item, depth + 1)) return false;
            out.items_.push_back(std::move(item));
            skip_space();
            if (peek() == ',') {
                ++at;
                continue;
            }
            return expect(']');
        }
    }

    bool parse_object(JsonValue& out, int depth) {
        if (!expect('{')) return false;
        out.type_ = JsonValue::Type::Object;
        out.members_.clear();
        skip_space();
        if (peek() == '}') {
            ++at;
            return true;
        }
        while (true) {
            skip_space();
            std::string key;
            if (!parse_string(key)) return false;
            // A repeated key is a mistake in a hand-edited table, and json has no
            // way to express it, so it is refused rather than resolved last-wins.
            for (const auto& member : out.members_) {
                if (member.first == key) {
                    const std::string what = "duplicate key \"" + key + "\"";
                    return fail(error, at, what.c_str());
                }
            }
            skip_space();
            if (!expect(':')) return false;
            JsonValue value;
            if (!parse_value(value, depth + 1)) return false;
            out.members_.emplace_back(std::move(key), std::move(value));
            skip_space();
            if (peek() == ',') {
                ++at;
                continue;
            }
            return expect('}');
        }
    }
};

} // namespace detail

const JsonValue* JsonValue::member(const std::string& key) const noexcept {
    for (const auto& entry : members_) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

const char* JsonValue::type_name() const noexcept {
    switch (type_) {
        case Type::Null: return "null";
        case Type::Bool: return "boolean";
        case Type::Number: return "number";
        case Type::String: return "string";
        case Type::Array: return "array";
        case Type::Object: return "object";
    }
    return "unknown";
}

bool parse_json(const char* text, size_t length, JsonValue& out, std::string* error) {
    out = JsonValue{};
    if (text == nullptr) return fail(error, 0, "no input");
    detail::JsonParser parser;
    parser.text = text;
    parser.size = length;
    parser.error = error;
    // Parsed into a local and committed only on success, so a caller that reads
    // its value despite a false return gets an empty document rather than one
    // that stops wherever the file went wrong.
    JsonValue document;
    if (!parser.parse_value(document, 0)) return false;
    parser.skip_space();
    if (!parser.done()) return fail(error, parser.at, "trailing content after the document");
    out = std::move(document);
    return true;
}

} // namespace schematic
} // namespace VoxelEngine
