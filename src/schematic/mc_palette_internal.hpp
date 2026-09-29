#pragma once

// The one piece of the Minecraft block table that both halves of the palette
// need, and the reason it is not file-local: a "names" row is parsed while the
// table loads (mc_palette.cpp) and its {wood} placeholder is substituted while a
// state is resolved (mc_palette_resolve.cpp).

#include "schematic/mc_palette.hpp"
#include "schematic/minimal_json.hpp"

namespace VoxelEngine {
namespace schematic {
namespace mc_palette_detail {

// The placeholder a "names" row uses for the species its pattern captured.
constexpr const char* kWoodPlaceholder = "{wood}";

// The placeholder a named variant set uses for the block it is applied to.
constexpr const char* kFamilyPlaceholder = "{family}";

// Whether `text` carries that placeholder at all.
[[nodiscard]] bool has_placeholder(const std::string& text) noexcept;

// The readers the classic rows (mc_palette_classic_rows.cpp) share with the name
// rows (mc_palette.cpp): how a load failure is reported, which keys are comments,
// and one "variants" object. They used to be file-local in mc_palette.cpp.

// Legacy ids were a byte, plus the AddBlocks nibble for later additions. The cap is
// generous rather than exact: it exists to catch a typo like 5300, not to model one
// particular version's id space.
constexpr int64_t kMaxLegacyId = 4095;

// Records `message` as the load's failure reason and returns false, so a reader can
// `return fail(error, ...)`.
bool fail(std::string* error, const std::string& message);

// The same, naming the row by its id / by its key.
bool fail_row(std::string* error, uint16_t id, const std::string& message);
bool fail_name(std::string* error, const std::string& key, const std::string& message);

// Keys starting with '_' are notes to the reader, everywhere in the file.
[[nodiscard]] bool is_comment_key(const std::string& key) noexcept;

// One "variants" object: data value -> block name (or null to skip that value).
bool parse_variants(const JsonValue& object, std::vector<std::pair<uint8_t, std::string>>& out,
                    const std::string& where, std::string* error);

} // namespace mc_palette_detail
} // namespace schematic
} // namespace VoxelEngine
