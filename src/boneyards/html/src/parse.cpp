// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Markup tokenizer, tag handlers and document lifecycle.

#include <cstdio>
#include <cstring>

#include "internal.hpp"
#include "oa/boneyards/html_scripts.hpp"

namespace oa::boneyards::html {

using namespace detail;

namespace {

// Tag keys; handlers compare some of them by identity.
constexpr const char k_tag_html[] = "HTML";
constexpr const char k_tag_body[] = "BODY";
constexpr const char k_tag_font[] = "FONT";
constexpr const char k_tag_center[] = "CENTER";
constexpr const char k_tag_bold[] = "B";
constexpr const char k_tag_italic[] = "I";
constexpr const char k_tag_underline[] = "U";
constexpr const char k_tag_img[] = "IMG";
constexpr const char k_tag_br[] = "BR";
constexpr const char k_tag_p[] = "P";
constexpr const char k_tag_h1[] = "H1";
constexpr const char k_tag_h2[] = "H2";
constexpr const char k_tag_h3[] = "H3";
constexpr const char k_tag_anchor[] = "A";
constexpr const char k_tag_table[] = "TABLE";
constexpr const char k_tag_tr[] = "TR";
constexpr const char k_tag_td[] = "TD";

constexpr char k_ascii_upper_delta = 'a' - 'A';
constexpr int32_t k_hex_color_digits = 6;
constexpr const char k_remote_image_prefix[] = "http:";
constexpr int32_t k_remote_image_prefix_length = 5;
// FONT SIZE=n maps to a character height of n*2+6; +n/-n step by 2.
constexpr int32_t k_font_size_scale = 2;
constexpr int32_t k_font_size_base = 6;
// H1..H3 grow the current height by these amounts.
constexpr int32_t k_h1_growth = 5;
constexpr int32_t k_h2_growth = 3;
constexpr int32_t k_h3_growth = 1;

struct Entity {
    const char* name;
    char value;
};

// Named references; a name is matched without case and its ';' is optional.
// &nbsp; is a space inside the word, so it neither breaks the line nor
// collapses into the space between words.
constexpr Entity k_entities[] = {
    {"reg", static_cast<char>(0x99)},
    {"copy", static_cast<char>(0xa9)},
    {"gt", '>'},
    {"lt", '<'},
    {"amp", '&'},
    {"nbsp", ' '},
    {"quot", '"'},
};

// Numeric references: at most this many digits, and no character past the
// last Unicode scalar value or among the surrogates.
constexpr int32_t k_max_decimal_digits = 7;
constexpr int32_t k_max_hex_digits = 6;
constexpr uint32_t k_last_code_point = 0x10ffff;
constexpr uint32_t k_first_surrogate = 0xd800;
constexpr uint32_t k_last_surrogate = 0xdfff;
// &#160; is the no-break space, written as &nbsp; is.
constexpr uint32_t k_no_break_space = 0xa0;
// &#128; to &#159; name the Windows-1252 bytes of those values, as browsers
// read them.
constexpr uint32_t k_cp1252_first_extra = 0x80;
constexpr uint32_t k_cp1252_last_extra = 0x9f;
constexpr uint32_t k_delete = 0x7f;
constexpr uint32_t k_first_printable = 0x20;
// UTF-8: the first characters that take three and four bytes, the lead byte
// of each length, and the continuation bytes' mark and payload. Characters
// below 0x100 are written as Windows-1252 bytes, so the two-byte form starts
// there.
constexpr uint32_t k_utf8_three_bytes_from = 0x800;
constexpr uint32_t k_utf8_four_bytes_from = 0x10000;
constexpr uint32_t k_utf8_lead_two = 0xc0;
constexpr uint32_t k_utf8_lead_three = 0xe0;
constexpr uint32_t k_utf8_lead_four = 0xf0;
constexpr uint32_t k_utf8_continuation = 0x80;
constexpr uint32_t k_utf8_payload_mask = 0x3f;
constexpr int32_t k_utf8_payload_bits = 6;
// The most bytes one reference writes: a four-byte UTF-8 character.
constexpr int32_t k_max_reference_bytes = 4;

using TagHandler = bool (*)(Document& doc, int32_t flow, char** cursor, const char* key);

struct TagEntry {
    const char* key;
    int32_t length;
    TagHandler handler;
};

char upper_ascii(char c) {
    return (c >= 'a' && c <= 'z') ? static_cast<char>(c - k_ascii_upper_delta) : c;
}

/// Tests for markup whitespace: a space, tab, line feed, form feed or
/// carriage return.
///
/// @param c character to test
/// @return true when the character separates words
bool is_markup_space(char c) {
    return c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == '\f';
}

/// Skips markup whitespace.
///
/// @param[in,out] cursor parse position; left at the next other character or
///     the end of the text
void skip_spaces(char** cursor) {
    while (**cursor != '\0' && is_markup_space(**cursor))
        ++*cursor;
}

/// Finds the end of the current tag.
///
/// @param p position inside the tag
/// @return the '>' ending the tag, or the terminating NUL
char* find_tag_end(char* p) {
    while (*p != '\0' && *p != '>')
        ++p;
    return p;
}

void skip_past_tag(char** cursor) {
    *cursor = find_tag_end(*cursor) + 1;
}

int32_t c_length(const char* text) {
    return static_cast<int32_t>(std::strlen(text));
}

/// Pushes an open tag's key on the flow's tag stack.
///
/// @param[in,out] flow flow the element opens in
/// @param key tag key from k_tags
void push_tag(Flow& flow, const char* key) {
    flow.tag_stack.push_back(key);
}

/// Pops the innermost open tag key; an empty stack is left alone.
///
/// @param[in,out] flow flow the element closes in
void pop_tag(Flow& flow) {
    if (!flow.tag_stack.empty())
        flow.tag_stack.pop_back();
}

/// Tests whether a close tag names any element open in the flow.
///
/// @param flow flow whose tag stack is searched
/// @param name markup after "</"
/// @return true when an open key matches (see key_matches)
bool close_tag_is_open(const Flow& flow, const char* name) {
    for (auto it = flow.tag_stack.rbegin(); it != flow.tag_stack.rend(); ++it)
        if (key_matches(*it, name, c_length(*it)))
            return true;
    return false;
}

/// Tests whether a close tag names the innermost open element.
///
/// @param flow flow whose tag stack is read; must not be empty
/// @param name markup after "</"
/// @return true when the innermost key matches
bool close_tag_is_innermost(const Flow& flow, const char* name) {
    const char* top = flow.tag_stack.back();
    return key_matches(top, name, c_length(top));
}

/// Pushes a font on the flow's font stack; later words use it.
///
/// @param[in,out] flow flow to change
/// @param font font index in Document::fonts
void push_font(Flow& flow, int32_t font) {
    flow.font_stack.push_back(font);
}

/// Pops the innermost font; an empty stack is left alone.
///
/// @param[in,out] flow flow to change
void pop_font(Flow& flow) {
    if (!flow.font_stack.empty())
        flow.font_stack.pop_back();
}

/// Appends a copy of the flow's current font to the document's fonts.
///
/// @param[in,out] doc document whose font pool grows
/// @param flow flow index
/// @return the copy's index
int32_t clone_current_font(Document& doc, int32_t flow) {
    return clone_font(doc, flow_current_font(doc.flows[flow]));
}

/// Appends a break item (the flow's shared break node).
///
/// @param[in,out] flow flow to extend
/// @param flow_index index of `flow`, which the break item names
void push_break(Flow& flow, int32_t flow_index) {
    flow.items.push_back(NodeRef{NodeKind::line_break, flow_index});
}

/// Returns a markup font's character height.
///
/// @param font font to read
/// @return the height in pixels (the negated lfHeight)
int32_t font_size(const MarkupFont& font) {
    return -font.height;
}

/// Sets a markup font's character height, marking the font dirty when it
/// changes.
///
/// @param[in,out] font font to change
/// @param size character height in pixels
void font_set_size(MarkupFont& font, int32_t size) {
    if (size != -font.height) {
        font.height = -size;
        font.dirty = true;
    }
}

/// Sets a markup font's weight to bold (800) or normal (400), marking the
/// font dirty when it changes.
///
/// @param[in,out] font font to change
/// @param bold true for bold
void font_set_bold(MarkupFont& font, bool bold) {
    const int32_t weight = bold ? k_weight_bold : k_weight_normal;
    if (weight != font.weight) {
        font.weight = weight;
        font.dirty = true;
    }
}

/// Sets a markup font's italic flag, marking the font dirty when it changes.
///
/// @param[in,out] font font to change
/// @param italic true for italic
void font_set_italic(MarkupFont& font, bool italic) {
    if (italic != font.italic) {
        font.italic = italic;
        font.dirty = true;
    }
}

/// Sets a markup font's underline flag, marking the font dirty when it
/// changes.
///
/// @param[in,out] font font to change
/// @param underline true for underlined
void font_set_underline(MarkupFont& font, bool underline) {
    if (underline != font.underline) {
        font.underline = underline;
        font.dirty = true;
    }
}

/// Sets a markup font's colour, marking the font dirty when it changes.
///
/// @param[in,out] font font to change
/// @param rgb colour as 0xRRGGBB; stored as the COLORREF 0x00BBGGRR
void font_set_rgb(MarkupFont& font, uint32_t rgb) {
    const uint32_t colorref = ((rgb >> 16) & 0xffu) | (rgb & 0xff00u) | ((rgb & 0xffu) << 16);
    if (colorref != font.colorref) {
        font.colorref = colorref;
        font.dirty = true;
    }
}

/// Sets a markup font's face (at most 31 characters), marking the font dirty
/// when its first 31 characters change.
///
/// @param[in,out] font font to change
/// @param face face name
void font_set_face(MarkupFont& font, const char* face) {
    if (std::strncmp(face, font.face, k_face_bytes - 1) != 0) {
        std::memset(font.face, 0, k_face_bytes);
        std::snprintf(font.face, sizeof font.face, "%s", face);
        font.face[k_face_bytes - 1] = '\0';
        font.dirty = true;
    }
}

uint32_t decode_hex(const char* digits, bool td_letters) {
    uint32_t value = 0;
    for (int32_t i = 0; i < k_hex_color_digits; ++i) {
        const char c = digits[i];
        // TD BGCOLOR subtracts 'A' instead of 'A'-10, so A..F read as 0..5.
        const int32_t nibble = c <= '9' ? c - '0' : (td_letters ? c - 'A' : c - ('A' - 10));
        value = (value << 4) + static_cast<uint32_t>(nibble & 0xf);
    }
    return value;
}

/// Starts a word node at the cursor.
///
/// The word takes the flow's current font and link. When no whitespace or
/// break separated it from the previous word, it joins the end of that
/// word's run (so "a<B>b</B>" forms one run); otherwise it starts a new
/// item. Words outside BODY are dropped.
///
/// @param[in,out] doc document receiving the node
/// @param flow_index flow index
/// @param text start of the word in the document buffer
void start_word(Document& doc, int32_t flow_index, char* text) {
    if ((doc.flows[flow_index].flags & k_flow_in_body) == 0)
        return;
    const int32_t index = new_text(doc);
    Flow& flow = doc.flows[flow_index];
    TextRun& run = doc.texts[index];
    run.cx = -1;
    run.cy = -1;
    run.ascent = -1;
    run.text = text;
    run.font = flow_current_font(flow);
    run.head.href = flow.href;
    const NodeRef ref{NodeKind::text, index};
    if (!flow.start_new_item && !flow.items.empty()) {
        NodeHeader* tail = &header(doc, flow.items.back());
        while (tail->next.kind != NodeKind::none)
            tail = &header(doc, tail->next);
        tail->next = ref;
    } else {
        flow.items.push_back(ref);
    }
    flow.start_new_item = false;
}

/// Handles HTML: a plain container.
///
/// @param[in,out] doc document receiving the nodes
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
bool handle_html(Document& doc, int32_t flow, char** cursor, const char* key) {
    push_tag(doc.flows[flow], key);
    skip_past_tag(cursor);
    const bool ok = parse_content(doc, flow, cursor);
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Handles BODY: enables text for its content and stores positive TOPMARGIN
/// and LEFTMARGIN values as the flow's margins; BGCOLOR is read and ignored.
///
/// @param[in,out] doc document receiving the nodes
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
bool handle_body(Document& doc, int32_t flow, char** cursor, const char* key) {
    push_tag(doc.flows[flow], key);
    AttributeSlot slots[3] = {
        {"BGCOLOR", nullptr, 0}, {"TOPMARGIN", nullptr, 0}, {"LEFTMARGIN", nullptr, 0}
    };
    scan_attributes(cursor, slots, 3);
    Flow& f = doc.flows[flow];
    if (slots[1].number > 0)
        flow_set_pad_y(f, slots[1].number);
    if (slots[2].number > 0)
        flow_set_pad_x(f, slots[2].number);
    f.flags |= k_flow_in_body;
    const bool ok = parse_content(doc, flow, cursor);
    doc.flows[flow].flags &= ~k_flow_in_body;
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Handles FONT COLOR=RRGGBB FACE=name SIZE=n|+n|-n: its content uses a copy
/// of the current font with the changes.
///
/// A COLOR must have exactly six hex digits. SIZE=n sets a character height
/// of 2n + 6 pixels; +n and -n change the current height by 2n.
///
/// @param[in,out] doc document receiving the nodes and fonts
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
bool handle_font(Document& doc, int32_t flow, char** cursor, const char* key) {
    AttributeSlot slots[3] = {{"COLOR", nullptr, 0}, {"FACE", nullptr, 0}, {"SIZE", nullptr, 0}};
    scan_attributes(cursor, slots, 3);
    const int32_t font = clone_current_font(doc, flow);
    push_tag(doc.flows[flow], key);
    MarkupFont& f = doc.fonts[font];
    if (slots[0].value != nullptr && c_length(slots[0].value) == k_hex_color_digits)
        font_set_rgb(f, decode_hex(slots[0].value, false));
    if (slots[1].value != nullptr)
        font_set_face(f, slots[1].value);
    if (slots[2].value != nullptr) {
        const char sign = slots[2].value[0];
        const int32_t size = (sign == '+' || sign == '-')
                                 ? font_size(f) + slots[2].number * k_font_size_scale
                                 : slots[2].number * k_font_size_scale + k_font_size_base;
        font_set_size(f, size);
    }
    push_font(doc.flows[flow], font);
    const bool ok = parse_content(doc, flow, cursor);
    pop_font(doc.flows[flow]);
    pop_tag(doc.flows[flow]);
    return ok;
}

bool parse_cell(
    Document& doc, int32_t table, int32_t parent, uint32_t flags, char** cursor, int32_t width
);

/// Handles CENTER: a one-cell table with no spacing whose cell centres its
/// content and spans the flow width.
///
/// @param[in,out] doc document receiving the nodes
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
bool handle_center(Document& doc, int32_t flow, char** cursor, const char* key) {
    const int32_t table = new_table(doc);
    Flow& f = doc.flows[flow];
    f.table = table;
    push_tag(f, key);
    skip_past_tag(cursor);
    f.items.push_back(NodeRef{NodeKind::table, table});
    Table& t = doc.tables[table];
    table_reset(t, f.href);
    t.row_spacing = 0;
    t.column_spacing = 0;
    table_push_row(t);
    const bool ok = parse_cell(doc, table, flow, f.flags | k_flow_align_center, cursor, 0);
    table_fill_auto_columns(doc, table, doc.flows[flow].width);
    doc.flows[flow].table = -1;
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Handles B, I and U: the content uses a bold, italic or underlined copy of
/// the current font.
///
/// @param[in,out] doc document receiving the nodes and fonts
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags; compared by identity with the B and
///     U keys
/// @return false when the content failed to parse
bool handle_style(Document& doc, int32_t flow, char** cursor, const char* key) {
    const int32_t font = clone_current_font(doc, flow);
    push_tag(doc.flows[flow], key);
    if (key == k_tag_bold)
        font_set_bold(doc.fonts[font], true);
    else if (key == k_tag_underline)
        font_set_underline(doc.fonts[font], true);
    else
        font_set_italic(doc.fonts[font], true);
    push_font(doc.flows[flow], font);
    skip_past_tag(cursor);
    const bool ok = parse_content(doc, flow, cursor);
    pop_font(doc.flows[flow]);
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Handles IMG WIDTH HEIGHT SRC: appends an image loaded from a local PNG.
///
/// WIDTH and HEIGHT are clamped to 256. An IMG without SRC, or with an
/// "http:" source, adds nothing.
///
/// @param[in,out] doc document receiving the image
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the tag
/// @param key the tag's key (unused)
/// @return true
bool handle_img(Document& doc, int32_t flow, char** cursor, const char* /*key*/) {
    AttributeSlot slots[3] = {{"WIDTH", nullptr, 0}, {"HEIGHT", nullptr, 0}, {"SRC", nullptr, 0}};
    scan_attributes(cursor, slots, 3);
    const char* src = slots[2].value;
    if (src == nullptr ||
        std::strncmp(src, k_remote_image_prefix, k_remote_image_prefix_length) == 0)
        return true;
    int32_t width = slots[0].number;
    int32_t height = slots[1].number;
    if (width > k_image_max_side)
        width = k_image_max_side;
    if (height > k_image_max_side)
        height = k_image_max_side;
    const int32_t image = new_image(doc);
    image_load_png(doc, image, src, width, height, doc.flows[flow].href);
    doc.flows[flow].items.push_back(NodeRef{NodeKind::image, image});
    return true;
}

/// Handles BR and P: BR ends the line, P ends it and adds a blank line.
/// Neither is a container.
///
/// @param[in,out] doc document receiving the breaks
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the tag
/// @param key the tag's key; one starting with 'P' adds two breaks
/// @return true
bool handle_break(Document& doc, int32_t flow, char** cursor, const char* key) {
    Flow& f = doc.flows[flow];
    f.start_new_item = true;
    push_break(f, flow);
    if (key[0] == 'P')
        push_break(f, flow);
    skip_past_tag(cursor);
    return true;
}

/// Handles H1 (5 pixels taller, bold), H2 (3 taller, italic) and H3 (1
/// taller, bold), set off by a break before and after.
///
/// @param[in,out] doc document receiving the nodes and fonts
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags; compared by identity
/// @return false when the content failed to parse
bool handle_heading(Document& doc, int32_t flow, char** cursor, const char* key) {
    const int32_t font = clone_current_font(doc, flow);
    push_tag(doc.flows[flow], key);
    MarkupFont& f = doc.fonts[font];
    if (key == k_tag_h1) {
        font_set_size(f, font_size(f) + k_h1_growth);
        font_set_bold(f, true);
    } else if (key == k_tag_h2) {
        font_set_size(f, font_size(f) + k_h2_growth);
        font_set_italic(f, true);
    } else {
        font_set_size(f, font_size(f) + k_h3_growth);
        font_set_bold(f, true);
    }
    push_font(doc.flows[flow], font);
    doc.flows[flow].start_new_item = true;
    skip_past_tag(cursor);
    push_break(doc.flows[flow], flow);
    const bool ok = parse_content(doc, flow, cursor);
    push_break(doc.flows[flow], flow);
    pop_font(doc.flows[flow]);
    doc.flows[flow].start_new_item = true;
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Handles A HREF: the content's words are underlined and carry the link;
/// the previous link returns afterwards.
///
/// @param[in,out] doc document receiving the nodes and fonts
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
bool handle_anchor(Document& doc, int32_t flow, char** cursor, const char* key) {
    AttributeSlot slot = {"HREF", nullptr, 0};
    scan_attributes(cursor, &slot, 1);
    const char* saved = doc.flows[flow].href;
    doc.flows[flow].href = slot.value;
    const int32_t font = clone_current_font(doc, flow);
    push_tag(doc.flows[flow], key);
    font_set_underline(doc.fonts[font], true);
    push_font(doc.flows[flow], font);
    const bool ok = parse_content(doc, flow, cursor);
    pop_font(doc.flows[flow]);
    pop_tag(doc.flows[flow]);
    doc.flows[flow].href = saved;
    return ok;
}

/// Handles TABLE ROWSPACING COLSPACING: a table whose rows and cells come
/// from the content; BORDER is read and ignored.
///
/// @param[in,out] doc document receiving the table
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false when the content failed to parse
/// @quirk The flow forgets its table when the element ends, so a TR after a
///     table nested directly in another fails.
bool handle_table(Document& doc, int32_t flow, char** cursor, const char* key) {
    AttributeSlot slots[3] = {
        {"BORDER", nullptr, 0}, {"ROWSPACING", nullptr, 0}, {"COLSPACING", nullptr, 0}
    };
    scan_attributes(cursor, slots, 3);
    const int32_t table = new_table(doc);
    Flow& f = doc.flows[flow];
    f.table = table;
    push_tag(f, key);
    f.items.push_back(NodeRef{NodeKind::table, table});
    Table& t = doc.tables[table];
    table_reset(t, f.href);
    if (slots[1].value != nullptr)
        t.row_spacing = slots[1].number;
    if (slots[2].value != nullptr)
        t.column_spacing = slots[2].number;
    const bool ok = parse_content(doc, flow, cursor);
    pop_tag(doc.flows[flow]);
    table_fill_auto_columns(doc, table, doc.flows[flow].width);
    doc.flows[flow].table = -1;
    return ok;
}

/// Handles TR HEIGHT: starts a row of the flow's table with that height (0
/// measures it).
///
/// @param[in,out] doc document holding the table
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false outside a table or when the content failed to parse
bool handle_row(Document& doc, int32_t flow, char** cursor, const char* key) {
    const int32_t table = doc.flows[flow].table;
    if (table < 0)
        return false;
    AttributeSlot slot = {"HEIGHT", nullptr, 0};
    scan_attributes(cursor, &slot, 1);
    push_tag(doc.flows[flow], key);
    Table& t = doc.tables[table];
    table_push_row(t);
    t.row_heights.back() = slot.number;
    const bool ok = parse_content(doc, flow, cursor);
    pop_tag(doc.flows[flow]);
    return ok;
}

bool equals_nocase(const char* a, const char* b) {
    for (;; ++a, ++b) {
        if (upper_ascii(*a) != upper_ascii(*b))
            return false;
        if (*a == '\0')
            return true;
    }
}

/// Handles TD WIDTH ALIGN VALIGN BGCOLOR: adds a cell of the flow's table and
/// parses its content into the cell's own flow.
///
/// ALIGN=CENTER or RIGHT aligns the cell's lines; VALIGN=CENTER or BOTTOM is
/// stored but never used. A BGCOLOR must have exactly six hex digits.
///
/// @param[in,out] doc document holding the table
/// @param flow flow index the tag appears in
/// @param[in,out] cursor parse position at the tag name; left after the
///     element
/// @param key the tag's key from k_tags
/// @return false outside a table or when the content failed to parse
/// @quirk BGCOLOR reads the hex letters A-F as 0-5.
bool handle_cell(Document& doc, int32_t flow, char** cursor, const char* key) {
    const int32_t table = doc.flows[flow].table;
    if (table < 0)
        return false;
    AttributeSlot slots[4] = {
        {"WIDTH", nullptr, 0},
        {"ALIGN", nullptr, 0},
        {"VALIGN", nullptr, 0},
        {"BGCOLOR", nullptr, 0}
    };
    scan_attributes(cursor, slots, 4);
    push_tag(doc.flows[flow], key);
    uint32_t flags = doc.flows[flow].flags;
    if (slots[1].value != nullptr) {
        if (equals_nocase("CENTER", slots[1].value))
            flags |= k_flow_align_center;
        if (equals_nocase("RIGHT", slots[1].value))
            flags |= k_flow_align_right;
    }
    if (slots[2].value != nullptr) {
        if (equals_nocase("CENTER", slots[2].value))
            flags |= k_flow_valign_center;
        if (equals_nocase("BOTTOM", slots[2].value))
            flags |= k_flow_valign_bottom;
    }
    const bool ok = parse_cell(doc, table, flow, flags, cursor, slots[0].number);
    if (slots[3].value != nullptr && c_length(slots[3].value) == k_hex_color_digits) {
        const NodeRef last = doc.tables[table].cells.back();
        Cell& cell = doc.cells[last.index];
        cell.background_rgb = decode_hex(slots[3].value, true);
        cell.has_background = true;
    }
    pop_tag(doc.flows[flow]);
    return ok;
}

/// Tags the tokenizer recognises, matched in order by key and key length.
constexpr TagEntry k_tags[] = {
    {k_tag_html, 4, handle_html},
    {k_tag_body, 4, handle_body},
    {k_tag_font, 4, handle_font},
    {k_tag_center, 6, handle_center},
    {k_tag_bold, 1, handle_style},
    {k_tag_italic, 1, handle_style},
    {k_tag_underline, 1, handle_style},
    {k_tag_img, 3, handle_img},
    {k_tag_br, 2, handle_break},
    {k_tag_p, 1, handle_break},
    {k_tag_h1, 2, handle_heading},
    {k_tag_h2, 2, handle_heading},
    {k_tag_h3, 2, handle_heading},
    {k_tag_anchor, 1, handle_anchor},
    {k_tag_table, 5, handle_table},
    {k_tag_tr, 2, handle_row},
    {k_tag_td, 2, handle_cell},
};

/// Sets a cell up and parses its content into its nested flow.
///
/// The cell gets a measured size, no background, the parent flow's link and
/// its table; its flow (created when missing) is reset with `flags` and
/// starts with the default font. The cell inherits the parent's open tags,
/// so their close tags end it.
///
/// @param[in,out] doc document holding the cell
/// @param cell_index cell index
/// @param table index of the cell's table
/// @param parent index of the flow the TD appears in
/// @param flags k_flow_* bits for the cell's flow
/// @param[in,out] cursor parse position after the TD tag; left after the
///     cell's content
/// @return false when the content failed to parse
bool bind_cell(
    Document& doc, int32_t cell_index, int32_t table, int32_t parent, uint32_t flags, char** cursor
) {
    {
        Cell& cell = doc.cells[cell_index];
        cell.height = -1;
        cell.width = -1;
        cell.has_background = false;
        cell.head.href = doc.flows[parent].href;
        cell.table = table;
    }
    if (doc.cells[cell_index].flow < 0) {
        const int32_t nested = new_flow(doc);
        doc.cells[cell_index].flow = nested;
    }
    const int32_t nested = doc.cells[cell_index].flow;
    flow_reset(doc, nested, flags);
    flow_reset_fonts(doc, nested);
    doc.flows[nested].tag_stack = doc.flows[parent].tag_stack;
    return parse_content(doc, nested, cursor);
}

/// Adds a cell to a table at its column cursor and parses the cell's
/// content into a nested flow (see bind_cell).
///
/// @param[in,out] doc document holding the table
/// @param table table index
/// @param parent index of the flow the cell appears in
/// @param flags k_flow_* bits for the cell's flow
/// @param[in,out] cursor parse position after the cell's tag
/// @param width TD WIDTH in pixels (0 for an automatic column); used in the
///     table's first row
/// @return false when the content failed to parse
bool parse_cell(
    Document& doc, int32_t table, int32_t parent, uint32_t flags, char** cursor, int32_t width
) {
    const int32_t cell = new_cell(doc);
    table_add_cell_slot(doc.tables[table], cell, width);
    return bind_cell(doc, cell, table, parent, flags, cursor);
}

/// Lowers an ASCII capital letter; other characters are returned as they are.
char lower_ascii(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + k_ascii_upper_delta) : c;
}

/// Tests whether text starts with a reference name, compared without case.
///
/// @param text the word after the '&'
/// @param name lower-case name
/// @return the name's length when the text starts with it, else 0
int32_t match_name(const char* text, const char* name) {
    int32_t length = 0;
    for (; name[length] != '\0'; ++length)
        if (lower_ascii(text[length]) != name[length])
            return 0;
    return length;
}

/// Returns the value of a digit in a base.
///
/// @param c character to read
/// @param hex true for base 16, false for base 10
/// @return the digit's value, or -1 when `c` is not a digit of the base
int32_t digit_value(char c, bool hex) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (hex && lower_ascii(c) >= 'a' && lower_ascii(c) <= 'f')
        return lower_ascii(c) - 'a' + 10;
    return -1;
}

/// Reads the number of a numeric reference: decimal digits, or 'x' and hex
/// digits.
///
/// @param text the word after "&#"
/// @param[out] code_point the character's value; left alone when there is
///     none
/// @return how many characters the number takes, 0 when it has no digits,
///     too many, or names no Unicode scalar value
int32_t read_reference_number(const char* text, uint32_t* code_point) {
    const bool hex = lower_ascii(text[0]) == 'x';
    const int32_t first = hex ? 1 : 0;
    const int32_t limit = hex ? k_max_hex_digits : k_max_decimal_digits;
    const uint32_t base = hex ? 16u : 10u;
    uint32_t value = 0;
    int32_t at = first;
    for (; digit_value(text[at], hex) >= 0; ++at) {
        if (at - first == limit)
            return 0;
        value = value * base + static_cast<uint32_t>(digit_value(text[at], hex));
    }
    if (at == first || value > k_last_code_point ||
        (value >= k_first_surrogate && value <= k_last_surrogate))
        return 0;
    *code_point = value;
    return at;
}

/// Writes the bytes a numeric reference stands for.
///
/// &#160; is a space, as &nbsp; is. &#128; to &#159; are the Windows-1252
/// bytes of those values; a character Windows-1252 holds is written as its
/// byte, any other as UTF-8. A control character writes nothing.
///
/// @param code_point the character, a Unicode scalar value
/// @param[out] out receives the bytes, at most k_max_reference_bytes
/// @return how many bytes were written
int32_t write_reference(uint32_t code_point, char* out) {
    if (code_point == k_no_break_space)
        code_point = ' ';
    if (code_point < k_first_printable || code_point == k_delete)
        return 0;
    if (code_point >= k_cp1252_first_extra && code_point <= k_cp1252_last_extra) {
        out[0] = static_cast<char>(code_point);
        return 1;
    }
    const int32_t byte = markup_cp1252_byte(code_point);
    if (byte >= 0) {
        out[0] = static_cast<char>(byte);
        return 1;
    }
    auto continuation = [](uint32_t value, int32_t shift) {
        return static_cast<char>(k_utf8_continuation | ((value >> shift) & k_utf8_payload_mask));
    };
    if (code_point < k_utf8_three_bytes_from) {
        out[0] = static_cast<char>(k_utf8_lead_two | (code_point >> k_utf8_payload_bits));
        out[1] = continuation(code_point, 0);
        return 2;
    }
    if (code_point < k_utf8_four_bytes_from) {
        out[0] = static_cast<char>(k_utf8_lead_three | (code_point >> (2 * k_utf8_payload_bits)));
        out[1] = continuation(code_point, k_utf8_payload_bits);
        out[2] = continuation(code_point, 0);
        return 3;
    }
    out[0] = static_cast<char>(k_utf8_lead_four | (code_point >> (3 * k_utf8_payload_bits)));
    out[1] = continuation(code_point, 2 * k_utf8_payload_bits);
    out[2] = continuation(code_point, k_utf8_payload_bits);
    out[3] = continuation(code_point, 0);
    return 4;
}

} // namespace

bool is_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool key_matches(const char* key, const char* text, int32_t count) {
    while (count != 0) {
        if (*key == '\0' || *text == '\0' || upper_ascii(*text) != *key)
            break;
        ++key;
        ++text;
        --count;
    }
    if (count != 0)
        return false;
    return !is_alnum(*text);
}

void decode_entities(char* text) {
    char* out = text;
    const char* in = text;
    while (*in != '\0') {
        if (*in != '&') {
            *out++ = *in++;
            continue;
        }
        const char* name = in + 1;
        // The whole reference is read before anything is written, since the
        // output can overwrite it.
        int32_t length = 0;
        char bytes[k_max_reference_bytes]{};
        int32_t count = 0;
        if (*name == '#') {
            uint32_t code_point = 0;
            const int32_t digits = read_reference_number(name + 1, &code_point);
            if (digits > 0) {
                length = 1 + digits;
                count = write_reference(code_point, bytes);
            }
        } else {
            for (const Entity& entity : k_entities) {
                length = match_name(name, entity.name);
                if (length > 0) {
                    bytes[0] = entity.value;
                    count = 1;
                    break;
                }
            }
        }
        if (length == 0) {
            *out++ = *in++;
            continue;
        }
        in = name + length;
        if (*in == ';')
            ++in;
        for (int32_t i = 0; i < count; ++i)
            *out++ = bytes[i];
    }
    *out = '\0';
}

int32_t decode_int(const char* text) {
    const auto* p = reinterpret_cast<const unsigned char*>(text);
    while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
        ++p;
    const unsigned char sign = *p;
    if (sign == '-' || sign == '+')
        ++p;
    uint32_t value = 0;
    while (*p >= '0' && *p <= '9') {
        value = value * 10u + static_cast<uint32_t>(*p - '0');
        ++p;
    }
    if (sign == '-')
        value = 0u - value;
    return static_cast<int32_t>(value);
}

void scan_attributes(char** cursor, AttributeSlot* slots, int32_t count) {
    while (**cursor != '\0' && is_alnum(**cursor))
        ++*cursor;
    for (;;) {
        const char c = **cursor;
        if (c == '\0' || c == '>') {
            if (c != '\0') {
                **cursor = '\0';
                ++*cursor;
            }
            return;
        }
        if (!is_alnum(c)) {
            ++*cursor;
            continue;
        }
        bool matched = false;
        for (int32_t i = 0; i < count; ++i) {
            AttributeSlot& slot = slots[i];
            const int32_t name_length = c_length(slot.name);
            if (!key_matches(slot.name, *cursor, name_length))
                continue;
            matched = true;
            *cursor += name_length;
            skip_spaces(cursor);
            char* at = *cursor;
            const char separator = *at;
            *cursor = at + 1;
            if (separator != '=')
                break; // rescans from the next character
            slot.value = at + 1;
            const char quote = **cursor;
            if (quote == '"' || quote == '\'') {
                slot.value = at + 2;
                ++*cursor;
                char* out = *cursor;
                for (char* in = *cursor; *in != '\0' && *in != quote; in = *cursor) {
                    if (*in == '\\' && in[1] == quote)
                        *cursor = in + 1;
                    *out++ = **cursor;
                    ++*cursor;
                }
                *out = '\0';
            } else {
                while (**cursor != '\0' &&
                       (is_alnum(**cursor) || **cursor == '+' || **cursor == '-'))
                    ++*cursor;
            }
            const char end = **cursor;
            **cursor = '\0';
            ++*cursor;
            slot.number = decode_int(slot.value);
            if (end == '>')
                return;
        }
        if (!matched) {
            while (**cursor != '\0' && is_alnum(**cursor))
                ++*cursor;
            skip_spaces(cursor);
        }
    }
}

namespace detail {

bool parse_content(Document& doc, int32_t flow, char** cursor) {
    bool in_word = false;
    char* word = nullptr;
    bool ok = true;
    for (;;) {
        char* p = *cursor;
        const char c = *p;
        if (c == '\0')
            break;
        if (is_markup_space(c)) {
            *p = '\0';
            ++*cursor;
            skip_spaces(cursor);
            in_word = false;
            doc.flows[flow].start_new_item = true;
            continue;
        }
        if (c == '<') {
            *p = '\0';
            ++*cursor;
            in_word = false;
            skip_spaces(cursor);
            if (**cursor == '/') {
                ++*cursor;
                if (**cursor == '\0' || close_tag_is_open(doc.flows[flow], *cursor)) {
                    if (**cursor != '\0') {
                        if (!close_tag_is_innermost(doc.flows[flow], *cursor)) {
                            *cursor -= 2;
                            **cursor = '<';
                        } else {
                            skip_past_tag(cursor);
                        }
                    }
                    break;
                }
            } else {
                const TagEntry* found = nullptr;
                for (const TagEntry& tag : k_tags) {
                    if (key_matches(tag.key, *cursor, tag.length)) {
                        found = &tag;
                        break;
                    }
                }
                if (found != nullptr) {
                    if (!found->handler(doc, flow, cursor, found->key)) {
                        ok = false;
                        break;
                    }
                    continue;
                }
            }
            skip_past_tag(cursor);
            continue;
        }
        if (!in_word) {
            if (word != nullptr)
                decode_entities(word);
            word = *cursor;
            start_word(doc, flow, *cursor);
            in_word = true;
        }
        ++*cursor;
    }
    if (word != nullptr)
        decode_entities(word);
    return ok;
}

void flow_reset(Document& doc, int32_t index, uint32_t flags) {
    Flow& flow = doc.flows[index];
    flow.width = -1;
    flow.height = -1;
    flow.start_new_item = false;
    flow.href = doc.flows[k_root_flow].href;
    flow.flags = flags;
    if ((flags & k_flow_inherit_font) != 0)
        flow.font_stack.push_back(flow_current_font(doc.flows[k_root_flow]));
    flow.pad_x = 0;
    flow.pad_y = 0;
}

void flow_reset_fonts(Document& doc, int32_t index) {
    const int32_t font = new_default_font(doc);
    Flow& flow = doc.flows[index];
    flow.font_stack.clear();
    flow.font_stack.push_back(font);
    flow.items.clear();
}

int32_t flow_width(const Flow& flow) {
    return flow.width;
}

int32_t flow_height(const Flow& flow) {
    return flow.height;
}

int32_t flow_pad_x(const Flow& flow) {
    return flow.pad_x;
}

int32_t flow_pad_y(const Flow& flow) {
    return flow.pad_y;
}

void flow_set_pad_x(Flow& flow, int32_t pad) {
    flow.pad_x = pad;
}

void flow_set_pad_y(Flow& flow, int32_t pad) {
    flow.pad_y = pad;
}

int32_t flow_current_font(const Flow& flow) {
    return flow.font_stack.back();
}

void flow_init_fields(Flow& flow) {
    flow = Flow{};
    flow.width = -1;
    flow.height = -1;
    flow.href = "";
    flow.table = -1;
    flow.line_break = fresh_header();
}

} // namespace detail

void document_init(Document& doc, const Environment& env) {
    doc.env = env;
    doc.buffer.clear();
    doc.flows.assign(1, Flow{});
    flow_init_fields(doc.flows[k_root_flow]);
    doc.texts.clear();
    doc.images.clear();
    doc.tables.clear();
    doc.cells.clear();
    doc.fonts.clear();
    doc.canvas = Canvas{};
    doc.has_canvas = false;
    doc.shimmer = Shimmer{};
    flow_reset(doc, k_root_flow, 0);
}

void document_set_width(Document& doc, int32_t width) {
    canvas_free(doc);
    doc.flows[k_root_flow].width = width;
    doc.flows[k_root_flow].height = -1;
}

/// Copies markup into the document buffer (NUL padded) and parses it into
/// the document flow, which first gets a fresh default font.
///
/// @param[in,out] doc document to fill
/// @param markup markup text; longer than k_max_markup_bytes parses nothing
/// @return false when the markup is too long or the parse stopped early
bool copy_and_parse(Document& doc, std::string_view markup) {
    flow_reset_fonts(doc, k_root_flow);
    if (markup.size() > k_max_markup_bytes)
        return false;
    // Padding keeps the in-place scanners, which may step one byte past a
    // terminator, inside the buffer.
    constexpr std::size_t k_padding = 8;
    doc.buffer.assign(markup.size() + k_padding, '\0');
    // An empty view can have no data pointer. The buffer is already clear.
    if (!markup.empty())
        std::memcpy(doc.buffer.data(), markup.data(), markup.size());
    char* cursor = doc.buffer.data();
    doc.flows[k_root_flow].buffer_cursor = cursor;
    const bool ok = parse_content(doc, k_root_flow, &cursor);
    doc.flows[k_root_flow].buffer_cursor = cursor;
    return ok;
}

bool document_load(Document& doc, std::string_view markup) {
    canvas_free(doc);
    const int32_t width = flow_width(doc.flows[k_root_flow]);
    doc.flows.resize(1);
    doc.texts.clear();
    doc.images.clear();
    doc.tables.clear();
    doc.cells.clear();
    doc.fonts.clear();
    flow_reset(doc, k_root_flow, 0);
    doc.flows[k_root_flow].width = width;
    doc.flows[k_root_flow].height = -1;
    return copy_and_parse(doc, markup);
}

} // namespace oa::boneyards::html
