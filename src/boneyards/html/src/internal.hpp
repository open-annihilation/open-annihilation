// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Shared node dispatch and pool helpers of the markup renderer. Each node
// operation below switches on NodeKind to the behaviour of the node's class.
#pragma once

#include "oa/boneyards/html.hpp"

namespace oa::boneyards::html::detail {

inline constexpr int32_t k_root_flow = 0;

/// Returns the header every node shares.
///
/// @param[in,out] doc document holding the node
/// @param node node to look up; a line break names its flow in `index` and
///     yields that flow's shared break header
/// @return the node's header, valid until its pool grows
NodeHeader& header(Document& doc, NodeRef node);
/// Returns base node fields: no chain, no link, position (-1, -1).
///
/// @return the fresh header
NodeHeader fresh_header();

// Pool constructors; the returned index stays valid while the pools grow.

/// Appends an unmeasured word with no text and no font.
///
/// @param[in,out] doc document whose word pool grows
/// @return the new word's index
int32_t new_text(Document& doc);
/// Appends an image with no name, no pixels and an unknown size (-1).
///
/// @param[in,out] doc document whose image pool grows
/// @return the new image's index
int32_t new_image(Document& doc);
/// Appends an empty table (see table_reset) with no link.
///
/// @param[in,out] doc document whose table pool grows
/// @return the new table's index
int32_t new_table(Document& doc);
/// Appends a cell with no table, no flow, a measured size (-1) and no
/// background.
///
/// @param[in,out] doc document whose cell pool grows
/// @return the new cell's index
int32_t new_cell(Document& doc);
/// Appends a nested flow set up by flow_init_fields.
///
/// @param[in,out] doc document whose flow pool grows
/// @return the new flow's index
int32_t new_flow(Document& doc);
/// Appends the default markup font: ARIAL with 12-pixel characters (height
/// -12), normal weight, charset 1 and light grey 0x00E6E6E6, marked dirty.
///
/// @param[in,out] doc document whose font pool grows
/// @return the new font's index
int32_t new_default_font(Document& doc);
/// Appends a copy of a font.
///
/// @param[in,out] doc document whose font pool grows
/// @param font index of the font to copy
/// @return the copy's index
int32_t clone_font(Document& doc, int32_t font);

/// Resets a flow record: no width or height (-1), an empty link, no table
/// under construction and a fresh shared break header.
///
/// @param[out] flow flow to reset
void flow_init_fields(Flow& flow);
/// Returns a flow's wrap width.
///
/// @param flow flow to read
/// @return the width in pixels, -1 until set
int32_t flow_width(const Flow& flow);
/// Returns the height of a flow's last layout.
///
/// @param flow flow to read
/// @return the height in pixels, -1 until laid out
int32_t flow_height(const Flow& flow);
/// Returns the left and right inset of a padded layout (BODY LEFTMARGIN).
///
/// @param flow flow to read
/// @return the inset in pixels
int32_t flow_pad_x(const Flow& flow);
/// Returns the top and bottom inset of a padded layout (BODY TOPMARGIN).
///
/// @param flow flow to read
/// @return the inset in pixels
int32_t flow_pad_y(const Flow& flow);
/// Stores BODY LEFTMARGIN.
///
/// @param[out] flow flow to change
/// @param pad left and right inset in pixels
void flow_set_pad_x(Flow& flow, int32_t pad);
/// Stores BODY TOPMARGIN.
///
/// @param[out] flow flow to change
/// @param pad top and bottom inset in pixels
void flow_set_pad_y(Flow& flow, int32_t pad);
/// Returns the innermost font of a flow's font stack.
///
/// @param flow flow to read; its font stack must not be empty
/// @return the font's index in Document::fonts
int32_t flow_current_font(const Flow& flow);
/// Clears a flow's per-parse fields; its tag stack, items and layout mark are
/// kept.
///
/// The width and height become unknown (-1), the pending word break and the
/// margins are cleared and the link becomes the document flow's current
/// one. With k_flow_inherit_font the document flow's current font is pushed.
///
/// @param[in,out] doc document holding the flow
/// @param flow flow index
/// @param flags k_flow_* bits the flow takes
void flow_reset(Document& doc, int32_t flow, uint32_t flags);
/// Empties a flow's items and font stack and pushes a fresh default font.
///
/// @param[in,out] doc document holding the flow; its font pool grows
/// @param flow flow index
void flow_reset_fonts(Document& doc, int32_t flow);

/// Returns a node's width, measuring a word on first use.
///
/// @param[in,out] doc document holding the node
/// @param node node to measure
/// @return the width in pixels; -1 for a break
int32_t node_measure(Document& doc, NodeRef node);
/// Returns a node's width as it stands: a word's cached extent,
/// an image's displayed width, a table's or cell's width.
///
/// @param[in,out] doc document holding the node
/// @param node node to read
/// @return the width in pixels; -1 for a break or an unmeasured word
int32_t node_width_raw(Document& doc, NodeRef node);
/// Returns a node's box height.
///
/// Words, tables and cells measure their own; images and breaks, which keep
/// the base behaviour, report their raw height.
///
/// @param[in,out] doc document holding the node
/// @param node node to measure
/// @return the height in pixels
int32_t node_box_height(Document& doc, NodeRef node);
/// Returns a node's height as it stands: a word's cached extent,
/// an image's displayed height, 0 for a break.
///
/// @param[in,out] doc document holding the node
/// @param node node to read
/// @return the height in pixels; -1 for tables and cells, which keep none
int32_t node_height_raw(Document& doc, NodeRef node);
/// Returns a node's ascent.
///
/// Words measure their own; every other node class keeps the base
/// behaviour, which reports the box height.
///
/// @param[in,out] doc document holding the node
/// @param node node to measure
/// @return the ascent in pixels
int32_t node_ascent(Document& doc, NodeRef node);
/// Stores a node's left edge and bottom; a table places its
/// cells instead and leaves its own fields untouched.
///
/// @param[in,out] doc document holding the node
/// @param node node to place
/// @param x left edge in canvas pixels
/// @param y bottom edge in canvas pixels
void node_set_position(Document& doc, NodeRef node, int32_t x, int32_t y);
/// Tests whether a point hits a node.
///
/// Words, images and breaks test their box (edges inclusive); a cell is hit
/// when a link in its flow is, which it hands to its table; a table is hit
/// when any of its cells is.
///
/// @param[in,out] doc document holding the node
/// @param node node to test
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return true when hit; false for no node
bool node_hit(Document& doc, NodeRef node, int32_t x, int32_t y);
/// Draws a node into the canvas: a word, an image, a table's
/// cells or a cell's background and flow; breaks draw nothing.
///
/// @param[in,out] doc document holding the node and the canvas
/// @param node node to draw
void node_draw(Document& doc, NodeRef node);

/// Fixes a cell's width and its flow's wrap width; the flow's height becomes
/// unknown.
///
/// @param[in,out] doc document holding the cell
/// @param cell cell index
/// @param width width in pixels
void cell_set_width(Document& doc, int32_t cell, int32_t width);
/// Fixes a cell's height (a TR HEIGHT row).
///
/// @param[in,out] doc document holding the cell
/// @param cell cell index
/// @param height height in pixels; -1 measures the cell's flow
void cell_set_height(Document& doc, int32_t cell, int32_t height);
/// Clears a table's grid and sets its link; both spacings return to 1.
///
/// @param[out] table table to reset
/// @param href link target the table carries
void table_reset(Table& table, const char* href);
/// Starts a new row with a measured height (0) and the column cursor at 0.
///
/// @param[in,out] table table to extend
void table_push_row(Table& table);
/// Finishes a table's grid for a flow width.
///
/// Pads the cell list with empty slots to rows x columns, then splits the
/// width left over by the sized columns and the spacing evenly among the
/// zero-width columns, when any width is left.
///
/// @param[in,out] doc document holding the table
/// @param table table index
/// @param width width available, in pixels
void table_fill_auto_columns(Document& doc, int32_t table, int32_t width);
/// Adds a cell at the column cursor, widening the table as needed; in the
/// first row the cell's width becomes its column's width.
///
/// @param[in,out] table table to extend
/// @param cell cell index
/// @param width TD WIDTH in pixels, 0 for an automatic column
void table_add_cell_slot(Table& table, int32_t cell, int32_t width);

/// Makes a font the canvas's current font and takes the space width lines
/// use.
///
/// The space is measured before the font is selected, so it is always the
/// device context's stock font space. The metrics are refreshed on the next
/// canvas_metrics.
///
/// @param[in,out] doc document holding the canvas and fonts
/// @param font font index; the current font changes nothing
void canvas_apply_font(Document& doc, int32_t font);
/// Forgets the canvas's current font, so the next canvas_apply_font takes
/// effect.
///
/// @param[out] canvas canvas to change
void canvas_forget_font(Canvas& canvas);
/// Returns the current font's metrics, refreshing them after a font change.
///
/// @param[in,out] doc document holding the canvas; a current font must be set
/// @return the cached height and ascent
const TextMetrics& canvas_metrics(Document& doc);
/// Maps a 0x00BBGGRR colour to the nearest palette entry, as Windows does for
/// a bitmap's colour table; the first of equally near entries wins.
///
/// @param palette palette to search
/// @param colorref colour as 0x00BBGGRR
/// @return the palette index
uint8_t nearest_color_index(const Palette& palette, uint32_t colorref);
/// Fills a rectangle of the canvas, clipped to it.
///
/// @param[in,out] canvas canvas to draw into
/// @param left first column
/// @param top first row
/// @param right column after the last one
/// @param bottom row after the last one
/// @param index palette index to fill with
void canvas_fill_rect(
    Canvas& canvas, int32_t left, int32_t top, int32_t right, int32_t bottom, uint8_t index
);
/// Returns a canvas pixel.
///
/// @param canvas canvas to address
/// @param x column
/// @param y row
/// @return the pixel, or null outside the canvas
uint8_t* canvas_pixel(Canvas& canvas, int32_t x, int32_t y);
/// Creates the canvas at the document size, replacing one shorter than the
/// laid-out height.
///
/// The canvas is the document width (at least 0) by its height (at least 1),
/// rows padded to 4 bytes and cleared to k_canvas_clear_index, with no
/// current font.
///
/// @param[in,out] doc document whose canvas is created
/// @return true when a canvas was created, false when the existing one is
///     kept
bool canvas_create(Document& doc);
/// Releases the canvas, if any.
///
/// @param[in,out] doc document whose canvas is released
void canvas_free(Document& doc);

/// Draws an image whose bottom row lies on its position's y.
///
/// A decoded image copies its rows bottom-up as palette indices, inverted
/// (255 - value) unless `raw_indices` is set; each row's bytes are copied as
/// indices whatever the PNG colour type. An image without pixels draws the
/// shimmer placeholder instead: a box of index 0 with a midpoint-displacement
/// pattern on the blue-grey ramp, seeded from the image's position and size
/// and animated by the shared shimmer phase. Writes are clipped to the
/// canvas and reads to the row.
///
/// @param[in,out] doc document holding the image and the canvas
/// @param image image index
void image_draw(Document& doc, int32_t image);

/// Parses markup in place into a flow until the end of the text or the close
/// tag of an element open in this flow.
///
/// Whitespace ends a word, '<' starts a tag and recognised tags run their
/// handlers. A close tag of the innermost open element is consumed; one of an
/// outer element is left for that element's handler. A word's entities are
/// decoded once the next word starts or the parse ends.
///
/// @param[in,out] doc document receiving the nodes
/// @param flow flow index
/// @param[in,out] cursor parse position in the document buffer; left after
///     the text parsed
/// @return false when a handler failed (a TR or TD outside a table), which
///     stops the parse
bool parse_content(Document& doc, int32_t flow, char** cursor);

/// Finds the first link under a point among a flow's items.
///
/// @param[in,out] doc document holding the flow
/// @param flow flow index
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return the HREF of the first linked node hit (inside cells too), or null
const char* flow_find_link(Document& doc, int32_t flow, int32_t x, int32_t y);

} // namespace oa::boneyards::html::detail
