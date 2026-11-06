// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Node pools, per-class measurement, line layout, tables and hit testing.

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "internal.hpp"

namespace oa::boneyards::html {

using namespace detail;

namespace {

// Gap added above every line's ascent, and to a cell's measured height.
constexpr int32_t k_line_gap = 2;
constexpr int32_t k_cell_extra_height = 4;
constexpr uint8_t k_default_charset = 1;
// Extent reported by a node class that keeps none.
constexpr int32_t k_unmeasured = -1;

/// Returns the extent the base node class reports for a size it keeps none
/// of: breaks keep no width, tables and cells no raw height.
///
/// @return -1
int32_t node_unmeasured() {
    return k_unmeasured;
}

/// Measures a word in its font on first use, caching its extent and the
/// font's ascent.
///
/// @param[in,out] doc document holding the word and the canvas
/// @param index word index
/// @return the word's width in pixels
int32_t text_measure(Document& doc, int32_t index) {
    if (doc.texts[index].cx == -1) {
        const int32_t font = doc.texts[index].font;
        canvas_apply_font(doc, font);
        TextRun& run = doc.texts[index];
        const TextExtent extent = doc.env.text.extent(
            doc.env.text.user,
            doc.fonts[font],
            run.text,
            static_cast<int32_t>(std::strlen(run.text))
        );
        run.cx = extent.cx;
        run.cy = extent.cy;
        run.ascent = canvas_metrics(doc).ascent;
    }
    return doc.texts[index].cx;
}

/// Returns a word's height, measuring it on first use.
///
/// @param[in,out] doc document holding the word
/// @param index word index
/// @return the height in pixels
int32_t text_height(Document& doc, int32_t index) {
    if (doc.texts[index].cy == -1) {
        doc.texts[index].cx = -1;
        text_measure(doc, index);
    }
    return doc.texts[index].cy;
}

/// Returns the ascent of a word's font, measuring the word on first use.
///
/// @param[in,out] doc document holding the word
/// @param index word index
/// @return the ascent in pixels
int32_t text_ascent(Document& doc, int32_t index) {
    if (doc.texts[index].ascent == -1) {
        doc.texts[index].cx = -1;
        text_measure(doc, index);
    }
    return doc.texts[index].ascent;
}

/// Returns a table's width: its column widths plus the spacing between
/// columns.
///
/// @param table table to measure
/// @return the width in pixels
int32_t table_width(const Table& table) {
    int32_t width = (table.columns - 1) * table.column_spacing;
    for (const int32_t column : table.column_widths)
        width += column;
    return width;
}

/// Returns the cell at a grid position, reading the parse-order cell list
/// as rows x columns.
///
/// @param table table to read
/// @param row row index
/// @param column column index
/// @return the cell, or no node outside the grid
NodeRef table_cell_at(const Table& table, int32_t row, int32_t column) {
    if (column < table.columns && row < table.rows) {
        const auto index = static_cast<std::size_t>(table.columns * row + column);
        if (index < table.cells.size())
            return table.cells[index];
    }
    return NodeRef{};
}

/// Returns a column's width.
///
/// The widths come from the first row; a column past them, in a later and
/// wider row, is 0 wide.
///
/// @param table table to read
/// @param column column index
/// @return the width in pixels
int32_t column_width(const Table& table, int32_t column) {
    return column < static_cast<int32_t>(table.column_widths.size()) ? table.column_widths[column]
                                                                     : 0;
}

/// Returns a table's height: every row whose height is still 0 is measured
/// as its tallest cell (each cell first taking its column's width), then the
/// rows and the spacing between them are summed.
///
/// @param[in,out] doc document holding the table; row heights are stored
/// @param index table index
/// @return the height in pixels
int32_t table_height(Document& doc, int32_t index) {
    int32_t total = (doc.tables[index].rows - 1) * doc.tables[index].row_spacing;
    for (int32_t row = 0; row < doc.tables[index].rows; ++row) {
        if (doc.tables[index].row_heights[row] == 0) {
            int32_t tallest = 0;
            for (int32_t column = 0; column < doc.tables[index].columns; ++column) {
                const NodeRef cell = table_cell_at(doc.tables[index], row, column);
                if (cell.kind != NodeKind::cell)
                    continue;
                cell_set_width(doc, cell.index, column_width(doc.tables[index], column));
                const int32_t height = node_box_height(doc, cell);
                if (tallest < height) {
                    tallest = height;
                    doc.tables[index].row_heights[row] = tallest;
                }
            }
            doc.tables[index].row_heights[row] = tallest;
        }
        total += doc.tables[index].row_heights[row];
    }
    return total;
}

/// Lays out a cell's flow inside the cell's box.
///
/// @param[in,out] doc document holding the cell
/// @param index cell index
void cell_layout(Document& doc, int32_t index) {
    const NodeRef ref{NodeKind::cell, index};
    const int32_t height = node_box_height(doc, ref);
    const int32_t x = doc.cells[index].head.x;
    const int32_t y = doc.cells[index].head.y;
    const int32_t rect[4] = {x, y - height, x + node_width_raw(doc, ref), y};
    flow_layout(doc, doc.cells[index].flow, rect);
}

/// Places a table's cells, bottom row first, from the table's bottom-left
/// corner, then lays each cell out; the table's own position fields are left
/// untouched.
///
/// Each placed cell takes its column's width and its row's height when those
/// are non-zero.
///
/// @param[in,out] doc document holding the table
/// @param index table index
/// @param x left edge in canvas pixels
/// @param y bottom edge in canvas pixels
void table_place(Document& doc, int32_t index, int32_t x, int32_t y) {
    int32_t row_bottom = y;
    for (int32_t row = doc.tables[index].rows - 1; row >= 0; --row) {
        int32_t column_x = x;
        for (int32_t column = 0; column < doc.tables[index].columns; ++column) {
            const Table& table = doc.tables[index];
            const NodeRef cell = table_cell_at(table, row, column);
            const int32_t width = column_width(table, column);
            const int32_t height = table.row_heights[row];
            if (cell.kind == NodeKind::cell) {
                node_set_position(doc, cell, column_x, row_bottom);
                if (width != 0)
                    cell_set_width(doc, cell.index, width);
                if (height != 0)
                    cell_set_height(doc, cell.index, height);
                cell_layout(doc, cell.index);
            }
            column_x += width + doc.tables[index].column_spacing;
        }
        row_bottom -= doc.tables[index].row_heights[row] + doc.tables[index].row_spacing;
    }
}

/// Returns a cell's height: its fixed height, else its flow's laid-out
/// height plus 4 (laying the flow out at the cell's width when needed).
///
/// @param[in,out] doc document holding the cell
/// @param index cell index
/// @return the height in pixels
int32_t cell_height(Document& doc, int32_t index) {
    if (doc.cells[index].height != -1)
        return doc.cells[index].height;
    const int32_t flow = doc.cells[index].flow;
    if (flow_height(doc.flows[flow]) < 1) {
        const int32_t rect[4] = {0, 0, node_width_raw(doc, NodeRef{NodeKind::cell, index}), -1};
        flow_layout(doc, flow, rect);
    }
    return flow_height(doc.flows[flow]) + k_cell_extra_height;
}

/// Returns a cell's width: its fixed width, else its flow's wrap width.
///
/// @param doc document holding the cell
/// @param index cell index
/// @return the width in pixels
int32_t cell_width(const Document& doc, int32_t index) {
    const int32_t width = doc.cells[index].width;
    return width != -1 ? width : flow_width(doc.flows[doc.cells[index].flow]);
}

/// Tests a point against a node's box, x..x + width by y - height..y with
/// every edge inclusive.
///
/// @param[in,out] doc document holding the node
/// @param node node to test
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return true when the point is inside
bool box_hit(Document& doc, NodeRef node, int32_t x, int32_t y) {
    const NodeHeader& head = header(doc, node);
    const int32_t left = head.x;
    const int32_t bottom = head.y;
    return left <= x && x <= left + node_width_raw(doc, node) && y <= bottom &&
           bottom - node_height_raw(doc, node) <= y;
}

/// Tests whether a point hits a link in a cell's flow; the link found is
/// handed to the cell's table, so the enclosing run walk reports it.
///
/// @param[in,out] doc document holding the cell
/// @param index cell index
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return true when a link was hit
bool cell_hit(Document& doc, int32_t index, int32_t x, int32_t y) {
    const char* link = flow_find_link(doc, doc.cells[index].flow, x, y);
    if (link == nullptr)
        return false;
    doc.tables[doc.cells[index].table].head.href = link;
    return true;
}

/// Tests whether a point hits any cell of a table.
///
/// @param[in,out] doc document holding the table
/// @param index table index
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return true when a cell was hit
bool table_hit(Document& doc, int32_t index, int32_t x, int32_t y) {
    for (std::size_t i = 0; i < doc.tables[index].cells.size(); ++i) {
        const NodeRef cell = doc.tables[index].cells[i];
        if (cell.kind != NodeKind::none && node_hit(doc, cell, x, y))
            return true;
    }
    return false;
}

/// Sums the widths of a run's nodes.
///
/// @param[in,out] doc document holding the run
/// @param node first node of the run
/// @return the width in pixels; -1 for a break
int32_t run_width(Document& doc, NodeRef node) {
    int32_t total = 0;
    for (; node.kind != NodeKind::none; node = header(doc, node).next)
        total += node_measure(doc, node);
    return total;
}

/// Returns the tallest box height among a run's nodes.
///
/// @param[in,out] doc document holding the run
/// @param node first node of the run
/// @return the height in pixels, at least 0
int32_t run_height(Document& doc, NodeRef node) {
    int32_t tallest = 0;
    for (; node.kind != NodeKind::none; node = header(doc, node).next) {
        const int32_t height = node_box_height(doc, node);
        if (tallest < height)
            tallest = height;
    }
    return tallest;
}

/// Returns the largest ascent among a run's nodes.
///
/// @param[in,out] doc document holding the run
/// @param node first node of the run
/// @return the ascent in pixels, at least 0
int32_t run_ascent(Document& doc, NodeRef node) {
    int32_t tallest = 0;
    for (; node.kind != NodeKind::none; node = header(doc, node).next) {
        const int32_t ascent = node_ascent(doc, node);
        if (tallest < ascent)
            tallest = ascent;
    }
    return tallest;
}

/// Places a run's nodes left to right on a shared baseline, each node's
/// bottom at the baseline less its ascent plus its height.
///
/// @param[in,out] doc document holding the run
/// @param node first node of the run
/// @param x left edge of the first node in canvas pixels
/// @param baseline baseline in canvas pixels
void run_place(Document& doc, NodeRef node, int32_t x, int32_t baseline) {
    for (; node.kind != NodeKind::none; node = header(doc, node).next) {
        const int32_t ascent = node_ascent(doc, node);
        const int32_t height = node_box_height(doc, node);
        node_set_position(doc, node, x, baseline - ascent + height);
        x += node_measure(doc, node);
    }
}

/// Finds the first non-empty link of a node in a run that the point hits.
///
/// @param[in,out] doc document holding the run
/// @param node first node of the run
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return the link, or null
const char* run_find_link(Document& doc, NodeRef node, int32_t x, int32_t y) {
    for (; node.kind != NodeKind::none; node = header(doc, node).next) {
        if (!node_hit(doc, node, x, y))
            continue;
        const char* href = header(doc, node).href;
        if (href != nullptr && href[0] != '\0')
            return href;
    }
    return nullptr;
}

} // namespace

namespace detail {

NodeHeader fresh_header() {
    NodeHeader head{};
    head.next = NodeRef{};
    head.href = nullptr;
    head.x = -1;
    head.y = -1;
    return head;
}

NodeHeader& header(Document& doc, NodeRef node) {
    switch (node.kind) {
    case NodeKind::text:
        return doc.texts[node.index].head;
    case NodeKind::image:
        return doc.images[node.index].head;
    case NodeKind::table:
        return doc.tables[node.index].head;
    case NodeKind::cell:
        return doc.cells[node.index].head;
    case NodeKind::line_break:
    case NodeKind::none:
        break;
    }
    return doc.flows[node.index].line_break;
}

int32_t new_text(Document& doc) {
    TextRun run{};
    run.head = fresh_header();
    run.cx = -1;
    run.cy = -1;
    run.ascent = -1;
    run.font = -1;
    doc.texts.push_back(run);
    return static_cast<int32_t>(doc.texts.size() - 1);
}

int32_t new_image(Document& doc) {
    doc.images.emplace_back();
    Image& image = doc.images.back();
    image.head = fresh_header();
    image.name[0] = '\0';
    image.source_width = 0;
    image.source_height = 0;
    image.row_bytes = 0;
    image.width = -1;
    image.height = -1;
    image.raw_indices = false;
    image.loaded = false;
    return static_cast<int32_t>(doc.images.size() - 1);
}

int32_t new_table(Document& doc) {
    doc.tables.emplace_back();
    doc.tables.back().head = fresh_header();
    table_reset(doc.tables.back(), nullptr);
    return static_cast<int32_t>(doc.tables.size() - 1);
}

int32_t new_cell(Document& doc) {
    Cell cell{};
    cell.head = fresh_header();
    cell.table = -1;
    cell.flow = -1;
    cell.height = -1;
    cell.width = -1;
    cell.has_background = false;
    cell.background_rgb = 0;
    doc.cells.push_back(cell);
    return static_cast<int32_t>(doc.cells.size() - 1);
}

int32_t new_flow(Document& doc) {
    doc.flows.emplace_back();
    flow_init_fields(doc.flows.back());
    return static_cast<int32_t>(doc.flows.size() - 1);
}

int32_t new_default_font(Document& doc) {
    MarkupFont font{};
    font.height = k_default_font_height;
    font.weight = k_weight_normal;
    font.italic = false;
    font.underline = false;
    font.charset = k_default_charset;
    std::snprintf(font.face, sizeof font.face, "%s", k_default_face);
    font.face[k_face_bytes - 1] = '\0';
    font.colorref = k_default_text_colorref;
    font.dirty = true;
    doc.fonts.push_back(font);
    return static_cast<int32_t>(doc.fonts.size() - 1);
}

int32_t clone_font(Document& doc, int32_t font) {
    const MarkupFont copy = doc.fonts[font];
    doc.fonts.push_back(copy);
    return static_cast<int32_t>(doc.fonts.size() - 1);
}

void table_reset(Table& table, const char* href) {
    table.columns = 0;
    table.head.href = href;
    table.column_widths.clear();
    table.rows = 0;
    table.row_heights.clear();
    table.column = 0;
    table.cells.clear();
    table.reserved_after_column = 0;
    table.column_spacing = 1;
    table.row_spacing = 1;
}

void table_push_row(Table& table) {
    table.column = 0;
    ++table.rows;
    table.row_heights.push_back(0);
}

/// Advances a table's column cursor, widening the table as needed.
///
/// @param[in,out] table table to change
void table_next_column(Table& table) {
    ++table.column;
    if (table.columns < table.column)
        table.columns = table.column;
}

void table_add_cell_slot(Table& table, int32_t cell, int32_t width) {
    table_next_column(table);
    table.cells.push_back(NodeRef{NodeKind::cell, cell});
    if (table.rows == 1)
        table.column_widths.push_back(width);
}

void table_fill_auto_columns(Document& doc, int32_t index, int32_t width) {
    Table& table = doc.tables[index];
    const auto grid = static_cast<uint32_t>(table.columns * table.rows);
    while (static_cast<uint32_t>(table.cells.size()) < grid)
        table.cells.push_back(NodeRef{});
    int32_t automatic = 0;
    int32_t used = (table.columns - 1) * table.column_spacing;
    for (const int32_t column : table.column_widths) {
        if (column == 0)
            ++automatic;
        else
            used += column;
    }
    if (automatic != 0 && width - used > 0) {
        const int32_t share = (width - used) / automatic;
        for (int32_t& column : table.column_widths)
            if (column == 0)
                column = share;
    }
}

void cell_set_height(Document& doc, int32_t cell, int32_t height) {
    doc.cells[cell].height = height;
}

void cell_set_width(Document& doc, int32_t cell, int32_t width) {
    doc.cells[cell].width = width;
    Flow& flow = doc.flows[doc.cells[cell].flow];
    flow.width = width;
    flow.height = -1;
}

int32_t node_measure(Document& doc, NodeRef node) {
    if (node.kind == NodeKind::text)
        return text_measure(doc, node.index);
    return node_width_raw(doc, node);
}

int32_t node_width_raw(Document& doc, NodeRef node) {
    switch (node.kind) {
    case NodeKind::text:
        return doc.texts[node.index].cx;
    case NodeKind::image:
        return doc.images[node.index].width;
    case NodeKind::table:
        return table_width(doc.tables[node.index]);
    case NodeKind::cell:
        return cell_width(doc, node.index);
    case NodeKind::line_break:
    case NodeKind::none:
        break;
    }
    return node_unmeasured();
}

int32_t node_box_height(Document& doc, NodeRef node) {
    switch (node.kind) {
    case NodeKind::text:
        return text_height(doc, node.index);
    case NodeKind::table:
        return table_height(doc, node.index);
    case NodeKind::cell:
        return cell_height(doc, node.index);
    case NodeKind::image:
    case NodeKind::line_break:
    case NodeKind::none:
        break;
    }
    return node_height_raw(doc, node);
}

int32_t node_height_raw(Document& doc, NodeRef node) {
    switch (node.kind) {
    case NodeKind::text:
        return doc.texts[node.index].cy;
    case NodeKind::image:
        return doc.images[node.index].height;
    case NodeKind::line_break:
        return 0;
    case NodeKind::table:
    case NodeKind::cell:
    case NodeKind::none:
        break;
    }
    return node_unmeasured();
}

int32_t node_ascent(Document& doc, NodeRef node) {
    if (node.kind == NodeKind::text)
        return text_ascent(doc, node.index);
    return node_box_height(doc, node);
}

void node_set_position(Document& doc, NodeRef node, int32_t x, int32_t y) {
    if (node.kind == NodeKind::table) {
        table_place(doc, node.index, x, y);
        return;
    }
    NodeHeader& head = header(doc, node);
    head.x = x;
    head.y = y;
}

bool node_hit(Document& doc, NodeRef node, int32_t x, int32_t y) {
    switch (node.kind) {
    case NodeKind::table:
        return table_hit(doc, node.index, x, y);
    case NodeKind::cell:
        return cell_hit(doc, node.index, x, y);
    case NodeKind::none:
        return false;
    case NodeKind::text:
    case NodeKind::image:
    case NodeKind::line_break:
        break;
    }
    return box_hit(doc, node, x, y);
}

const char* flow_find_link(Document& doc, int32_t flow, int32_t x, int32_t y) {
    for (std::size_t i = 0; i < doc.flows[flow].items.size(); ++i) {
        const char* link = run_find_link(doc, doc.flows[flow].items[i], x, y);
        if (link != nullptr)
            return link;
    }
    return nullptr;
}

} // namespace detail

int32_t node_width(Document& doc, NodeRef node) {
    return node_measure(doc, node);
}

int32_t node_height(Document& doc, NodeRef node) {
    return node_box_height(doc, node);
}

bool flow_layout(Document& doc, int32_t flow, const int32_t rect[4]) {
    const int32_t left = rect[0];
    const int32_t top = rect[1];
    const int32_t right = rect[2];
    const int32_t bottom = rect[3];
    canvas_forget_font(doc.canvas);
    int32_t y = top;
    std::size_t start = 0;
    while (start != doc.flows[flow].items.size()) {
        int32_t line_width = 0;
        int32_t space = 0;
        int32_t line_height = 0;
        int32_t line_ascent = 0;
        bool first = true;
        std::size_t end = start;
        for (; end != doc.flows[flow].items.size(); ++end) {
            const NodeRef item = doc.flows[flow].items[end];
            const int32_t width = run_width(doc, item);
            if (width == -1) {
                ++end; // a break closes the line and belongs to it
                break;
            }
            if (right - left < width + line_width) {
                if (start != end)
                    break;
                // An overlong first item is placed alone.
                line_height = std::max(line_height, run_height(doc, item));
                line_ascent = std::max(line_ascent, run_ascent(doc, item));
                line_width += width;
                ++end;
                break;
            }
            line_height = std::max(line_height, run_height(doc, item));
            line_ascent = std::max(line_ascent, run_ascent(doc, item));
            if (first) {
                first = false;
                space = doc.canvas.space_cx;
            }
            line_width += width + space;
        }
        if (top - bottom != -1 && bottom > 0 && bottom < line_height + y) {
            // Out of room: the rest is parked off-canvas.
            for (std::size_t i = start; i != doc.flows[flow].items.size(); ++i)
                run_place(doc, doc.flows[flow].items[i], -1, -1);
            doc.flows[flow].laid_out = true;
            return false;
        }
        y += k_line_gap + line_ascent;
        int32_t x = left;
        const uint32_t flags = doc.flows[flow].flags;
        if ((flags & k_flow_align_right) != 0) {
            x = right - line_width;
            if (x < 0)
                x = 0;
        }
        if ((flags & k_flow_align_center) != 0) {
            x = (right - line_width + left) / 2;
            if (x < 0)
                x = 0;
        }
        for (; start != end; ++start) {
            const NodeRef item = doc.flows[flow].items[start];
            run_place(doc, item, x, y);
            x += run_width(doc, item) + space;
        }
    }
    doc.flows[flow].height = y - top;
    doc.flows[flow].laid_out = true;
    return true;
}

bool flow_layout_padded(Document& doc, int32_t flow) {
    const int32_t rect[4] = {
        flow_pad_x(doc.flows[flow]),
        flow_pad_y(doc.flows[flow]),
        doc.canvas.surface.width - flow_pad_x(doc.flows[flow]),
        doc.canvas.surface.height - flow_pad_y(doc.flows[flow])
    };
    const bool ok = flow_layout(doc, flow, rect);
    doc.flows[flow].height += flow_pad_y(doc.flows[flow]) * 2;
    return ok;
}

int32_t document_height(Document& doc) {
    if (flow_height(doc.flows[k_root_flow]) < 1) {
        canvas_create(doc);
        flow_layout_padded(doc, k_root_flow);
    }
    return flow_height(doc.flows[k_root_flow]);
}

const char* document_link_at(Document& doc, int32_t x, int32_t y) {
    return flow_find_link(doc, k_root_flow, x, y);
}

} // namespace oa::boneyards::html
