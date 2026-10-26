// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// 8-bit drawing over an RGB frame.
#include "oa/present/model/rgb_bridge.hpp"

#include "oa/present/scene_grid.hpp"
#include "oa/present/surface.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>

namespace oa::present::model {
namespace {

std::atomic<bool> allocations_fail{false};

// A picture block. A refusal returns null and does not call the new handler.
void* allocate_bridge_bytes(std::size_t bytes) noexcept {
    if (bytes == 0 || allocations_fail.load(std::memory_order_relaxed))
        return nullptr;
    return std::malloc(bytes);
}

std::size_t doubled_room(std::size_t room) noexcept {
    if (room > std::numeric_limits<std::size_t>::max() / 2)
        return std::numeric_limits<std::size_t>::max();
    return room * 2;
}

uint64_t twice_bytes(std::size_t bytes) noexcept {
    if (static_cast<uint64_t>(bytes) > std::numeric_limits<uint64_t>::max() / 2)
        return std::numeric_limits<uint64_t>::max();
    return static_cast<uint64_t>(bytes) * 2;
}

void take_block(
    BridgeBytes& block, uint8_t* storage, std::size_t need, std::size_t target
) noexcept {
    std::memset(storage, 0, target);
    block.storage = storage;
    block.count = need;
    block.room = target;
}

// Grows both blocks to `target`, used as `need`. Tries while the old blocks
// are live, then frees them and tries once more, so a peak of the old and
// the new together does not refuse a picture that fits on its own.
bool grow_picture(
    BridgeBytes& picture,
    BridgeBytes& captured,
    std::size_t need,
    std::size_t target,
    uint32_t& growths
) noexcept {
    uint8_t* fresh_picture = static_cast<uint8_t*>(allocate_bridge_bytes(target));
    uint8_t* fresh_captured = static_cast<uint8_t*>(allocate_bridge_bytes(target));
    if (fresh_picture == nullptr || fresh_captured == nullptr) {
        std::free(fresh_picture);
        std::free(fresh_captured);
        picture.release();
        captured.release();
        fresh_picture = static_cast<uint8_t*>(allocate_bridge_bytes(target));
        fresh_captured = static_cast<uint8_t*>(allocate_bridge_bytes(target));
        if (fresh_picture == nullptr || fresh_captured == nullptr) {
            std::free(fresh_picture);
            std::free(fresh_captured);
            return false;
        }
    } else {
        std::free(picture.storage);
        std::free(captured.storage);
        picture.storage = nullptr;
        captured.storage = nullptr;
    }
    take_block(picture, fresh_picture, need, target);
    take_block(captured, fresh_captured, need, target);
    ++growths;
    return true;
}

// Releases both blocks down to `need`. A refusal keeps the room they have.
void release_picture(BridgeBytes& picture, BridgeBytes& captured, std::size_t need) noexcept {
    uint8_t* fresh_picture = static_cast<uint8_t*>(allocate_bridge_bytes(need));
    uint8_t* fresh_captured = static_cast<uint8_t*>(allocate_bridge_bytes(need));
    if (fresh_picture == nullptr || fresh_captured == nullptr) {
        std::free(fresh_picture);
        std::free(fresh_captured);
        picture.count = need;
        captured.count = need;
        return;
    }
    picture.release();
    captured.release();
    take_block(picture, fresh_picture, need, need);
    take_block(captured, fresh_captured, need, need);
}

// Holds `need` bytes in each block. `budget` is the two together; zero means
// no limit. Grows by doubling, never past the budget, and releases after
// bridge_release_frames begins that needed at most half the room.
bool hold_picture(
    BridgeBytes& picture,
    BridgeBytes& captured,
    std::size_t need,
    uint64_t budget,
    uint32_t& growths,
    uint32_t& small_frames,
    uint64_t& refused
) noexcept {
    const bool unlimited = budget == 0;
    std::size_t limit = std::numeric_limits<std::size_t>::max();
    if (!unlimited) {
        const uint64_t half = budget / 2;
        limit = static_cast<std::size_t>(
            std::min(half, static_cast<uint64_t>(std::numeric_limits<std::size_t>::max()))
        );
    }
    if (need > limit) {
        refused = twice_bytes(need);
        return false;
    }
    if (picture.room >= need && captured.room >= need) {
        const bool small = picture.room > need && captured.room > need &&
                           need <= picture.room / 2 && need <= captured.room / 2;
        if (small) {
            if (++small_frames >= bridge_release_frames) {
                small_frames = 0;
                release_picture(picture, captured, need);
            }
        } else {
            small_frames = 0;
        }
        picture.count = need;
        captured.count = need;
        refused = 0;
        return true;
    }
    small_frames = 0;
    const std::size_t room = std::max(picture.room, captured.room);
    std::size_t target = room == 0 ? need : std::max(need, doubled_room(room));
    if (target > limit)
        target = limit;
    if (target < need) {
        refused = twice_bytes(need);
        return false;
    }
    if (!grow_picture(picture, captured, need, target, growths)) {
        refused = twice_bytes(target);
        return false;
    }
    refused = 0;
    return true;
}

} // namespace

void bridge_fail_allocations(bool fail) noexcept {
    allocations_fail.store(fail, std::memory_order_relaxed);
}

namespace {

constexpr std::size_t exact_slots = 1024; // power of two, > 2 * palette size
constexpr uint32_t slot_used = 0x1000000U;
/// Bytes of a frame pixel: red, green and blue.
constexpr std::ptrdiff_t pixel_bytes = 3;

uint32_t pack(uint8_t r, uint8_t g, uint8_t b) noexcept {
    return (static_cast<uint32_t>(r) << 16) | (static_cast<uint32_t>(g) << 8) | b;
}

std::size_t slot_of(uint32_t rgb) noexcept {
    return static_cast<std::size_t>((rgb * 2654435761U) >> 22) & (exact_slots - 1);
}

// The slot a colour outside the palette is remembered in.
std::size_t nearest_slot_of(uint32_t rgb) noexcept {
    return static_cast<std::size_t>((rgb * 2654435761U) >> 16) & (bridge_nearest_slots - 1);
}

// What a capture or write-back may touch: the whole bridge, or one band of
// it (BridgeBand).
struct Scope {
    Surface* surface{};                    ///< the 8-bit view whose clip a capture sets
    std::vector<uint32_t>* open_tiles{};   ///< the tiles captured and not written back
    int32_t first_tile_row{};              ///< tile rows the scope captures
    int32_t end_tile_row{};                ///< the tile row after the last
    int32_t first_row{};                   ///< 8-bit rows the scope captures
    int32_t end_row{};                     ///< the 8-bit row after the last
    int32_t frame_first_row{};             ///< frame rows the scope writes
    int32_t frame_end_row{};               ///< the frame row after the last
    std::vector<uint32_t>* nearest_keys{}; ///< the colour memory it uses
    std::vector<uint8_t>* nearest_values{};
    uint64_t* mapped_pixels{}; ///< the count its captures add to
    bool banded{};             ///< a band of the bridge, not the whole
};

// The whole bridge as one scope.
Scope whole_scope(RgbBridge& bridge) noexcept {
    return {
        &bridge.surface,
        &bridge.open_tiles,
        0,
        bridge.tiles_y,
        0,
        bridge.surface.height,
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::max(),
        &bridge.nearest_keys,
        &bridge.nearest_values,
        &bridge.copy.mapped_pixels,
        false
    };
}

// One band of a bridge as a scope.
Scope band_scope(RgbBridge& bridge, BridgeBand& band) noexcept {
    const bool own_colours = !band.nearest_keys.empty();
    return {
        &band.surface,
        &band.open_tiles,
        band.first_row / bridge_tile_side,
        (band.end_row + bridge_tile_side - 1) / bridge_tile_side,
        band.first_row,
        band.end_row,
        band.frame_first_row,
        band.frame_end_row,
        own_colours ? &band.nearest_keys : &bridge.nearest_keys,
        own_colours ? &band.nearest_values : &bridge.nearest_values,
        &band.mapped_pixels,
        true
    };
}

// The palette index of a colour, remembering the nearest entry of a colour
// outside the palette in the given memory.
uint8_t lookup_index(
    const RgbBridge& bridge,
    std::vector<uint32_t>& nearest_keys,
    std::vector<uint8_t>& nearest_values,
    uint8_t r,
    uint8_t g,
    uint8_t b
) {
    const uint32_t rgb = pack(r, g, b);
    for (std::size_t slot = slot_of(rgb);; slot = (slot + 1) & (exact_slots - 1)) {
        const uint32_t key = bridge.exact_keys[slot];
        if (key == (rgb | slot_used))
            return bridge.exact_values[slot];
        if (key == 0)
            break;
    }
    const std::size_t remembered = nearest_slot_of(rgb);
    if (nearest_keys[remembered] == (rgb | slot_used))
        return nearest_values[remembered];
    uint8_t best = 0;
    int32_t best_distance = INT32_MAX;
    for (int32_t index = 0; index < OA_PALETTE_COLORS; ++index) {
        const PaletteEntry& entry = bridge.palette.entries[index];
        const int32_t dr = entry.r - r;
        const int32_t dg = entry.g - g;
        const int32_t db = entry.b - b;
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<uint8_t>(index);
        }
    }
    nearest_keys[remembered] = rgb | slot_used;
    nearest_values[remembered] = best;
    return best;
}

int32_t rgb_x(const RgbBridge& bridge, int32_t x) noexcept {
    return bridge.area.x1 + static_cast<int32_t>(std::floor(
                                (static_cast<double>(x) + 0.5) * bridge.scale -
                                static_cast<double>(bridge.phase_x) / scene_step_one
                            ));
}

int32_t rgb_y(const RgbBridge& bridge, int32_t y) noexcept {
    return bridge.area.y1 + static_cast<int32_t>(std::floor(
                                (static_cast<double>(y) + 0.5) * bridge.scale -
                                static_cast<double>(bridge.phase_y) / scene_step_one
                            ));
}

// The step the bridge's pixels are written back to the frame with: the
// scene grid's step of its scale (scene_grid.hpp), which the terrain is
// filled with, so that an 8-bit pixel lands on the frame pixels of the map
// pixel it holds.
uint32_t written_step(const RgbBridge& bridge) noexcept {
    return scene_step(bridge.scale);
}

// First RGB column (or row) whose 8-bit source is at or after `index`.
int32_t rgb_start(int32_t origin, int32_t index, uint32_t step, uint32_t phase) noexcept {
    return origin + static_cast<int32_t>(first_scene_pixel(index, step, phase));
}

// The 8-bit column (or row) a frame column (or row) at `index` is written
// from, from an origin.
int32_t written_index(int32_t origin, int32_t index, uint32_t step, uint32_t phase) noexcept {
    return static_cast<int32_t>(map_pixel_shown(int64_t{index} - origin, step, phase));
}

// The frame row an 8-bit row captures from.
int32_t captured_row(const RgbBridge& bridge, int32_t y) noexcept {
    return std::clamp(
        rgb_y(bridge, y),
        std::max(bridge.area.y1, 0),
        std::min(bridge.area.y2, bridge.frame.height - 1)
    );
}

// The frame column an 8-bit column captures from.
int32_t captured_column(const RgbBridge& bridge, int32_t x) noexcept {
    return std::clamp(
        rgb_x(bridge, x),
        std::max(bridge.area.x1, 0),
        std::min(bridge.area.x2, bridge.frame.width - 1)
    );
}

// The 8-bit row (or column) a frame row (or column) takes its pixel from
// when written back, as commit_tile maps it, kept within [first, last].
int32_t written_from(
    int32_t origin, int32_t index, uint32_t step, uint32_t phase, int32_t first, int32_t last
) noexcept {
    return std::clamp(written_index(origin, index, step, phase), first, last);
}

// Whether `count` bytes all hold `value`.
bool all_bytes(const uint8_t* bytes, uint32_t count, uint8_t value) noexcept {
    constexpr uint64_t every_byte = 0x0101010101010101ULL;
    uint32_t i = 0;
    for (const uint64_t pattern = every_byte * value; i + sizeof(uint64_t) <= count;
         i += sizeof(uint64_t)) {
        uint64_t word = 0;
        std::memcpy(&word, bytes + i, sizeof(word));
        if (word != pattern)
            return false;
    }
    for (; i < count; ++i)
        if (bytes[i] != value)
            return false;
    return true;
}

// Sums, for each bridge pixel of a row of a sampled region, the palette
// colours of its drawn samples and counts them: four words a pixel, red,
// green, blue and the count. Returns whether any sample was drawn.
bool sum_drawn_samples(
    const RgbBridge& bridge, const SampledRegion& sampled, int32_t row, std::vector<uint32_t>& sums
) {
    const auto factor = static_cast<int32_t>(sampled.factor);
    const int32_t columns = sampled.region.x2 - sampled.region.x1 + 1;
    const int32_t width = sampled.surface.width;
    const int32_t offset = row - sampled.region.y1;
    const uint8_t* baseline =
        sampled.baseline.data() + static_cast<std::ptrdiff_t>(offset) * columns;
    const uint8_t* captured =
        sampled.captured_rows.data() + static_cast<std::ptrdiff_t>(offset) * width;
    std::fill(sums.begin(), sums.end(), 0);
    bool drawn = false;
    for (int32_t line = 0; line < factor; ++line) {
        const uint8_t* samples =
            sampled.surface.pixels +
            static_cast<std::ptrdiff_t>(offset * factor + line) * sampled.surface.pitch;
        if (std::memcmp(samples, captured, static_cast<std::size_t>(width)) == 0)
            continue;
        drawn = true;
        for (int32_t column = 0; column < columns; ++column) {
            const uint8_t under = baseline[column];
            const uint8_t* cell = samples + static_cast<std::ptrdiff_t>(column) * factor;
            if (all_bytes(cell, sampled.factor, under))
                continue;
            uint32_t* sum = sums.data() + static_cast<std::ptrdiff_t>(column) * 4;
            if (all_bytes(cell, sampled.factor, cell[0])) {
                const PaletteEntry& entry = bridge.palette.entries[cell[0]];
                sum[0] += entry.r * sampled.factor;
                sum[1] += entry.g * sampled.factor;
                sum[2] += entry.b * sampled.factor;
                sum[3] += sampled.factor;
                continue;
            }
            for (int32_t i = 0; i < factor; ++i) {
                const uint8_t value = cell[i];
                if (value == under)
                    continue;
                const PaletteEntry& entry = bridge.palette.entries[value];
                sum[0] += entry.r;
                sum[1] += entry.g;
                sum[2] += entry.b;
                ++sum[3];
            }
        }
    }
    return drawn;
}

// Whether `count` bytes at `a` equal those at `b`.
bool same_bytes(const uint8_t* a, const uint8_t* b, std::size_t count) noexcept {
    std::size_t i = 0;
    uint64_t differ = 0;
    for (; i + sizeof(uint64_t) <= count; i += sizeof(uint64_t)) {
        uint64_t left = 0;
        uint64_t right = 0;
        std::memcpy(&left, a + i, sizeof(left));
        std::memcpy(&right, b + i, sizeof(right));
        differ |= left ^ right;
    }
    for (; i < count; ++i)
        differ |= static_cast<uint64_t>(a[i] ^ b[i]);
    return differ == 0;
}

// Copies the `count` indices of a row of a tile, a whole tile's row at once
// when it has bridge_tile_side of them.
void copy_tile_row(uint8_t* to, const uint8_t* from, int32_t count) noexcept {
    if (count == bridge_tile_side)
        std::memcpy(to, from, bridge_tile_side);
    else
        std::memcpy(to, from, static_cast<std::size_t>(count));
}

// Whether two frame pixels differ in colour.
bool colour_differs(const uint8_t* a, const uint8_t* b) noexcept {
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
}

// Sets the copy's pixel `at` of a row to the frame pixel `source`, with its
// palette index.
void map_pixel(
    const RgbBridge& bridge,
    const Scope& scope,
    uint8_t* colours,
    uint8_t* indices,
    int32_t at,
    const uint8_t* source
) {
    uint8_t* colour = colours + static_cast<std::ptrdiff_t>(at) * pixel_bytes;
    colour[0] = source[0];
    colour[1] = source[1];
    colour[2] = source[2];
    indices[at] = lookup_index(
        bridge, *scope.nearest_keys, *scope.nearest_values, source[0], source[1], source[2]
    );
    ++*scope.mapped_pixels;
}

// Lays out the capture copy for the bridge's surface, area and frame, once
// after each change of them: the frame row and column each 8-bit row and
// column captures from, one row (or column) of the copy for each distinct
// frame row (or column).
void lay_out_copy(RgbBridge& bridge) {
    CaptureCopy& copy = bridge.copy;
    if (copy.laid_out)
        return;
    copy.laid_out = true;
    const int32_t width = bridge.surface.width;
    const int32_t height = bridge.surface.height;
    copy.column_of.resize(static_cast<std::size_t>(width));
    copy.frame_columns.clear();
    for (int32_t x = 0; x < width; ++x) {
        const std::ptrdiff_t offset =
            static_cast<std::ptrdiff_t>(captured_column(bridge, x)) * pixel_bytes;
        if (copy.frame_columns.empty() || copy.frame_columns.back() != offset)
            copy.frame_columns.push_back(offset);
        copy.column_of[static_cast<std::size_t>(x)] =
            static_cast<int32_t>(copy.frame_columns.size()) - 1;
    }
    copy.row_of.resize(static_cast<std::size_t>(height));
    copy.frame_rows.clear();
    for (int32_t y = 0; y < height; ++y) {
        const std::ptrdiff_t offset =
            static_cast<std::ptrdiff_t>(captured_row(bridge, y)) * bridge.frame.stride;
        if (copy.frame_rows.empty() || copy.frame_rows.back() != offset)
            copy.frame_rows.push_back(offset);
        copy.row_of[static_cast<std::size_t>(y)] = static_cast<int32_t>(copy.frame_rows.size()) - 1;
    }
    copy.width = static_cast<int32_t>(copy.frame_columns.size());
    copy.height = static_cast<int32_t>(copy.frame_rows.size());
    copy.written_in_place = bridge.scale == 1.0F && bridge.area.x1 >= 0 && bridge.area.y1 >= 0 &&
                            bridge.area.x2 < bridge.frame.width &&
                            bridge.area.y2 < bridge.frame.height;
    copy.copy_columns_side_by_side = true;
    for (int32_t column = 1; copy.copy_columns_side_by_side && column < copy.width; ++column)
        copy.copy_columns_side_by_side =
            copy.frame_columns[static_cast<std::size_t>(column)] ==
            copy.frame_columns[static_cast<std::size_t>(column - 1)] + pixel_bytes;
    copy.columns_side_by_side = copy.width == width && copy.copy_columns_side_by_side;
    const auto size = static_cast<std::size_t>(copy.width) * static_cast<std::size_t>(copy.height);
    if (copy.indices.size() < size) {
        copy.colours.resize(size * pixel_bytes);
        copy.indices.resize(size);
    }
}

// Brings the copy up to date with the frame pixels a tile captures from:
// those whose colour differs from the copy's are mapped again, and every one
// of them when the tile was never mapped. With `start_pixels` the tile's
// 8-bit pixels and baseline then start as the copy's indices.
void map_tile(
    RgbBridge& bridge, const Scope& scope, int32_t tx, int32_t ty, bool mapped, bool start_pixels
) {
    CaptureCopy& copy = bridge.copy;
    const int32_t x0 = tx * bridge_tile_side;
    const int32_t y0 = ty * bridge_tile_side;
    const int32_t x1 = std::min(x0 + bridge_tile_side, bridge.surface.width);
    const int32_t y1 = std::min(y0 + bridge_tile_side, bridge.surface.height);
    const int32_t first = copy.column_of[static_cast<std::size_t>(x0)];
    const int32_t count = x1 - x0;
    // The copy row the tile's row above mapped, while its rows share one.
    int32_t mapped_row = -1;
    for (int32_t y = y0; y < y1; ++y) {
        const int32_t row = copy.row_of[static_cast<std::size_t>(y)];
        const uint8_t* frame_row =
            bridge.frame.rgb + copy.frame_rows[static_cast<std::size_t>(row)];
        uint8_t* colours =
            copy.colours.data() + static_cast<std::ptrdiff_t>(row) * copy.width * pixel_bytes;
        uint8_t* indices = copy.indices.data() + static_cast<std::ptrdiff_t>(row) * copy.width;
        uint8_t* pixels =
            bridge.pixels.data() + static_cast<std::ptrdiff_t>(y) * bridge.surface.pitch;
        uint8_t* baseline =
            bridge.baseline.data() + static_cast<std::ptrdiff_t>(y) * bridge.surface.pitch;
        if (copy.columns_side_by_side) {
            const uint8_t* source = frame_row + copy.frame_columns[static_cast<std::size_t>(first)];
            const uint8_t* kept = colours + static_cast<std::ptrdiff_t>(first) * pixel_bytes;
            if (!mapped || !same_bytes(kept, source, static_cast<std::size_t>(count) * pixel_bytes))
                for (int32_t i = 0; i < count; ++i)
                    if (!mapped || colour_differs(kept + i * pixel_bytes, source + i * pixel_bytes))
                        map_pixel(
                            bridge, scope, colours, indices, first + i, source + i * pixel_bytes
                        );
            if (start_pixels) {
                copy_tile_row(baseline + x0, indices + first, count);
                copy_tile_row(pixels + x0, indices + first, count);
            }
            continue;
        }
        if (copy.copy_columns_side_by_side) {
            // The copy's columns of the tile lie side by side in the frame:
            // they compare as one run, each mapped once, and an 8-bit row
            // that shares its copy row with the row above repeats it.
            if (row == mapped_row) {
                if (start_pixels) {
                    const std::ptrdiff_t above = -static_cast<std::ptrdiff_t>(bridge.surface.pitch);
                    copy_tile_row(baseline + x0, baseline + above + x0, count);
                    copy_tile_row(pixels + x0, pixels + above + x0, count);
                }
                continue;
            }
            mapped_row = row;
            const int32_t last = copy.column_of[static_cast<std::size_t>(x1 - 1)];
            const int32_t columns = last - first + 1;
            const uint8_t* source = frame_row + copy.frame_columns[static_cast<std::size_t>(first)];
            const uint8_t* kept = colours + static_cast<std::ptrdiff_t>(first) * pixel_bytes;
            if (!mapped ||
                !same_bytes(kept, source, static_cast<std::size_t>(columns) * pixel_bytes))
                for (int32_t i = 0; i < columns; ++i)
                    if (!mapped || colour_differs(kept + i * pixel_bytes, source + i * pixel_bytes))
                        map_pixel(
                            bridge, scope, colours, indices, first + i, source + i * pixel_bytes
                        );
            if (start_pixels)
                for (int32_t x = x0; x < x1; ++x) {
                    const uint8_t index = indices[copy.column_of[static_cast<std::size_t>(x)]];
                    baseline[x] = index;
                    pixels[x] = index;
                }
            continue;
        }
        for (int32_t x = x0; x < x1; ++x) {
            const int32_t column = copy.column_of[static_cast<std::size_t>(x)];
            const uint8_t* source =
                frame_row + copy.frame_columns[static_cast<std::size_t>(column)];
            if (!mapped ||
                colour_differs(colours + static_cast<std::ptrdiff_t>(column) * pixel_bytes, source))
                map_pixel(bridge, scope, colours, indices, column, source);
            if (start_pixels) {
                baseline[x] = indices[column];
                pixels[x] = indices[column];
            }
        }
    }
}

// Writes the pixels of a captured tile that draws changed back to the frame
// through the palette.
void commit_tile(RgbBridge& bridge, const Scope& scope, int32_t tx, int32_t ty) {
    const int32_t x0 = tx * bridge_tile_side;
    const int32_t y0 = ty * bridge_tile_side;
    const int32_t x1 = std::min(x0 + bridge_tile_side, bridge.surface.width);
    const int32_t y1 = std::min(y0 + bridge_tile_side, bridge.surface.height);
    const uint32_t step = written_step(bridge);
    const int32_t top = std::max(
        {rgb_start(bridge.area.y1, y0, step, bridge.phase_y),
         bridge.area.y1,
         0,
         scope.frame_first_row}
    );
    const int32_t bottom = std::min(
        {rgb_start(bridge.area.y1, y1, step, bridge.phase_y) - 1,
         bridge.area.y2,
         bridge.frame.height - 1,
         scope.frame_end_row - 1}
    );
    const int32_t left =
        std::max({rgb_start(bridge.area.x1, x0, step, bridge.phase_x), bridge.area.x1, 0});
    const int32_t right = std::min(
        {rgb_start(bridge.area.x1, x1, step, bridge.phase_x) - 1,
         bridge.area.x2,
         bridge.frame.width - 1}
    );
    if (left > right)
        return;
    // The 8-bit column each frame column of the tile takes its pixel from.
    thread_local std::vector<int32_t> columns;
    columns.resize(static_cast<std::size_t>(right - left + 1));
    for (int32_t x = left; x <= right; ++x)
        columns[static_cast<std::size_t>(x - left)] =
            std::min(written_index(bridge.area.x1, x, step, bridge.phase_x), x1 - 1);
    // The 8-bit row last read, and whether any of its pixels in the tile
    // differ from the baseline.
    int32_t compared = -1;
    bool changed = false;
    for (int32_t y = top; y <= bottom; ++y) {
        const int32_t sy = std::min(written_index(bridge.area.y1, y, step, bridge.phase_y), y1 - 1);
        const auto offset = static_cast<std::ptrdiff_t>(sy) * bridge.surface.pitch;
        if (sy != compared) {
            compared = sy;
            changed = !same_bytes(
                bridge.pixels.data() + offset + x0,
                bridge.baseline.data() + offset + x0,
                static_cast<std::size_t>(x1 - x0)
            );
        }
        if (!changed)
            continue;
        const uint8_t* drawn = bridge.pixels.data() + offset;
        const uint8_t* captured = bridge.baseline.data() + offset;
        uint8_t* row = bridge.frame.rgb + static_cast<std::ptrdiff_t>(y) * bridge.frame.stride;
        // Each pixel written is the one its 8-bit pixel captures from: the
        // copy takes its new colour and index.
        CaptureCopy& copy = bridge.copy;
        const std::ptrdiff_t kept = static_cast<std::ptrdiff_t>(sy) * copy.width;
        uint8_t* kept_colours =
            copy.written_in_place ? copy.colours.data() + kept * pixel_bytes : nullptr;
        uint8_t* kept_indices = copy.written_in_place ? copy.indices.data() + kept : nullptr;
        for (int32_t x = left; x <= right; ++x) {
            const int32_t sx = columns[static_cast<std::size_t>(x - left)];
            const uint8_t value = drawn[sx];
            if (value == captured[sx])
                continue;
            const PaletteEntry& entry = bridge.palette.entries[value];
            uint8_t* pixel = row + static_cast<std::ptrdiff_t>(x) * 3;
            pixel[0] = entry.r;
            pixel[1] = entry.g;
            pixel[2] = entry.b;
            if (kept_colours != nullptr) {
                std::memcpy(
                    kept_colours + static_cast<std::ptrdiff_t>(sx) * pixel_bytes,
                    pixel,
                    static_cast<std::size_t>(pixel_bytes)
                );
                kept_indices[sx] = copy.entry_indices[value];
            }
        }
    }
}

// Captures the tiles of a scope overlapping a region and clips the scope's
// surface to the region (bridge_open).
void open_region(RgbBridge& bridge, const Scope& scope, const Rect32& region) {
    const int32_t left = region.x1;
    const int32_t top = region.y1;
    const int32_t right = region.x2;
    const int32_t bottom = region.y2;
    Rect32 clip{
        std::max(left, 0),
        std::max(top, 0),
        std::min(right, bridge.surface.width - 1),
        std::min(bottom, bridge.surface.height - 1)
    };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) {
        // An empty clip: the draw routines reject everything.
        present::set_surface_clip(*scope.surface, {0, 0, -1, -1});
        return;
    }
    lay_out_copy(bridge);
    const int32_t first_tile_row = std::max(clip.y1 / bridge_tile_side, scope.first_tile_row);
    const int32_t last_tile_row = std::min(clip.y2 / bridge_tile_side, scope.end_tile_row - 1);
    for (int32_t ty = first_tile_row; ty <= last_tile_row; ++ty) {
        for (int32_t tx = clip.x1 / bridge_tile_side; tx <= clip.x2 / bridge_tile_side; ++tx) {
            const auto tile = static_cast<uint32_t>(ty * bridge.tiles_x + tx);
            uint8_t& state = bridge.tiles[tile];
            if ((state & bridge_tile_open) != 0)
                continue;
            map_tile(bridge, scope, tx, ty, (state & bridge_tile_mapped) != 0, true);
            state = bridge_tile_mapped | bridge_tile_open;
            scope.open_tiles->push_back(tile);
        }
    }
    present::set_surface_clip(*scope.surface, clip);
}

// Writes a scope's captured tiles back to the frame (bridge_end).
void end_scope(RgbBridge& bridge, const Scope& scope) {
    for (const uint32_t tile : *scope.open_tiles) {
        commit_tile(
            bridge,
            scope,
            static_cast<int32_t>(tile % static_cast<uint32_t>(bridge.tiles_x)),
            static_cast<int32_t>(tile / static_cast<uint32_t>(bridge.tiles_x))
        );
        bridge.tiles[tile] = bridge_tile_mapped;
    }
    scope.open_tiles->clear();
}

// Covers a region with samples, capturing the scope's rows of it
// (bridge_open_sampled).
void open_sampled_region(
    RgbBridge& bridge,
    const Scope& scope,
    SampledRegion& sampled,
    const Rect32& region,
    uint32_t factor
) {
    sampled.factor = factor == 0 ? 1 : factor;
    // Rect32 is packed: its fields are read into locals before std::max and
    // std::min take them by reference.
    const int32_t left = region.x1;
    const int32_t top = region.y1;
    const int32_t right = region.x2;
    const int32_t bottom = region.y2;
    const Rect32 clip{
        std::max(left, 0),
        std::max(top, 0),
        std::min(right, bridge.surface.width - 1),
        std::min(bottom, bridge.surface.height - 1)
    };
    if (clip.x1 > clip.x2 || clip.y1 > clip.y2) {
        sampled.region = {0, 0, -1, -1};
        sampled.samples.resize(std::max<std::size_t>(sampled.samples.size(), 1));
        present::init_surface(sampled.surface, 0, 0, 0, sampled.samples.data());
        return;
    }
    sampled.region = clip;
    const auto factor_rows = static_cast<int32_t>(sampled.factor);
    const int32_t columns = clip.x2 - clip.x1 + 1;
    const int32_t rows = clip.y2 - clip.y1 + 1;
    const int32_t width = columns * factor_rows;
    const int32_t height = rows * factor_rows;
    const auto size = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    if (sampled.samples.size() < size + 1)
        sampled.samples.resize(size + 1);
    sampled.baseline.resize(static_cast<std::size_t>(columns) * static_cast<std::size_t>(rows));
    sampled.captured_rows.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(rows));
    present::init_surface(sampled.surface, width, height, width, sampled.samples.data());
    // The bridge rows captured: the scope's rows of the region. A band's
    // samples change only the sample rows over them.
    const int32_t clip_top = clip.y1;
    const int32_t clip_bottom = clip.y2;
    const int32_t first_row = std::max(clip_top, scope.first_row);
    const int32_t last_row = std::min(clip_bottom, scope.end_row - 1);
    if (scope.banded) {
        if (first_row > last_row) {
            present::set_surface_band(sampled.surface, 0, 0);
            return;
        }
        present::set_surface_band(
            sampled.surface,
            (first_row - clip.y1) * factor_rows,
            (last_row + 1 - clip.y1) * factor_rows
        );
    }
    // The copy gives the indices under tiles not captured; a captured tile
    // may hold draws, so the frame under it is mapped as it is.
    lay_out_copy(bridge);
    bool any_open = false;
    for (int32_t ty = first_row / bridge_tile_side; ty <= last_row / bridge_tile_side; ++ty) {
        for (int32_t tx = clip.x1 / bridge_tile_side; tx <= clip.x2 / bridge_tile_side; ++tx) {
            uint8_t& state = bridge.tiles[static_cast<std::size_t>(ty * bridge.tiles_x + tx)];
            if ((state & bridge_tile_open) != 0) {
                any_open = true;
                continue;
            }
            map_tile(bridge, scope, tx, ty, (state & bridge_tile_mapped) != 0, true);
            state = bridge_tile_mapped;
        }
    }
    const CaptureCopy& copy = bridge.copy;
    const auto index_under = [&](int32_t x, int32_t y) -> uint8_t {
        const int32_t row = copy.row_of[static_cast<std::size_t>(y)];
        const int32_t column = copy.column_of[static_cast<std::size_t>(x)];
        if (!any_open || (bridge.tiles[static_cast<std::size_t>(
                              (y / bridge_tile_side) * bridge.tiles_x + x / bridge_tile_side
                          )] &
                          bridge_tile_open) == 0)
            return copy.indices[static_cast<std::size_t>(row) * copy.width + column];
        const uint8_t* pixel = bridge.frame.rgb + copy.frame_rows[static_cast<std::size_t>(row)] +
                               copy.frame_columns[static_cast<std::size_t>(column)];
        return lookup_index(
            bridge, *scope.nearest_keys, *scope.nearest_values, pixel[0], pixel[1], pixel[2]
        );
    };
    for (int32_t row = first_row - clip.y1; row <= last_row - clip.y1; ++row) {
        uint8_t* first =
            sampled.samples.data() + static_cast<std::ptrdiff_t>(row) * factor_rows * width;
        uint8_t* baseline = sampled.baseline.data() + static_cast<std::ptrdiff_t>(row) * columns;
        for (int32_t column = 0; column < columns; ++column) {
            const uint8_t index = index_under(clip.x1 + column, clip.y1 + row);
            baseline[column] = index;
            std::memset(
                first + static_cast<std::ptrdiff_t>(column) * factor_rows, index, sampled.factor
            );
        }
        for (int32_t line = 1; line < factor_rows; ++line)
            std::memcpy(
                first + static_cast<std::ptrdiff_t>(line) * width,
                first,
                static_cast<std::size_t>(width)
            );
        std::memcpy(
            sampled.captured_rows.data() + static_cast<std::ptrdiff_t>(row) * width,
            first,
            static_cast<std::size_t>(width)
        );
    }
}

// Writes a sampled region back to the frame rows between `frame_first_row`
// and `frame_end_row` (bridge_end_sampled).
void end_sampled_region(
    const RgbBridge& bridge,
    const SampledRegion& sampled,
    int32_t frame_first_row,
    int32_t frame_end_row
) {
    const Rect32& region = sampled.region;
    if (region.x1 > region.x2 || region.y1 > region.y2)
        return;
    const uint32_t all = sampled.factor * sampled.factor;
    const uint32_t step = written_step(bridge);
    const int32_t top = std::max(
        {rgb_start(bridge.area.y1, region.y1, step, bridge.phase_y),
         bridge.area.y1,
         0,
         frame_first_row}
    );
    const int32_t bottom = std::min(
        {rgb_start(bridge.area.y1, region.y2 + 1, step, bridge.phase_y) - 1,
         bridge.area.y2,
         bridge.frame.height - 1,
         frame_end_row - 1}
    );
    const int32_t left =
        std::max({rgb_start(bridge.area.x1, region.x1, step, bridge.phase_x), bridge.area.x1, 0});
    const int32_t right = std::min(
        {rgb_start(bridge.area.x1, region.x2 + 1, step, bridge.phase_x) - 1,
         bridge.area.x2,
         bridge.frame.width - 1}
    );
    if (top > bottom || left > right)
        return;
    thread_local std::vector<int32_t> columns;
    thread_local std::vector<uint32_t> sums;
    columns.resize(static_cast<std::size_t>(right - left + 1));
    for (int32_t x = left; x <= right; ++x)
        columns[static_cast<std::size_t>(x - left)] =
            written_from(bridge.area.x1, x, step, bridge.phase_x, region.x1, region.x2) - region.x1;
    sums.resize(static_cast<std::size_t>(region.x2 - region.x1 + 1) * 4);
    int32_t summed = region.y1 - 1;
    bool drawn = false;
    for (int32_t y = top; y <= bottom; ++y) {
        const int32_t row =
            written_from(bridge.area.y1, y, step, bridge.phase_y, region.y1, region.y2);
        if (row != summed) {
            drawn = sum_drawn_samples(bridge, sampled, row, sums);
            summed = row;
        }
        if (!drawn)
            continue;
        uint8_t* pixel = bridge.frame.rgb + static_cast<std::ptrdiff_t>(y) * bridge.frame.stride +
                         static_cast<std::ptrdiff_t>(left) * 3;
        for (int32_t x = left; x <= right; ++x, pixel += 3) {
            const uint32_t* sum =
                sums.data() +
                static_cast<std::ptrdiff_t>(columns[static_cast<std::size_t>(x - left)]) * 4;
            const uint32_t count = sum[3];
            if (count == 0)
                continue;
            const uint32_t under = all - count;
            for (int32_t channel = 0; channel < 3; ++channel)
                pixel[channel] =
                    static_cast<uint8_t>((sum[channel] + under * pixel[channel] + all / 2) / all);
        }
    }
}

// Tells whether the bands of a bridge can meet at an 8-bit row: the rows
// above it capture from and are written back to frame rows above the first
// one the rows from it on are written back to, and those rows capture from
// and are written to that frame row or below. `row` is a whole number of
// tiles down, inside the surface.
bool splits_at(const RgbBridge& bridge, int32_t row) noexcept {
    const uint32_t step = written_step(bridge);
    const int32_t frame_row = rgb_start(bridge.area.y1, row, step, bridge.phase_y);
    if (frame_row <= std::max(bridge.area.y1, 0) ||
        frame_row > std::min(bridge.area.y2, bridge.frame.height - 1))
        return false;
    // The rows each side capture from (captured_row), and the rows the frame
    // rows each side are written from, as commit_tile and written_from work
    // them out.
    const auto written = [&](int32_t y) {
        return written_index(bridge.area.y1, y, step, bridge.phase_y);
    };
    return captured_row(bridge, row - 1) < frame_row && captured_row(bridge, row) >= frame_row &&
           written(frame_row) >= row && written(frame_row - 1) < row;
}

} // namespace

uint8_t bridge_index(RgbBridge& bridge, uint8_t r, uint8_t g, uint8_t b) {
    return lookup_index(bridge, bridge.nearest_keys, bridge.nearest_values, r, g, b);
}

bool bridge_begin(
    RgbBridge& bridge,
    const RgbFrame& frame,
    const Rect32& area,
    float scale,
    const Palette& palette,
    uint32_t phase_x,
    uint32_t phase_y
) {
    const float kept_scale = scale > 0.0F ? scale : 1.0F;
    const uint32_t kept_step = scene_step(kept_scale);
    phase_x %= kept_step;
    phase_y %= kept_step;
    const bool palette_changed = std::memcmp(&bridge.palette, &palette, sizeof(Palette)) != 0;
    // The copy maps frame pixels by place and colours by palette; a frame
    // laid out as before keeps it, whatever the frame now holds.
    const bool layout_kept =
        !palette_changed && !bridge.exact_keys.empty() && frame.width == bridge.frame.width &&
        frame.height == bridge.frame.height && frame.stride == bridge.frame.stride &&
        area.x1 == bridge.area.x1 && area.y1 == bridge.area.y1 && area.x2 == bridge.area.x2 &&
        area.y2 == bridge.area.y2 && kept_scale == bridge.scale && phase_x == bridge.phase_x &&
        phase_y == bridge.phase_y;
    bridge.frame = frame;
    bridge.area = area;
    bridge.scale = kept_scale;
    bridge.phase_x = phase_x;
    bridge.phase_y = phase_y;
    if (palette_changed || bridge.exact_keys.empty()) {
        bridge.palette = palette;
        bridge.exact_keys.assign(exact_slots, 0);
        bridge.exact_values.assign(exact_slots, 0);
        bridge.nearest_keys.assign(bridge_nearest_slots, 0);
        bridge.nearest_values.assign(bridge_nearest_slots, 0);
        for (int32_t index = 0; index < OA_PALETTE_COLORS; ++index) {
            const PaletteEntry& entry = palette.entries[index];
            const uint32_t rgb = pack(entry.r, entry.g, entry.b);
            std::size_t slot = slot_of(rgb);
            while (bridge.exact_keys[slot] != 0 && bridge.exact_keys[slot] != (rgb | slot_used))
                slot = (slot + 1) & (exact_slots - 1);
            if (bridge.exact_keys[slot] == 0) {
                bridge.exact_keys[slot] = rgb | slot_used;
                bridge.exact_values[slot] = static_cast<uint8_t>(index);
            }
        }
        ++bridge.lookup_builds;
        for (int32_t index = 0; index < OA_PALETTE_COLORS; ++index) {
            const PaletteEntry& entry = palette.entries[index];
            bridge.copy.entry_indices[static_cast<std::size_t>(index)] =
                bridge_index(bridge, entry.r, entry.g, entry.b);
        }
    }
    // The 8-bit view holds every map pixel a frame pixel of the rectangle
    // shows when written back. A span that does not fit the surface, or a
    // picture past the budget or that cannot be allocated, returns; it does
    // not end the process.
    const auto span_of = [](int32_t first, int32_t last) -> std::optional<int32_t> {
        const auto span = static_cast<int64_t>(last) - static_cast<int64_t>(first) + 1;
        if (span <= 0 || span > std::numeric_limits<int32_t>::max())
            return std::nullopt;
        return static_cast<int32_t>(span);
    };
    const std::optional<int32_t> across = span_of(area.x1, area.x2);
    const std::optional<int32_t> down = span_of(area.y1, area.y2);
    if (!across || !down) {
        bridge.refused_bytes = 0;
        return false;
    }
    const uint32_t step = written_step(bridge);
    const auto covered = [&](int32_t pixels, uint32_t phase) -> std::optional<int32_t> {
        if (pixels <= 0)
            return 0;
        const float rounded = std::ceil(static_cast<float>(pixels) / bridge.scale);
        if (!(rounded <= static_cast<float>(std::numeric_limits<int32_t>::max())))
            return std::nullopt;
        const int64_t shown = map_pixel_shown(static_cast<int64_t>(pixels) - 1, step, phase) + 1;
        if (shown > std::numeric_limits<int32_t>::max())
            return std::nullopt;
        return std::max(static_cast<int32_t>(rounded), static_cast<int32_t>(shown));
    };
    const std::optional<int32_t> width = covered(*across, bridge.phase_x);
    const std::optional<int32_t> height = covered(*down, bridge.phase_y);
    if (!width || !height ||
        *width > std::numeric_limits<int32_t>::max() - (bridge_tile_side - 1) ||
        *height > std::numeric_limits<int32_t>::max() - (bridge_tile_side - 1)) {
        bridge.refused_bytes = 0;
        return false;
    }
    const auto product = static_cast<uint64_t>(*width) * static_cast<uint64_t>(*height);
    if (product >= std::numeric_limits<std::size_t>::max()) {
        bridge.refused_bytes = 0;
        return false;
    }
    const auto bytes = static_cast<std::size_t>(product) + 1;
    if (!hold_picture(
            bridge.pixels,
            bridge.baseline,
            bytes,
            bridge.byte_budget,
            bridge.buffer_growths,
            bridge.small_frames,
            bridge.refused_bytes
        )) {
        if (bridge.pixels.storage == nullptr)
            present::init_surface(bridge.surface, 0, 0, 0, nullptr);
        return false;
    }
    present::init_surface(bridge.surface, *width, *height, *width, bridge.pixels.data());
    bridge.tiles_x = (*width + bridge_tile_side - 1) / bridge_tile_side;
    bridge.tiles_y = (*height + bridge_tile_side - 1) / bridge_tile_side;
    const auto tile_product =
        static_cast<uint64_t>(bridge.tiles_x) * static_cast<uint64_t>(bridge.tiles_y);
    if (tile_product > std::numeric_limits<std::size_t>::max()) {
        bridge.refused_bytes = twice_bytes(bytes);
        return false;
    }
    const auto tiles = static_cast<std::size_t>(tile_product);
    bridge.open_tiles.clear();
    if (layout_kept && bridge.tiles.size() == tiles) {
        for (uint8_t& state : bridge.tiles)
            state = static_cast<uint8_t>(state & ~bridge_tile_open);
        return true;
    }
    bridge.tiles.assign(tiles, 0);
    bridge.open_tiles.reserve(tiles);
    bridge.copy.laid_out = false;
    return true;
}

void bridge_open(RgbBridge& bridge, const Rect32& region) {
    open_region(bridge, whole_scope(bridge), region);
}

void bridge_end(RgbBridge& bridge) {
    end_scope(bridge, whole_scope(bridge));
}

void bridge_open_sampled(
    RgbBridge& bridge, SampledRegion& sampled, const Rect32& region, uint32_t factor
) {
    open_sampled_region(bridge, whole_scope(bridge), sampled, region, factor);
}

void bridge_end_sampled(const RgbBridge& bridge, const SampledRegion& sampled) {
    end_sampled_region(
        bridge, sampled, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()
    );
}

int32_t bridge_split(RgbBridge& bridge, int32_t count, std::vector<BridgeBand>& bands) {
    // The copy is laid out once, here, before any band captures.
    lay_out_copy(bridge);
    const int32_t tile_rows = std::max(bridge.tiles_y, 1);
    count = std::clamp(count, 1, tile_rows);
    // The 8-bit rows the bands start at: the first at the top, then the
    // tile row nearest an even share of them where the frame splits, or the
    // next one down that splits.
    std::vector<int32_t> starts{0};
    for (int32_t band = 1; band < count; ++band) {
        const int32_t even = (band * tile_rows + count / 2) / count;
        const int32_t after = starts.back() / bridge_tile_side + 1;
        for (int32_t tile_row = std::max(even, after); tile_row < tile_rows; ++tile_row) {
            const int32_t row = tile_row * bridge_tile_side;
            if (row < bridge.surface.height && splits_at(bridge, row)) {
                starts.push_back(row);
                break;
            }
        }
    }
    const auto made = static_cast<int32_t>(starts.size());
    bands.resize(static_cast<std::size_t>(made));
    for (int32_t index = 0; index < made; ++index) {
        BridgeBand& band = bands[static_cast<std::size_t>(index)];
        band.first_row = starts[static_cast<std::size_t>(index)];
        band.end_row =
            index + 1 < made ? starts[static_cast<std::size_t>(index) + 1] : bridge.surface.height;
        band.frame_first_row =
            index == 0
                ? 0
                : rgb_start(bridge.area.y1, band.first_row, written_step(bridge), bridge.phase_y);
        band.frame_end_row =
            index + 1 < made
                ? rgb_start(bridge.area.y1, band.end_row, written_step(bridge), bridge.phase_y)
                : bridge.frame.height;
        band.surface = bridge.surface;
        present::set_surface_band(band.surface, band.first_row, band.end_row);
        band.open_tiles.clear();
        band.open_tiles.reserve(bridge.open_tiles.capacity());
        band.mapped_pixels = 0;
        // Nearest entries remembered for another palette are forgotten.
        if (!band.nearest_keys.empty() && band.lookup_builds != bridge.lookup_builds) {
            std::fill(band.nearest_keys.begin(), band.nearest_keys.end(), 0);
            std::fill(band.nearest_values.begin(), band.nearest_values.end(), 0);
        }
        band.lookup_builds = bridge.lookup_builds;
    }
    return made;
}

void bridge_band_colours(const RgbBridge& bridge, BridgeBand& band) {
    if (band.nearest_keys.size() == bridge_nearest_slots &&
        band.lookup_builds == bridge.lookup_builds)
        return;
    band.nearest_keys.assign(bridge_nearest_slots, 0);
    band.nearest_values.assign(bridge_nearest_slots, 0);
    band.lookup_builds = bridge.lookup_builds;
}

void bridge_open(RgbBridge& bridge, BridgeBand& band, const Rect32& region) {
    open_region(bridge, band_scope(bridge, band), region);
}

void bridge_end(RgbBridge& bridge, BridgeBand& band) {
    end_scope(bridge, band_scope(bridge, band));
}

void bridge_open_sampled(
    RgbBridge& bridge,
    BridgeBand& band,
    SampledRegion& sampled,
    const Rect32& region,
    uint32_t factor
) {
    open_sampled_region(bridge, band_scope(bridge, band), sampled, region, factor);
}

void bridge_end_sampled(
    const RgbBridge& bridge, const BridgeBand& band, const SampledRegion& sampled
) {
    end_sampled_region(bridge, sampled, band.frame_first_row, band.frame_end_row);
}

uint8_t bridge_index(RgbBridge& bridge, BridgeBand& band, uint8_t r, uint8_t g, uint8_t b) {
    if (band.nearest_keys.empty())
        return lookup_index(bridge, bridge.nearest_keys, bridge.nearest_values, r, g, b);
    return lookup_index(bridge, band.nearest_keys, band.nearest_values, r, g, b);
}

void bridge_join_band(RgbBridge& bridge, BridgeBand& band) noexcept {
    bridge.copy.mapped_pixels += band.mapped_pixels;
    band.mapped_pixels = 0;
}

} // namespace oa::present::model
