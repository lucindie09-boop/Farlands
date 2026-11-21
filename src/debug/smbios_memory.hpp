#ifndef FARLANDS_DEBUG_SMBIOS_MEMORY_HPP
#define FARLANDS_DEBUG_SMBIOS_MEMORY_HPP

#include <cstddef>
#include <cstdint>
#include <string>

#include "debug/system_stats.hpp"

// The RAM's own description -- generation, rated speed, module sizes -- which no
// OS memory total carries. It is in the firmware's SMBIOS table, one Memory
// Device record (type 17) per populated slot:
//
//   Windows  GetSystemFirmwareTable('RSMB', ...): the table follows an 8-byte
//            RawSMBIOSData header, so the caller passes the payload, not the file.
//   Linux    /sys/firmware/dmi/tables/DMI: the same table with no header (and
//            root-only on most distributions, so this usually reports unknown).
//
// The parser is header-only and godot-free so tests/test_system_stats.cpp can
// drive it with a synthetic table.
namespace farlands {
namespace detail {

// Offsets from the SMBIOS specification (DSP0134), Type 17. The structure's own
// Length byte says which of the later ones exist, so every read is bounded by it:
// a 2.1 record is 0x15 bytes and has no extended size at all.
namespace type17 {
constexpr std::size_t kSize = 0x0C;              // WORD, MB; 0x7FFF -> kExtendedSize
constexpr std::size_t kMemoryType = 0x12;        // BYTE
constexpr std::size_t kSpeed = 0x15;             // WORD, MT/s; 0xFFFF -> kExtendedSpeed
constexpr std::size_t kExtendedSize = 0x1C;      // DWORD, MB
constexpr std::size_t kExtendedSpeed = 0x5C;     // DWORD, MT/s (3.3+)
constexpr std::uint16_t kUseExtendedSize = 0x7FFF;
constexpr std::uint16_t kUnknown = 0xFFFF;
}  // namespace type17

inline const char* ram_type_name(std::uint8_t code) {
    switch (code) {
        case 0x12: return "DDR";
        case 0x13: return "DDR2";
        case 0x18: return "DDR3";
        case 0x1A: return "DDR4";
        case 0x1B: return "LPDDR";
        case 0x1C: return "LPDDR2";
        case 0x1D: return "LPDDR3";
        case 0x1E: return "LPDDR4";
        case 0x1F: return "NVDIMM";
        case 0x20: return "HBM";
        case 0x21: return "HBM2";
        case 0x22: return "DDR5";
        case 0x23: return "LPDDR5";
        case 0x24: return "HBM3";
        default: return "";
    }
}

}  // namespace detail

// Fill `out` from the structures in `data`. False when the table holds no
// populated Memory Device record, in which case nothing is written.
[[nodiscard]] inline bool parse_smbios_memory(const unsigned char* data, std::size_t size,
                                              RamSummary& out) {
    if (data == nullptr || size < 4) {
        return false;
    }
    int modules = 0;
    int total_mb = 0;
    int first_mb = 0;
    int speed = 0;
    bool uniform = true;
    std::string type;

    std::size_t offset = 0;
    while (offset + 4 <= size) {
        const unsigned char* record = data + offset;
        const auto length = static_cast<std::size_t>(record[1]);
        if (length < 4 || offset + length > size) {
            break;  // a truncated or malformed record: nothing after it is trustworthy
        }
        const std::uint8_t record_type = record[0];
        if (record_type == 127) {
            break;  // end-of-table
        }
        if (record_type == 17) {
            const auto word_at = [&](std::size_t at) {
                return at + 2 <= length
                           ? static_cast<std::uint16_t>(record[at] | (record[at + 1] << 8))
                           : 0;
            };
            const auto dword_at = [&](std::size_t at) {
                return at + 4 <= length
                           ? (static_cast<std::uint32_t>(record[at]) |
                              (static_cast<std::uint32_t>(record[at + 1]) << 8) |
                              (static_cast<std::uint32_t>(record[at + 2]) << 16) |
                              (static_cast<std::uint32_t>(record[at + 3]) << 24))
                           : 0;
            };
            const std::uint16_t size_mb = word_at(detail::type17::kSize);
            const std::uint64_t megabytes =
                size_mb == detail::type17::kUseExtendedSize
                    ? static_cast<std::uint64_t>(dword_at(detail::type17::kExtendedSize))
                    : static_cast<std::uint64_t>(size_mb);
            // 0 is an empty slot and 0xFFFF a device whose size the firmware does
            // not report: neither is a module that is fitted.
            if (size_mb != detail::type17::kUnknown && megabytes > 0 && megabytes <= 0xFFFFFFFFull) {
                const auto record_speed = word_at(detail::type17::kSpeed);
                const int rate = record_speed == detail::type17::kUnknown
                                     ? static_cast<int>(dword_at(detail::type17::kExtendedSpeed))
                                     : static_cast<int>(record_speed);
                total_mb += static_cast<int>(megabytes);
                ++modules;
                // Only a set of identical modules can be described as "2x 16 GB";
                // a mixed pair is left to the total alone.
                if (first_mb == 0) {
                    first_mb = static_cast<int>(megabytes);
                } else if (static_cast<std::uint64_t>(first_mb) != megabytes) {
                    uniform = false;
                }
                if (speed == 0 && rate > 0) {
                    speed = rate;
                }
                if (type.empty()) {
                    type = detail::ram_type_name(length > detail::type17::kMemoryType
                                                     ? record[detail::type17::kMemoryType]
                                                     : 0);
                }
            }
        }
        // The formatted area is `length` bytes; the strings after it are NUL
        // terminated and the set ends with an empty one (a double NUL), which is
        // where the next structure starts.
        std::size_t next = offset + length;
        while (next + 1 < size && !(data[next] == 0 && data[next + 1] == 0)) {
            ++next;
        }
        offset = next + 2;
    }

    if (modules == 0) {
        return false;
    }
    out.type = type;
    out.speed_mts = speed;
    out.modules = modules;
    out.module_mb = uniform ? first_mb : 0;
    out.total_mb = total_mb;
    return true;
}

}  // namespace farlands

#endif  // FARLANDS_DEBUG_SMBIOS_MEMORY_HPP
