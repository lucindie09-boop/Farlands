#include "schematic/nbt_reader.hpp"

#include <cstring>

namespace VoxelEngine {
namespace schematic {
namespace {

// A name is length-prefixed with a 16-bit count, so it can never exceed this.
constexpr size_t kMaxNameBytes = 0xFFFF;
// Nesting a malformed file may describe. The formats this reads are two or three
// levels deep; the cap is what stops a corrupt length from recursing forever.
constexpr int kMaxNesting = 64;

} // namespace

const char* nbt_tag_name(NbtTag tag) noexcept {
    switch (tag) {
        case NbtTag::End: return "End";
        case NbtTag::Byte: return "Byte";
        case NbtTag::Short: return "Short";
        case NbtTag::Int: return "Int";
        case NbtTag::Long: return "Long";
        case NbtTag::Float: return "Float";
        case NbtTag::Double: return "Double";
        case NbtTag::ByteArray: return "ByteArray";
        case NbtTag::String: return "String";
        case NbtTag::List: return "List";
        case NbtTag::Compound: return "Compound";
        case NbtTag::IntArray: return "IntArray";
        case NbtTag::LongArray: return "LongArray";
    }
    return "Unknown";
}

NbtReader::NbtReader(const uint8_t* data, size_t size, NbtByteOrder order) noexcept
    : data_(data), size_(size), order_(order) {
    if (data == nullptr) fail("no input bytes");
}

bool NbtReader::fail(const std::string& message) {
    if (error_.empty()) error_ = message;
    return false;
}

bool NbtReader::read_bytes(void* destination, size_t length) {
    if (cursor_ + length > size_) return fail("read past end of buffer");
    std::memcpy(destination, data_ + cursor_, length);
    cursor_ += length;
    return true;
}

bool NbtReader::read_tag(NbtTag& tag) {
    uint8_t raw = 0;
    if (!read_bytes(&raw, 1)) return false;
    if (raw > static_cast<uint8_t>(NbtTag::LongArray)) return fail("unknown tag type " + std::to_string(raw));
    tag = static_cast<NbtTag>(raw);
    return true;
}

bool NbtReader::read_raw_i32(int32_t& value) {
    uint8_t raw[4] = {0, 0, 0, 0};
    if (!read_bytes(raw, 4)) return false;
    uint32_t joined = 0;
    if (order_ == NbtByteOrder::BigEndian) {
        joined = (static_cast<uint32_t>(raw[0]) << 24) | (static_cast<uint32_t>(raw[1]) << 16) |
                 (static_cast<uint32_t>(raw[2]) << 8) | static_cast<uint32_t>(raw[3]);
    } else {
        joined = (static_cast<uint32_t>(raw[3]) << 24) | (static_cast<uint32_t>(raw[2]) << 16) |
                 (static_cast<uint32_t>(raw[1]) << 8) | static_cast<uint32_t>(raw[0]);
    }
    value = static_cast<int32_t>(joined);
    return true;
}

bool NbtReader::read_root(std::string& root_name) {
    NbtTag root = NbtTag::End;
    if (!read_tag(root)) return false;
    if (root != NbtTag::Compound) {
        return fail(std::string("root tag is ") + nbt_tag_name(root) + ", expected Compound");
    }
    return read_string(root_name);
}

bool NbtReader::next_entry(Entry& out) {
    if (!ok()) return false;
    if (payload_pending_) return fail("previous entry's payload was never read or skipped");
    // Reading on at the level a list sits on would walk straight past that
    // list's unread elements. Inside a pushed compound (depth above zero) the
    // entries belong to some element, so the check does not apply.
    if (list_remaining_ > 0 && depth_ == 0) return fail("list still has unread elements");

    NbtTag type = NbtTag::End;
    if (!read_tag(type)) return false;
    if (type == NbtTag::End) {
        // The compound just closed. At depth 0 that is the root; deeper in, it is
        // the compound push_compound opened.
        if (depth_ > 0) --depth_;
        return false;
    }
    std::string name;
    if (!read_string(name)) return false;
    out.type = type;
    out.name = std::move(name);
    pending_type_ = type;
    payload_pending_ = true;
    return true;
}

bool NbtReader::require_payload(NbtTag expected) {
    if (!ok()) return false;
    if (payload_pending_) {
        if (pending_type_ != expected) {
            return fail(std::string("field is ") + nbt_tag_name(pending_type_) + ", expected " +
                        nbt_tag_name(expected));
        }
        return true;
    }
    if (list_element_type_ != NbtTag::End && list_remaining_ > 0) {
        if (list_element_type_ != expected) {
            return fail(std::string("list element is ") + nbt_tag_name(list_element_type_) +
                        ", expected " + nbt_tag_name(expected));
        }
        return true;
    }
    return fail("no value to read");
}

bool NbtReader::read_i8(int8_t& value) {
    if (!require_payload(NbtTag::Byte)) return false;
    uint8_t raw = 0;
    if (!read_bytes(&raw, 1)) return false;
    value = static_cast<int8_t>(raw);
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_i16(int16_t& value) {
    if (!require_payload(NbtTag::Short)) return false;
    uint8_t raw[2] = {0, 0};
    if (!read_bytes(raw, 2)) return false;
    const uint16_t joined = order_ == NbtByteOrder::BigEndian
                                ? static_cast<uint16_t>((raw[0] << 8) | raw[1])
                                : static_cast<uint16_t>((raw[1] << 8) | raw[0]);
    value = static_cast<int16_t>(joined);
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_i32(int32_t& value) {
    if (!require_payload(NbtTag::Int)) return false;
    if (!read_raw_i32(value)) return false;
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_i64(int64_t& value) {
    if (!require_payload(NbtTag::Long)) return false;
    uint8_t raw[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    if (!read_bytes(raw, 8)) return false;
    uint64_t joined = 0;
    for (int i = 0; i < 8; ++i) {
        const int shift = order_ == NbtByteOrder::BigEndian ? (7 - i) : i;
        joined |= static_cast<uint64_t>(raw[i]) << (shift * 8);
    }
    value = static_cast<int64_t>(joined);
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_any_int(int64_t& value) {
    if (!ok()) return false;
    if (!payload_pending_ && !(list_remaining_ > 0 && list_element_type_ != NbtTag::End)) {
        return fail("no value to read");
    }
    const NbtTag type = payload_pending_ ? pending_type_ : list_element_type_;
    switch (type) {
        case NbtTag::Byte: {
            int8_t small = 0;
            if (!read_i8(small)) return false;
            value = small;
            return true;
        }
        case NbtTag::Short: {
            int16_t small = 0;
            if (!read_i16(small)) return false;
            value = small;
            return true;
        }
        case NbtTag::Int: {
            int32_t small = 0;
            if (!read_i32(small)) return false;
            value = small;
            return true;
        }
        case NbtTag::Long: {
            int64_t big = 0;
            if (!read_i64(big)) return false;
            value = big;
            return true;
        }
        default:
            return fail(std::string("field is ") + nbt_tag_name(type) + ", expected an integer");
    }
}

bool NbtReader::read_string(std::string& value) {
    if (!ok()) return false;
    // read_root needs to read a name before any payload is pending, so a String
    // payload is read directly when nothing is pending instead of via
    // require_payload.
    if (payload_pending_ && pending_type_ != NbtTag::String) {
        return fail(std::string("field is ") + nbt_tag_name(pending_type_) + ", expected String");
    }

    uint8_t length_bytes[2] = {0, 0};
    if (!read_bytes(length_bytes, 2)) return false;
    const size_t length = order_ == NbtByteOrder::BigEndian
                              ? (static_cast<size_t>(length_bytes[0]) << 8) | length_bytes[1]
                              : (static_cast<size_t>(length_bytes[1]) << 8) | length_bytes[0];
    if (length > kMaxNameBytes * 4) return fail("string longer than its length prefix allows");
    value.resize(length);
    if (length > 0 && !read_bytes(value.data(), length)) return false;
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_byte_array(const uint8_t*& data, size_t& length) {
    if (!require_payload(NbtTag::ByteArray)) return false;
    int32_t count = 0;
    if (!read_raw_i32(count)) return false;
    if (count < 0) return fail("negative byte array length");
    if (cursor_ + static_cast<size_t>(count) > size_) return fail("byte array runs past end of buffer");
    data = data_ + cursor_;
    length = static_cast<size_t>(count);
    cursor_ += length;
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_int_array(std::vector<int32_t>& out) {
    if (!require_payload(NbtTag::IntArray)) return false;
    int32_t count = 0;
    if (!read_raw_i32(count)) return false;
    if (count < 0) return fail("negative int array length");
    out.clear();
    out.resize(static_cast<size_t>(count));
    for (int32_t i = 0; i < count; ++i) {
        if (!read_raw_i32(out[static_cast<size_t>(i)])) return false;
    }
    payload_pending_ = false;
    return true;
}

bool NbtReader::read_list_header(NbtTag& element_type, int32_t& count) {
    if (!require_payload(NbtTag::List)) return false;
    NbtTag type = NbtTag::End;
    if (!read_tag(type)) return false;
    if (!read_raw_i32(count)) return false;
    if (count < 0) return fail("negative list length");
    payload_pending_ = false;
    element_type = type;
    list_element_type_ = type;
    list_remaining_ = count;
    return true;
}

bool NbtReader::skip_payload(NbtTag type, int nesting) {
    if (nesting > kMaxNesting) return fail("nested values deeper than the cap");
    switch (type) {
        case NbtTag::Byte:
        case NbtTag::Float:
            cursor_ += 1;
            break;
        case NbtTag::Short:
            cursor_ += 2;
            break;
        case NbtTag::Int:
            cursor_ += 4;
            break;
        case NbtTag::Long:
        case NbtTag::Double:
            cursor_ += 8;
            break;
        case NbtTag::ByteArray:
        case NbtTag::IntArray:
        case NbtTag::LongArray: {
            int32_t count = 0;
            if (!read_raw_i32(count)) return false;
            if (count < 0) return fail("negative array length");
            const size_t stride = type == NbtTag::ByteArray ? 1 : (type == NbtTag::IntArray ? 4 : 8);
            if (static_cast<size_t>(count) > (size_ - (cursor_ < size_ ? cursor_ : size_)) / stride) {
                return fail("array runs past end of buffer");
            }
            cursor_ += static_cast<size_t>(count) * stride;
            break;
        }
        case NbtTag::String: {
            std::string discarded;
            if (!read_string(discarded)) return false;
            break;
        }
        case NbtTag::List: {
            // The header is read raw here: this path is reached with no payload
            // pending (skip_value cleared it), so the accessor would refuse it.
            NbtTag element_type = NbtTag::End;
            if (!read_tag(element_type)) return false;
            int32_t count = 0;
            if (!read_raw_i32(count)) return false;
            if (count < 0) return fail("negative list length");
            if (count > 0 && element_type == NbtTag::End) return fail("list of End tags");
            for (int32_t i = 0; i < count; ++i) {
                if (!skip_payload(element_type, nesting + 1)) return false;
            }
            break;
        }
        case NbtTag::Compound: {
            // Walk the entries to its matching End tag.
            for (;;) {
                NbtTag child = NbtTag::End;
                if (!read_tag(child)) return false;
                if (child == NbtTag::End) break;
                std::string name;
                if (!read_string(name)) return false;
                if (!skip_payload(child, nesting + 1)) return false;
            }
            break;
        }
        case NbtTag::End:
            return fail("an End tag has no payload");
    }
    if (cursor_ > size_) return fail("value runs past end of buffer");
    return true;
}

bool NbtReader::skip_value() {
    if (!ok()) return false;
    if (payload_pending_) {
        const NbtTag type = pending_type_;
        payload_pending_ = false;
        return skip_payload(type, 0);
    }
    if (list_remaining_ > 0 && list_element_type_ != NbtTag::End) {
        if (!skip_payload(list_element_type_, 0)) return false;
        if (--list_remaining_ == 0) list_element_type_ = NbtTag::End;
        return true;
    }
    return fail("no value to skip");
}

bool NbtReader::push_compound() {
    if (!ok()) return false;
    if (list_remaining_ > 0 && !payload_pending_) {
        if (list_element_type_ != NbtTag::Compound) return fail("list element is not a compound");
        compound_depth_ = ++depth_;
        return true;
    }
    if (!require_payload(NbtTag::Compound)) return false;
    payload_pending_ = false;
    compound_depth_ = ++depth_;
    return true;
}

bool NbtReader::pop_compound() {
    if (!ok()) return false;
    // Only keep consuming while the compound this opened is still open: once its
    // End tag has been read (by the caller or by the loop below) depth_ drops
    // below the level it was entered at, and anything further belongs to the
    // enclosing compound or the next list element.
    Entry entry;
    while (depth_ >= compound_depth_) {
        if (!next_entry(entry)) break;
        if (!skip_value()) return false;
    }
    if (!ok()) return false;
    if (list_element_type_ != NbtTag::End && list_remaining_ > 0) --list_remaining_;
    return true;
}

} // namespace schematic
} // namespace VoxelEngine
