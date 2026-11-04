// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards markup renderer.
//
// The online client shows help pages, profile popups and news as a small
// HTML dialect: HTML, BODY, FONT, CENTER, B, I, U, IMG, BR, P, H1-H3, A,
// TABLE, TR and TD, the &reg; &copy; &gt; &lt; &amp; &quot; &nbsp; entities
// and numeric character references (the module's README lists what each
// reads). Spaces, tabs, line feeds, form feeds and carriage returns separate
// words. A document parses its text in place into runs (a word or a chain of
// words joined without whitespace), images, tables and table cells; each cell owns
// a nested flow. Layout wraps runs into baseline-aligned lines inside a
// rectangle; painting draws into an 8-bit off-screen canvas that the owning
// widget blits with index 9 as the transparent key.
//
// 3.1c draws text with Windows TrueType fonts into an 8-bit bitmap and
// decodes PNG images for IMG sources. Here text measurement and
// rasterisation go through `TextBackend`, image files through `FileReader`,
// PNG decoding through the native codec (oa/formats/png.hpp) and its message boxes
// through `Notifier`; everything else behaves as the game does.
#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

#include "oa/present/surface.h"

namespace oa::boneyards::html {

// Canvas fill index; the palette maps it to magenta for colour keying.
inline constexpr uint8_t k_canvas_clear_index = 9;
// Face, height and weights of the default markup font.
inline constexpr const char* k_default_face = "ARIAL";
inline constexpr int32_t k_default_font_height = -12;
inline constexpr int32_t k_weight_normal = 400;
inline constexpr int32_t k_weight_bold = 800;
inline constexpr uint32_t k_default_text_colorref = 0x00e6e6e6;
// IMG WIDTH/HEIGHT clamp and the largest PNG side that is decoded.
inline constexpr int32_t k_image_max_side = 256;
inline constexpr int32_t k_image_name_bytes = 0x104;
inline constexpr int32_t k_face_bytes = 0x20;
// Bounds the markup copy; 3.1c has no limit.
inline constexpr std::size_t k_max_markup_bytes = 1u << 20;

// Flow.flags bits.
inline constexpr uint32_t k_flow_in_body = 0x10;
inline constexpr uint32_t k_flow_align_center = 0x20;
inline constexpr uint32_t k_flow_align_right = 0x40;
inline constexpr uint32_t k_flow_valign_center = 0x80;  // stored, never read
inline constexpr uint32_t k_flow_valign_bottom = 0x100; // stored, never read
inline constexpr uint32_t k_flow_inherit_font = 0x1000;

// Markup font: a dirty flag, the font's height, weight, style, character
// set and face, and its colour (0x00BBGGRR).
struct MarkupFont {
    bool dirty;
    int32_t height; // lfHeight; negative is the character height
    int32_t weight;
    bool italic;
    bool underline;
    uint8_t charset;
    char face[k_face_bytes];
    uint32_t colorref;
};

// Text measurement and drawing boundary.
struct TextExtent {
    int32_t cx;
    int32_t cy;
};

struct TextMetrics {
    int32_t height;
    int32_t ascent;
};

struct TextBackend {
    void* user;
    // GetTextExtentPoint32 of `length` bytes of `text`.
    TextExtent (*extent)(void* user, const MarkupFont& font, const char* text, int32_t length);
    // GetTextExtentPoint32 of " " in the device context's stock font; word
    // spacing never follows the markup font.
    TextExtent (*stock_space)(void* user);
    // GetTextMetrics height and ascent.
    TextMetrics (*metrics)(void* user, const MarkupFont& font);
    // Transparent TextOut with the cell's top-left at (x, y).
    void (*draw)(
        void* user,
        Surface& surface,
        const MarkupFont& font,
        const char* text,
        int32_t length,
        int32_t x,
        int32_t y,
        uint8_t color_index
    );
};

// Reads a file (an IMG source); returns false when it cannot be opened.
struct FileReader {
    void* user;
    bool (*read)(void* user, const char* path, std::vector<uint8_t>* out);
};

// Modal notice with a title; `show` may be null to drop notices.
struct Notifier {
    void* user;
    void (*show)(void* user, const char* title, const char* text);
};

enum class NodeKind : uint8_t { none, line_break, text, image, table, cell };

// Reference to one layout node; index is into the matching Document pool
// (for line_break it is the owning flow).
struct NodeRef {
    NodeKind kind = NodeKind::none;
    int32_t index = -1;
};

// Fields every node shares: the run chain link, link target and position.
// `y` is the bottom edge of the node's box.
struct NodeHeader {
    NodeRef next;
    const char* href;
    int32_t y;
    int32_t x;
};

// One word.
struct TextRun {
    NodeHeader head;
    int32_t cx; // cached extent, -1 until measured
    int32_t cy;
    int32_t ascent;
    const char* text; // points into the document buffer
    int32_t font;     // Document::fonts index
};

// One IMG.
struct Image {
    NodeHeader head;
    char name[k_image_name_bytes]; // SRC as given
    std::vector<uint8_t> pixels;   // decoded rows, empty when not loaded
    int32_t row_bytes;             // stride of pixels
    int32_t source_width;          // PNG width
    int32_t source_height;         // PNG height
    int32_t width;                 // displayed width, -1 unknown
    int32_t height;
    bool raw_indices; // PNG palette entry 0 is black: copy indices
    bool loaded;
};

// TABLE, or the one-cell table CENTER builds.
struct Table {
    NodeHeader head;
    int32_t columns;                    // widest row seen
    std::vector<int32_t> column_widths; // from the first row's TD WIDTH
    int32_t rows;
    std::vector<int32_t> row_heights; // TR HEIGHT, 0 = measure
    int32_t column;                   // cursor inside the current row
    int32_t reserved_after_column;    // cleared on reset; nothing reads it
    int32_t column_spacing;
    int32_t row_spacing;
    std::vector<NodeRef> cells; // in parse order, read as rows*columns
};

// TD.
struct Cell {
    NodeHeader head;
    int32_t table;  // owning table
    int32_t flow;   // nested flow
    int32_t height; // -1 = measure the flow
    int32_t width;  // -1 = flow width
    bool has_background;
    uint32_t background_rgb; // 0xRRGGBB
};

// Parser and layout state of the document or of one cell.
struct Flow {
    uint32_t flags; // k_flow_*
    char* buffer_cursor;
    std::vector<NodeRef> items; // one per run, image, table or break
    bool start_new_item;        // whitespace or a break precedes the next word
    std::vector<int32_t> font_stack;
    int32_t width;
    int32_t height; // -1 until laid out
    int32_t pad_x;  // BODY LEFTMARGIN
    int32_t pad_y;  // BODY TOPMARGIN
    bool laid_out;
    NodeHeader line_break;              // shared break item
    std::vector<const char*> tag_stack; // open tag keys
    const char* href;                   // current A target
    int32_t table;                      // table under construction, -1 none
};

// Off-screen 8-bit canvas and its cached text state.
struct Canvas {
    Surface surface; // view of pixels; pitch is width rounded up to 4
    std::vector<uint8_t> pixels;
    int32_t font; // font whose colour/space width are current, -1 none
    bool metrics_dirty;
    TextMetrics metrics; // font's cached height and ascent
    int32_t space_cx;    // stock-font space width used between runs
    int32_t space_cy;
};

// Placeholder shimmer state shared by every image drawn without pixels.
struct Shimmer {
    uint32_t frame;    // wraps at 256
    float phase;       // frame / 256
    uint8_t ramp[256]; // palette index per ramp step, 0 = not yet computed
};

struct Environment {
    TextBackend text;
    FileReader files;
    Palette palette;  // see markup_palette()
    Notifier notices; // PNG decoder warnings and errors
};

struct Document {
    Environment env;
    std::vector<char> buffer; // markup copy, parsed in place
    std::vector<Flow> flows;  // [0] is the document itself
    std::vector<TextRun> texts;
    std::vector<Image> images;
    std::vector<Table> tables;
    std::vector<Cell> cells;
    std::vector<MarkupFont> fonts;
    Canvas canvas;
    bool has_canvas;
    Shimmer shimmer;
};

// Attribute slot for the tag attribute scanner: name in, value out.
struct AttributeSlot {
    const char* name;
    const char* value; // null when absent
    int32_t number;    // atol(value)
};

// ---- parsing ------------------------------------------------------------

/// Tests whether text starts with a tag or attribute key.
///
/// @param key upper-case key to match
/// @param text markup at the key's position; compared upper-cased (ASCII)
/// @param count number of characters to compare
/// @return true when the first `count` characters of `text` equal those of
///     `key` (both having that many) and the next character of `text` is not
///     a letter or digit
bool key_matches(const char* key, const char* text, int32_t count);

/// Tests for an ASCII letter or digit.
///
/// @param c character to test
/// @return true for 0-9, a-z and A-Z
bool is_alnum(char c);

/// Decodes the character references of a NUL-terminated word in place.
///
/// &amp; &lt; &gt; &quot; &nbsp; &copy; and &reg; (names compared without
/// case, the ';' optional) and the numeric &#NNN; and &#xHH; become their
/// characters. &nbsp; and &#160; become a space, which is part of the word.
/// A numeric reference to a character Windows-1252 holds becomes that byte
/// (&#128; to &#159; name the bytes of those values), any other character its
/// UTF-8 bytes, and a control character nothing. An '&' that starts no
/// reference is kept with what follows it.
///
/// @param[in,out] text word to decode; it can only get shorter
/// @quirk &reg; becomes 0x99, which Windows-1252 draws as the trade mark
///     sign, as in 3.1c.
void decode_entities(char* text);

/// Reads the NAME=value attributes of the tag at the cursor up to and
/// including its '>', terminating each value in place.
///
/// The tag name is skipped first. A value is quoted with " or ' (a backslash
/// before the quote escapes it) or is a run of letters, digits, '+' and '-'.
/// Names are matched case-insensitively against the slots; unknown
/// attributes are skipped. The closing '>' is overwritten with NUL.
///
/// @param[in,out] cursor position inside the tag's text; left after the '>'
///     or at the end of the text
/// @param[in,out] slots attributes to look for; a found one gets its value
///     and decode_int of it, the others are left as the caller set them
/// @param count number of slots
void scan_attributes(char** cursor, AttributeSlot* slots, int32_t count);

/// Converts decimal text to an integer as atol does.
///
/// Leading whitespace and a sign are accepted; conversion stops at the first
/// non-digit and wraps round on overflow.
///
/// @param text text to convert
/// @return the value, 0 when there are no digits
int32_t decode_int(const char* text);

/// Initialises a document: installs the environment and leaves an empty
/// document flow with no width, no fonts and no canvas.
///
/// @param[out] doc document to initialise; its previous contents are dropped
/// @param env text, file, palette and notice hooks; copied
void document_init(Document& doc, const Environment& env);

/// Frees the canvas and sets the wrap width; the height becomes unknown so
/// the next document_height or document_paint lays the document out again.
///
/// @param[in,out] doc document to resize
/// @param width wrap width in canvas pixels
void document_set_width(Document& doc, int32_t width);

/// Replaces the content with parsed markup, keeping the width.
///
/// Frees the canvas, drops every node, font and nested flow, then copies the
/// markup and parses it in place.
///
/// @param[in,out] doc document to fill
/// @param markup markup text; at most k_max_markup_bytes
/// @return false when the markup is too long or the parse stopped early (for
///     example on a TR or TD outside a table)
bool document_load(Document& doc, std::string_view markup);

/// Reads a markup file through env.files and loads it (see document_load).
///
/// Like 3.1c, the text ends at the file's first NUL.
///
/// @param[in,out] doc document to fill
/// @param path file path handed to the file reader
/// @return false when the file cannot be read or the load fails
bool document_load_file(Document& doc, const char* path);

// ---- layout and hit testing ---------------------------------------------

/// Returns the document's height, laying it out first while it is unknown.
///
/// The layout creates the canvas at the document width and lays the flow out
/// inside its margins.
///
/// @param[in,out] doc document to measure
/// @return the laid-out height in canvas pixels, including the top and
///     bottom margins
int32_t document_height(Document& doc);

/// Finds the link under a canvas point.
///
/// @param[in,out] doc laid-out document
/// @param x canvas x in pixels
/// @param y canvas y in pixels
/// @return the HREF of the first linked node hit, or null
const char* document_link_at(Document& doc, int32_t x, int32_t y);

/// Lays a flow's items out in lines inside a rectangle and records its
/// height.
///
/// Items join a line while they fit its width, each followed by the stock
/// font's space; an item wider than an empty line is placed alone, and a
/// break ends the line. Each line's baseline sits 2 pixels plus the line's
/// largest ascent below the previous one, and the flow's alignment flags
/// place the line (never left of 0). When a bounded rectangle runs out of
/// room, the remaining items are parked at (-1, -1). The flow's height
/// becomes the last baseline less `top`.
///
/// @param[in,out] doc document holding the flow
/// @param flow flow index (0 is the document)
/// @param rect {left, top, right, bottom} in canvas pixels; a bottom of -1 is
///     unbounded
/// @return false when a line did not fit
bool flow_layout(Document& doc, int32_t flow, const int32_t rect[4]);

/// Lays a flow out over the canvas inset by its margins (BODY LEFTMARGIN and
/// TOPMARGIN), then adds both vertical margins to its height.
///
/// @param[in,out] doc document holding the flow and the canvas
/// @param flow flow index
/// @return the flow_layout result
bool flow_layout_padded(Document& doc, int32_t flow);

/// Returns a node's width, measuring a word on first use.
///
/// @param[in,out] doc document holding the node
/// @param node node to measure
/// @return the width in pixels; -1 for a break
int32_t node_width(Document& doc, NodeRef node);
/// Returns a node's box height, measuring words, tables and cells as needed.
///
/// @param[in,out] doc document holding the node
/// @param node node to measure
/// @return the height in pixels; 0 for a break
int32_t node_height(Document& doc, NodeRef node);

// ---- painting -----------------------------------------------------------

/// Paints the document into its canvas.
///
/// Lays the document out first while its height is unknown, makes sure the
/// canvas covers the laid-out height (replacing a shorter one), then draws
/// the document flow. A canvas that is kept is drawn over without clearing.
///
/// @param[in,out] doc document to paint
/// @return true
bool document_paint(Document& doc);

/// Returns the canvas surface.
///
/// @param doc document to read
/// @return the canvas after document_paint or document_height, or null while
///     there is none
const Surface* document_surface(const Document& doc);

/// Draws a flow's items into the canvas, laying the flow out over the canvas
/// first when it has not been laid out.
///
/// @param[in,out] doc document holding the flow and the canvas
/// @param flow flow index
void flow_draw(Document& doc, int32_t flow);

/// Builds the palette the canvas uses: the game palette with entries 1-15 and
/// 240-254 forced to green and index 9 to magenta, every flag cleared.
///
/// @param game_palette palette to start from
/// @return the markup palette
Palette markup_palette(const Palette& game_palette);

/// Finds the palette entry nearest a colour among the ramp range
/// 0x10-0xF0.
///
/// An exact match wins at once; otherwise the smallest squared RGB distance
/// wins, the first of equals.
///
/// @param palette palette to search
/// @param r red component, 0-255
/// @param g green component, 0-255
/// @param b blue component, 0-255
/// @return the palette index
uint8_t ramp_palette_index(const Palette& palette, uint8_t r, uint8_t g, uint8_t b);

/// Advances the Park-Miller generator (16807 modulo 2^31 - 1) used by the
/// image placeholder.
///
/// @param[in,out] seed generator state; replaced by the new value
/// @return the new value
uint32_t shimmer_random(uint32_t* seed);

// ---- images -------------------------------------------------------------

/// Lists the images still waiting for data.
///
/// @param doc document to search
/// @param[out] names cleared, then given the SRC of every image whose PNG
///     has not been fully decoded
/// @return true when any image is waiting
bool document_pending_images(const Document& doc, std::vector<const char*>* names);

/// Reloads every image whose SRC equals `name` (case-sensitively) from
/// `path`; each such image takes `path` as its new name.
///
/// @param[in,out] doc document holding the images
/// @param name SRC to look for
/// @param path file to load the PNG from
void document_reload_image(Document& doc, const char* name, const char* path);

/// Loads an image's PNG.
///
/// Stores the link, the size overrides and `path` as the image's name, then
/// reads the file through env.files. The displayed size defaults to the
/// PNG's. Images larger than 256 pixels on either side are not decoded. As
/// in 3.1c, the decoder cuts 16-bit samples to 8 bits and widens 1/2/4-bit
/// samples to one unscaled byte, so palette and grey images give one byte
/// per pixel and the other colour types their samples. `raw_indices` is set
/// when the PNG palette's entry 0 is black.
///
/// @param[in,out] doc document holding the image
/// @param image image index
/// @param path file to read; null does nothing
/// @param width displayed width override; values below 1 keep the current
///     width
/// @param height displayed height override; values below 1 keep the current
///     height
/// @param href link target for the image, or null to keep the current one
/// @return false when the file is missing or not a PNG; true otherwise, also
///     when decoding stops early, which keeps the rows decoded before it and
///     leaves `loaded` clear
bool image_load_png(
    Document& doc, int32_t image, const char* path, int32_t width, int32_t height, const char* href
);

// Title of the decoder's notices.
inline constexpr const char* k_png_notice_title = "PNG DECOMPRESS";

/// Shows a PNG decoder message through env.notices as
/// "<file>: libpng warning: <text>\n" titled k_png_notice_title.
///
/// @param env environment whose notifier shows the message; a null `show`
///     drops it
/// @param file image file name, or null for "UNKNOWN (ERROR!)"
/// @param text decoder message
void png_decoder_warning(const Environment& env, const char* file, const char* text);

/// Shows a PNG decoder error exactly as a warning (see
/// png_decoder_warning).
///
/// @param env environment whose notifier shows the message
/// @param file image file name, or null
/// @param text decoder message
void png_decoder_error(const Environment& env, const char* file, const char* text);

} // namespace oa::boneyards::html
