// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Parser, layout, hit-test and painting behaviour of the markup renderer.

#include <cstring>
#include <map>
#include <string>

#include <zlib.h>

#include "../src/internal.hpp"
#include "oa/formats/png.hpp"
#include "test_support.hpp"

using namespace html_test;

namespace {

constexpr int32_t k_space = 4;
constexpr uint8_t k_text_grey = 0xe6; // default COLORREF on the grey palette

struct MemoryFiles {
    std::map<std::string, std::vector<uint8_t>> files;
};

bool read_memory(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* files = static_cast<MemoryFiles*>(user);
    const auto it = files->files.find(path);
    if (it == files->files.end())
        return false;
    *out = it->second;
    return true;
}

struct Fixture {
    FixedBackend backend;
    MemoryFiles files;
    Document doc;

    explicit Fixture(int32_t width) {
        Environment env{};
        env.text = fixed_backend(&backend);
        env.files = FileReader{&files, read_memory};
        env.palette = grey_palette();
        document_init(doc, env);
        document_set_width(doc, width);
    }
};

std::string text_of(const Document& doc, int32_t index) {
    return doc.texts[static_cast<std::size_t>(index)].text;
}

uint8_t pixel(const Document& doc, int32_t x, int32_t y) {
    const Surface* s = document_surface(doc);
    return s->pixels[y * s->pitch + x];
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(static_cast<uint8_t>(v >> 24));
    out.push_back(static_cast<uint8_t>(v >> 16));
    out.push_back(static_cast<uint8_t>(v >> 8));
    out.push_back(static_cast<uint8_t>(v));
}

void put_chunk(std::vector<uint8_t>& out, const char* type, const std::vector<uint8_t>& body) {
    put_be32(out, static_cast<uint32_t>(body.size()));
    std::vector<uint8_t> typed(type, type + 4);
    typed.insert(typed.end(), body.begin(), body.end());
    out.insert(out.end(), typed.begin(), typed.end());
    put_be32(out, static_cast<uint32_t>(crc32(0, typed.data(), static_cast<uInt>(typed.size()))));
}

// Builds a non-interlaced PNG from already filtered scanlines.
std::vector<uint8_t> make_png(
    uint32_t width,
    uint32_t height,
    uint8_t depth,
    uint8_t color,
    const std::vector<uint8_t>& palette,
    const std::vector<uint8_t>& scanlines
) {
    std::vector<uint8_t> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<uint8_t> ihdr;
    put_be32(ihdr, width);
    put_be32(ihdr, height);
    ihdr.insert(ihdr.end(), {depth, color, 0, 0, 0});
    put_chunk(out, "IHDR", ihdr);
    if (!palette.empty())
        put_chunk(out, "PLTE", palette);
    uLongf size = compressBound(static_cast<uLong>(scanlines.size()));
    std::vector<uint8_t> packed(size);
    compress(packed.data(), &size, scanlines.data(), static_cast<uLong>(scanlines.size()));
    packed.resize(size);
    put_chunk(out, "IDAT", packed);
    put_chunk(out, "IEND", {});
    return out;
}

// 3x2 palette image; row 0 unfiltered, row 1 with the Sub filter.
std::vector<uint8_t> sample_png(bool black_first) {
    const std::vector<uint8_t> palette = {black_first ? uint8_t{0} : uint8_t{10}, 0, 0, 1, 2, 3};
    const std::vector<uint8_t> lines = {0, 20, 21, 22, 1, 30, 1, 1};
    return make_png(3, 2, 8, 3, palette, lines);
}

void test_key_matching() {
    CHECK(key_matches("B", "b>", 1));
    CHECK(!key_matches("B", "BR>", 1));
    CHECK(key_matches("BR", "br ", 2));
    CHECK(!key_matches("TD", "T", 2));
    CHECK(key_matches("TABLE", "table\n", 5));
    CHECK(is_alnum('z') && is_alnum('7') && !is_alnum('_') && !is_alnum('>'));
}

void test_entities() {
    auto decode = [](const char* input) {
        std::vector<char> buffer(input, input + std::strlen(input));
        buffer.resize(buffer.size() + 8, '\0');
        decode_entities(buffer.data());
        return std::string(buffer.data());
    };
    CHECK(decode("a&amp;b") == "a&b");
    CHECK(decode("&lt;x&gt;") == "<x>");
    CHECK(decode("&nbsp;") == " ");
    CHECK(decode("&nbsp&nbsp;x") == "  x");
    CHECK(decode("&copyx") == "\xa9x");
    CHECK(decode("&reg;") == "\x99");
    CHECK(decode("&quot;x&QUOT;") == "\"x\"");
    CHECK(decode("&AMP;&Lt;&NBSP;") == "&< ");
    // One reference at each '&': what follows a name is kept.
    CHECK(decode("&regt;") == "\x99t;");
    // An '&' that starts no reference is kept, and so is what follows it.
    CHECK(decode("a&zb") == "a&zb");
    CHECK(decode("Q&A") == "Q&A");
    CHECK(decode("a&") == "a&");
    CHECK(decode("&;") == "&;");
    // Numeric references, decimal and hexadecimal, the ';' optional.
    CHECK(decode("&#65;&#x42;&#X43") == "ABC");
    CHECK(decode("&#169;") == "\xa9");
    CHECK(decode("&#160;") == " ");
    // Windows-1252 bytes: 128-159 name them, and a character the code page
    // holds is written as its byte.
    CHECK(decode("&#150;") == "\x96");
    CHECK(decode("&#8212;&#x2122;") == "\x97\x99");
    // Any other character is written as UTF-8.
    CHECK(decode("&#20013;") == "\xe4\xb8\xad");
    CHECK(decode("&#x1F642;") == "\xf0\x9f\x99\x82");
    // A control character is dropped.
    CHECK(decode("a&#0;&#9;&#13;b") == "ab");
    // What is not a character stays as written.
    CHECK(decode("&#;&#x;") == "&#;&#x;");
    CHECK(decode("&#x110000;") == "&#x110000;");
    CHECK(decode("&#xD800;") == "&#xD800;");
    CHECK(decode("&#12345678;") == "&#12345678;");
}

void test_decode_int() {
    CHECK(decode_int(" -12") == -12);
    CHECK(decode_int("+4") == 4);
    CHECK(decode_int("12px") == 12);
    CHECK(decode_int("") == 0);
}

void test_attributes() {
    char tag[] = "FONT SIZE=\"+2\" COLOR=FF0000 FACE='A\\'B'>rest";
    char* cursor = tag;
    AttributeSlot slots[3] = {{"COLOR", nullptr, 0}, {"FACE", nullptr, 0}, {"SIZE", nullptr, 0}};
    scan_attributes(&cursor, slots, 3);
    CHECK(slots[0].value != nullptr && std::string(slots[0].value) == "FF0000");
    CHECK(slots[1].value != nullptr && std::string(slots[1].value) == "A'B");
    CHECK(slots[2].value != nullptr && std::string(slots[2].value) == "+2" && slots[2].number == 2);
    CHECK(std::string(cursor) == "rest");

    char hashed[] = "FONT COLOR=#FF0000>";
    cursor = hashed;
    AttributeSlot color = {"COLOR", nullptr, 0};
    scan_attributes(&cursor, &color, 1);
    CHECK(color.value != nullptr && color.value[0] == '\0');

    // Unquoted values stop at the first character that is not alnum, + or -.
    char dotted[] = "IMG SRC=q.png>";
    cursor = dotted;
    AttributeSlot src = {"SRC", nullptr, 0};
    scan_attributes(&cursor, &src, 1);
    CHECK(src.value != nullptr && std::string(src.value) == "q");
}

void test_runs_and_body() {
    Fixture f(100);
    CHECK(document_load(f.doc, "hidden <BODY>ab<B>cd</B> ef&amp;g</BODY> tail"));
    const Flow& root = f.doc.flows[0];
    CHECK(root.items.size() == 2);
    CHECK(f.doc.texts.size() == 3);
    CHECK(text_of(f.doc, 0) == "ab" && text_of(f.doc, 1) == "cd" && text_of(f.doc, 2) == "ef&g");
    CHECK(root.items[0].kind == NodeKind::text && root.items[0].index == 0);
    CHECK(f.doc.texts[0].head.next.kind == NodeKind::text && f.doc.texts[0].head.next.index == 1);
    CHECK(f.doc.fonts[static_cast<std::size_t>(f.doc.texts[1].font)].weight == k_weight_bold);
    CHECK(f.doc.fonts[static_cast<std::size_t>(f.doc.texts[2].font)].weight == k_weight_normal);
}

// Spaces, tabs, line feeds, form feeds and carriage returns separate words
// and make none; a page saved with CR LF line ends reads as one saved with LF.
void test_whitespace() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY>\r\n\r\nab\r\n\t\f cd\r\n&nbsp;<BR>\r\n</BODY>\r\n"));
    const Flow& root = f.doc.flows[0];
    CHECK(f.doc.texts.size() == 3);
    CHECK(text_of(f.doc, 0) == "ab" && text_of(f.doc, 1) == "cd" && text_of(f.doc, 2) == " ");
    CHECK(root.items.size() == 4 && root.items[3].kind == NodeKind::line_break);

    Fixture lf(100);
    CHECK(document_load(lf.doc, "<BODY>\nab\ncd\n&nbsp;<BR>\n</BODY>\n"));
    CHECK(document_height(f.doc) == document_height(lf.doc));
    CHECK(f.doc.texts[1].head.x == lf.doc.texts[1].head.x);
    CHECK(f.doc.texts[2].head.y == lf.doc.texts[2].head.y);

    // A carriage return inside a tag separates its attributes.
    Fixture tag(100);
    CHECK(document_load(tag.doc, "<BODY><FONT\r\nSIZE=\"+2\"\r\nCOLOR=\"FF0000\">a</FONT></BODY>"));
    CHECK(tag.doc.texts.size() == 1);
    const MarkupFont& font = tag.doc.fonts[static_cast<std::size_t>(tag.doc.texts[0].font)];
    CHECK(font.height == -16 && font.colorref == 0x000000ffu);
}

void test_breaks() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY>a<BR>b<P>c</P></BODY>"));
    const Flow& root = f.doc.flows[0];
    CHECK(root.items.size() == 6);
    CHECK(
        root.items[1].kind == NodeKind::line_break && root.items[3].kind == NodeKind::line_break &&
        root.items[4].kind == NodeKind::line_break
    );
    CHECK(document_height(f.doc) == 44);
    CHECK(f.doc.texts[2].head.y == 47);
}

void test_fonts() {
    Fixture f(200);
    CHECK(document_load(
        f.doc,
        "<BODY><FONT SIZE=+2>a</FONT><FONT SIZE=3 COLOR=\"FF8000\">b</FONT>"
        "<FONT SIZE=-1 FACE=Courier>c</FONT><H1>x</H1><H2>y</H2><H3>z</H3></BODY>"
    ));
    auto font = [&](int32_t text) {
        return f.doc.fonts[static_cast<std::size_t>(f.doc.texts[text].font)];
    };
    CHECK(font(0).height == -16);
    CHECK(font(1).height == -12 && font(1).colorref == 0x000080ffu);
    CHECK(font(2).height == -10 && std::string(font(2).face) == "Courier");
    CHECK(font(3).height == -17 && font(3).weight == k_weight_bold && !font(3).italic);
    CHECK(font(4).height == -15 && font(4).weight == k_weight_normal && font(4).italic);
    CHECK(font(5).height == -13 && font(5).weight == k_weight_bold);
    CHECK(
        std::string(font(0).face) == k_default_face && font(0).colorref == k_default_text_colorref
    );
}

void test_close_tags() {
    Fixture f(100);
    // </BODY> inside B rewinds so BODY closes; the tail is outside BODY.
    CHECK(document_load(f.doc, "<BODY><B>x</BODY> y"));
    CHECK(f.doc.texts.size() == 1 && text_of(f.doc, 0) == "x");
    // Unknown tags are skipped without breaking the run.
    CHECK(document_load(f.doc, "<BODY>a<SPAN>b</SPAN> c</BODY>"));
    CHECK(f.doc.flows[0].items.size() == 2 && f.doc.texts[0].head.next.index == 1);
    // U underlines its content, which joins the run like B and I.
    CHECK(document_load(f.doc, "<BODY>a<U>b</U> c</BODY>"));
    CHECK(f.doc.flows[0].items.size() == 2 && f.doc.texts[0].head.next.index == 1);
    CHECK(!f.doc.fonts[static_cast<std::size_t>(f.doc.texts[0].font)].underline);
    CHECK(f.doc.fonts[static_cast<std::size_t>(f.doc.texts[1].font)].underline);
    CHECK(!f.doc.fonts[static_cast<std::size_t>(f.doc.texts[2].font)].underline);
    // A close tag of an element that is not open, such as </IMG>, is dropped.
    CHECK(document_load(f.doc, "<BODY>a</IMG> b</BODY>"));
    CHECK(f.doc.texts.size() == 2 && text_of(f.doc, 1) == "b");
    // TD outside a table stops the parse.
    CHECK(!document_load(f.doc, "<BODY><TD>a</TD></BODY>"));
}

void test_line_layout() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY>ab cd</BODY>"));
    CHECK(document_height(f.doc) == 14);
    CHECK(f.doc.texts[0].head.x == 0 && f.doc.texts[0].head.y == 17);
    CHECK(f.doc.texts[1].head.x == 12 + k_space && f.doc.texts[1].head.y == 17);

    Fixture narrow(20);
    CHECK(document_load(narrow.doc, "<BODY>ab cd</BODY>"));
    CHECK(document_height(narrow.doc) == 28);
    CHECK(narrow.doc.texts[1].head.x == 0 && narrow.doc.texts[1].head.y == 31);

    Fixture margins(100);
    CHECK(document_load(margins.doc, "<BODY TOPMARGIN=5 LEFTMARGIN=7>ab</BODY>"));
    CHECK(document_height(margins.doc) == 14 + 10);
    CHECK(margins.doc.texts[0].head.x == 7 && margins.doc.texts[0].head.y == 22);
}

void test_center_and_align() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY><CENTER>ab</CENTER></BODY>"));
    CHECK(document_height(f.doc) == 20);
    CHECK(f.doc.tables.size() == 1 && f.doc.tables[0].column_widths[0] == 100);
    CHECK(f.doc.texts[0].head.x == (100 - (12 + k_space)) / 2 && f.doc.texts[0].head.y == 19);

    Fixture right(100);
    CHECK(document_load(right.doc, "<BODY><TABLE><TR><TD ALIGN=RIGHT>ab</TD></TR></TABLE></BODY>"));
    document_height(right.doc);
    CHECK(right.doc.texts[0].head.x == 100 - (12 + k_space));
}

void test_table_layout() {
    Fixture f(100);
    CHECK(document_load(
        f.doc,
        "<BODY><TABLE><TR><TD WIDTH=30>ab</TD><TD>cd</TD></TR>"
        "<TR><TD>e</TD><TD BGCOLOR=\"00FF00\">f</TD></TR></TABLE></BODY>"
    ));
    const Table& table = f.doc.tables[0];
    CHECK(table.rows == 2 && table.columns == 2 && table.cells.size() == 4);
    CHECK(
        table.column_widths.size() == 2 && table.column_widths[0] == 30 &&
        table.column_widths[1] == 69
    );
    // TD BGCOLOR reads hex letters as 'A' = 0.
    CHECK(f.doc.cells[3].has_background && f.doc.cells[3].background_rgb == 0x005500u);
    CHECK(document_height(f.doc) == 39);
    CHECK(f.doc.tables[0].row_heights[0] == 18 && f.doc.tables[0].row_heights[1] == 18);
    CHECK(f.doc.texts[0].head.x == 0 && f.doc.texts[0].head.y == 19);
    CHECK(f.doc.texts[1].head.x == 31 && f.doc.texts[1].head.y == 19);
    CHECK(f.doc.texts[2].head.x == 0 && f.doc.texts[2].head.y == 38);
    CHECK(f.doc.texts[3].head.x == 31 && f.doc.texts[3].head.y == 38);
}

void test_links() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY><A HREF=\"x.htm\">go</A> no</BODY>"));
    document_height(f.doc);
    const char* hit = document_link_at(f.doc, 5, 10);
    CHECK(hit != nullptr && std::string(hit) == "x.htm");
    CHECK(document_link_at(f.doc, 20, 10) == nullptr);
    CHECK(document_link_at(f.doc, 5, 20) == nullptr);
    CHECK(f.doc.fonts[static_cast<std::size_t>(f.doc.texts[0].font)].underline);
    CHECK(std::string(f.doc.texts[1].head.href) == "");

    Fixture cell(100);
    CHECK(document_load(
        cell.doc, "<BODY><TABLE><TR><TD><A HREF='t.htm'>ln</A></TD></TR></TABLE></BODY>"
    ));
    CHECK(document_height(cell.doc) == 20);
    const char* nested = document_link_at(cell.doc, 3, 10);
    CHECK(nested != nullptr && std::string(nested) == "t.htm");
    CHECK(document_link_at(cell.doc, 50, 10) == nullptr);
}

void test_paint() {
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY>ab cd</BODY>"));
    CHECK(document_paint(f.doc));
    const Surface* s = document_surface(f.doc);
    CHECK(s != nullptr && s->width == 100 && s->height == 14 && s->pitch == 100);
    CHECK(f.backend.calls.size() == 2);
    CHECK(
        f.backend.calls[0].text == "ab" && f.backend.calls[0].x == 0 && f.backend.calls[0].y == 2
    );
    CHECK(f.backend.calls[1].x == 16 && f.backend.calls[1].color == k_text_grey);
    CHECK(pixel(f.doc, 0, 2) == k_text_grey && pixel(f.doc, 13, 2) == k_canvas_clear_index);
    CHECK(pixel(f.doc, 0, 1) == k_canvas_clear_index && pixel(f.doc, 16, 13) == k_text_grey);

    Fixture odd(10);
    CHECK(document_load(odd.doc, "<BODY>a</BODY>"));
    document_paint(odd.doc);
    CHECK(document_surface(odd.doc)->pitch == 12);

    Fixture bg(40);
    CHECK(
        document_load(bg.doc, "<BODY><TABLE><TR><TD BGCOLOR=\"404040\"> </TD></TR></TABLE></BODY>")
    );
    document_paint(bg.doc);
    CHECK(pixel(bg.doc, 5, 2) == 0x40);
}

struct NoticeLog {
    std::vector<std::string> titles;
    std::vector<std::string> texts;
};

void record_notice(void* user, const char* title, const char* text) {
    auto* log = static_cast<NoticeLog*>(user);
    log->titles.emplace_back(title);
    log->texts.emplace_back(text);
}

// Loads `png` as the only IMG of a fresh document, recording notices.
const Image& load_single_image(Fixture& f, const std::vector<uint8_t>& png, NoticeLog* log) {
    f.files.files["p.png"] = png;
    f.doc.env.notices = Notifier{log, record_notice};
    CHECK(document_load(f.doc, "<BODY><IMG SRC=\"p.png\"></BODY>"));
    CHECK(f.doc.images.size() == 1);
    return f.doc.images[0];
}

std::vector<uint8_t>
encode_png(const oa::formats::png::Header& header, const std::vector<uint8_t>& rows) {
    std::vector<uint8_t> file;
    CHECK(oa::formats::png::write(oa::formats::png::Image{header, {}, rows}, &file));
    return file;
}

void test_png_loading() {
    NoticeLog log;
    Fixture palette(40);
    const std::vector<uint8_t> indexed_png = sample_png(true);
    const Image& indexed = load_single_image(palette, indexed_png, &log);
    CHECK(indexed.loaded && indexed.raw_indices && indexed.row_bytes == 3);
    CHECK(indexed.source_width == 3 && indexed.source_height == 2 && indexed.width == 3);
    CHECK((indexed.pixels == std::vector<uint8_t>{20, 21, 22, 30, 31, 32}));
    CHECK(log.texts.empty());

    // Sub-byte samples are widened without scaling.
    Fixture bits(40);
    const std::vector<uint8_t> grey_png = make_png(8, 1, 1, 0, {}, {0, 0xb1});
    const Image& grey = load_single_image(bits, grey_png, &log);
    CHECK(grey.loaded && !grey.raw_indices);
    CHECK((grey.pixels == std::vector<uint8_t>{1, 0, 1, 1, 0, 0, 0, 1}));

    // 16-bit samples keep their high byte.
    Fixture deep(40);
    const std::vector<uint8_t> wide_png =
        encode_png({2, 1, 16, oa::formats::png::ColorType::grey}, {9, 1, 7, 2});
    const Image& wide = load_single_image(deep, wide_png, &log);
    CHECK(wide.loaded && wide.row_bytes == 2 && (wide.pixels == std::vector<uint8_t>{9, 7}));

    // RGB rows keep three bytes a pixel and draw their first bytes as
    // indices; the top row lands one line above the image's bottom edge.
    Fixture rgb(40);
    const std::vector<uint8_t> rgb_rows = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120};
    const std::vector<uint8_t> colour_png =
        encode_png({2, 2, 8, oa::formats::png::ColorType::rgb}, rgb_rows);
    const Image& colour = load_single_image(rgb, colour_png, &log);
    CHECK(colour.loaded && colour.row_bytes == 6 && colour.pixels == rgb_rows);
    document_paint(rgb.doc);
    const int32_t top = colour.head.y - 1;
    CHECK(pixel(rgb.doc, 0, top) == 0xff - 10 && pixel(rgb.doc, 1, top) == 0xff - 20);
    CHECK(pixel(rgb.doc, 2, top) == k_canvas_clear_index);

    // Larger than 256 on a side: the size is taken but nothing is decoded.
    Fixture large(40);
    const std::vector<uint8_t> big_png =
        encode_png({257, 1, 8, oa::formats::png::ColorType::grey}, std::vector<uint8_t>(257));
    const Image& big = load_single_image(large, big_png, &log);
    CHECK(!big.loaded && big.pixels.empty() && big.source_width == 257 && big.width == 257);
    CHECK(log.texts.empty());

    // A header CRC error stops before the size is known, with a notice.
    Fixture corrupt_header(40);
    auto corrupt = sample_png(true);
    corrupt[24] ^= 0xff;
    const Image& broken = load_single_image(corrupt_header, corrupt, &log);
    CHECK(!broken.loaded && broken.pixels.empty() && broken.source_width == 0);
    CHECK(log.titles.size() == 1 && log.titles[0] == k_png_notice_title);
    CHECK(log.texts.size() == 1 && log.texts[0] == "p.png: libpng warning: IHDR: CRC error\n");

    // Not a PNG: the loader fails without a notice.
    log = {};
    Fixture junk(40);
    junk.files.files["p.png"] = {1, 2, 3, 4};
    junk.doc.env.notices = Notifier{&log, record_notice};
    CHECK(document_load(junk.doc, "<BODY><IMG SRC=\"p.png\"></BODY>"));
    CHECK(!image_load_png(junk.doc, 0, "p.png", 0, 0, nullptr));
    CHECK(log.texts.empty());

    // Image data that stops early keeps the rows it held; the image stays
    // pending.
    oa::formats::png::Header strip{4, 4, 8, oa::formats::png::ColorType::grey};
    std::vector<uint8_t> strip_rows(16);
    for (std::size_t i = 0; i < strip_rows.size(); ++i)
        strip_rows[i] = static_cast<uint8_t>(i + 1);
    const auto whole = encode_png(strip, strip_rows);
    // Two rows, each a filter byte of 0 and four samples.
    std::vector<uint8_t> scanlines(2 * 5, 0);
    for (uint32_t row = 0; row < 2; ++row)
        std::copy(
            strip_rows.begin() + row * 4,
            strip_rows.begin() + row * 4 + 4,
            scanlines.begin() + row * 5 + 1
        );
    Fixture partial(40);
    const std::vector<uint8_t> half_png = make_png(4, 4, 8, 0, {}, scanlines);
    const Image& half = load_single_image(partial, half_png, &log);
    CHECK(!half.loaded && half.pixels.size() == 16);
    CHECK(half.pixels[7] == 8 && half.pixels[8] == 0);
    CHECK(
        (log.texts == std::vector<std::string>{"p.png: libpng warning: Not enough image data\n"})
    );
    std::vector<const char*> pending;
    CHECK(document_pending_images(partial.doc, &pending) && pending.size() == 1);

    // An error after the last row still counts the image as loaded.
    log = {};
    auto late = whole;
    const std::vector<uint8_t> iend(late.end() - 12, late.end());
    late.resize(late.size() - 12);
    put_chunk(late, "IDAT", {});
    late.insert(late.end(), iend.begin(), iend.end());
    Fixture after(40);
    const Image& done = load_single_image(after, late, &log);
    CHECK(done.loaded && done.pixels == strip_rows);
    CHECK(
        (log.texts == std::vector<std::string>{"p.png: libpng warning: Too many IDAT's found\n"})
    );

    // A file cut inside its only IDAT still gives its size and rows: the
    // reader stops at that chunk's header before the image data.
    log = {};
    auto cut = whole;
    cut.resize(cut.size() - 12 - 6);
    Fixture truncated(40);
    const Image& short_file = load_single_image(truncated, cut, &log);
    CHECK(short_file.source_width == 4 && short_file.width == 4 && short_file.height == 4);
    CHECK(!short_file.loaded && short_file.pixels.size() == 16);
    CHECK((log.texts == std::vector<std::string>{"p.png: libpng warning: Read Error\n"}));
    Fixture intact(40);
    load_single_image(intact, whole, &log);
    CHECK(document_height(truncated.doc) == document_height(intact.doc));

    // Without a file name the notice names an unknown error.
    log = {};
    Environment env{};
    env.notices = Notifier{&log, record_notice};
    png_decoder_error(env, nullptr, "Read Error");
    CHECK(
        (log.texts == std::vector<std::string>{"UNKNOWN (ERROR!): libpng warning: Read Error\n"})
    );
}

void test_images() {
    Fixture f(40);
    f.files.files["p.png"] = sample_png(true);
    f.files.files["q.png"] = sample_png(false);
    CHECK(document_load(
        f.doc,
        "<BODY><IMG SRC=\"p.png\"> <IMG SRC=\"q.png\">"
        "<IMG SRC=\"http://x/y.png\"></BODY>"
    ));
    CHECK(f.doc.images.size() == 2 && f.doc.images[0].loaded && f.doc.images[0].raw_indices);
    CHECK(!f.doc.images[1].raw_indices && f.doc.images[1].width == 3);
    std::vector<const char*> pending;
    CHECK(!document_pending_images(f.doc, &pending));
    // Images sit on the baseline and are copied bottom row first.
    CHECK(document_height(f.doc) == 4);
    document_paint(f.doc);
    CHECK(f.doc.images[0].head.y == 4);
    CHECK(pixel(f.doc, 0, 3) == 20 && pixel(f.doc, 2, 3) == 22);
    // No text was measured, so the stock space width is still 0.
    CHECK(f.doc.images[1].head.x == 3 && pixel(f.doc, 3, 3) == 0xff - 20);

    Fixture missing(40);
    missing.files.files["p.png"] = sample_png(true);
    CHECK(document_load(missing.doc, "<BODY><IMG WIDTH=8 HEIGHT=6 SRC=\"gone.png\"></BODY>"));
    CHECK(
        document_pending_images(missing.doc, &pending) && pending.size() == 1 &&
        std::string(pending[0]) == "gone.png"
    );
    CHECK(document_height(missing.doc) == 8);
    document_paint(missing.doc);
    // The placeholder paints its corners and interior from the shimmer ramp.
    CHECK(pixel(missing.doc, 0, 2) != k_canvas_clear_index);
    CHECK(pixel(missing.doc, 7, 7) != k_canvas_clear_index);
    CHECK(missing.doc.shimmer.frame == 1);
    document_reload_image(missing.doc, "gone.png", "p.png");
    CHECK(missing.doc.images[0].loaded && missing.doc.images[0].width == 8);
    CHECK(std::string(missing.doc.images[0].name) == "p.png");
}

void test_shimmer_primitives() {
    uint32_t seed = 1;
    CHECK(shimmer_random(&seed) == 16807u);
    CHECK(shimmer_random(&seed) == 282475249u);
    CHECK(shimmer_random(&seed) == 1622650073u);
    const Palette grey = grey_palette();
    CHECK(ramp_palette_index(grey, 32, 32, 32) == 32);
    CHECK(ramp_palette_index(grey, 40, 40, 255) == 112);
    CHECK(ramp_palette_index(grey, 0, 0, 0) == 0x10);
}

void test_markup_palette() {
    Palette game = grey_palette();
    const Palette p = markup_palette(game);
    CHECK(p.entries[0].r == 0 && p.entries[255].r == 255 && p.entries[100].g == 100);
    CHECK(p.entries[1].g == 0xff && p.entries[1].r == 0 && p.entries[15].g == 0xff);
    CHECK(p.entries[9].r == 0xff && p.entries[9].g == 0 && p.entries[9].b == 0xff);
    CHECK(p.entries[240].g == 0xff && p.entries[254].g == 0xff && p.entries[16].r == 16);
}

void test_reload_keeps_width() {
    Fixture f(60);
    CHECK(document_load(f.doc, "<BODY>ab</BODY>"));
    document_paint(f.doc);
    CHECK(document_load(f.doc, "<BODY>cd ef</BODY>"));
    CHECK(document_surface(f.doc) == nullptr && f.doc.flows[0].width == 60);
    CHECK(f.doc.texts.size() == 2 && text_of(f.doc, 1) == "ef");
    CHECK(document_height(f.doc) == 14);
}

NodeRef first_item(const Document& doc, NodeKind kind) {
    for (const NodeRef& item : doc.flows[0].items)
        if (item.kind == kind)
            return item;
    return NodeRef{};
}

// Behaviour the break, image, table and cell nodes share with the base node:
// -1 for a missing raw extent, the raw height as the box height and the box
// height as the ascent.
void test_base_node_defaults() {
    namespace detail = oa::boneyards::html::detail;
    Fixture f(100);
    f.files.files["p.png"] = sample_png(true);
    CHECK(document_load(
        f.doc, "<BODY>ab<BR><IMG SRC=\"p.png\"><TABLE><TR><TD>c</TD></TR></TABLE></BODY>"
    ));
    const NodeRef line_break = first_item(f.doc, NodeKind::line_break);
    const NodeRef image = first_item(f.doc, NodeKind::image);
    const NodeRef table = first_item(f.doc, NodeKind::table);
    const NodeRef cell{NodeKind::cell, 0};
    CHECK(line_break.kind == NodeKind::line_break && image.kind == NodeKind::image);
    CHECK(table.kind == NodeKind::table && f.doc.cells.size() == 1);

    CHECK(detail::node_width_raw(f.doc, line_break) == -1);
    CHECK(node_width(f.doc, line_break) == -1);
    CHECK(node_height(f.doc, line_break) == 0);
    CHECK(detail::node_ascent(f.doc, line_break) == 0);

    CHECK(node_height(f.doc, image) == 2 && detail::node_ascent(f.doc, image) == 2);

    CHECK(detail::node_height_raw(f.doc, table) == -1);
    CHECK(detail::node_height_raw(f.doc, cell) == -1);
    const int32_t table_height = node_height(f.doc, table);
    const int32_t cell_height = node_height(f.doc, cell);
    CHECK(table_height > 0 && detail::node_ascent(f.doc, table) == table_height);
    CHECK(cell_height > 0 && detail::node_ascent(f.doc, cell) == cell_height);
}

// The flow's field accessors: width (flow_width), height (flow_height),
// insets (flow_pad_x/flow_pad_y, stored by BODY
// through flow_set_pad_x/flow_set_pad_y only when positive) and the
// innermost font (flow_current_font); a TR HEIGHT reaches its cells through
// cell_set_height.
// BODY BGCOLOR is accepted and does not paint the page. VALIGN is stored on
// the cell's flow; the lines stay at the top of a taller row.
void test_body_color_and_valign() {
    Fixture plain(80);
    CHECK(document_load(plain.doc, "<BODY>ab</BODY>"));
    document_paint(plain.doc);
    const int32_t plain_y = plain.doc.texts[0].head.y;

    Fixture colored(80);
    CHECK(document_load(colored.doc, "<BODY BGCOLOR=\"FF0000\">ab</BODY>"));
    document_paint(colored.doc);
    CHECK(colored.doc.texts[0].head.y == plain_y);
    CHECK(pixel(colored.doc, 0, 0) == k_canvas_clear_index);
    CHECK(pixel(colored.doc, 40, 6) == k_canvas_clear_index);
    CHECK(pixel(colored.doc, 0, 2) == k_text_grey);

    auto placed = [](const char* html, uint32_t* flags) {
        Fixture f(80);
        CHECK(document_load(f.doc, html));
        document_height(f.doc);
        const Cell& cell = f.doc.cells[0];
        *flags = f.doc.flows[static_cast<std::size_t>(cell.flow)].flags;
        return f.doc.texts[0].head.y - (cell.head.y - cell.height);
    };
    uint32_t top_flags = 0;
    uint32_t center_flags = 0;
    uint32_t bottom_flags = 0;
    const int32_t top =
        placed("<BODY><TABLE><TR HEIGHT=40><TD VALIGN=TOP>ab</TD></TR></TABLE></BODY>", &top_flags);
    const int32_t center = placed(
        "<BODY><TABLE><TR HEIGHT=40><TD VALIGN=center>ab</TD></TR></TABLE></BODY>", &center_flags
    );
    const int32_t bottom = placed(
        "<BODY><TABLE><TR HEIGHT=40><TD VALIGN=\"BOTTOM\">ab</TD></TR></TABLE></BODY>",
        &bottom_flags
    );
    CHECK(center == top && bottom == top);
    CHECK(top > 0 && top < 20);
    CHECK((top_flags & (k_flow_valign_center | k_flow_valign_bottom)) == 0);
    CHECK((center_flags & k_flow_valign_center) != 0);
    CHECK((center_flags & k_flow_valign_bottom) == 0);
    CHECK((bottom_flags & k_flow_valign_bottom) != 0);
    CHECK((bottom_flags & k_flow_valign_center) == 0);
}

void test_flow_fields() {
    namespace detail = oa::boneyards::html::detail;
    Fixture f(100);
    CHECK(document_load(f.doc, "<BODY TOPMARGIN=5 LEFTMARGIN=7>ab</BODY>"));
    const Flow& root = f.doc.flows[detail::k_root_flow];
    CHECK(detail::flow_width(root) == 100 && detail::flow_height(root) == -1);
    CHECK(detail::flow_pad_y(root) == 5 && detail::flow_pad_x(root) == 7);
    CHECK(document_height(f.doc) == 24 && detail::flow_height(root) == 24);
    CHECK(detail::flow_current_font(root) == f.doc.texts[0].font);

    Fixture ignored(100);
    CHECK(document_load(ignored.doc, "<BODY TOPMARGIN=0 LEFTMARGIN=-3>ab</BODY>"));
    const Flow& plain = ignored.doc.flows[detail::k_root_flow];
    CHECK(detail::flow_pad_y(plain) == 0 && detail::flow_pad_x(plain) == 0);

    Fixture fonts(100);
    CHECK(document_load(fonts.doc, "<BODY>a<B>b</B></BODY>"));
    CHECK(fonts.doc.texts[1].font != fonts.doc.texts[0].font);
    const Flow& styled = fonts.doc.flows[detail::k_root_flow];
    CHECK(detail::flow_current_font(styled) == fonts.doc.texts[0].font);

    Fixture rows(100);
    CHECK(document_load(rows.doc, "<BODY><TABLE><TR HEIGHT=30><TD>ab</TD></TR></TABLE></BODY>"));
    CHECK(rows.doc.cells.size() == 1 && rows.doc.cells[0].height == -1);
    document_height(rows.doc);
    CHECK(rows.doc.tables[0].row_heights[0] == 30 && rows.doc.cells[0].height == 30);
    detail::cell_set_height(rows.doc, 0, -1);
    CHECK(node_height(rows.doc, NodeRef{NodeKind::cell, 0}) == 18);
}

} // namespace

int main() {
    test_key_matching();
    test_entities();
    test_decode_int();
    test_attributes();
    test_runs_and_body();
    test_whitespace();
    test_breaks();
    test_fonts();
    test_close_tags();
    test_line_layout();
    test_center_and_align();
    test_table_layout();
    test_links();
    test_paint();
    test_png_loading();
    test_images();
    test_shimmer_primitives();
    test_markup_palette();
    test_reload_keeps_width();
    test_base_node_defaults();
    test_body_color_and_valign();
    test_flow_fields();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("markup tests passed");
    return 0;
}
