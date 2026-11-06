// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Off-screen canvas, text and cell drawing, images and the placeholder
// shimmer.

#include <cstring>

#include "internal.hpp"
#include "oa/present/surface.hpp"

namespace oa::boneyards::html {

using namespace detail;

namespace {

constexpr int32_t k_row_alignment = 4;
constexpr uint8_t k_reserved_green_first = 1;
constexpr uint8_t k_reserved_green_last = 15;
constexpr uint8_t k_reserved_high_first = 240;
constexpr uint8_t k_reserved_high_last = 254;
constexpr uint8_t k_ramp_first = 0x10;
constexpr uint8_t k_ramp_end = 0xf1;
constexpr int32_t k_ramp_worst_distance = 0x2fa03; // 3 * 255^2
constexpr uint32_t k_random_modulus = 0x7fffffff;
constexpr uint32_t k_random_multiplier = 16807;
constexpr double k_random_scale = 4.656612875245797e-10; // 2^-31
constexpr double k_phase_step = 0.00390625;              // 1/256
constexpr float k_displacement = 0.005f;
constexpr int32_t k_ramp_steps = 255;
// Placeholder colour ramp: grey-blue from (32,32,32) to (64,64,255).
constexpr float k_ramp_red_green_span = -32.0f;
constexpr float k_ramp_blue_span = -223.0f;
constexpr int32_t k_ramp_base = 32;

/// Returns the canvas row pitch for a width: the width rounded up to a
/// multiple of 4.
///
/// @param width canvas width in pixels
/// @return the pitch in bytes
int32_t aligned_pitch(int32_t width) {
    if (width % k_row_alignment != 0)
        width = width - width % k_row_alignment + k_row_alignment;
    return width;
}

/// Converts a floating-point value to an integer by truncation towards zero.
///
/// @param value value to convert
/// @return the truncated value
int32_t truncate(double value) {
    return static_cast<int32_t>(value);
}

/// Returns the placeholder ramp colour for a height-field value, animated by
/// the shimmer phase.
///
/// The phase is added to the value and folded into a triangle wave; the
/// resulting step picks a colour between (32, 32, 32) and (64, 64, 255),
/// whose nearest ramp index is cached per step.
///
/// @param[in,out] doc document holding the shimmer state and palette
/// @param value height-field value, 0-1
/// @return the palette index
uint8_t shimmer_color(Document& doc, float value) {
    double t = static_cast<double>(doc.shimmer.phase) + static_cast<double>(value);
    if (t > 1.0)
        t -= 1.0;
    if (t > 0.5)
        t = 1.0 - t;
    t += t;
    const int32_t step = truncate(t * k_ramp_steps);
    const auto slot = static_cast<std::size_t>(step & 0xff);
    if (doc.shimmer.ramp[slot] == 0) {
        const int32_t red_green = k_ramp_base - truncate(t * k_ramp_red_green_span);
        const int32_t blue = k_ramp_base - truncate(t * k_ramp_blue_span);
        doc.shimmer.ramp[slot] = ramp_palette_index(
            doc.env.palette,
            static_cast<uint8_t>(red_green),
            static_cast<uint8_t>(red_green),
            static_cast<uint8_t>(blue)
        );
    }
    return doc.shimmer.ramp[slot];
}

void put_pixel(Canvas& canvas, int32_t x, int32_t y, uint8_t index) {
    if (uint8_t* pixel = canvas_pixel(canvas, x, y))
        *pixel = index;
}

struct HeightField {
    float* cells;
    int32_t stride;
    int32_t origin_x;
    int32_t origin_y;

    float& at(int32_t x, int32_t y) { return cells[(y - origin_y) * stride + (x - origin_x)]; }
};

float displaced(float a, float b, uint32_t random, int32_t span) {
    const double length = span;
    const double offset =
        static_cast<double>(random) * static_cast<double>(k_displacement) * length * k_random_scale;
    const double value = (offset + (static_cast<double>(a) + static_cast<double>(b)) * 0.5) -
                         static_cast<double>(k_displacement) * length * 0.5;
    float stored = static_cast<float>(value);
    if (value < 0.0)
        stored = 0.0f;
    if (stored > 1.0f)
        stored = 1.0f;
    return stored;
}

void subdivide_point(
    Document& doc,
    HeightField& field,
    uint32_t* seed,
    int32_t x,
    int32_t y,
    int32_t ax,
    int32_t ay,
    int32_t bx,
    int32_t by,
    int32_t span
) {
    float& cell = field.at(x, y);
    if (cell != 0.0f)
        return;
    const uint32_t random = shimmer_random(seed);
    cell = displaced(field.at(ax, ay), field.at(bx, by), random, span);
    put_pixel(doc.canvas, x, y, shimmer_color(doc, cell));
}

/// Fills a rectangle of the placeholder by midpoint displacement, splitting
/// the longer side first.
///
/// Each new point is the mean of its two neighbours plus a random offset
/// scaled by the side length, clamped to 0-1, and is drawn in its ramp
/// colour; points already set are kept.
///
/// @param[in,out] doc document holding the canvas and shimmer state
/// @param[in,out] field height field covering the rectangle
/// @param[in,out] seed shimmer_random state
/// @param left left column
/// @param top top row
/// @param right right column (inclusive)
/// @param bottom bottom row (inclusive)
void subdivide(
    Document& doc,
    HeightField& field,
    uint32_t* seed,
    int32_t left,
    int32_t top,
    int32_t right,
    int32_t bottom
) {
    for (;;) {
        if (right <= left)
            return;
        const int32_t mid_y = (bottom + top) >> 1;
        const int32_t mid_x = (left + right) >> 1;
        if (bottom <= top)
            return;
        const int32_t height = bottom - top;
        const int32_t width = right - left;
        int32_t remaining;
        if (height < width) {
            subdivide_point(doc, field, seed, mid_x, top, left, top, right, top, width);
            subdivide_point(doc, field, seed, mid_x, bottom, left, bottom, right, bottom, width);
            if (mid_x - left > 1 || height > 1)
                subdivide(doc, field, seed, left, top, mid_x, bottom);
            left = mid_x;
            if (right - mid_x > 1)
                continue;
            remaining = height;
        } else {
            subdivide_point(doc, field, seed, left, mid_y, left, top, left, bottom, height);
            subdivide_point(doc, field, seed, right, mid_y, right, top, right, bottom, height);
            if (width > 1 || mid_y - top > 1)
                subdivide(doc, field, seed, left, top, right, mid_y);
            top = mid_y;
            if (width > 1)
                continue;
            remaining = bottom - mid_y;
        }
        if (remaining < 2)
            return;
    }
}

/// Draws a word in its font and colour with its top at its bottom less its
/// height.
///
/// @param[in,out] doc document holding the word and the canvas
/// @param index word index
void text_draw(Document& doc, int32_t index) {
    const int32_t font = doc.texts[index].font;
    canvas_apply_font(doc, font);
    const TextRun& run = doc.texts[index];
    const int32_t height = node_box_height(doc, NodeRef{NodeKind::text, index});
    const MarkupFont& style = doc.fonts[font];
    doc.env.text.draw(
        doc.env.text.user,
        doc.canvas.surface,
        style,
        run.text,
        static_cast<int32_t>(std::strlen(run.text)),
        run.head.x,
        run.head.y - height,
        nearest_color_index(doc.env.palette, style.colorref)
    );
}

/// Draws a cell: its TD BGCOLOR background over its box, then its flow.
///
/// @param[in,out] doc document holding the cell and the canvas
/// @param index cell index
void cell_draw(Document& doc, int32_t index) {
    if (doc.cells[index].has_background) {
        const NodeRef ref{NodeKind::cell, index};
        const int32_t height = node_box_height(doc, ref);
        const Cell& cell = doc.cells[index];
        const uint32_t rgb = cell.background_rgb;
        const uint32_t colorref = ((rgb >> 16) & 0xffu) | (rgb & 0xff00u) | ((rgb & 0xffu) << 16);
        canvas_fill_rect(
            doc.canvas,
            cell.head.x,
            cell.head.y - height,
            cell.head.x + node_width_raw(doc, ref),
            cell.head.y,
            nearest_color_index(doc.env.palette, colorref)
        );
    }
    flow_draw(doc, doc.cells[index].flow);
}

/// Draws every cell of a table.
///
/// @param[in,out] doc document holding the table and the canvas
/// @param index table index
void table_draw(Document& doc, int32_t index) {
    for (std::size_t i = 0; i < doc.tables[index].cells.size(); ++i) {
        const NodeRef cell = doc.tables[index].cells[i];
        if (cell.kind != NodeKind::none)
            node_draw(doc, cell);
    }
}

} // namespace

namespace detail {

uint8_t* canvas_pixel(Canvas& canvas, int32_t x, int32_t y) {
    const Surface& s = canvas.surface;
    if (x < 0 || y < 0 || x >= s.width || y >= s.height)
        return nullptr;
    return s.pixels + static_cast<std::ptrdiff_t>(y) * s.pitch + x;
}

void canvas_fill_rect(
    Canvas& canvas, int32_t left, int32_t top, int32_t right, int32_t bottom, uint8_t index
) {
    for (int32_t y = top; y < bottom; ++y)
        for (int32_t x = left; x < right; ++x)
            put_pixel(canvas, x, y, index);
}

void canvas_apply_font(Document& doc, int32_t font) {
    Canvas& canvas = doc.canvas;
    if (canvas.font == font)
        return;
    canvas.font = font;
    const TextExtent space = doc.env.text.stock_space(doc.env.text.user);
    canvas.metrics_dirty = true;
    canvas.space_cx = space.cx;
    canvas.space_cy = space.cy;
}

void canvas_forget_font(Canvas& canvas) {
    canvas.font = -1;
}

const TextMetrics& canvas_metrics(Document& doc) {
    Canvas& canvas = doc.canvas;
    if (canvas.metrics_dirty) {
        canvas.metrics = doc.env.text.metrics(doc.env.text.user, doc.fonts[canvas.font]);
        canvas.metrics_dirty = false;
    }
    return canvas.metrics;
}

uint8_t nearest_color_index(const Palette& palette, uint32_t colorref) {
    const int32_t r = static_cast<int32_t>(colorref & 0xffu);
    const int32_t g = static_cast<int32_t>((colorref >> 8) & 0xffu);
    const int32_t b = static_cast<int32_t>((colorref >> 16) & 0xffu);
    int32_t best = -1;
    uint8_t best_index = 0;
    for (int32_t i = 0; i < OA_PALETTE_COLORS; ++i) {
        const PaletteEntry& e = palette.entries[i];
        const int32_t dr = e.r - r;
        const int32_t dg = e.g - g;
        const int32_t db = e.b - b;
        const int32_t distance = dr * dr + dg * dg + db * db;
        if (best < 0 || distance < best) {
            best = distance;
            best_index = static_cast<uint8_t>(i);
        }
    }
    return best_index;
}

bool canvas_create(Document& doc) {
    if (doc.has_canvas && doc.canvas.surface.height < flow_height(doc.flows[k_root_flow]))
        canvas_free(doc);
    if (doc.has_canvas)
        return false;
    int32_t height = flow_height(doc.flows[k_root_flow]);
    if (height < 1)
        height = 1;
    int32_t width = flow_width(doc.flows[k_root_flow]);
    if (width < 0)
        width = 0;
    // Off-screen 8-bit canvas filled with the key colour.
    Canvas& canvas = doc.canvas;
    canvas = Canvas{};
    const int32_t pitch = aligned_pitch(width);
    canvas.pixels.assign(
        static_cast<std::size_t>(pitch) * static_cast<std::size_t>(height), k_canvas_clear_index
    );
    oa::present::init_surface(canvas.surface, width, height, pitch, canvas.pixels.data());
    canvas.font = -1;
    canvas.metrics_dirty = true;
    canvas.space_cx = 0;
    canvas.space_cy = 0;
    doc.has_canvas = true;
    return true;
}

void canvas_free(Document& doc) {
    if (!doc.has_canvas)
        return;
    doc.canvas = Canvas{};
    doc.canvas.font = -1;
    doc.has_canvas = false;
}

void node_draw(Document& doc, NodeRef node) {
    switch (node.kind) {
    case NodeKind::text:
        text_draw(doc, node.index);
        break;
    case NodeKind::image:
        image_draw(doc, node.index);
        break;
    case NodeKind::table:
        table_draw(doc, node.index);
        break;
    case NodeKind::cell:
        cell_draw(doc, node.index);
        break;
    case NodeKind::line_break:
    case NodeKind::none:
        break;
    }
}

void image_draw(Document& doc, int32_t index) {
    const Image& image = doc.images[index];
    const int32_t x = image.head.x;
    const int32_t y = image.head.y;
    const int32_t width = image.width;
    const int32_t height = image.height;
    int32_t copy_width = doc.canvas.surface.width - x;
    if (width < copy_width)
        copy_width = width;
    if (!image.pixels.empty()) {
        int32_t row_y = y;
        for (int32_t row = height - 1; row >= 0; --row, --row_y) {
            if (row >= image.source_height)
                continue;
            const uint8_t* source =
                image.pixels.data() + static_cast<std::size_t>(row) * image.row_bytes;
            for (int32_t column = 0; column < copy_width && column < image.row_bytes; ++column) {
                const uint8_t value = image.raw_indices
                                          ? source[column]
                                          : static_cast<uint8_t>(0xff - source[column]);
                put_pixel(doc.canvas, x + column, row_y, value);
            }
        }
        return;
    }
    const auto ux = static_cast<uint32_t>(x);
    const auto uy = static_cast<uint32_t>(y);
    const auto uw = static_cast<uint32_t>(width);
    const auto uh = static_cast<uint32_t>(height);
    uint32_t seed = (ux + (uy + (uw + uh * 2u) * 2u) * 2u) % k_random_modulus;
    doc.shimmer.frame = (doc.shimmer.frame + 1) & 0xffu;
    doc.shimmer.phase = static_cast<float>(static_cast<double>(doc.shimmer.frame) * k_phase_step);
    float corner[4];
    for (float& value : corner)
        value = static_cast<float>(static_cast<double>(shimmer_random(&seed)) * k_random_scale);
    const int32_t left = x;
    const int32_t top = y - height;
    const int32_t right = x + width - 1;
    const int32_t bottom = y - 1;
    const int32_t rows = height + 1;
    const int32_t columns = width + 1;
    std::vector<float> cells;
    if (rows > 0 && columns > 0)
        cells.assign(static_cast<std::size_t>(rows) * static_cast<std::size_t>(columns), 0.0f);
    for (int32_t row = 0; row < height; ++row)
        for (int32_t column = 0; column < width; ++column)
            put_pixel(doc.canvas, x + column, y - row, 0);
    put_pixel(doc.canvas, left, top, shimmer_color(doc, corner[0]));
    put_pixel(doc.canvas, right, top, shimmer_color(doc, corner[1]));
    put_pixel(doc.canvas, left, bottom, shimmer_color(doc, corner[2]));
    put_pixel(doc.canvas, right, bottom, shimmer_color(doc, corner[3]));
    if (cells.empty())
        return;
    HeightField field{cells.data(), columns, left, top};
    subdivide(doc, field, &seed, left, top, right, bottom);
}

} // namespace detail

uint32_t shimmer_random(uint32_t* seed) {
    const uint32_t low = (*seed & 0xffffu) * k_random_multiplier;
    uint32_t high = (*seed >> 16) * k_random_multiplier + (low >> 16);
    high = ((high & 0x7fffu) << 16) + (high >> 15) + (low & 0xffffu);
    *seed = high;
    if (high > k_random_modulus - 1)
        *seed = high + 0x80000001u;
    return *seed;
}

uint8_t ramp_palette_index(const Palette& palette, uint8_t r, uint8_t g, uint8_t b) {
    int32_t best = k_ramp_worst_distance;
    uint8_t best_index = 0;
    for (int32_t i = k_ramp_first; i < k_ramp_end; ++i) {
        const PaletteEntry& e = palette.entries[i];
        if (e.r == r && e.g == g && e.b == b)
            return static_cast<uint8_t>(i);
        const int32_t distance =
            (e.r - r) * (e.r - r) + (e.b - b) * (e.b - b) + (e.g - g) * (e.g - g);
        if (distance < best) {
            best = distance;
            best_index = static_cast<uint8_t>(i);
        }
    }
    return best_index;
}

Palette markup_palette(const Palette& game_palette) {
    Palette palette = game_palette;
    const PaletteEntry green{0, 0xff, 0, 0};
    for (int32_t i = k_reserved_green_first; i <= k_reserved_green_last; ++i)
        palette.entries[i] = green;
    for (int32_t i = k_reserved_high_first; i <= k_reserved_high_last; ++i)
        palette.entries[i] = green;
    palette.entries[k_canvas_clear_index] = PaletteEntry{0xff, 0, 0xff, 0};
    for (PaletteEntry& entry : palette.entries)
        entry.flags = 0;
    return palette;
}

/// Draws every node of a run.
///
/// @param[in,out] doc document holding the run and the canvas
/// @param node first node of the run
void run_draw(Document& doc, NodeRef node) {
    for (; node.kind != NodeKind::none; node = header(doc, node).next)
        node_draw(doc, node);
}

void flow_draw(Document& doc, int32_t flow) {
    if (!doc.flows[flow].laid_out)
        flow_layout_padded(doc, flow);
    for (std::size_t i = 0; i < doc.flows[flow].items.size(); ++i)
        run_draw(doc, doc.flows[flow].items[i]);
}

bool document_paint(Document& doc) {
    if (flow_height(doc.flows[k_root_flow]) < 1) {
        canvas_create(doc);
        flow_layout_padded(doc, k_root_flow);
    }
    canvas_create(doc);
    flow_draw(doc, k_root_flow);
    return true;
}

const Surface* document_surface(const Document& doc) {
    return doc.has_canvas ? &doc.canvas.surface : nullptr;
}

} // namespace oa::boneyards::html
