// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The font stack: the fonts found beside the program, the sizes that match
// the game's fonts, the fallback chain, the interface faces, text that is
// not UTF-8, the limits, and, with --pixels, a short mixed line drawn pixel for pixel as the pinned
// FreeType draws it. --show TEXT prints a line as it is drawn.

#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace text_font = oa::platform::text_font;
using text_font::Face;
using text_font::FaceRole;
using text_font::Family;
using text_font::Rendering;
using text_font::Style;
using text_font::Weight;

/// The FreeType release whose drawing the pixel check pins.
constexpr std::string_view pinned_freetype = "2.14.3";

/// A style of the stack.
Style style_of(int32_t pixel_size, Weight weight, Rendering rendering = Rendering::mono) {
    Style style;
    style.pixel_size = pixel_size;
    style.weight = weight;
    style.rendering = rendering;
    return style;
}

/// The rows of a line that hold ink: the first and one past the last.
struct InkRows {
    int32_t first{};
    int32_t end{};
};

InkRows ink_rows(const text_font::Coverage& line) {
    InkRows rows{line.height, 0};
    for (int32_t row = 0; row < line.height; ++row)
        for (int32_t column = 0; column < line.width; ++column)
            if (line.alpha[static_cast<std::size_t>(row * line.width + column)] != 0) {
                rows.first = std::min(rows.first, row);
                rows.end = std::max(rows.end, row + 1);
            }
    return rows;
}

/// Rows of ink a text stands in a style.
int32_t ink_height(text_font::FontStack& stack, std::string_view text, const Style& style) {
    const auto line = stack.draw(text, style);
    OA_CHECK(line.has_value());
    if (!line)
        return 0;
    const auto rows = ink_rows(*line);
    return std::max(rows.end - rows.first, 0);
}

/// A line as text: '#' for a covered pixel, '.' for an empty one, a row a line.
std::string picture(const text_font::Coverage& line) {
    std::string out;
    for (int32_t row = 0; row < line.height; ++row) {
        for (int32_t column = 0; column < line.width; ++column)
            out.push_back(
                line.alpha[static_cast<std::size_t>(row * line.width + column)] != 0 ? '#' : '.'
            );
        out.push_back('\n');
    }
    return out;
}

/// Opens the stack beside this program, where the build copies the fonts.
std::unique_ptr<text_font::FontStack> open_beside() {
    const auto directory = text_font::bundled_font_directory();
    OA_CHECK(directory.filename() == "fonts");
    auto stack = text_font::FontStack::open(directory);
    OA_CHECK(stack != nullptr);
    return stack;
}

void decodes_only_utf8() {
    const auto mixed = text_font::decode_utf8("A\xC3\xA9\xE4\xB8\xAD\xF0\x9F\x9A\x80");
    OA_CHECK(mixed.has_value());
    if (mixed)
        OA_CHECK((*mixed == std::vector<char32_t>{U'A', 0xE9, 0x4E2D, 0x1F680}));
    OA_CHECK(text_font::decode_utf8("")->empty());
    // A lone continuation byte, a truncated sequence, an overlong slash, a
    // surrogate, a value past U+10FFFF and a lead byte no sequence has.
    for (std::string_view bad :
         {"\x80",
          "A\xE4\xB8",
          "\xC0\xAF",
          "\xE0\x80\xAF",
          "\xED\xA0\x80",
          "\xF4\x90\x80\x80",
          "\xFF",
          "\xC3("})
        OA_CHECK(!text_font::decode_utf8(bad));
}

void refuses_what_it_cannot_draw() {
    OA_CHECK(text_font::FontStack::open("no such folder") == nullptr);
    auto stack = open_beside();
    if (!stack)
        return;
    const auto bold = style_of(14, Weight::bold);
    OA_CHECK(!stack->draw("A\xE4\xB8", bold));
    OA_CHECK(!stack->draw("\xC0\xAF", bold));
    OA_CHECK(!stack->layout("\xED\xA0\x80", bold));
    OA_CHECK(!stack->draw(std::string(text_font::max_text_bytes + 1, 'A'), bold));
    OA_CHECK(stack->draw(std::string(text_font::max_text_bytes, 'A'), bold).has_value());
    OA_CHECK(!stack->draw("A", style_of(0, Weight::bold)));
    OA_CHECK(!stack->draw("A", style_of(text_font::max_pixel_size + 1, Weight::bold)));
    OA_CHECK(!stack->metrics(style_of(0, Weight::bold)));
    OA_CHECK(!stack->face_metrics(Face::dejavu_sans_bold, style_of(0, Weight::bold)));
    auto spaced = bold;
    spaced.letter_spacing = text_font::max_letter_spacing + 1;
    OA_CHECK(!stack->draw("A", spaced));
    spaced.letter_spacing = -1;
    OA_CHECK(!stack->draw("A", spaced));
    // An empty line, and one of characters that draw nothing, are as tall as
    // the style and take no room.
    for (std::string_view nothing :
         {std::string_view(""), std::string_view("\x01\xE2\x80\x8B\xEF\xB8\x8F")}) {
        const auto line = stack->draw(nothing, bold);
        OA_CHECK(line.has_value());
        if (line)
            OA_CHECK(line->width == 0 && line->advance == 0 && line->height > 0);
    }
    // The longest line is cut off at max_line_width.
    const auto widest =
        stack->draw(std::string(text_font::max_text_bytes, 'W'), style_of(64, Weight::bold));
    OA_CHECK(
        widest && widest->width == text_font::max_line_width && widest->advance > widest->width
    );
}

void falls_back_through_the_chain() {
    auto stack = open_beside();
    if (!stack)
        return;
    const auto bold = text_font::fallback_chain(Weight::bold);
    const auto regular = text_font::fallback_chain(Weight::regular);
    OA_CHECK(bold.size() == 4 && regular.size() == 3);
    OA_CHECK(
        bold[0] == Face::dejavu_sans_bold && bold[1] == Face::dejavu_sans &&
        bold[2] == Face::endonyms && bold[3] == Face::noto_emoji
    );
    OA_CHECK(
        regular[0] == Face::dejavu_sans && regular[1] == Face::endonyms &&
        regular[2] == Face::noto_emoji
    );
    OA_CHECK(stack->face_for(U'A', Weight::bold) == bold.front());
    OA_CHECK(stack->face_for(U'A', Weight::regular) == regular.front());
    OA_CHECK(stack->face_for(U'A', Weight::bold) == Face::dejavu_sans_bold);
    OA_CHECK(stack->face_for(U'A', Weight::regular) == Face::dejavu_sans);
    OA_CHECK(stack->face_for(0x0416, Weight::bold) == Face::dejavu_sans_bold); // Cyrillic Zhe
    // A mathematical letter DejaVu Sans Bold lacks comes from DejaVu Sans.
    OA_CHECK(stack->face_for(0x1D5A0, Weight::bold) == Face::dejavu_sans);
    OA_CHECK(
        stack->face_for(0x4E2D, Weight::bold) == Face::endonyms
    ); // Chinese, in the endonym face
    // Hiragana is in none of the faces that travel with the game. It draws
    // the first face's missing-glyph box.
    OA_CHECK(stack->face_for(0x3042, Weight::regular) == Face::dejavu_sans);
    OA_CHECK(stack->face_for(0x3042, Weight::bold) == Face::dejavu_sans_bold);
    OA_CHECK(stack->face_for(0x1F680, Weight::bold) == Face::noto_emoji); // rocket
    OA_CHECK(stack->face_for(0x1F680, Weight::regular) == Face::noto_emoji);
    // A character no font has draws the chain's first font's box.
    OA_CHECK(stack->face_for(0x10FFFD, Weight::bold) == Face::dejavu_sans_bold);
    OA_CHECK(stack->face_for(0x10FFFD, Weight::regular) == Face::dejavu_sans);
    OA_CHECK(!stack->face_for(0x200D, Weight::bold));
    OA_CHECK(!stack->face_for(U'\n', Weight::bold));

    const auto placed =
        stack->layout("A\xE4\xB8\xAD\xE2\x80\x8D\xF0\x9F\x9A\x80", style_of(14, Weight::bold));
    OA_CHECK(placed && placed->size() == 3);
    if (placed && placed->size() == 3) {
        OA_CHECK((*placed)[0].face == Face::dejavu_sans_bold && (*placed)[0].pen == 0);
        OA_CHECK((*placed)[1].face == Face::endonyms && (*placed)[1].pen == (*placed)[0].advance);
        OA_CHECK((*placed)[2].face == Face::noto_emoji);
        OA_CHECK((*placed)[2].pen == (*placed)[1].pen + (*placed)[1].advance);
    }
}

/// The line's rows are the greatest rows of the faces its weight looks in.
void chain_rows_match(text_font::FontStack& stack, const Style& style) {
    const auto line = stack.metrics(style);
    OA_CHECK(line.has_value());
    int32_t ascent = 0;
    int32_t descent = 0;
    for (const Face face : text_font::fallback_chain(style.weight)) {
        const auto measured = stack.face_metrics(face, style);
        OA_CHECK(measured.has_value());
        if (!measured)
            return;
        ascent = std::max(ascent, measured->ascent);
        descent = std::max(descent, measured->descent);
    }
    if (line)
        OA_CHECK(line->ascent == ascent && line->descent == descent);
}

void matches_the_game_fonts_sizes() {
    auto stack = open_beside();
    if (!stack)
        return;
    OA_CHECK(text_font::message_log_pixel_size == 14);
    OA_CHECK(text_font::status_readout_pixel_size == 11);
    OA_CHECK(text_font::label_pixel_size == 11);
    OA_CHECK(text_font::least_cjk_language_pixel_size == 12);
    // Next to hattfont12, the message log's font: capitals 10 rows and an
    // x-height of 8, from DejaVu Sans Bold at 14 px.
    const auto log = style_of(text_font::message_log_pixel_size, Weight::bold);
    OA_CHECK(ink_height(*stack, "H", log) == 10);
    OA_CHECK(ink_height(*stack, "x", log) == 8);
    // Next to CONSOLE.FNT, the chat line's and the labels' font: an x-height
    // of 6, from DejaVu Sans at 11 px.
    const auto label = style_of(text_font::label_pixel_size, Weight::regular);
    OA_CHECK(ink_height(*stack, "x", label) == 6);
    OA_CHECK(ink_height(*stack, "H", label) == 8);
    // The endonym face at 12 px beside the 14-px log font: 中 is 11 or 12 rows.
    OA_CHECK(text_font::related_pixel_size(14) == 12);
    OA_CHECK(text_font::related_pixel_size(11) == 11);
    OA_CHECK(text_font::related_pixel_size(28) == 24);
    OA_CHECK(text_font::related_pixel_size(1) == 1);
    const int32_t ideograph = ink_height(*stack, "\xE4\xB8\xAD", log);
    OA_CHECK(ideograph >= 11 && ideograph <= 12);
    // The line fits every font: it is at least as tall as the log's 14 rows.
    const auto bold_sans = stack->face_metrics(Face::dejavu_sans_bold, log);
    const auto cjk = stack->face_metrics(Face::endonyms, log);
    const auto emoji = stack->face_metrics(Face::noto_emoji, log);
    const auto regular_sans = stack->face_metrics(Face::dejavu_sans, label);
    // Rows as FreeType 2.14.3 reports them: the bold sans at 14 px, the
    // endonym and emoji faces at 12 px beside it, and the regular sans at 11 px.
    OA_CHECK(bold_sans && bold_sans->pixel_size == 14);
    OA_CHECK(bold_sans && bold_sans->ascent == 13 && bold_sans->descent == 4);
    OA_CHECK(cjk && cjk->pixel_size == 12 && cjk->ascent == 14 && cjk->descent == 4);
    OA_CHECK(emoji && emoji->pixel_size == 12 && emoji->ascent == 12 && emoji->descent == 3);
    OA_CHECK(regular_sans && regular_sans->pixel_size == 11);
    OA_CHECK(regular_sans && regular_sans->ascent == 11 && regular_sans->descent == 3);
    // A regular line still measures the bold face when asked for it.
    OA_CHECK(
        stack->face_metrics(Face::dejavu_sans_bold, label) &&
        stack->face_metrics(Face::dejavu_sans_bold, label)->pixel_size ==
            text_font::label_pixel_size
    );
    chain_rows_match(*stack, log);
    chain_rows_match(*stack, label);
    auto cjk_floor = style_of(text_font::label_pixel_size, Weight::bold);
    cjk_floor.least_cjk_pixel_size = text_font::least_cjk_language_pixel_size;
    const auto held_cjk = stack->face_metrics(Face::endonyms, cjk_floor);
    const auto held_emoji = stack->face_metrics(Face::noto_emoji, cjk_floor);
    OA_CHECK(held_cjk && held_cjk->pixel_size == 12);
    OA_CHECK(
        held_emoji &&
        held_emoji->pixel_size == text_font::related_pixel_size(text_font::label_pixel_size)
    );
    chain_rows_match(*stack, cjk_floor);
    const auto line = stack->metrics(log);
    OA_CHECK(line.has_value());
    // The line is the endonym face's rows: 14 above the baseline and 4 below.
    if (line)
        OA_CHECK(line->ascent == 14 && line->descent == 4);
    // A drawn line is at least the style's rows, its baseline at the ascent.
    const auto drawn = stack->draw("Hg", log);
    OA_CHECK(
        drawn && line && drawn->baseline == line->ascent &&
        drawn->height == line->ascent + line->descent
    );
    // H's foot sits on the row above the baseline's.
    const auto capital = stack->draw("H", log);
    OA_CHECK(capital && ink_rows(*capital).end == capital->baseline);
}

void draws_mono_and_antialiased() {
    auto stack = open_beside();
    if (!stack)
        return;
    const auto mono = stack->draw("Aa\xE4\xB8\xAD", style_of(14, Weight::bold));
    OA_CHECK(mono.has_value());
    if (mono)
        OA_CHECK(std::all_of(mono->alpha.begin(), mono->alpha.end(), [](uint8_t a) {
            return a == 0 || a == 255;
        }));
    const auto smooth =
        stack->draw("Aa\xE4\xB8\xAD", style_of(14, Weight::bold, Rendering::antialiased));
    OA_CHECK(smooth.has_value());
    if (smooth)
        OA_CHECK(std::any_of(smooth->alpha.begin(), smooth->alpha.end(), [](uint8_t a) {
            return a > 0 && a < 255;
        }));
    // Letter spacing moves every character after the first by its pixels.
    auto spaced = style_of(14, Weight::bold);
    spaced.letter_spacing = 1;
    const auto plain = stack->draw("HHH", style_of(14, Weight::bold));
    const auto wider = stack->draw("HHH", spaced);
    OA_CHECK(plain && wider && wider->advance == plain->advance + 3);
    // Bold is wider than regular, and a larger size wider than a smaller.
    const auto regular = stack->draw("HHH", style_of(14, Weight::regular));
    const auto larger = stack->draw("HHH", style_of(28, Weight::bold));
    OA_CHECK(
        plain && regular && larger && regular->advance < plain->advance &&
        larger->advance > plain->advance
    );
    // Drawn glyphs are kept for the lines that draw them again.
    const std::size_t kept = stack->cached_glyphs();
    OA_CHECK(kept > 0 && kept <= text_font::FontStack::kept_glyphs);
    const auto again = stack->draw("HHH", style_of(14, Weight::bold));
    OA_CHECK(stack->cached_glyphs() == kept);
    OA_CHECK(again && plain && again->alpha == plain->alpha);
}

/// Writes one code point as UTF-8.
std::string utf8_of(char32_t character) {
    std::string text;
    if (character < 0x80) {
        text.push_back(static_cast<char>(character));
    } else if (character < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (character >> 6)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else if (character < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (character >> 12)));
        text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (character >> 18)));
        text.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    }
    return text;
}

void keeps_the_glyphs_used_last() {
    auto stack = open_beside();
    if (!stack)
        return;
    const auto style = style_of(8, Weight::bold);
    // Lines of a hundred characters DejaVu holds, and an A in every one,
    // until more glyphs than the store keeps have been drawn. Looking a
    // character up does not draw it, so a character the faces lack is skipped
    // and never fills the store with one missing-glyph box.
    std::string first;
    char32_t next = 0x00A1;
    while (stack->drawn_glyphs() <= text_font::FontStack::kept_glyphs + 200) {
        std::string line = "A";
        for (int count = 0; count < 100; ++count) {
            std::string one;
            while (next <= 0x10FFFF) {
                one = utf8_of(next++);
                if (stack->draws(one, Weight::bold))
                    break;
                one.clear();
            }
            OA_CHECK(!one.empty());
            if (one.empty())
                return;
            if (first.empty())
                first = one;
            line += one;
        }
        OA_CHECK(stack->draw(line, style).has_value());
    }
    // The store stays within its bound and forgets a glyph at a time,
    // never starting again.
    OA_CHECK(stack->cached_glyphs() <= text_font::FontStack::kept_glyphs);
    OA_CHECK(stack->cached_glyphs() > text_font::FontStack::kept_glyphs - 101);
    // The A, used in every line, is kept; the first character, used longest
    // ago, is drawn again.
    const std::size_t drawn = stack->drawn_glyphs();
    OA_CHECK(stack->draw("A", style).has_value());
    OA_CHECK(stack->drawn_glyphs() == drawn);
    OA_CHECK(stack->draw(first, style).has_value());
    OA_CHECK(stack->drawn_glyphs() == drawn + 1);
}

void holds_ideographs_to_a_least_size() {
    auto stack = open_beside();
    if (!stack)
        return;
    const auto small = style_of(8, Weight::bold);
    auto floored = small;
    floored.least_cjk_pixel_size = 12;
    // An ideograph at 8 px is drawn at 12 px; Latin letters keep their
    // size, in a line grown to hold the ideographs.
    const auto cjk_face = stack->face_metrics(Face::endonyms, floored);
    const auto sans_face = stack->face_metrics(Face::dejavu_sans_bold, floored);
    const auto emoji_face = stack->face_metrics(Face::noto_emoji, floored);
    OA_CHECK(cjk_face && cjk_face->pixel_size == 12);
    OA_CHECK(sans_face && sans_face->pixel_size == 8);
    OA_CHECK(emoji_face && emoji_face->pixel_size == text_font::related_pixel_size(8));
    const auto plain = stack->draw("\xE4\xB8\xAD", small);
    const auto held = stack->draw("\xE4\xB8\xAD", floored);
    OA_CHECK(plain && held && plain->advance < 12 && held->advance >= 12);
    const auto latin = stack->draw("H", small);
    const auto latin_held = stack->draw("H", floored);
    OA_CHECK(
        latin && latin_held && latin->advance == latin_held->advance &&
        latin->width == latin_held->width && latin_held->height > latin->height
    );
    auto beyond = small;
    beyond.least_cjk_pixel_size = text_font::max_pixel_size + 1;
    OA_CHECK(!stack->draw("H", beyond));
}

/// A copy of the bundled fonts, removed when this dies. The stack that opened
/// it must die first: it keeps the files open.
struct TemporaryFonts {
    std::filesystem::path directory;

    TemporaryFonts() {
        static int made = 0;
        directory = std::filesystem::temp_directory_path() /
                    ("oa-text-font-" + std::to_string(++made) + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(directory);
        const auto bundled = text_font::bundled_font_directory();
        for (const std::string_view name : text_font::face_files) {
            std::error_code error;
            std::filesystem::copy_file(
                bundled / std::filesystem::path(name),
                directory / std::filesystem::path(name),
                error
            );
            OA_CHECK(!error);
        }
    }

    ~TemporaryFonts() {
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }

    TemporaryFonts(const TemporaryFonts&) = delete;
    TemporaryFonts& operator=(const TemporaryFonts&) = delete;
};

/// Covered pixels cropped to their bounding box, so two lines can be compared
/// when their heights and baselines differ by a row.
std::vector<uint8_t> covered_pixels(const text_font::Coverage& line) {
    int32_t left = line.width;
    int32_t top = line.height;
    int32_t right = 0;
    int32_t bottom = 0;
    for (int32_t row = 0; row < line.height; ++row)
        for (int32_t column = 0; column < line.width; ++column)
            if (line.alpha[static_cast<std::size_t>(row * line.width + column)] != 0) {
                left = std::min(left, column);
                top = std::min(top, row);
                right = std::max(right, column + 1);
                bottom = std::max(bottom, row + 1);
            }
    std::vector<uint8_t> pixels;
    if (left >= right || top >= bottom)
        return pixels;
    pixels.reserve(static_cast<std::size_t>(right - left) * static_cast<std::size_t>(bottom - top));
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column)
            pixels.push_back(line.alpha[static_cast<std::size_t>(row * line.width + column)]);
    return pixels;
}

/// The fixture face, copied beside the test and never into the fonts folder.
std::filesystem::path fixture_font() {
    return text_font::bundled_font_directory().parent_path() / "NotoSansCJKsc-Bold-Fixture.otf";
}

void endonyms_keep_the_cjk_rows() {
    auto stack = open_beside();
    if (!stack)
        return;
    OA_CHECK(stack->add_face(fixture_font(), FaceRole::ideographs));
    const Face fixture = text_font::pack_face(0);
    for (int32_t size = 7; size <= 48; ++size) {
        for (const Weight weight : {Weight::bold, Weight::regular}) {
            const Style style = style_of(size, weight);
            const auto cjk = stack->face_metrics(fixture, style);
            const auto endonyms = stack->face_metrics(Face::endonyms, style);
            OA_CHECK(
                cjk && endonyms && cjk->pixel_size == endonyms->pixel_size &&
                cjk->ascent == endonyms->ascent && cjk->descent == endonyms->descent
            );
        }
    }
    const auto line = stack->metrics(style_of(14, Weight::bold));
    OA_CHECK(line && line->ascent == 14 && line->descent == 4);
}

void endonyms_draw_without_the_cjk_face() {
    TemporaryFonts fonts;
    auto stack = text_font::FontStack::open(fonts.directory);
    OA_CHECK(stack != nullptr);
    if (!stack)
        return;
    OA_CHECK(stack->has_face(Face::endonyms));
    OA_CHECK(stack->draws("简体中文", Weight::bold));
    OA_CHECK(stack->face_for(0x7B80, Weight::bold) == Face::endonyms);
    OA_CHECK(!stack->draws("漢", Weight::bold));
    const auto line = stack->metrics(style_of(14, Weight::bold));
    // The endonym face keeps the rows of the face it was cut from: 14 above
    // the baseline and 4 below beside the 14 px bold face.
    OA_CHECK(line && line->ascent == 14 && line->descent == 4);
}

void pack_faces_join_the_chain() {
    TemporaryFonts fonts;
    auto stack = text_font::FontStack::open(fonts.directory);
    auto full = open_beside();
    OA_CHECK(stack != nullptr && full != nullptr);
    if (!stack || !full)
        return;
    const Style style = style_of(14, Weight::bold);
    const auto rows = stack->metrics(style);
    OA_CHECK(stack->add_face(fixture_font(), FaceRole::ideographs));
    OA_CHECK(stack->pack_face_count() == 1);
    OA_CHECK(stack->face_for(0x4E2D, Weight::bold) == text_font::pack_face(0));
    const auto from_pack = stack->draw("\xE4\xB8\xAD", style);
    const auto from_full = full->draw("\xE4\xB8\xAD", style);
    OA_CHECK(from_pack && from_full);
    if (from_pack && from_full) {
        const auto pack_ink = covered_pixels(*from_pack);
        const auto full_ink = covered_pixels(*from_full);
        OA_CHECK(!pack_ink.empty() && pack_ink == full_ink);
    }
    // A pack face does not change the line's rows.
    const auto after = stack->metrics(style);
    OA_CHECK(rows && after && rows->ascent == after->ascent && rows->descent == after->descent);
    OA_CHECK(after && after->ascent == 14 && after->descent == 4);

    OA_CHECK(stack->add_face(fonts.directory / "DejaVuSans.ttf", FaceRole::letters));
    const auto bold = stack->chain(Weight::bold);
    const auto regular = stack->chain(Weight::regular);
    OA_CHECK(bold.size() == 6 && regular.size() == 5);
    if (bold.size() == 6) {
        OA_CHECK(bold[0] == Face::dejavu_sans_bold);
        OA_CHECK(bold[1] == Face::dejavu_sans);
        OA_CHECK(bold[2] == text_font::pack_face(1));
        OA_CHECK(bold[3] == text_font::pack_face(0));
        OA_CHECK(bold[4] == Face::endonyms);
        OA_CHECK(bold[5] == Face::noto_emoji);
    }
    if (regular.size() == 5) {
        OA_CHECK(regular[0] == Face::dejavu_sans);
        OA_CHECK(regular[1] == text_font::pack_face(1));
        OA_CHECK(regular[2] == text_font::pack_face(0));
        OA_CHECK(regular[3] == Face::endonyms);
        OA_CHECK(regular[4] == Face::noto_emoji);
    }
    // U+4E2D is still the ideographs face: the letters face has no such character.
    OA_CHECK(stack->face_for(0x4E2D, Weight::bold) == text_font::pack_face(0));

    // Refused while there is room, so a full stack is not what refuses them.
    OA_CHECK(!stack->add_face(fonts.directory / "missing.otf", FaceRole::letters));
    {
        std::ofstream notes(fonts.directory / "notes.otf", std::ios::binary);
        notes << "not a font\n";
    }
    OA_CHECK(!stack->add_face(fonts.directory / "notes.otf", FaceRole::ideographs));
    OA_CHECK(stack->pack_face_count() == 2);

    OA_CHECK(stack->add_face(fonts.directory / "DejaVuSans-Bold.ttf", FaceRole::letters));
    OA_CHECK(stack->add_face(fonts.directory / "NotoEmoji.ttf", FaceRole::letters));
    OA_CHECK(stack->pack_face_count() == 4);
    OA_CHECK(!stack->add_face(fixture_font(), FaceRole::ideographs));
    OA_CHECK(stack->pack_face_count() == 4);
}

void removed_faces_leave_no_glyphs() {
    TemporaryFonts fonts;
    auto stack = text_font::FontStack::open(fonts.directory);
    auto plain = text_font::FontStack::open(fonts.directory);
    OA_CHECK(stack != nullptr && plain != nullptr);
    if (!stack || !plain)
        return;
    const Style style = style_of(14, Weight::bold);
    // U+4E00 is in the fixture face and not in the endonym face, so removing
    // the pack face leaves the missing-glyph box rather than a matching glyph.
    constexpr char32_t ideograph_character = 0x4E00;
    constexpr std::string_view traditional = "\xE4\xB8\x80";
    OA_CHECK(stack->add_face(fixture_font(), FaceRole::ideographs));
    const auto ideograph = stack->draw(traditional, style);
    const auto missing_box = plain->draw(traditional, style);
    OA_CHECK(ideograph && missing_box && stack->cached_glyphs() >= 1);
    const auto ideograph_ink = ideograph ? covered_pixels(*ideograph) : std::vector<uint8_t>{};
    stack->remove_pack_faces();
    OA_CHECK(stack->pack_face_count() == 0);
    OA_CHECK(stack->cached_glyphs() == 0);
    // The same index, a face with no U+4E00. The cached ideograph must not be drawn.
    OA_CHECK(stack->add_face(fonts.directory / "DejaVuSans.ttf", FaceRole::ideographs));
    OA_CHECK(stack->face_for(ideograph_character, Weight::bold) == Face::dejavu_sans_bold);
    const auto drawn = stack->draw(traditional, style);
    OA_CHECK(drawn && missing_box);
    if (drawn && missing_box) {
        const auto ink = covered_pixels(*drawn);
        OA_CHECK(ink == covered_pixels(*missing_box));
        OA_CHECK(ink != ideograph_ink);
    }
}

/// A style of the stack in a family.
Style family_style(int32_t pixel_size, Weight weight, Family family) {
    Style style = style_of(pixel_size, weight, Rendering::antialiased);
    style.family = family;
    return style;
}

/// The faces a line's characters are drawn from, in order.
std::vector<Face> faces_of(text_font::FontStack& stack, std::string_view text, const Style& style) {
    std::vector<Face> faces;
    if (const auto placed = stack.layout(text, style))
        for (const text_font::Placement& placement : *placed)
            faces.push_back(placement.face);
    return faces;
}

void interface_faces_go_first() {
    auto stack = open_beside();
    if (!stack)
        return;
    for (std::size_t index = 0; index < text_font::interface_face_count; ++index) {
        OA_CHECK(stack->has_face(text_font::interface_face(index)));
        OA_CHECK(text_font::is_interface_face(text_font::interface_face(index)));
    }
    OA_CHECK(!text_font::is_interface_face(Face::noto_emoji));
    OA_CHECK(!text_font::is_interface_face(text_font::pack_face(0)));
    // Each weight of the interface family puts its Barlow face first, then
    // the sans faces of the weight.
    const std::vector<Face> semibold = stack->chain(Weight::semibold, Family::interface);
    const std::vector<Face> semibold_chain{
        text_font::barlow_semibold,
        Face::dejavu_sans_bold,
        Face::dejavu_sans,
        Face::endonyms,
        Face::noto_emoji,
    };
    OA_CHECK(semibold == semibold_chain);
    OA_CHECK(stack->chain(Weight::regular, Family::interface).front() == text_font::barlow_regular);
    OA_CHECK(stack->chain(Weight::medium, Family::interface).front() == text_font::barlow_medium);
    OA_CHECK(stack->chain(Weight::bold, Family::interface).front() == text_font::barlow_bold);
    OA_CHECK(
        stack->chain(Weight::bold, Family::interface_condensed).front() ==
        text_font::barlow_condensed_bold
    );
    OA_CHECK(
        stack->chain(Weight::regular, Family::interface_condensed).front() ==
        text_font::barlow_condensed_semibold
    );
    // The sans family draws medium as regular and semibold as bold.
    OA_CHECK(stack->face_for(U'A', Weight::medium) == Face::dejavu_sans);
    OA_CHECK(stack->face_for(U'A', Weight::semibold) == Face::dejavu_sans_bold);
    OA_CHECK(stack->chain(Weight::semibold) == stack->chain(Weight::bold));
    // What Barlow lacks comes from the sans faces, the endonym face and the
    // emoji face.
    OA_CHECK(
        stack->face_for(U'A', Weight::regular, Family::interface) == text_font::barlow_regular
    );
    OA_CHECK(
        stack->face_for(0x0416, Weight::regular, Family::interface) == Face::dejavu_sans
    ); // Cyrillic Zhe
    OA_CHECK(
        stack->face_for(0x0416, Weight::semibold, Family::interface) == Face::dejavu_sans_bold
    );
    OA_CHECK(stack->face_for(0x4E2D, Weight::semibold, Family::interface) == Face::endonyms);
    OA_CHECK(stack->face_for(0x1F680, Weight::semibold, Family::interface) == Face::noto_emoji);
    OA_CHECK(stack->draws("Settings \xD0\x96 \xE4\xB8\xAD", Weight::semibold, Family::interface));
    // A word in an alphabet Barlow lacks is drawn whole in the sans faces,
    // its digits with it; ideographs leave the Latin letters in Barlow.
    const Style label = family_style(14, Weight::semibold, Family::interface);
    OA_CHECK(stack->line_family("\xD0\x96\xD0\xB4 1", label) == Family::sans);
    OA_CHECK(stack->line_family("Ab 1", label) == Family::interface);
    OA_CHECK(stack->line_family("\xFF", label) == Family::sans);
    const std::vector<Face> cyrillic = faces_of(*stack, "\xD0\x96\xD0\xB4 1", label);
    OA_CHECK(cyrillic.size() == 4);
    OA_CHECK(std::all_of(cyrillic.begin(), cyrillic.end(), [](Face face) {
        return face == Face::dejavu_sans_bold;
    }));
    OA_CHECK(faces_of(*stack, "Ab 1", label) == std::vector<Face>(4, text_font::barlow_semibold));
    const std::vector<Face> with_ideograph{
        text_font::barlow_semibold, text_font::barlow_semibold, Face::endonyms
    };
    OA_CHECK(faces_of(*stack, "Ab\xE4\xB8\xAD", label) == with_ideograph);
    // Barlow is narrower than DejaVu Sans at a size, and its condensed face
    // narrower again; the interface faces take the style's size.
    const auto width = [&](Family family, Weight weight) {
        const auto placed = stack->layout("Restore defaults", family_style(14, weight, family));
        OA_CHECK(placed && !placed->empty());
        return placed && !placed->empty() ? placed->back().pen + placed->back().advance : 0;
    };
    const int32_t sans = width(Family::sans, Weight::bold);
    const int32_t barlow = width(Family::interface, Weight::semibold);
    const int32_t condensed = width(Family::interface_condensed, Weight::bold);
    OA_CHECK(condensed > 0 && condensed < barlow && barlow < sans);
    const auto rows = stack->face_metrics(
        text_font::barlow_regular, family_style(14, Weight::regular, Family::interface)
    );
    OA_CHECK(rows && rows->pixel_size == 14);
    const auto line = stack->metrics(label);
    const auto plain = stack->metrics(family_style(14, Weight::semibold, Family::sans));
    OA_CHECK(line && plain && line->ascent >= plain->ascent && line->descent >= plain->descent);
    // A pack face leaving takes none of the interface faces' glyphs with it.
    OA_CHECK(stack->draw("Ab", label).has_value());
    const std::size_t drawn = stack->drawn_glyphs();
    stack->remove_pack_faces();
    OA_CHECK(stack->draw("Ab", label).has_value());
    OA_CHECK(stack->drawn_glyphs() == drawn);
    // A folder without the interface faces still opens, and their families
    // draw in the sans faces.
    TemporaryFonts fonts;
    auto bare = text_font::FontStack::open(fonts.directory);
    OA_CHECK(bare != nullptr);
    if (bare) {
        OA_CHECK(!bare->has_face(text_font::barlow_regular));
        OA_CHECK(
            bare->face_for(U'A', Weight::semibold, Family::interface) == Face::dejavu_sans_bold
        );
        const std::vector<Face> in_sans(2, Face::dejavu_sans_bold);
        OA_CHECK(faces_of(*bare, "Ab", label) == in_sans);
    }
}

void face_file_opens() {
    const auto bundled = text_font::bundled_font_directory();
    for (const std::string_view name : text_font::face_files)
        OA_CHECK(text_font::FontStack::face_file_opens(bundled / std::filesystem::path(name)));
    for (const std::string_view name : text_font::interface_face_files)
        OA_CHECK(text_font::FontStack::face_file_opens(bundled / std::filesystem::path(name)));
    TemporaryFonts fonts;
    {
        std::ofstream notes(fonts.directory / "notes.otf", std::ios::binary);
        notes << "not a font\n";
    }
    OA_CHECK(!text_font::FontStack::face_file_opens(fonts.directory / "notes.otf"));
    OA_CHECK(!text_font::FontStack::face_file_opens(fonts.directory / "missing.otf"));
}

/// "Ab", a Chinese character and the rocket emoji, bold at 14 px, mono:
/// the line as FreeType 2.14.3 draws it with the pinned fonts.
constexpr std::string_view mixed_line = "Ab\xE4\xB8\xAD\xF0\x9F\x9A\x80";
constexpr std::string_view mixed_picture = "................................................\n"
                                           "................................................\n"
                                           "................................................\n"
                                           "............##............##....................\n"
                                           "....###.....##............##...............###..\n"
                                           "....###.....##........##########.........##..#..\n"
                                           "...##.##....##.###....#...##..##........#.#..#..\n"
                                           "...##.##....###..##...#...##..##.......#...##...\n"
                                           "..##...##...##....##..#...##..##......#..##.#...\n"
                                           "..##...##...##....##..#...##..##...###...###....\n"
                                           "..#######...##....##..##########..#..#.#..#.....\n"
                                           ".##.....##..##....##..#...##..##...###..##......\n"
                                           ".##.....##..###..##.......##.......#######......\n"
                                           "##.......##.##.###........##.......#.####.......\n"
                                           "..........................##.......####.#.......\n"
                                           ".......................................#........\n"
                                           "................................................\n"
                                           "................................................\n";

int check_pixels() {
    if (text_font::freetype_version() != pinned_freetype) {
        std::cout << "platform-text-font --pixels: skipped, FreeType "
                  << text_font::freetype_version() << " is not the pinned " << pinned_freetype
                  << '\n';
        return 77;
    }
    auto stack = open_beside();
    if (!stack)
        return oa::test::check_exit_status();
    const auto line = stack->draw(mixed_line, style_of(14, Weight::bold));
    OA_CHECK(line.has_value());
    if (line) {
        const std::string drawn = picture(*line);
        OA_CHECK(drawn == mixed_picture);
        if (drawn != mixed_picture)
            std::cout << "drawn:\n" << drawn;
    }
    return oa::test::check_exit_status();
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--show") {
        auto stack = open_beside();
        const auto line = stack ? stack->draw(argv[2], style_of(14, Weight::bold)) : std::nullopt;
        if (line)
            std::cout << line->width << 'x' << line->height << " baseline " << line->baseline
                      << " origin " << line->origin << " advance " << line->advance << '\n'
                      << picture(*line);
        return line ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[1]) == "--pixels")
        return check_pixels();
    decodes_only_utf8();
    refuses_what_it_cannot_draw();
    falls_back_through_the_chain();
    matches_the_game_fonts_sizes();
    draws_mono_and_antialiased();
    keeps_the_glyphs_used_last();
    holds_ideographs_to_a_least_size();
    endonyms_keep_the_cjk_rows();
    endonyms_draw_without_the_cjk_face();
    pack_faces_join_the_chain();
    removed_faces_leave_no_glyphs();
    interface_faces_go_first();
    face_file_opens();
    return oa::test::check_exit_status();
}
