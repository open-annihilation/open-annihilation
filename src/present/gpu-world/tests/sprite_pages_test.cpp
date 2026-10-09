// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Card-ready sprite pages: frames decoded through the palette and the
// display gamma in each draw mode, the row-RLE edge cases through the GAF
// reader, malformed frames refused, packing in aligned cells, eviction
// under the memory limit and the memory figures; with --data, every frame
// of the installed game's GAF files against today's sprite drawers.

#include "oa/present/gpu_world/sprite_pages.hpp"

#include "oa/formats/gaf.hpp"
#include "oa/present/blit.hpp"
#include "oa/present/display.hpp"
#include "oa/present/gaf_sprites.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/surface.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using oa::Palette;
using oa::formats::gaf::RenderedFrame;
using oa::present::gpu_world::DrawMode;
using oa::present::gpu_world::FrameRecord;
using oa::present::gpu_world::FrameResult;
using oa::present::gpu_world::FrameStatus;
using oa::present::gpu_world::Limits;
using oa::present::gpu_world::Page;
using oa::present::gpu_world::SpritePages;
using oa::present::gpu_world::TexelRect;
using oa::present::gpu_world::texel_bytes;

using Texel = std::array<uint8_t, texel_bytes>;

constexpr Texel transparent{0, 0, 0, 0};
constexpr uint8_t full_alpha = oa::present::gpu_world::opaque_alpha;
constexpr uint32_t gutter = oa::present::gpu_world::frame_gutter;
constexpr uint32_t alignment = oa::present::gpu_world::cell_alignment;
constexpr size_t palette_entries = oa::present::gpu_world::gray_table_entries;
constexpr float plain_gamma = 1.0F;

// A palette whose channels tell the indices apart.
Palette test_palette() {
    Palette palette{};
    for (size_t index = 0; index < palette_entries; ++index) {
        palette.entries[index].r = static_cast<uint8_t>(index);
        palette.entries[index].g = static_cast<uint8_t>(255 - index);
        palette.entries[index].b = static_cast<uint8_t>((index * 37) & 0xFF);
    }
    return palette;
}

// A gray table that is a permutation, so a greyed texel differs from the
// opaque one.
std::array<uint8_t, palette_entries> test_gray_table() {
    std::array<uint8_t, palette_entries> table{};
    for (size_t index = 0; index < palette_entries; ++index)
        table[index] = static_cast<uint8_t>((index * 7 + 3) & 0xFF);
    return table;
}

RenderedFrame frame_of(
    uint16_t width,
    uint16_t height,
    std::vector<uint8_t> pixels,
    std::vector<uint8_t> coverage,
    int16_t origin_x = 0,
    int16_t origin_y = 0
) {
    RenderedFrame frame;
    frame.width = width;
    frame.height = height;
    frame.origin_x = origin_x;
    frame.origin_y = origin_y;
    frame.pixels = std::move(pixels);
    frame.coverage = std::move(coverage);
    return frame;
}

// A width x height frame with every pixel covered, the pixel at (x, y)
// holding first + y * width + x.
RenderedFrame covered_frame(uint16_t width, uint16_t height, uint8_t first = 0) {
    const size_t count = size_t{width} * height;
    std::vector<uint8_t> pixels(count);
    for (size_t index = 0; index < count; ++index)
        pixels[index] = static_cast<uint8_t>(first + index);
    return frame_of(width, height, std::move(pixels), std::vector<uint8_t>(count, 1));
}

Texel texel_at(const SpritePages& pages, uint32_t page, uint32_t x, uint32_t y) {
    const Page& held = pages.pages()[page];
    const size_t at = (size_t{y} * held.size + x) * texel_bytes;
    return {held.texels[at], held.texels[at + 1], held.texels[at + 2], held.texels[at + 3]};
}

Texel opaque_texel(const Palette& palette, float gamma, uint8_t index) {
    const auto& entry = palette.entries[index];
    return {
        oa::present::gamma_channel(entry.r, gamma),
        oa::present::gamma_channel(entry.g, gamma),
        oa::present::gamma_channel(entry.b, gamma),
        full_alpha
    };
}

bool same_rect(const TexelRect& a, const TexelRect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// The side of the cell a frame side takes: the frame with its gutter on
// both sides, rounded up to the alignment.
constexpr uint16_t cell_side(uint32_t frame_side) {
    const uint32_t with_gutter = frame_side + 2 * gutter;
    return static_cast<uint16_t>((with_gutter + alignment - 1) / alignment * alignment);
}

// The cell a record occupies: its rectangle, the gutter around it and the
// rounding up to the alignment.
TexelRect slot_of(const FrameRecord& record) {
    return {
        static_cast<uint16_t>(record.rect.x - gutter),
        static_cast<uint16_t>(record.rect.y - gutter),
        cell_side(record.rect.width),
        cell_side(record.rect.height)
    };
}

// Reports whether a record's cell keeps the packing contract: a corner and
// sides that are multiples of the alignment.
bool aligned(const FrameRecord& record) {
    const TexelRect slot = slot_of(record);
    return slot.x % alignment == 0 && slot.y % alignment == 0 && slot.width % alignment == 0 &&
           slot.height % alignment == 0;
}

bool overlap(const TexelRect& a, const TexelRect& b) {
    return a.x < b.x + b.width && b.x < a.x + a.width && a.y < b.y + b.height &&
           b.y < a.y + a.height;
}

// Checks the texels of a frame on its page: covered pixels through the
// palette, opaque, the rest and the whole of the cell around them
// transparent. Returns the number of texels that differ.
size_t mismatched_texels(
    const SpritePages& pages,
    const FrameRecord& record,
    const RenderedFrame& source,
    const Palette& palette,
    float gamma,
    const std::array<uint8_t, palette_entries>* gray
) {
    size_t mismatches = 0;
    const TexelRect slot = slot_of(record);
    for (uint32_t y = 0; y < slot.height; ++y) {
        for (uint32_t x = 0; x < slot.width; ++x) {
            const Texel texel = texel_at(pages, record.page, slot.x + x, slot.y + y);
            const bool inside = x >= gutter && y >= gutter && x < gutter + record.rect.width &&
                                y < gutter + record.rect.height;
            Texel expected = transparent;
            if (inside) {
                const size_t offset = (size_t{y} - gutter) * source.width + (x - gutter);
                if (source.coverage[offset] != 0) {
                    uint8_t index = source.pixels[offset];
                    if (record.mode == DrawMode::greyed)
                        index = (*gray)[index];
                    expected = opaque_texel(palette, gamma, index);
                }
            }
            if (texel != expected)
                ++mismatches;
        }
    }
    return mismatches;
}

void test_limits_normalised() {
    const SpritePages odd(Limits{100, 100, 1});
    OA_CHECK(odd.limits().page_size == 128);
    OA_CHECK(odd.limits().largest_page_size == 128);
    OA_CHECK(odd.limits().memory_limit == 1);
    const SpritePages wide(Limits{3, 5000, 0});
    OA_CHECK(wide.limits().page_size == 64);
    OA_CHECK(wide.limits().largest_page_size == 2048);
    const SpritePages inverted(Limits{2048, 64, 7});
    OA_CHECK(inverted.limits().page_size == 2048);
    OA_CHECK(inverted.limits().largest_page_size == 2048);
    const SpritePages given;
    OA_CHECK(given.limits().page_size == 1024);
    OA_CHECK(given.limits().largest_page_size == 2048);
    OA_CHECK(given.limits().memory_limit == size_t{32} * 1024 * 1024);
    OA_CHECK(!given.has_palette() && !given.has_gray_table());
    OA_CHECK(given.palette_generation() == 0);
}

void test_opaque_frame() {
    const Palette palette = test_palette();
    SpritePages pages;
    // A 4x3 frame, a hotspot, two uncovered pixels and a covered key pixel.
    RenderedFrame source = frame_of(
        4, 3, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}, {1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 0}, 2, -1
    );
    source.transparency_index = 1;

    FrameResult refused = pages.frame(7, DrawMode::opaque, source);
    OA_CHECK(refused.status == FrameStatus::no_palette);
    OA_CHECK(pages.statistics().refusals == 1);

    pages.set_palette(palette, plain_gamma);
    OA_CHECK(pages.has_palette() && pages.palette_generation() == 1);
    const FrameResult placed = pages.frame(7, DrawMode::opaque, source);
    OA_CHECK(placed.status == FrameStatus::ok);
    OA_CHECK(placed.record.page == 0);
    // The frame sits two texels inside an 8x8 cell: 4 + 4 wide, and 3 + 4
    // rounded up to even high.
    OA_CHECK(same_rect(placed.record.rect, TexelRect{2, 2, 4, 3}));
    OA_CHECK(aligned(placed.record));
    OA_CHECK(placed.record.origin_x == 2 && placed.record.origin_y == -1);
    OA_CHECK(placed.record.mode == DrawMode::opaque);
    OA_CHECK(pages.pages().size() == 1);
    const Page& page = pages.pages()[0];
    OA_CHECK(page.size == 1024);
    OA_CHECK(page.texels.size() == size_t{1024} * 1024 * texel_bytes);
    OA_CHECK(page.revision == 2);
    OA_CHECK(same_rect(page.dirty, TexelRect{0, 0, 8, 8}));
    OA_CHECK(mismatched_texels(pages, placed.record, source, palette, plain_gamma, nullptr) == 0);
    // The covered key pixel is drawn in the key's colour, as today.
    OA_CHECK(texel_at(pages, 0, 2, 2) == opaque_texel(palette, plain_gamma, 1));
    OA_CHECK(texel_at(pages, 0, 3, 3) == transparent);
    OA_CHECK(texel_at(pages, 0, 0, 0) == transparent && texel_at(pages, 0, 7, 7) == transparent);

    const auto use = pages.memory();
    OA_CHECK(use.page_bytes == size_t{1024} * 1024 * texel_bytes);
    OA_CHECK(use.frame_bytes == size_t{8} * 8 * texel_bytes);
    OA_CHECK(use.pages == 1 && use.frames == 1);
    OA_CHECK(use.memory_limit == pages.limits().memory_limit);

    const FrameResult found = pages.find(7, DrawMode::opaque);
    OA_CHECK(found.status == FrameStatus::ok && same_rect(found.record.rect, placed.record.rect));
    OA_CHECK(pages.holds(7, DrawMode::opaque));
    OA_CHECK(!pages.holds(7, DrawMode::greyed));
    OA_CHECK(pages.find(7, DrawMode::greyed).status == FrameStatus::not_held);
    OA_CHECK(pages.find(8, DrawMode::opaque).status == FrameStatus::not_held);
    const FrameResult again = pages.frame(7, DrawMode::opaque, source);
    OA_CHECK(again.status == FrameStatus::ok && again.record.page == 0);
    OA_CHECK(pages.statistics().decodes == 1 && pages.statistics().hits == 2);
    OA_CHECK(page.revision == 2);

    pages.clear_dirty(0);
    OA_CHECK(page.dirty.empty());
    pages.clear_dirty(99);
}

void test_gamma_matches_device_palette() {
    const Palette palette = test_palette();
    constexpr float gamma = 1.25F;
    oa::present::DisplayContext display;
    display.palette = palette;
    Palette device{};
    oa::present::set_palette_gamma(display, gamma, device);

    SpritePages pages;
    pages.set_palette(palette, gamma);
    const RenderedFrame all = covered_frame(16, 16);
    const FrameResult placed = pages.frame(1, DrawMode::opaque, all);
    OA_CHECK(placed.status == FrameStatus::ok);
    size_t mismatches = 0;
    for (uint32_t y = 0; y < 16; ++y) {
        for (uint32_t x = 0; x < 16; ++x) {
            const auto& entry = device.entries[y * 16 + x];
            const Texel expected{entry.r, entry.g, entry.b, full_alpha};
            if (texel_at(
                    pages, placed.record.page, placed.record.rect.x + x, placed.record.rect.y + y
                ) != expected)
                ++mismatches;
        }
    }
    OA_CHECK(mismatches == 0);
    OA_CHECK(mismatched_texels(pages, placed.record, all, palette, gamma, nullptr) == 0);
    // Gamma above 1 brightens and clamps: a visible effect, not the identity.
    OA_CHECK(device.entries[200].r == 250 && device.entries[210].r == 255);
}

void test_modes() {
    const Palette palette = test_palette();
    const auto gray = test_gray_table();
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);
    const RenderedFrame source = frame_of(2, 2, {10, 20, 30, 40}, {1, 1, 0, 1});

    OA_CHECK(pages.frame(1, DrawMode::greyed, source).status == FrameStatus::no_gray_table);
    pages.set_gray_table(gray);
    OA_CHECK(pages.has_gray_table());

    const FrameResult greyed = pages.frame(1, DrawMode::greyed, source);
    const FrameResult opaque = pages.frame(1, DrawMode::opaque, source);
    OA_CHECK(greyed.status == FrameStatus::ok && opaque.status == FrameStatus::ok);
    OA_CHECK(greyed.record.mode == DrawMode::greyed);
    OA_CHECK(opaque.record.mode == DrawMode::opaque);
    OA_CHECK(mismatched_texels(pages, greyed.record, source, palette, plain_gamma, &gray) == 0);
    OA_CHECK(mismatched_texels(pages, opaque.record, source, palette, plain_gamma, &gray) == 0);
    // Two records of one frame, side by side on one shelf in 6x6 cells.
    OA_CHECK(!overlap(slot_of(greyed.record), slot_of(opaque.record)));
    OA_CHECK(greyed.record.page == 0 && opaque.record.page == 0);
    OA_CHECK(same_rect(greyed.record.rect, TexelRect{2, 2, 2, 2}));
    OA_CHECK(same_rect(opaque.record.rect, TexelRect{8, 2, 2, 2}));
    OA_CHECK(aligned(greyed.record) && aligned(opaque.record));
    OA_CHECK(texel_at(pages, 0, 2, 2) == opaque_texel(palette, plain_gamma, gray[10]));
    OA_CHECK(texel_at(pages, 0, 8, 2) == opaque_texel(palette, plain_gamma, 10));
    // The uncovered pixel is transparent in both.
    OA_CHECK(texel_at(pages, 0, 2, 3) == transparent && texel_at(pages, 0, 8, 3) == transparent);
    OA_CHECK(pages.memory().frames == 2);
    OA_CHECK(pages.memory().frame_bytes == 2 * size_t{6} * 6 * texel_bytes);

    const uint64_t revision = pages.pages()[0].revision;
    pages.forget(1);
    OA_CHECK(pages.memory().frames == 0 && pages.memory().frame_bytes == 0);
    OA_CHECK(pages.memory().pages == 0 && pages.memory().page_bytes == 0);
    OA_CHECK(pages.pages()[0].size == 0 && pages.pages()[0].texels.empty());
    OA_CHECK(pages.pages()[0].revision == revision + 1);
    OA_CHECK(!pages.holds(1, DrawMode::opaque) && !pages.holds(1, DrawMode::greyed));
    pages.forget(1);
    // The released index is taken again.
    const FrameResult next = pages.frame(2, DrawMode::opaque, source);
    OA_CHECK(next.status == FrameStatus::ok && next.record.page == 0);
    OA_CHECK(pages.pages().size() == 1 && pages.pages()[0].size == 1024);
}

// --- A GAF file built by hand for the row-RLE edge cases. ---

void put_u16(std::vector<uint8_t>& bytes, size_t at, uint16_t value) {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8);
}

void put_u32(std::vector<uint8_t>& bytes, size_t at, uint32_t value) {
    for (size_t i = 0; i < 4; ++i)
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

void put_frame_info(
    std::vector<uint8_t>& bytes,
    size_t at,
    uint16_t width,
    uint16_t height,
    int16_t origin_x,
    int16_t origin_y,
    uint8_t key,
    uint8_t compressed,
    uint8_t layers,
    uint32_t data
) {
    put_u16(bytes, at, width);
    put_u16(bytes, at + 2, height);
    put_u16(bytes, at + 4, static_cast<uint16_t>(origin_x));
    put_u16(bytes, at + 6, static_cast<uint16_t>(origin_y));
    bytes[at + 8] = key;
    bytes[at + 9] = compressed;
    bytes[at + 10] = layers;
    bytes[at + 11] = 0;
    put_u32(bytes, at + 16, data);
}

// Row-RLE commands.
constexpr uint8_t rle_skip(uint8_t count) {
    return static_cast<uint8_t>((count << 1) | 1);
}

constexpr uint8_t rle_literal(uint8_t count) {
    return static_cast<uint8_t>((count - 1) << 2);
}

constexpr uint8_t rle_run(uint8_t count) {
    return static_cast<uint8_t>(((count - 1) << 2) | 2);
}

// Offsets inside the sample file.
constexpr size_t sample_sequence = 16;
constexpr size_t sample_frame_list = sample_sequence + 40;
constexpr size_t sample_frame_raw = sample_frame_list + 4 * 8;
constexpr size_t sample_frame_rle = sample_frame_raw + 24;
constexpr size_t sample_frame_rows = sample_frame_rle + 24;
constexpr size_t sample_frame_composite = sample_frame_rows + 24;
constexpr size_t sample_child_raw = sample_frame_composite + 24;
constexpr size_t sample_child_rle = sample_child_raw + 24;
constexpr size_t sample_raw_pixels = sample_child_rle + 24;
constexpr size_t sample_rle_stream = sample_raw_pixels + 6;
constexpr size_t sample_rows_stream = sample_rle_stream + 13;
constexpr size_t sample_child_table = sample_rows_stream + 9;
constexpr size_t sample_child_pixels = sample_child_table + 8;
constexpr size_t sample_child_stream = sample_child_pixels + 4;
constexpr size_t sample_size = sample_child_stream + 5;
constexpr uint8_t sample_key = 9;

// One sequence of four frames: a raw frame with key pixels, a row-RLE frame
// with a skip, literals, a literal equal to the key and a run, a row-RLE
// frame with an empty row, a skip past the width and a run, and a composite
// of a raw child and a row-RLE child.
std::vector<uint8_t> sample_gaf() {
    std::vector<uint8_t> bytes(sample_size, 0);
    put_u32(bytes, 0, 0x00010100);
    put_u32(bytes, 4, 1);
    put_u32(bytes, 12, sample_sequence);
    put_u16(bytes, sample_sequence, 4);
    std::memcpy(bytes.data() + sample_sequence + 8, "EDGES", 5);
    put_u32(bytes, sample_frame_list, sample_frame_raw);
    put_u32(bytes, sample_frame_list + 8, sample_frame_rle);
    put_u32(bytes, sample_frame_list + 16, sample_frame_rows);
    put_u32(bytes, sample_frame_list + 24, sample_frame_composite);

    put_frame_info(bytes, sample_frame_raw, 3, 2, 1, -1, sample_key, 0, 0, sample_raw_pixels);
    const uint8_t raw[] = {1, sample_key, 2, 3, 4, sample_key};
    std::memcpy(bytes.data() + sample_raw_pixels, raw, sizeof raw);

    put_frame_info(bytes, sample_frame_rle, 4, 2, 0, 0, 0, 1, 0, sample_rle_stream);
    // Row 0: skip 1, copy 21 22 23. Row 1: copy 0 (the key) and 5, skip 2.
    const uint8_t stream[] = {
        5,
        0,
        rle_skip(1),
        rle_literal(3),
        21,
        22,
        23, //
        4,
        0,
        rle_literal(2),
        0,
        5,
        rle_skip(2)
    };
    static_assert(sizeof stream == 13);
    std::memcpy(bytes.data() + sample_rle_stream, stream, sizeof stream);

    put_frame_info(bytes, sample_frame_rows, 3, 3, 0, 0, 0, 1, 0, sample_rows_stream);
    // Row 0: empty. Row 1: skip 5, past the width. Row 2: a run of 6.
    const uint8_t rows[] = {0, 0, 1, 0, rle_skip(5), 2, 0, rle_run(3), 6};
    static_assert(sizeof rows == 9);
    std::memcpy(bytes.data() + sample_rows_stream, rows, sizeof rows);

    put_frame_info(bytes, sample_frame_composite, 6, 5, 3, 2, 0, 0, 2, sample_child_table);
    put_u32(bytes, sample_child_table, sample_child_raw);
    put_u32(bytes, sample_child_table + 4, sample_child_rle);
    put_frame_info(bytes, sample_child_raw, 2, 2, 0, 0, 0, 0, 0, sample_child_pixels);
    const uint8_t child_pixels[] = {5, 0, 0, 6};
    std::memcpy(bytes.data() + sample_child_pixels, child_pixels, sizeof child_pixels);
    put_frame_info(bytes, sample_child_rle, 2, 1, 1, 0, 0, 1, 0, sample_child_stream);
    const uint8_t child_stream[] = {3, 0, rle_literal(2), 8, 8};
    std::memcpy(bytes.data() + sample_child_stream, child_stream, sizeof child_stream);
    return bytes;
}

// Decodes a frame of the sample through the GAF reader as the match does.
RenderedFrame rendered_sample_frame(const std::vector<uint8_t>& bytes, size_t index) {
    const auto parsed = oa::formats::gaf::parse(bytes);
    OA_CHECK(parsed.ok());
    if (!parsed.ok())
        return {};
    const auto rendered =
        oa::formats::gaf::render_normal(parsed.archive->sequences[0].frames[index]);
    OA_CHECK(rendered.ok());
    return rendered.ok() ? *rendered.frame : RenderedFrame{};
}

uint8_t alpha_at(const SpritePages& pages, const FrameRecord& record, uint32_t x, uint32_t y) {
    return texel_at(pages, record.page, record.rect.x + x, record.rect.y + y)[3];
}

void test_rle_edges() {
    const Palette palette = test_palette();
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);
    const auto bytes = sample_gaf();

    // Raw: a pixel equal to the key is not drawn.
    const RenderedFrame raw = rendered_sample_frame(bytes, 0);
    const FrameResult raw_placed = pages.frame(1, DrawMode::opaque, raw);
    OA_CHECK(raw_placed.status == FrameStatus::ok);
    OA_CHECK(raw_placed.record.origin_x == 1 && raw_placed.record.origin_y == -1);
    OA_CHECK(alpha_at(pages, raw_placed.record, 0, 0) == full_alpha);
    OA_CHECK(alpha_at(pages, raw_placed.record, 1, 0) == 0);
    OA_CHECK(alpha_at(pages, raw_placed.record, 2, 1) == 0);
    OA_CHECK(mismatched_texels(pages, raw_placed.record, raw, palette, plain_gamma, nullptr) == 0);

    // Row-RLE: a skip is transparent; a literal equal to the key is drawn.
    const RenderedFrame rle = rendered_sample_frame(bytes, 1);
    const FrameResult rle_placed = pages.frame(2, DrawMode::opaque, rle);
    OA_CHECK(rle_placed.status == FrameStatus::ok);
    OA_CHECK(alpha_at(pages, rle_placed.record, 0, 0) == 0);
    OA_CHECK(
        texel_at(pages, 0, rle_placed.record.rect.x + 1, rle_placed.record.rect.y) ==
        opaque_texel(palette, plain_gamma, 21)
    );
    OA_CHECK(
        texel_at(pages, 0, rle_placed.record.rect.x, rle_placed.record.rect.y + 1) ==
        opaque_texel(palette, plain_gamma, 0)
    );
    OA_CHECK(alpha_at(pages, rle_placed.record, 1, 1) == full_alpha);
    OA_CHECK(alpha_at(pages, rle_placed.record, 2, 1) == 0);
    OA_CHECK(alpha_at(pages, rle_placed.record, 3, 1) == 0);
    OA_CHECK(mismatched_texels(pages, rle_placed.record, rle, palette, plain_gamma, nullptr) == 0);

    // Row-RLE: an empty row and a skip past the width are transparent; a run
    // past the width stops at the edge.
    const RenderedFrame rows = rendered_sample_frame(bytes, 2);
    const FrameResult rows_placed = pages.frame(3, DrawMode::opaque, rows);
    OA_CHECK(rows_placed.status == FrameStatus::ok);
    for (uint32_t x = 0; x < 3; ++x) {
        OA_CHECK(alpha_at(pages, rows_placed.record, x, 0) == 0);
        OA_CHECK(alpha_at(pages, rows_placed.record, x, 1) == 0);
        OA_CHECK(alpha_at(pages, rows_placed.record, x, 2) == full_alpha);
    }
    OA_CHECK(
        texel_at(pages, 0, rows_placed.record.rect.x, rows_placed.record.rect.y + 2) ==
        opaque_texel(palette, plain_gamma, 6)
    );

    // Composite: each child at the parent's hotspot less its own, the later
    // child over the earlier.
    const RenderedFrame composite = rendered_sample_frame(bytes, 3);
    const FrameResult composite_placed = pages.frame(4, DrawMode::opaque, composite);
    OA_CHECK(composite_placed.status == FrameStatus::ok);
    OA_CHECK(same_rect(
        composite_placed.record.rect,
        TexelRect{composite_placed.record.rect.x, composite_placed.record.rect.y, 6, 5}
    ));
    const auto colour = [&](uint32_t x, uint32_t y) {
        return texel_at(
            pages, 0, composite_placed.record.rect.x + x, composite_placed.record.rect.y + y
        );
    };
    OA_CHECK(colour(2, 2) == opaque_texel(palette, plain_gamma, 8));
    OA_CHECK(colour(3, 2) == opaque_texel(palette, plain_gamma, 8));
    OA_CHECK(colour(4, 2) == transparent);
    OA_CHECK(colour(3, 3) == transparent);
    OA_CHECK(colour(4, 3) == opaque_texel(palette, plain_gamma, 6));
    OA_CHECK(colour(0, 0) == transparent && colour(5, 4) == transparent);
    OA_CHECK(
        mismatched_texels(
            pages, composite_placed.record, composite, palette, plain_gamma, nullptr
        ) == 0
    );

    // Malformed streams never reach the pages: the reader refuses them. A
    // literal past the width is cut to it, and a leftover byte is read as
    // one more command, so a row goes wrong only when its byte count ends
    // before the literal's bytes or before the width is covered.
    const auto before = pages.memory();
    auto literal_short = bytes;
    put_u16(literal_short, sample_rle_stream, 4);
    OA_CHECK(!oa::formats::gaf::parse(literal_short).ok());
    auto row_short = bytes;
    put_u16(row_short, sample_rle_stream, 4);
    row_short[sample_rle_stream + 3] = rle_literal(2);
    OA_CHECK(!oa::formats::gaf::parse(row_short).ok());
    auto literal_cut = bytes;
    literal_cut[sample_rle_stream + 3] = rle_literal(5);
    OA_CHECK(oa::formats::gaf::parse(literal_cut).ok());
    auto row_cut = bytes;
    put_u16(row_cut, sample_child_stream, 0x100);
    OA_CHECK(!oa::formats::gaf::parse(row_cut).ok());
    auto run_bare = bytes;
    put_u16(run_bare, sample_rows_stream + 5, 1);
    run_bare[sample_rows_stream + 7] = rle_run(3);
    OA_CHECK(!oa::formats::gaf::parse(run_bare).ok());
    const auto after = pages.memory();
    OA_CHECK(after.frames == before.frames && after.frame_bytes == before.frame_bytes);
    OA_CHECK(pages.statistics().decodes == 4 && pages.statistics().refusals == 0);
}

void test_malformed_frames_refused() {
    const Palette palette = test_palette();
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);

    RenderedFrame short_pixels = covered_frame(2, 2);
    short_pixels.pixels.resize(3);
    OA_CHECK(pages.frame(1, DrawMode::opaque, short_pixels).status == FrameStatus::malformed);
    RenderedFrame long_pixels = covered_frame(2, 2);
    long_pixels.pixels.resize(5);
    OA_CHECK(pages.frame(2, DrawMode::opaque, long_pixels).status == FrameStatus::malformed);
    RenderedFrame short_coverage = covered_frame(2, 2);
    short_coverage.coverage.resize(3);
    OA_CHECK(pages.frame(3, DrawMode::opaque, short_coverage).status == FrameStatus::malformed);
    OA_CHECK(pages.frame(4, DrawMode::opaque, frame_of(0, 3, {}, {})).status == FrameStatus::empty);
    OA_CHECK(pages.frame(5, DrawMode::opaque, frame_of(3, 0, {}, {})).status == FrameStatus::empty);
    OA_CHECK(pages.memory().frames == 0 && pages.memory().pages == 0);
    OA_CHECK(pages.statistics().refusals == 5);

    // The frame's cell must fit the largest page: 2044 with its gutters is
    // 2048, and 2045 rounds up to 2050.
    const RenderedFrame widest = covered_frame(2044, 1);
    const FrameResult widest_placed = pages.frame(6, DrawMode::opaque, widest);
    OA_CHECK(widest_placed.status == FrameStatus::ok);
    OA_CHECK(pages.pages()[widest_placed.record.page].size == 2048);
    OA_CHECK(pages.memory().page_bytes == size_t{2048} * 2048 * texel_bytes);
    OA_CHECK(aligned(widest_placed.record));
    const RenderedFrame too_wide = covered_frame(2045, 1);
    OA_CHECK(pages.frame(7, DrawMode::opaque, too_wide).status == FrameStatus::too_large);
    OA_CHECK(
        pages.frame(8, DrawMode::opaque, covered_frame(1, 2045)).status == FrameStatus::too_large
    );
    OA_CHECK(
        mismatched_texels(pages, widest_placed.record, widest, palette, plain_gamma, nullptr) == 0
    );

    // A page the limit never allows is refused without evicting anything.
    SpritePages small(Limits{64, 2048, size_t{64} * 64 * texel_bytes});
    small.set_palette(palette, plain_gamma);
    OA_CHECK(small.frame(1, DrawMode::opaque, covered_frame(10, 10)).status == FrameStatus::ok);
    OA_CHECK(
        small.frame(2, DrawMode::opaque, covered_frame(63, 63)).status == FrameStatus::no_room
    );
    OA_CHECK(small.holds(1, DrawMode::opaque));
    OA_CHECK(small.statistics().evictions == 0);

    SpritePages none(Limits{64, 64, 0});
    none.set_palette(palette, plain_gamma);
    OA_CHECK(none.frame(1, DrawMode::opaque, covered_frame(1, 1)).status == FrameStatus::no_room);
    OA_CHECK(none.memory().pages == 0);
}

void test_packing() {
    const Palette palette = test_palette();
    SpritePages pages(Limits{256, 2048, size_t{64} * 1024 * 1024});
    pages.set_palette(palette, plain_gamma);

    struct Placed {
        uint32_t page{};
        TexelRect slot{};
    };

    std::vector<Placed> placed;
    uint32_t seed = 12345;
    const auto next = [&seed]() {
        seed = seed * 1103515245U + 12345U;
        return (seed >> 16) & 0x7FFF;
    };
    size_t slot_bytes = 0;
    for (uint64_t id = 1; id <= 300; ++id) {
        const auto width = static_cast<uint16_t>(1 + next() % 60);
        const auto height = static_cast<uint16_t>(1 + next() % 60);
        const RenderedFrame source = covered_frame(width, height, static_cast<uint8_t>(id));
        const FrameResult result = pages.frame(id, DrawMode::opaque, source);
        OA_CHECK(result.status == FrameStatus::ok);
        if (result.status != FrameStatus::ok)
            continue;
        const TexelRect slot = slot_of(result.record);
        const Page& page = pages.pages()[result.record.page];
        OA_CHECK(page.size >= 256 && page.size <= 2048 && (page.size & (page.size - 1)) == 0);
        OA_CHECK(
            uint32_t{slot.x} + uint32_t{slot.width} <= page.size &&
            uint32_t{slot.y} + uint32_t{slot.height} <= page.size
        );
        // The packing contract: every cell's corner and sides even, odd
        // frames included, so that a level 1 of the page is exact per cell.
        OA_CHECK(aligned(result.record));
        OA_CHECK(slot.width == cell_side(width) && slot.height == cell_side(height));
        for (const Placed& other : placed) {
            if (other.page == result.record.page)
                OA_CHECK(!overlap(other.slot, slot));
        }
        OA_CHECK(
            mismatched_texels(pages, result.record, source, palette, plain_gamma, nullptr) == 0
        );
        placed.push_back({result.record.page, slot});
        slot_bytes += size_t{slot.width} * slot.height * texel_bytes;
    }
    OA_CHECK(pages.memory().frames == 300);
    OA_CHECK(pages.memory().frame_bytes == slot_bytes);
    OA_CHECK(pages.statistics().evictions == 0);
    size_t page_bytes = 0;
    uint32_t alive = 0;
    for (const Page& page : pages.pages()) {
        page_bytes += page.texels.size();
        alive += page.size != 0 ? 1 : 0;
    }
    OA_CHECK(pages.memory().page_bytes == page_bytes && pages.memory().pages == alive);
    OA_CHECK(alive >= 2);

    // A frame too big for an ordinary page gets a larger page of its own.
    const FrameResult wide = pages.frame(301, DrawMode::opaque, covered_frame(300, 10));
    OA_CHECK(wide.status == FrameStatus::ok);
    OA_CHECK(pages.pages()[wide.record.page].size == 512);
    const FrameResult tall = pages.frame(302, DrawMode::opaque, covered_frame(10, 1000));
    OA_CHECK(tall.status == FrameStatus::ok);
    OA_CHECK(pages.pages()[tall.record.page].size == 1024);
    OA_CHECK(aligned(wide.record) && aligned(tall.record));
}

void test_eviction() {
    const Palette palette = test_palette();
    const size_t one_page = size_t{64} * 64 * texel_bytes;
    SpritePages pages(Limits{64, 64, one_page});
    pages.set_palette(palette, plain_gamma);
    // 28x28 frames take 32x32 cells: four to a page.
    for (uint64_t id = 1; id <= 4; ++id)
        OA_CHECK(
            pages.frame(id, DrawMode::opaque, covered_frame(28, 28)).status == FrameStatus::ok
        );
    OA_CHECK(pages.memory().pages == 1 && pages.memory().frames == 4);
    OA_CHECK(pages.memory().page_bytes == one_page);

    // A used frame survives; the least recently used one goes.
    OA_CHECK(pages.find(1, DrawMode::opaque).status == FrameStatus::ok);
    const FrameResult fifth = pages.frame(5, DrawMode::opaque, covered_frame(28, 28));
    OA_CHECK(fifth.status == FrameStatus::ok);
    OA_CHECK(pages.statistics().evictions == 1);
    OA_CHECK(!pages.holds(2, DrawMode::opaque));
    OA_CHECK(pages.holds(1, DrawMode::opaque) && pages.holds(3, DrawMode::opaque));
    OA_CHECK(pages.holds(4, DrawMode::opaque) && pages.holds(5, DrawMode::opaque));
    OA_CHECK(pages.memory().page_bytes <= one_page && pages.memory().frames == 4);
    // The evicted frame's cell was the one reused.
    OA_CHECK(same_rect(fifth.record.rect, TexelRect{34, 2, 28, 28}));

    // A frame that needs the whole page evicts every other frame.
    const FrameResult whole = pages.frame(6, DrawMode::opaque, covered_frame(60, 60));
    OA_CHECK(whole.status == FrameStatus::ok);
    OA_CHECK(pages.statistics().evictions == 5);
    OA_CHECK(pages.memory().frames == 1 && pages.memory().pages == 1);
    OA_CHECK(pages.memory().page_bytes == one_page);
    OA_CHECK(same_rect(whole.record.rect, TexelRect{2, 2, 60, 60}));
    for (uint64_t id = 1; id <= 5; ++id)
        OA_CHECK(!pages.holds(id, DrawMode::opaque));
    OA_CHECK(
        mismatched_texels(
            pages, whole.record, covered_frame(60, 60), palette, plain_gamma, nullptr
        ) == 0
    );

    // With more room, pages are added before anything is evicted.
    SpritePages roomy(Limits{64, 64, 3 * one_page});
    roomy.set_palette(palette, plain_gamma);
    for (uint64_t id = 1; id <= 12; ++id)
        OA_CHECK(
            roomy.frame(id, DrawMode::opaque, covered_frame(28, 28)).status == FrameStatus::ok
        );
    OA_CHECK(roomy.memory().pages == 3 && roomy.statistics().evictions == 0);
    OA_CHECK(roomy.frame(13, DrawMode::opaque, covered_frame(28, 28)).status == FrameStatus::ok);
    OA_CHECK(roomy.statistics().evictions == 1 && !roomy.holds(1, DrawMode::opaque));
    OA_CHECK(roomy.memory().page_bytes == 3 * one_page);
    // Forgetting every frame of a page releases it, and the next frame
    // takes the released index.
    roomy.forget(2);
    roomy.forget(3);
    roomy.forget(4);
    roomy.forget(13);
    OA_CHECK(roomy.memory().pages == 2 && roomy.pages()[0].size == 0);
    OA_CHECK(roomy.memory().page_bytes == 2 * one_page);
    const FrameResult reused = roomy.frame(14, DrawMode::opaque, covered_frame(28, 28));
    OA_CHECK(reused.status == FrameStatus::ok && reused.record.page == 0);
    OA_CHECK(roomy.memory().pages == 3);
}

/// The frames of the frame under way are never evicted: where they fill the
/// limit, it grows as far as the growth hooks allow and the largest limit,
/// doubling, and past that a new frame is refused; the next frame holds
/// only what it uses.
void test_held_frames_and_growth() {
    const Palette palette = test_palette();
    const size_t one_page = size_t{64} * 64 * texel_bytes;
    SpritePages pages(Limits{64, 64, one_page, 4 * one_page});
    pages.set_palette(palette, plain_gamma);
    const auto place = [&](uint64_t id) {
        return pages.frame(id, DrawMode::opaque, covered_frame(28, 28)).status;
    };
    // 28x28 frames take 32x32 cells: four fill a page.
    pages.begin_frame();
    for (uint64_t id = 1; id <= 4; ++id)
        OA_CHECK(place(id) == FrameStatus::ok);
    // Without growth hooks the limit stays, and no frame of this frame goes.
    OA_CHECK(place(5) == FrameStatus::no_room);
    OA_CHECK(pages.statistics().evictions == 0 && pages.statistics().held_refusals == 1);
    for (uint64_t id = 1; id <= 4; ++id)
        OA_CHECK(pages.holds(id, DrawMode::opaque));
    // The next frame holds only what it uses.
    pages.begin_frame();
    OA_CHECK(pages.find(1, DrawMode::opaque).status == FrameStatus::ok);
    OA_CHECK(place(5) == FrameStatus::ok);
    OA_CHECK(pages.statistics().evictions == 1 && !pages.holds(2, DrawMode::opaque));

    struct Asked {
        size_t bytes{};
        bool allowed{true};
    } asked;

    pages.set_growth_hooks({&asked, [](void* context, size_t bytes) {
                                auto& hooks = *static_cast<Asked*>(context);
                                hooks.bytes += bytes;
                                return hooks.allowed;
                            }});
    // A frame of five frames: four evict the last frame's, the fifth grows
    // the limit to two pages.
    pages.begin_frame();
    for (uint64_t id = 10; id <= 14; ++id)
        OA_CHECK(place(id) == FrameStatus::ok);
    OA_CHECK(pages.statistics().evictions == 5 && pages.statistics().growths == 1);
    OA_CHECK(asked.bytes == one_page && pages.limits().memory_limit == 2 * one_page);
    OA_CHECK(pages.memory().pages == 2);
    // Refused growth refuses the frame.
    for (uint64_t id = 15; id <= 17; ++id)
        OA_CHECK(place(id) == FrameStatus::ok);
    asked.allowed = false;
    OA_CHECK(place(18) == FrameStatus::no_room);
    OA_CHECK(pages.statistics().held_refusals == 2 && pages.limits().memory_limit == 2 * one_page);
    // Allowed again, the limit doubles to the largest, and stops there.
    asked.allowed = true;
    for (uint64_t id = 18; id <= 25; ++id)
        OA_CHECK(place(id) == FrameStatus::ok);
    OA_CHECK(pages.statistics().growths == 2 && pages.limits().memory_limit == 4 * one_page);
    OA_CHECK(place(26) == FrameStatus::no_room);
    OA_CHECK(pages.statistics().held_refusals == 3 && pages.memory().pages == 4);
    // The limit goes back to the one the pages were made with.
    pages.reset_memory_limit();
    OA_CHECK(pages.limits().memory_limit == one_page);
    // A largest limit under the limit is the limit.
    const SpritePages fixed(Limits{64, 64, 2 * one_page, one_page});
    OA_CHECK(fixed.limits().largest_memory_limit == 2 * one_page);
}

void test_slot_reuse() {
    const Palette palette = test_palette();
    SpritePages pages(Limits{256, 256, size_t{256} * 256 * texel_bytes});
    pages.set_palette(palette, plain_gamma);
    // 20x10 frames take 24x14 cells along one shelf.
    FrameResult first = pages.frame(1, DrawMode::opaque, covered_frame(20, 10));
    FrameResult second = pages.frame(2, DrawMode::opaque, covered_frame(20, 10));
    FrameResult third = pages.frame(3, DrawMode::opaque, covered_frame(20, 10));
    OA_CHECK(first.status == FrameStatus::ok && second.status == FrameStatus::ok);
    OA_CHECK(third.status == FrameStatus::ok);
    OA_CHECK(second.record.rect.x == 26 && second.record.rect.y == 2);
    // A frame of the same size takes the freed cell in the middle of the
    // shelf; a smaller one takes its start and leaves the rest free.
    pages.forget(2);
    const FrameResult same = pages.frame(4, DrawMode::opaque, covered_frame(20, 10));
    OA_CHECK(same.status == FrameStatus::ok && same_rect(same.record.rect, second.record.rect));
    pages.forget(4);
    const FrameResult smaller = pages.frame(5, DrawMode::opaque, covered_frame(8, 8));
    OA_CHECK(smaller.status == FrameStatus::ok);
    OA_CHECK(smaller.record.rect.x == 26 && smaller.record.rect.y == 2);
    const FrameResult rest = pages.frame(6, DrawMode::opaque, covered_frame(8, 10));
    OA_CHECK(rest.status == FrameStatus::ok);
    OA_CHECK(rest.record.rect.x == 38 && rest.record.rect.y == 2);
    OA_CHECK(!overlap(slot_of(rest.record), slot_of(third.record)));
    OA_CHECK(aligned(smaller.record) && aligned(rest.record));
    // Freeing the last cells of a shelf gives its rows back: a taller frame
    // then starts a shelf where the old one was.
    pages.forget(3);
    pages.forget(6);
    pages.forget(5);
    pages.forget(1);
    OA_CHECK(pages.memory().pages == 0);
    const FrameResult tall = pages.frame(7, DrawMode::opaque, covered_frame(5, 40));
    OA_CHECK(tall.status == FrameStatus::ok);
    OA_CHECK(tall.record.rect.x == 2 && tall.record.rect.y == 2);
}

void test_palette_and_gray_changes() {
    const Palette palette = test_palette();
    const auto gray = test_gray_table();
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);
    pages.set_gray_table(gray);
    const RenderedFrame source = covered_frame(4, 4);
    OA_CHECK(pages.frame(1, DrawMode::opaque, source).status == FrameStatus::ok);
    OA_CHECK(pages.frame(1, DrawMode::greyed, source).status == FrameStatus::ok);
    OA_CHECK(pages.frame(2, DrawMode::greyed, source).status == FrameStatus::ok);
    OA_CHECK(pages.memory().frames == 3);

    // The same palette again changes nothing.
    pages.set_palette(palette, plain_gamma);
    OA_CHECK(pages.palette_generation() == 1 && pages.memory().frames == 3);
    // Nor does the same gray table again: the greyed frames stay.
    pages.set_gray_table(gray);
    OA_CHECK(pages.memory().frames == 3);
    OA_CHECK(pages.holds(1, DrawMode::greyed) && pages.holds(2, DrawMode::greyed));

    // A new gray table drops the greyed frames only.
    auto other_gray = gray;
    std::reverse(other_gray.begin(), other_gray.end());
    pages.set_gray_table(other_gray);
    OA_CHECK(pages.memory().frames == 1);
    OA_CHECK(pages.holds(1, DrawMode::opaque));
    OA_CHECK(!pages.holds(1, DrawMode::greyed) && !pages.holds(2, DrawMode::greyed));
    const FrameResult regreyed = pages.frame(2, DrawMode::greyed, source);
    OA_CHECK(regreyed.status == FrameStatus::ok);
    OA_CHECK(
        mismatched_texels(pages, regreyed.record, source, palette, plain_gamma, &other_gray) == 0
    );

    // A new gamma empties the pages and starts a new generation.
    pages.set_palette(palette, 1.5F);
    OA_CHECK(pages.palette_generation() == 2);
    OA_CHECK(pages.memory().frames == 0 && pages.memory().pages == 0);
    OA_CHECK(!pages.holds(1, DrawMode::opaque));
    const FrameResult brighter = pages.frame(1, DrawMode::opaque, source);
    OA_CHECK(brighter.status == FrameStatus::ok);
    OA_CHECK(mismatched_texels(pages, brighter.record, source, palette, 1.5F, nullptr) == 0);
    pages.clear();
    OA_CHECK(pages.memory().frames == 0 && pages.memory().pages == 0);
    OA_CHECK(pages.memory().page_bytes == 0 && pages.memory().frame_bytes == 0);
}

void test_dirty_and_revisions() {
    const Palette palette = test_palette();
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);
    const FrameResult first = pages.frame(1, DrawMode::opaque, covered_frame(4, 4));
    const FrameResult second = pages.frame(2, DrawMode::opaque, covered_frame(4, 4));
    OA_CHECK(first.status == FrameStatus::ok && second.status == FrameStatus::ok);
    const Page& page = pages.pages()[0];
    OA_CHECK(page.revision == 3);
    // Two 8x8 cells on one shelf.
    OA_CHECK(same_rect(page.dirty, TexelRect{0, 0, 16, 8}));
    pages.clear_dirty(0);
    OA_CHECK(page.dirty.empty());
    const FrameResult third = pages.frame(3, DrawMode::opaque, covered_frame(4, 4));
    OA_CHECK(third.status == FrameStatus::ok);
    OA_CHECK(page.revision == 4);
    OA_CHECK(same_rect(page.dirty, slot_of(third.record)));
    // A use changes no texel.
    OA_CHECK(pages.find(1, DrawMode::opaque).status == FrameStatus::ok);
    OA_CHECK(page.revision == 4);
}

// --- The installed game. ---

// Draws a present sprite at its origin on a frame-sized surface filled with
// `fill`: today's sprite drawer.
std::vector<uint8_t> draw_over(const oa::Sprite* sprite, uint8_t fill) {
    auto surface = oa::present::create_surface(sprite->width, sprite->height);
    std::fill(surface.pixels.begin(), surface.pixels.end(), fill);
    oa::present::draw_sprite(&surface.surface, sprite, sprite->origin_x, sprite->origin_y);
    return surface.pixels;
}

struct SweepCounts {
    int files = 0;
    int frames = 0;
    int special = 0;
    int empty = 0;
    int placed = 0;
    int too_large = 0;
    size_t mismatched_texels = 0;
    size_t drawer_disagreements = 0;
    size_t peak_page_bytes = 0;
};

// Every frame of every GAF the installed game provides, placed on the pages
// and compared with the GAF reader's pixels through the palette and with
// draw_sprite.
void test_installed_sweep(const oa::AssetStore& assets) {
    const auto palette_bytes = oa::test::read_game_file(assets, "palettes/PALETTE.PAL");
    OA_CHECK(palette_bytes.size() >= palette_entries * 4);
    const Palette palette = oa::present::palette_from_bytes(palette_bytes);
    SpritePages pages;
    pages.set_palette(palette, plain_gamma);
    SweepCounts counts;
    uint64_t next_id = 1;
    for (const auto& name : assets.list_effective_recursive("", ".gaf")) {
        const auto bytes = oa::test::read_game_file(assets, name);
        const auto parsed = oa::formats::gaf::parse(bytes);
        oa::present::GafSprites relocated;
        const auto status = oa::present::relocate_gaf(bytes, relocated);
        if (!parsed.ok() || status != oa::present::GafStatus::ok) {
            std::fprintf(stderr, "%s: not read\n", name.c_str());
            OA_CHECK(false);
            continue;
        }
        ++counts.files;
        const auto& sequences = parsed.archive->sequences;
        for (size_t s = 0; s < sequences.size() && s < relocated.sequences.size(); ++s) {
            for (size_t k = 0; k < sequences[s].frames.size(); ++k) {
                const auto rendered = oa::formats::gaf::render_normal(sequences[s].frames[k]);
                if (!rendered.ok()) {
                    ++counts.special;
                    continue;
                }
                ++counts.frames;
                const RenderedFrame& source = *rendered.frame;
                const FrameResult result = pages.frame(next_id++, DrawMode::opaque, source);
                if (source.width == 0 || source.height == 0) {
                    OA_CHECK(result.status == FrameStatus::empty);
                    ++counts.empty;
                    continue;
                }
                if (result.status == FrameStatus::too_large) {
                    ++counts.too_large;
                    continue;
                }
                if (result.status != FrameStatus::ok) {
                    std::fprintf(
                        stderr, "%s: frame %zu of sequence %zu refused\n", name.c_str(), k, s
                    );
                    OA_CHECK(false);
                    continue;
                }
                ++counts.placed;
                counts.peak_page_bytes =
                    std::max(counts.peak_page_bytes, pages.memory().page_bytes);
                OA_CHECK(pages.memory().page_bytes <= pages.limits().memory_limit);
                counts.mismatched_texels +=
                    mismatched_texels(pages, result.record, source, palette, plain_gamma, nullptr);
                // Today's drawer: a pixel is drawn when both fills end alike.
                const oa::Sprite* sprite =
                    oa::present::gaf_frame(&relocated.sequences[s], static_cast<int32_t>(k));
                if (sprite == nullptr || sprite->width != source.width ||
                    sprite->height != source.height) {
                    ++counts.drawer_disagreements;
                    continue;
                }
                const auto low = draw_over(sprite, 0x00);
                const auto high = draw_over(sprite, 0xFF);
                for (size_t i = 0; i < low.size(); ++i) {
                    const bool drawn = low[i] == high[i];
                    const uint32_t x = static_cast<uint32_t>(i % source.width);
                    const uint32_t y = static_cast<uint32_t>(i / source.width);
                    const Texel texel = texel_at(
                        pages,
                        result.record.page,
                        result.record.rect.x + x,
                        result.record.rect.y + y
                    );
                    const Texel expected =
                        drawn ? opaque_texel(palette, plain_gamma, low[i]) : transparent;
                    if (texel != expected) {
                        ++counts.drawer_disagreements;
                        break;
                    }
                }
            }
        }
    }
    std::printf(
        "installed GAF sweep: %d files, %d frames, %d placed, %d empty, %d through the blended "
        "child path, %d too large, %zu texels differing from the reader, %zu frames differing "
        "from the drawer, peak %zu bytes of pages, %llu evictions\n",
        counts.files,
        counts.frames,
        counts.placed,
        counts.empty,
        counts.special,
        counts.too_large,
        counts.mismatched_texels,
        counts.drawer_disagreements,
        counts.peak_page_bytes,
        static_cast<unsigned long long>(pages.statistics().evictions)
    );
    OA_CHECK(counts.files > 0 && counts.placed > 0);
    OA_CHECK(counts.mismatched_texels == 0 && counts.drawer_disagreements == 0);
    OA_CHECK(counts.too_large == 0);
    OA_CHECK(counts.peak_page_bytes <= pages.limits().memory_limit);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        const auto assets = oa::test::require_game_assets("the installed GAF sprite pages sweep");
        test_installed_sweep(assets);
    } else {
        test_limits_normalised();
        test_opaque_frame();
        test_gamma_matches_device_palette();
        test_modes();
        test_rle_edges();
        test_malformed_frames_refused();
        test_packing();
        test_eviction();
        test_held_frames_and_growth();
        test_slot_reuse();
        test_palette_and_gray_changes();
        test_dirty_and_revisions();
    }
    return oa::test::check_exit_status();
}
