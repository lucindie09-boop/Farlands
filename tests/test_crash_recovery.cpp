#include "doctest.h"
#include "core/edit_map.hpp"
#include "core/block_types.hpp"
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

// =========================================================================
// Crash-during-save recovery tests.
//
// Chunk edits are persisted as chunk_*.edit files using the atomic pattern:
//   write .tmp -> remove old .bak -> rename .edit -> .bak -> rename .tmp -> .edit
// (ChunkWorld::write_edit_map_file_locked). A process killed at any point in
// that sequence leaves a specific on-disk residue:
//   crash #1 (writing .tmp):      .edit(old) + .bak(old) + .tmp(partial)
//   crash #2 (after rm .bak):     .edit(old) + .tmp(partial)
//   crash #3 (after edit->bak):   .edit(missing) + .bak(old) + .tmp(partial)
//   crash #4 (after tmp->edit):   .edit(new) + .bak(old)   <-- normal state
//
// These tests reconstruct each residue on real files, then load through
// recover_edit_map() — the SAME decision function ChunkWorld::load_edit_map_from_disk
// uses — so the production recovery path is exercised end-to-end at the byte
// level, not via a mirrored copy.
// =========================================================================

using namespace VoxelEngine;

namespace {

// Small deterministic LCG so the stress test is reproducible.
uint32_t g_rng = 0xABCDEF01;
uint32_t next_rand() {
    g_rng = g_rng * 1664525u + 1013904223u;
    return g_rng;
}

void write_bytes(const std::string& path, const std::vector<uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}

std::vector<uint8_t> read_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                 std::istreambuf_iterator<char>());
}

void cleanup_files(const std::vector<std::string>& paths) {
    for (const auto& p : paths) {
        std::remove(p.c_str());
    }
}

// A canonical edit map (valid, always decodes with initialize_default_blocks()).
EditMap make_edit_map() {
    EditMap m;
    m.set_block(3, 4, 5, BlockIDs::STONE);
    m.set_block(10, 10, 10, BlockIDs::GRASS);
    m.set_block(31, 0, 31, BlockIDs::LIGHT_BLOCK);
    m.set_block(5, 5, 5, BlockIDs::SAND);
    m.set_block(20, 20, 20, BlockIDs::WOOD);
    return m;
}

std::vector<uint8_t> encode(const EditMap& m) {
    std::vector<uint8_t> out;
    serialize_edit_map(m, out);
    return out;
}

// Flip a body byte so the header CRC no longer matches (header untouched).
std::vector<uint8_t> corrupt_body(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = data;
    if (out.size() > 12) {
        out[12 + (out.size() - 12) / 2] ^= 0xFF;
    }
    return out;
}

// Random byte flips: always at least one, so the result is (astronomically
// likely) invalid.
std::vector<uint8_t> mutate(const std::vector<uint8_t>& data) {
    std::vector<uint8_t> out = data;
    if (out.empty()) return out;
    int flips = 1 + static_cast<int>(next_rand() % 4);
    for (int i = 0; i < flips; i++) {
        out[next_rand() % out.size()] ^= static_cast<uint8_t>(1u << (next_rand() % 8));
    }
    return out;
}

// Cut the file short somewhere in the middle (simulates a truncated write).
std::vector<uint8_t> truncate_at(const std::vector<uint8_t>& data) {
    if (data.size() <= 12) {
        return std::vector<uint8_t>(data.data(), data.data() + data.size() / 2);
    }
    size_t cut = 12 + next_rand() % (data.size() - 12);
    return std::vector<uint8_t>(data.data(), data.data() + cut);
}

// Load from real on-disk .edit/.bak files through the shared recovery decision.
bool maps_equal(const EditMap& a, const EditMap& b) {
    if (a.size() != b.size()) return false;
    for (const auto& entry : a.edits) {
        auto it = b.edits.find(entry.first);
        if (it == b.edits.end() || it->second != entry.second) return false;
    }
    return true;
}

} // anonymous namespace

TEST_CASE("crash recovery: healthy .edit wins over stale .bak (normal state)") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap current = make_edit_map();
    const EditMap older = make_edit_map(); // same-block set here; sizes match

    const std::string edit_path = "test_recovery_normal.edit";
    const std::string bak_path = "test_recovery_normal.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, encode(current));
    write_bytes(bak_path, encode(older));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK_FALSE(rec.used_backup);
    CHECK(maps_equal(out, current));

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: crash #1/#2 writing .tmp orphans it; .edit still wins") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap current = make_edit_map();
    const EditMap half_written = make_edit_map(); // .tmp residue
    std::vector<uint8_t> partial = encode(half_written);
    if (partial.size() > 12) partial.resize(12 + (partial.size() - 12) / 2);

    const std::string edit_path = "test_recovery_tmp.edit";
    const std::string bak_path = "test_recovery_tmp.edit.bak";
    const std::string tmp_path = "test_recovery_tmp.edit.tmp";
    cleanup_files({edit_path, bak_path, tmp_path});

    write_bytes(edit_path, encode(current));
    write_bytes(bak_path, encode(current));
    write_bytes(tmp_path, partial); // killed mid-write of .tmp

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    // Recovery only consults .edit/.bak; the orphaned partial .tmp must be ignored
    // and the last committed .edit must load.
    CHECK(rec.recovered);
    CHECK_FALSE(rec.used_backup);
    CHECK(maps_equal(out, current));

    cleanup_files({edit_path, bak_path, tmp_path});
}

TEST_CASE("crash recovery: crash #3 between renames (.edit missing, .bak present)") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap committed = make_edit_map();
    const EditMap unrelated_tmp = make_edit_map();
    std::vector<uint8_t> partial = encode(unrelated_tmp);
    if (partial.size() > 12) partial.resize(12 + (partial.size() - 12) / 2);

    const std::string edit_path = "test_recovery_gap.edit";
    const std::string bak_path = "test_recovery_gap.edit.bak";
    const std::string tmp_path = "test_recovery_gap.edit.tmp";
    cleanup_files({edit_path, bak_path, tmp_path});

    // .edit is GONE (renamed to .bak), .bak holds the last committed snapshot,
    // and a partial .tmp from the aborted write is also present.
    write_bytes(bak_path, encode(committed));
    write_bytes(tmp_path, partial);

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK(rec.used_backup);
    CHECK(maps_equal(out, committed));

    cleanup_files({edit_path, bak_path, tmp_path});
}

TEST_CASE("crash recovery: corrupt .edit CRC falls back to .bak") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap committed = make_edit_map();
    std::vector<uint8_t> corrupt = corrupt_body(encode(committed));

    const std::string edit_path = "test_recovery_crc.edit";
    const std::string bak_path = "test_recovery_crc.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, corrupt);
    write_bytes(bak_path, encode(committed));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK(rec.used_backup);
    CHECK(maps_equal(out, committed));

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: truncated .edit falls back to .bak") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap committed = make_edit_map();
    std::vector<uint8_t> truncated = encode(committed);
    truncated.resize(13); // header + one body byte

    const std::string edit_path = "test_recovery_trunc.edit";
    const std::string bak_path = "test_recovery_trunc.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, truncated);
    write_bytes(bak_path, encode(committed));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK(rec.used_backup);
    CHECK(maps_equal(out, committed));

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: zero-length .edit falls back to .bak") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap committed = make_edit_map();

    const std::string edit_path = "test_recovery_empty.edit";
    const std::string bak_path = "test_recovery_empty.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, {});
    write_bytes(bak_path, encode(committed));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK(rec.used_backup);
    CHECK(maps_equal(out, committed));

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: both .edit and .bak corrupt -> no decode, no partial map") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap current = make_edit_map();

    const std::string edit_path = "test_recovery_both.edit";
    const std::string bak_path = "test_recovery_both.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, corrupt_body(encode(current)));
    write_bytes(bak_path, corrupt_body(encode(current)));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK_FALSE(rec.recovered);
    CHECK_FALSE(rec.used_backup);
    CHECK(out.size() == 0); // failed recovery must never leave partial data behind

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: valid .edit wins over corrupt .bak") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap current = make_edit_map();

    const std::string edit_path = "test_recovery_ok_badbak.edit";
    const std::string bak_path = "test_recovery_ok_badbak.edit.bak";
    cleanup_files({edit_path, bak_path});

    write_bytes(edit_path, encode(current));
    write_bytes(bak_path, corrupt_body(encode(current)));

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK(rec.recovered);
    CHECK_FALSE(rec.used_backup);
    CHECK(maps_equal(out, current));

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery: neither file present -> no recovery") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const std::string edit_path = "test_recovery_none.edit";
    const std::string bak_path = "test_recovery_none.edit.bak";
    cleanup_files({edit_path, bak_path});

    EditMap out;
    EditMapRecovery rec = recover_edit_map(read_bytes(edit_path), read_bytes(bak_path), out,
                                           BlockRegistry::get_instance());
    CHECK_FALSE(rec.recovered);
    CHECK_FALSE(rec.used_backup);
    CHECK(out.size() == 0);

    cleanup_files({edit_path, bak_path});
}

TEST_CASE("crash recovery stress: randomized crash residues never crash and honor invariants") {
    BlockRegistry::get_instance().initialize_default_blocks();

    const EditMap committed = make_edit_map();
    const std::vector<uint8_t> valid = encode(committed);
    const std::vector<uint8_t> empty;

    const int iterations = 3000;
    for (int i = 0; i < iterations; i++) {
        uint32_t r = next_rand() % 8;

        // Pick the primary (.edit) and backup (.bak) residue bytes.
        std::vector<uint8_t> primary;
        std::vector<uint8_t> backup;
        switch (r) {
            case 0: primary = valid;             backup = mutate(valid);              break;
            case 1: primary = mutate(valid);     backup = valid;                      break;
            case 2: primary = valid;             backup = valid;                      break;
            case 3: primary = truncate_at(valid);backup = valid;                      break;
            case 4: primary = valid;             backup = truncate_at(valid);         break;
            case 5: primary = corrupt_body(valid); backup = valid;                    break;
            case 6: primary = empty;             backup = valid;                      break;
            default: primary = mutate(valid);    backup = mutate(valid);              break;
        }

        EditMap out;
        EditMapRecovery rec = recover_edit_map(primary, backup, out,
                                               BlockRegistry::get_instance());

        // Invariant 1: failed recovery leaves no partial map behind.
        if (!rec.recovered) {
            CHECK(out.size() == 0);
            continue;
        }

        // Invariant 2: when the primary is the exact committed bytes, recovery
        // must always prefer it (a healthy .edit never yields to .bak).
        if (primary == valid) {
            CHECK_FALSE(rec.used_backup);
            CHECK(maps_equal(out, committed));
            continue;
        }

        // Invariant 3: when the primary is unusable but the backup is intact,
        // the backup must recover the exact committed map.
        if (backup == valid) {
            CHECK(rec.used_backup);
            CHECK(maps_equal(out, committed));
            continue;
        }

        // Invariant 4: otherwise the recovery is best-effort; whatever it
        // returned must at least round-trip consistently.
        CHECK(rec.recovered);
        std::vector<uint8_t> re_encoded;
        serialize_edit_map(out, re_encoded);
        EditMap again;
        CHECK(deserialize_edit_map(re_encoded.data(), re_encoded.size(), again,
                                   BlockRegistry::get_instance()));
        CHECK(maps_equal(out, again));
    }
}