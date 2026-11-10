#include "doctest.h"
#include "world/light_band_index.hpp"

using namespace VoxelEngine;

TEST_CASE("light band index is empty until a chunk is set") {
    LightBandIndex index;
    CHECK_FALSE(index.query(0, 0, 0).any());
    CHECK(index.column_count() == 0);
}

TEST_CASE("light band index separates the two channels") {
    LightBandIndex index;
    index.set_chunk(2, 5, 3, true, false);
    LightBandIndex::Flags flags = index.query(2, 5, 3);
    CHECK(flags.emissive);
    CHECK_FALSE(flags.shade);

    index.set_chunk(2, 5, 3, false, true);
    flags = index.query(2, 5, 3);
    CHECK_FALSE(flags.emissive);
    CHECK(flags.shade);
}

TEST_CASE("light band index covers the full 3x3x3 and one level either side") {
    LightBandIndex index;
    index.set_chunk(4, 6, 7, true, false);

    CHECK(index.query(4, 6, 7).emissive);
    CHECK(index.query(4, 5, 7).emissive);
    CHECK(index.query(4, 7, 7).emissive);
    CHECK(index.query(3, 6, 6).emissive);
    CHECK(index.query(5, 6, 8).emissive);

    CHECK_FALSE(index.query(4, 4, 7).any());
    CHECK_FALSE(index.query(4, 8, 7).any());
    CHECK_FALSE(index.query(2, 5, 7).any());
    CHECK_FALSE(index.query(4, 5, 9).any());
}

TEST_CASE("light band index keeps one column's levels independent") {
    LightBandIndex index;
    index.set_chunk(1, 1, 1, true, false);
    index.set_chunk(1, 31, 1, false, true);
    CHECK(index.column_count() == 1);

    LightBandIndex::Flags low = index.query(1, 1, 1);
    CHECK(low.emissive);
    CHECK_FALSE(low.shade);

    LightBandIndex::Flags high = index.query(1, 31, 1);
    CHECK_FALSE(high.emissive);
    CHECK(high.shade);

    CHECK_FALSE(index.query(1, 16, 1).any());
}

TEST_CASE("light band index clears a chunk exactly") {
    LightBandIndex index;
    index.set_chunk(0, 5, 0, true, true);
    index.set_chunk(0, 5, 0, false, false);
    CHECK_FALSE(index.query(0, 5, 0).any());
    CHECK(index.column_count() == 0);

    index.set_chunk(0, 5, 0, true, false);
    index.erase_chunk(0, 5, 0);
    CHECK_FALSE(index.query(0, 5, 0).any());
    CHECK(index.column_count() == 0);
}

TEST_CASE("light band index handles the world's edge levels") {
    LightBandIndex index;
    index.set_chunk(0, 0, 0, true, false);
    CHECK(index.query(0, 0, 0).emissive);
    CHECK(index.query(0, 1, 0).emissive);
    CHECK_FALSE(index.query(0, 2, 0).any());

    index.set_chunk(0, LightBandIndex::kChunkLevels - 1, 0, false, true);
    CHECK(index.query(0, LightBandIndex::kChunkLevels - 1, 0).shade);
    CHECK(index.query(0, LightBandIndex::kChunkLevels - 2, 0).shade);
}

TEST_CASE("light band index is conservative outside the world height") {
    LightBandIndex index;
    index.set_chunk(0, -1, 0, true, false);
    CHECK(index.query(0, 0, 0).emissive);
    CHECK(index.query(100, 100, 100).emissive);

    index.clear();
    CHECK_FALSE(index.query(0, 0, 0).any());
}