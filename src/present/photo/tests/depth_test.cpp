// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Tilt-shift on a fixed picture, and haze by inverting the classic
// projection over a flat plain, a ramp and a plateau. The bytes and the
// heights are pinned. One, two and eight threads, and no pool, agree. The
// fog of war keeps the pixels it is drawn on.
#include "oa/present/photo/looks.hpp"

#include "oa/base/sha256.hpp"
#include "oa/platform/job_pool.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

using oa::present::photo::HazeField;
using oa::present::photo::Look;
using oa::present::photo::Picture;
using oa::present::photo::apply_look;
using oa::present::photo::column_ground_heights;
using oa::present::photo::look_idle;
using oa::present::photo::photo_tilt_radius;
using oa::present::photo::photo_tilt_width;

constexpr int32_t kWidth = 64;
constexpr int32_t kHeight = 36;

/// The picture every tilt case starts from.
///
/// @return packed RGB
std::vector<uint8_t> source_picture() {
    std::vector<uint8_t> rgb(static_cast<std::size_t>(kWidth * kHeight * 3));
    for (int32_t row = 0; row < kHeight; ++row) {
        for (int32_t column = 0; column < kWidth; ++column) {
            uint8_t* pixel =
                rgb.data() +
                (static_cast<std::size_t>(row) * kWidth + static_cast<std::size_t>(column)) * 3U;
            pixel[0] = static_cast<uint8_t>((column * 3 + row * 5) & 255);
            pixel[1] = static_cast<uint8_t>((column * 7 + row) & 255);
            pixel[2] = static_cast<uint8_t>((column + row * 11) & 255);
        }
    }
    return rgb;
}

/// The digest of packed bytes, as lower-case hex.
///
/// @param bytes the bytes
/// @return 64 hex digits
std::string digest_of(const std::vector<uint8_t>& bytes) {
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return std::string(hex.data(), hex.size());
}

/// Applies a look and returns the picture.
///
/// @param look the look
/// @param pool the pool; null runs on the caller
/// @param haze the ground; null hazes nothing
/// @return packed RGB
std::vector<uint8_t>
painted(const Look& look, oa::platform::job_pool::Pool* pool, const HazeField* haze) {
    std::vector<uint8_t> rgb = source_picture();
    Picture picture;
    picture.bytes = rgb.data();
    picture.width = kWidth;
    picture.height = kHeight;
    picture.channels = 3;
    apply_look(picture, look, nullptr, pool, haze);
    return rgb;
}

/// One column of heights the field reports, the same for every map x.
struct FixedColumn {
    const int32_t* heights{nullptr};
    int32_t rows{0};
};

/// Writes the fixed column, from map row `first_row`.
void fixed_column(
    void* user, int32_t /*map_x*/, int32_t first_row, int32_t* heights, int32_t count
) {
    const auto* column = static_cast<const FixedColumn*>(user);
    if (column == nullptr || heights == nullptr)
        return;
    for (int32_t row = 0; row < count; ++row) {
        const int32_t at = first_row + row;
        if (column->heights != nullptr && at >= 0 && at < column->rows)
            heights[row] = column->heights[at];
        else
            heights[row] = -1;
    }
}

/// True when two pictures are the same.
bool same(const std::vector<uint8_t>& left, const std::vector<uint8_t>& right) {
    return left == right;
}

void check_scale() {
    CHECK(photo_tilt_width(0, 4) == 0);
    CHECK(photo_tilt_width(48, 1) == 48);
    CHECK(photo_tilt_width(48, 2) == 96);
    CHECK(photo_tilt_width(48, 4) == 192);
    CHECK(photo_tilt_radius(0, 4) == 0);
    CHECK(photo_tilt_radius(12, 3) == 36);
    CHECK(photo_tilt_width(-4, 2) == 0);
}

void check_tilt() {
    Look idle;
    idle.tilt_centre = 50;
    idle.tilt_width = 48;
    CHECK(look_idle(idle));
    const std::vector<uint8_t> source = source_picture();
    CHECK(same(painted(idle, nullptr, nullptr), source));

    Look look;
    look.tilt_centre = 50;
    look.tilt_width = 8;
    look.tilt_radius = 6;
    CHECK(!look_idle(look));
    oa::platform::job_pool::Pool one(1);
    oa::platform::job_pool::Pool two(2);
    oa::platform::job_pool::Pool eight(8);
    const std::vector<uint8_t> caller = painted(look, nullptr, nullptr);
    const std::vector<uint8_t> on_one = painted(look, &one, nullptr);
    const std::vector<uint8_t> on_two = painted(look, &two, nullptr);
    const std::vector<uint8_t> on_eight = painted(look, &eight, nullptr);
    CHECK(same(caller, on_one));
    CHECK(same(caller, on_two));
    CHECK(same(caller, on_eight));
    // The centre row of a 36-row picture at 50% is row 18. The band is 8
    // pixels wide, so that row stays sharp. A corner is outside the band.
    const std::size_t centre = static_cast<std::size_t>(18 * kWidth) * 3U;
    const std::size_t corner = 0;
    CHECK(std::memcmp(caller.data() + centre, source.data() + centre, 3) == 0);
    CHECK(std::memcmp(caller.data() + corner, source.data() + corner, 3) != 0);
    const std::string digest = digest_of(caller);
    constexpr const char* kPinned =
        "3ff444e2ec53bcec5d8f2304bbd5dc3084bf9c8fab0c7ecae06a8e9dc1565ce8";
    if (digest != kPinned) {
        std::fprintf(stderr, "tilt digest %s\n", digest.c_str());
        ++failures;
    }
}

void expect_column(
    const char* name, const std::vector<int32_t>& got, const std::vector<int32_t>& want
) {
    if (got == want)
        return;
    std::fprintf(stderr, "%s:\n", name);
    for (int32_t height : got)
        std::fprintf(stderr, " %d", height);
    std::fprintf(stderr, "\n");
    ++failures;
}

void check_heights() {
    constexpr int32_t kRows = 12;
    std::vector<int32_t> out(static_cast<std::size_t>(kRows), 7);
    std::vector<int32_t> flat(static_cast<std::size_t>(kRows), 0);
    column_ground_heights(flat.data(), kRows, 0, 256, kRows, out.data());
    expect_column("flat", out, flat);

    std::vector<int32_t> past(16, -1);
    column_ground_heights(flat.data(), kRows, 0, 256, 16, past.data());
    for (int row = 0; row < kRows; ++row)
        CHECK(past[static_cast<std::size_t>(row)] == 0);
    for (int row = kRows; row < 16; ++row)
        CHECK(past[static_cast<std::size_t>(row)] == -1);

    // Height 8 shifts the plain up by 4. Rows the samples reach read 8.
    std::vector<int32_t> raised(6, 8);
    std::vector<int32_t> shifted(8, -2);
    column_ground_heights(raised.data(), 6, 0, 256, 8, shifted.data());
    const int32_t shifted_want[] = {8, 8, -1, -1, -1, -1, -1, -1};
    expect_column(
        "shifted", shifted, std::vector<int32_t>(std::begin(shifted_want), std::end(shifted_want))
    );

    // A ramp whose height rises with the map row. The inversion is pinned.
    const int32_t ramp_heights[] = {0, 0, 2, 2, 4, 4};
    std::vector<int32_t> ramp(6, -2);
    column_ground_heights(ramp_heights, 6, 0, 256, 6, ramp.data());
    const int32_t ramp_want[] = {0, 2, 4, 4, -1, -1};
    expect_column("ramp", ramp, std::vector<int32_t>(std::begin(ramp_want), std::end(ramp_want)));
    CHECK(ramp[0] <= ramp[1] && ramp[1] <= ramp[2]);

    // A plateau of height 8 on rows 4, 5 and 6. Those rows project four
    // screen rows higher, and the rows they cover read the plateau.
    std::vector<int32_t> plateau(static_cast<std::size_t>(kRows), 0);
    plateau[4] = 8;
    plateau[5] = 8;
    plateau[6] = 8;
    std::vector<int32_t> covered(static_cast<std::size_t>(kRows), -2);
    column_ground_heights(plateau.data(), kRows, 0, 256, kRows, covered.data());
    const int32_t plateau_want[] = {8, 8, 8, 7, 5, 4, 2, 0, 0, 0, 0, 0};
    expect_column(
        "plateau", covered, std::vector<int32_t>(std::begin(plateau_want), std::end(plateau_want))
    );
    CHECK(covered[0] == 8 && covered[1] == 8 && covered[2] == 8);

    column_ground_heights(nullptr, kRows, 0, 256, kRows, out.data());
    for (int32_t height : out)
        CHECK(height == -1);
}

void check_haze_blend() {
    Look look;
    look.haze = 100;
    CHECK(!look_idle(look));
    const int32_t low[] = {0, 0};
    FixedColumn column;
    column.heights = low;
    column.rows = 2;
    HazeField field;
    field.amount = 100;
    field.column = fixed_column;
    field.user = &column;
    field.map_columns = 8;
    field.map_rows = 2;
    field.origin_x_256 = 0;
    field.origin_row_256 = 0;
    field.step_256 = 256;
    std::vector<uint8_t> rgb(2U * 2U * 3U, 0);
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        rgb[pixel * 3U] = 10;
        rgb[pixel * 3U + 1U] = 20;
        rgb[pixel * 3U + 2U] = 30;
    }
    Picture picture;
    picture.bytes = rgb.data();
    picture.width = 2;
    picture.height = 2;
    picture.channels = 3;
    apply_look(picture, look, nullptr, nullptr, &field);
    CHECK(rgb[0] == 176 && rgb[1] == 196 && rgb[2] == 214);

    look.haze = 50;
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        rgb[pixel * 3U] = 10;
        rgb[pixel * 3U + 1U] = 20;
        rgb[pixel * 3U + 2U] = 30;
    }
    apply_look(picture, look, nullptr, nullptr, &field);
    CHECK(rgb[0] == 93 && rgb[1] == 108 && rgb[2] == 122);

    const int32_t high[] = {255, 255};
    column.heights = high;
    look.haze = 100;
    for (std::size_t pixel = 0; pixel < 4; ++pixel) {
        rgb[pixel * 3U] = 10;
        rgb[pixel * 3U + 1U] = 20;
        rgb[pixel * 3U + 2U] = 30;
    }
    apply_look(picture, look, nullptr, nullptr, &field);
    CHECK(rgb[0] == 10 && rgb[1] == 20 && rgb[2] == 30);

    oa::platform::job_pool::Pool one(1);
    oa::platform::job_pool::Pool eight(8);
    Look wide;
    wide.haze = 40;
    wide.tilt_centre = 30;
    wide.tilt_width = 4;
    wide.tilt_radius = 3;
    const int32_t slope[] = {0, 2, 4, 8, 8, 4, 2, 0};
    column.heights = slope;
    column.rows = 8;
    field.map_rows = 8;
    field.map_columns = kWidth;
    const std::vector<uint8_t> caller = painted(wide, nullptr, &field);
    const std::vector<uint8_t> on_one = painted(wide, &one, &field);
    oa::platform::job_pool::Pool two(2);
    const std::vector<uint8_t> on_two = painted(wide, &two, &field);
    const std::vector<uint8_t> on_eight = painted(wide, &eight, &field);
    CHECK(same(caller, on_one));
    CHECK(same(caller, on_two));
    CHECK(same(caller, on_eight));
    CHECK(!same(caller, source_picture()));
}

/// A tall column, as the map's height grid is, and the rows asked for.
struct AskedColumn {
    std::vector<int32_t> heights;
    int32_t most_asked{0};
};

/// Writes the tall column from `first_row`, and counts what was asked.
void asked_column(
    void* user, int32_t /*map_x*/, int32_t first_row, int32_t* heights, int32_t count
) {
    auto* column = static_cast<AskedColumn*>(user);
    if (column == nullptr || heights == nullptr)
        return;
    column->most_asked = std::max(column->most_asked, count);
    for (int32_t row = 0; row < count; ++row) {
        const int32_t at = first_row + row;
        heights[row] = at >= 0 && at < static_cast<int32_t>(column->heights.size())
                           ? column->heights[static_cast<std::size_t>(at)]
                           : -1;
    }
}

/// Haze reads only the map rows that can reach the picture, and finds the
/// ground the whole column gives: rough ground of 0 to 255 over 3000 rows,
/// seen from its top, its middle and its foot, at three scales.
void check_reach() {
    AskedColumn column;
    column.heights.resize(3000);
    uint32_t state = 12345U;
    for (int32_t& height : column.heights) {
        state = state * 1103515245U + 12345U;
        height = static_cast<int32_t>((state >> 16U) % 256U);
    }
    constexpr int32_t kColumns = 3;
    constexpr int32_t kRows = 40;
    for (const int32_t origin : {0, 700, 2900}) {
        for (const int32_t step : {128, 256, 512}) {
            HazeField field;
            field.amount = 100;
            field.column = asked_column;
            field.user = &column;
            field.map_columns = 8;
            field.map_rows = static_cast<int32_t>(column.heights.size());
            field.origin_row_256 = origin * 256;
            field.step_256 = step;
            Look look;
            look.haze = 100;
            const auto hazed = [&](int32_t most_height) {
                std::vector<uint8_t> rgb(static_cast<std::size_t>(kColumns * kRows) * 3U);
                for (std::size_t at = 0; at < rgb.size(); ++at)
                    rgb[at] = static_cast<uint8_t>((at * 37U) & 255U);
                Picture picture;
                picture.bytes = rgb.data();
                picture.width = kColumns;
                picture.height = kRows;
                picture.channels = 3;
                HazeField asked = field;
                asked.most_height = most_height;
                column.most_asked = 0;
                apply_look(picture, look, nullptr, nullptr, &asked);
                return rgb;
            };
            const std::vector<uint8_t> whole = hazed(1 << 20);
            const int32_t whole_asked = column.most_asked;
            const std::vector<uint8_t> reached = hazed(255);
            CHECK(same(whole, reached));
            CHECK(
                whole_asked == static_cast<int32_t>(column.heights.size()) - std::max(origin - 1, 0)
            );
            CHECK(column.most_asked < 400);
        }
    }
    // A map row above the picture's first screen row never reaches it.
    std::vector<int32_t> part(column.heights.begin() + 699, column.heights.end());
    std::vector<int32_t> from_part(kRows, -2);
    std::vector<int32_t> from_whole(kRows, -2);
    column_ground_heights(
        part.data(), static_cast<int32_t>(part.size()), 700 * 256, 256, kRows, from_part.data(), 699
    );
    column_ground_heights(
        column.heights.data(),
        static_cast<int32_t>(column.heights.size()),
        700 * 256,
        256,
        kRows,
        from_whole.data()
    );
    CHECK(from_part == from_whole);
}

/// A rough column, and the screen rows the fog draws black over in one map
/// column.
struct FoggedColumn {
    AskedColumn ground;
    int32_t fog_column{0};
    int32_t fog_first{0};
    int32_t fog_end{0};
};

/// Writes the rough column's heights.
void fogged_heights(void* user, int32_t map_x, int32_t first_row, int32_t* heights, int32_t count) {
    auto* fogged = static_cast<FoggedColumn*>(user);
    if (fogged != nullptr)
        asked_column(&fogged->ground, map_x, first_row, heights, count);
}

/// Marks the fog's screen rows in its one map column.
void fogged_rows(void* user, int32_t map_x, int32_t first_row, uint8_t* black, int32_t count) {
    const auto* fogged = static_cast<const FoggedColumn*>(user);
    if (fogged == nullptr || black == nullptr)
        return;
    for (int32_t row = 0; row < count; ++row) {
        const int32_t at = first_row + row;
        black[row] = map_x == fogged->fog_column && at >= fogged->fog_first && at < fogged->fog_end
                         ? uint8_t{1}
                         : uint8_t{0};
    }
}

/// The fog keeps its pixels by the screen row it is drawn on, not by the
/// ground under it, and every other pixel is hazed as with no fog: rough
/// ground of 0 to 255, three origins and three scales, through no pool and
/// two threads. A fog that marks nothing changes nothing.
void check_fog() {
    FoggedColumn fogged;
    fogged.ground.heights.resize(3000);
    uint32_t state = 777U;
    for (int32_t& height : fogged.ground.heights) {
        state = state * 1103515245U + 12345U;
        height = static_cast<int32_t>((state >> 16U) % 256U);
    }
    constexpr int32_t kColumns = 3;
    constexpr int32_t kRows = 40;
    oa::platform::job_pool::Pool two(2);
    for (const int32_t origin : {0, 700, 2900}) {
        for (const int32_t step : {128, 256, 512}) {
            HazeField field;
            field.amount = 100;
            field.column = fogged_heights;
            field.user = &fogged;
            field.map_columns = 8;
            field.map_rows = static_cast<int32_t>(fogged.ground.heights.size());
            field.origin_row_256 = origin * 256;
            field.step_256 = step;
            Look look;
            look.haze = 100;
            std::vector<uint8_t> source(static_cast<std::size_t>(kColumns * kRows) * 3U);
            for (std::size_t at = 0; at < source.size(); ++at)
                source[at] = static_cast<uint8_t>((at * 37U) & 255U);
            const auto hazed = [&](const HazeField& with, oa::platform::job_pool::Pool* pool) {
                std::vector<uint8_t> rgb = source;
                Picture picture;
                picture.bytes = rgb.data();
                picture.width = kColumns;
                picture.height = kRows;
                picture.channels = 3;
                apply_look(picture, look, nullptr, pool, &with);
                return rgb;
            };
            const std::vector<uint8_t> clear = hazed(field, nullptr);
            HazeField with_fog = field;
            with_fog.fog = fogged_rows;
            fogged.fog_column = (2 * step) / 256;
            fogged.fog_first = origin + 5;
            fogged.fog_end = origin + 5;
            CHECK(same(hazed(with_fog, nullptr), clear));
            fogged.fog_end = origin + 12;
            const std::vector<uint8_t> kept = hazed(with_fog, nullptr);
            CHECK(same(hazed(with_fog, &two), kept));
            int32_t black = 0;
            for (int32_t row = 0; row < kRows; ++row) {
                const int32_t screen = (origin * 256 + row * step) / 256;
                for (int32_t column = 0; column < kColumns; ++column) {
                    const bool fog = (column * step) / 256 == fogged.fog_column &&
                                     screen >= fogged.fog_first && screen < fogged.fog_end;
                    const std::size_t at = static_cast<std::size_t>(row * kColumns + column) * 3U;
                    const std::vector<uint8_t>& want = fog ? source : clear;
                    CHECK(std::equal(kept.begin() + at, kept.begin() + at + 3, want.begin() + at));
                    black += fog ? 1 : 0;
                }
            }
            CHECK(black > 0);
        }
    }
}

} // namespace

int main() {
    check_scale();
    check_tilt();
    check_heights();
    check_haze_blend();
    check_reach();
    check_fog();
    if (failures != 0) {
        std::fprintf(stderr, "present-photo-depth: %d failed\n", failures);
        return 1;
    }
    std::printf("present-photo-depth: ok\n");
    return 0;
}
