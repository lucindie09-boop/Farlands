#ifndef FARLANDS_NBT_READER_HPP
#define FARLANDS_NBT_READER_HPP

// -----------------------------------------------------------------------------
// NBT, read-only and dependency-free.
//
// NBT is the tagged container every block-file format is built on: a tree of
// named values with explicit tags, and byte order that depends on the writer
// (the classic schematic/region files are big-endian, the newer structure files
// are little-endian). The reader is a forward cursor rather than a tree: a
// caller walks the fields it wants and skips the rest. That matters for the
// files this exists for — a schematic's payload is a handful of root fields,
// while its tile-entity list can be the largest thing in the file and is never
// wanted, so skipping it must not allocate.
//
// The cursor is strict on purpose. Reading past a payload, reading a field as
// the wrong type, or running off the end of the buffer is an error instead of a
// coercion, because a silently misread schematic pastes the wrong blocks.
// -----------------------------------------------------------------------------

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace VoxelEngine {
namespace schematic {

enum class NbtTag : uint8_t {
    End = 0,
    Byte = 1,
    Short = 2,
    Int = 3,
    Long = 4,
    Float = 5,
    Double = 6,
    ByteArray = 7,
    String = 8,
    List = 9,
    Compound = 10,
    IntArray = 11,
    LongArray = 12
};

// Which byte order the writer used for every multi-byte value in the tree.
enum class NbtByteOrder : uint8_t { BigEndian = 0, LittleEndian = 1 };

[[nodiscard]] const char* nbt_tag_name(NbtTag tag) noexcept;

class NbtReader {
public:
    NbtReader(const uint8_t* data, size_t size,
              NbtByteOrder order = NbtByteOrder::BigEndian) noexcept;

    [[nodiscard]] bool ok() const noexcept { return error_.empty(); }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }
    // Nesting depth the cursor is currently inside (0 = the root compound).
    [[nodiscard]] size_t depth() const noexcept { return depth_; }
    [[nodiscard]] size_t bytes_left() const noexcept { return size_ - (cursor_ < size_ ? cursor_ : size_); }

    // Reads the root tag header: it must be a compound, and its name is returned.
    bool read_root(std::string& root_name);

    struct Entry {
        NbtTag type = NbtTag::End;
        std::string name;
    };

    // Reads the next entry of the compound the cursor sits in. Returns false at
    // that compound's End tag, or on error — check ok() to tell them apart. The
    // previous entry's payload must have been read or skipped first.
    bool next_entry(Entry& out);

    // Payload reads for the entry next_entry just returned. A type mismatch is
    // an error. read_any_int exists because writers disagree about which integer
    // tag a dimension gets (the classic format uses Short, some tools Int).
    bool read_i8(int8_t& value);
    bool read_i16(int16_t& value);
    bool read_i32(int32_t& value);
    bool read_i64(int64_t& value);
    bool read_any_int(int64_t& value);
    bool read_string(std::string& value);
    // Zero-copy: the returned pointer is into the caller's buffer and stays valid
    // for as long as it does.
    bool read_byte_array(const uint8_t*& data, size_t& length);
    // Copies an int array's values, converted to host order. Unlike the block
    // arrays these are tiny — an offset, a block-entity position — so they are
    // converted on the way out rather than viewed in place.
    bool read_int_array(std::vector<int32_t>& out);
    // Reads a list entry's element type and count. The elements follow and must
    // each be consumed (push_compound / skip_value) before the enclosing
    // compound can be walked again.
    bool read_list_header(NbtTag& element_type, int32_t& count);

    // Skips the current entry's payload, recursing through compound and list
    // payloads and allocating nothing.
    bool skip_value();

    // Enters a compound payload so next_entry iterates inside it: either the
    // entry next_entry just returned, or, right after read_list_header, the
    // element the list is currently on. pop_compound consumes the rest of that
    // compound (skipping anything unread) and returns to the enclosing one.
    bool push_compound();
    bool pop_compound();

    // Iterates a list of compounds. `visit` runs once per element with the cursor
    // inside that element, reads what it wants through the usual accessors, and
    // returns false to abort the walk.
    template <typename Visit>
    bool for_each_compound_element(Visit&& visit) {
        NbtTag element_type = NbtTag::End;
        int32_t count = 0;
        if (!read_list_header(element_type, count)) return false;
        if (element_type != NbtTag::Compound) return fail("list elements are not compounds");
        for (int32_t i = 0; i < count; ++i) {
            if (!push_compound()) return false;
            if (!visit()) return false;
            if (!pop_compound()) return false;
        }
        return true;
    }

private:
    bool read_bytes(void* destination, size_t length);
    bool read_tag(NbtTag& tag);
    // A length/count integer read without the pending-type check, because the
    // payload itself is what is carrying it (array lengths, list counts).
    bool read_raw_i32(int32_t& value);
    bool skip_payload(NbtTag type, int nesting);
    bool fail(const std::string& message);
    bool require_payload(NbtTag expected);

    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    size_t cursor_ = 0;
    NbtByteOrder order_ = NbtByteOrder::BigEndian;
    size_t depth_ = 0;
    // The depth the compound open right now was entered at, so pop_compound can
    // tell whether the caller already walked it to its End tag.
    size_t compound_depth_ = 0;
    NbtTag pending_type_ = NbtTag::End;
    bool payload_pending_ = false;
    // A list header has been read and this many elements are still unread.
    int32_t list_remaining_ = 0;
    NbtTag list_element_type_ = NbtTag::End;
    std::string error_;
};

} // namespace schematic
} // namespace VoxelEngine

#endif // FARLANDS_NBT_READER_HPP
