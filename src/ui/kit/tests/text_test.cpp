// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// UTF-8 helpers, the estimated width, the stand-in characters, and the one
// wrap checked against copies of the six wraps it replaces; fit and
// fitting_start against the Game files screen's copies, path_break against
// the folder chooser's, and the modern fonts' measure through its hooks.

#include "oa/base/text/line_break.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/paint/painter.hpp"

#include <algorithm>
#include <cstdio>
#include <functional>
#include <stdint.h>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace text_font = oa::platform::text_font;

/// The ellipsis the painter and the Game files screen end a shortened line with.
constexpr std::string_view ellipsis = "\xE2\x80\xA6";
/// The three full stops a mod question ends a shortened line with.
constexpr std::string_view dots = "...";

// Copies of the six wraps as they were before the kit, compared with the
// kit's wrap.
namespace before_kit {

using Measure = std::function<int32_t(std::string_view)>;

/// Returns the bytes of the UTF-8 character a text starts with, by the bytes
/// that continue it.
///
/// @param text the text, not empty
/// @return 1 to 4; 1 for a byte that starts no sequence
std::size_t continued_bytes(std::string_view text) noexcept {
    std::size_t bytes = 1;
    while (bytes < text.size() && bytes < 4 &&
           (static_cast<unsigned char>(text[bytes]) & 0xC0U) == 0x80U)
        ++bytes;
    return bytes;
}

/// Tells whether a byte continues a UTF-8 character rather than starting one.
///
/// @param byte the byte
/// @return true for 10xxxxxx
bool continuing(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

/// Returns the place after the UTF-8 character that starts at a place, by the
/// lead byte's length.
///
/// @param text the text
/// @param at the character's first byte
/// @return the place after the character
std::size_t next_character(std::string_view text, std::size_t at) noexcept {
    if (at >= text.size())
        return text.size();
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t length = 1;
    if ((lead & 0xe0u) == 0xc0u)
        length = 2;
    else if ((lead & 0xf0u) == 0xe0u)
        length = 3;
    else if ((lead & 0xf8u) == 0xf0u)
        length = 4;
    return std::min(text.size(), at + length);
}

/// Returns the length in bytes of the UTF-8 character a byte starts.
///
/// @param lead the first byte
/// @return 1 to 4
std::size_t utf8_length(uint8_t lead) noexcept {
    if (lead >= 0xF0U)
        return 4;
    if (lead >= 0xE0U)
        return 3;
    if (lead >= 0xC0U)
        return 2;
    return 1;
}

/// Breaks a text that is wider than a line between its characters.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @param[in,out] lines receives every line but the last
/// @return the last line, which may take more after it
std::string break_characters(
    std::string_view text, int32_t width, const Measure& measure, std::vector<std::string>& lines
) {
    std::string line;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = continued_bytes(text.substr(at));
        const std::string_view character = text.substr(at, bytes);
        if (!line.empty() && measure(line + std::string(character)) > width) {
            lines.push_back(line);
            line.clear();
        }
        line += character;
        at += bytes;
    }
    return line;
}

/// Breaks a text at spaces, and a wide script between its characters.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @return the lines
std::vector<std::string> notice_wrap(std::string_view text, int32_t width, const Measure& measure) {
    std::vector<std::string> lines;
    if (oa::base::text::has_wide_script(text)) {
        const auto fits = [&](std::string_view start) { return measure(start) <= width; };
        for (;;) {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);
            if (text.empty())
                break;
            const auto row = oa::base::text::first_row(text, fits);
            lines.emplace_back(text.substr(0, row.bytes));
            text.remove_prefix(row.next);
        }
        return lines;
    }
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        if (text[at] == ' ') {
            ++at;
            continue;
        }
        const auto end = std::min(text.find(' ', at), text.size());
        const std::string_view word = text.substr(at, end - at);
        at = end;
        const std::string joined =
            line.empty() ? std::string(word) : line + " " + std::string(word);
        if (measure(joined) <= width) {
            line = joined;
            continue;
        }
        if (!line.empty())
            lines.push_back(line);
        line = measure(word) <= width ? std::string(word)
                                      : break_characters(word, width, measure, lines);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

/// Breaks a path after each separator, a wide component between its characters.
///
/// @param path the path
/// @param width the room
/// @param measure a text's width
/// @return the lines
std::vector<std::string> notice_path(std::string_view path, int32_t width, const Measure& measure) {
    std::vector<std::string> lines;
    std::string line;
    std::size_t at = 0;
    while (at < path.size()) {
        const auto separator = path.find_first_of("/\\", at);
        const auto end = separator == std::string_view::npos ? path.size() : separator + 1;
        const std::string_view piece = path.substr(at, end - at);
        at = end;
        if (measure(line + std::string(piece)) <= width) {
            line += piece;
            continue;
        }
        if (!line.empty())
            lines.push_back(line);
        line = measure(piece) <= width ? std::string(piece)
                                       : break_characters(piece, width, measure, lines);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

/// Shortens a word to a width, ending it with three full stops.
///
/// @param text the word
/// @param width the room
/// @param measure a text's width
/// @return the word when it fits, else its longest start that fits with the stops
std::string cut_text(std::string_view text, int32_t width, const Measure& measure) {
    if (measure(text) <= width)
        return std::string(text);
    std::size_t end = text.size();
    while (end > 0) {
        --end;
        while (end > 0 && continuing(text[end]))
            --end;
        const std::string shown = std::string(text.substr(0, end)) + std::string(dots);
        if (measure(shown) <= width)
            return shown;
    }
    return std::string(dots);
}

/// Breaks a text as a mod question does: a wide script between characters, a
/// wide word shortened with three full stops.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @return the lines
std::vector<std::string> mods_wrap(std::string_view text, int32_t width, const Measure& measure) {
    std::vector<std::string> lines;
    if (oa::base::text::has_wide_script(text)) {
        const auto fits = [&](std::string_view start) { return measure(start) <= width; };
        for (;;) {
            while (!text.empty() && text.front() == ' ')
                text.remove_prefix(1);
            if (text.empty())
                break;
            const auto row = oa::base::text::first_row(text, fits);
            lines.emplace_back(text.substr(0, row.bytes));
            text.remove_prefix(row.next);
        }
        return lines;
    }
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t space = text.find(' ', at);
        const std::size_t end = space == std::string_view::npos ? text.size() : space;
        const std::string_view word = text.substr(at, end - at);
        at = end == text.size() ? end : end + 1;
        if (word.empty())
            continue;
        const std::string longer =
            line.empty() ? std::string(word) : line + ' ' + std::string(word);
        if (measure(longer) <= width) {
            line = longer;
            continue;
        }
        if (!line.empty())
            lines.push_back(line);
        line = cut_text(word, width, measure);
    }
    if (!line.empty())
        lines.push_back(line);
    return lines;
}

/// Shortens a word to a width, ending it with the painter's ellipsis.
///
/// @param text the word
/// @param width the room
/// @param measure a text's width
/// @return the word when it fits; empty when not even the ellipsis fits
std::string paint_fit(std::string_view text, int32_t width, const Measure& measure) {
    if (measure(text) <= width)
        return std::string(text);
    std::vector<std::size_t> starts;
    for (std::size_t at = 0; at < text.size(); at += utf8_length(static_cast<uint8_t>(text[at])))
        starts.push_back(at);
    std::size_t low = 0;
    std::size_t high = starts.size();
    while (low < high) {
        const std::size_t mid = (low + high + 1) / 2;
        std::string candidate(text.substr(0, mid < starts.size() ? starts[mid] : text.size()));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate += ellipsis;
        if (measure(candidate) <= width)
            low = mid;
        else
            high = mid - 1;
    }
    if (low == 0)
        return measure(ellipsis) <= width ? std::string(ellipsis) : std::string{};
    std::string kept(text.substr(0, low < starts.size() ? starts[low] : text.size()));
    while (!kept.empty() && kept.back() == ' ')
        kept.pop_back();
    return kept + std::string(ellipsis);
}

/// Breaks a text as the painter does: no wide-script branch, a wide word
/// shortened with the ellipsis.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @return the lines
std::vector<std::string> paint_wrap(std::string_view text, int32_t width, const Measure& measure) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t space = text.find(' ', at);
        const std::size_t end = space == std::string_view::npos ? text.size() : space;
        const std::string_view word = text.substr(at, end - at);
        at = end == text.size() ? end : end + 1;
        if (word.empty())
            continue;
        std::string joined =
            current.empty() ? std::string(word) : current + " " + std::string(word);
        if (measure(joined) <= width) {
            current = std::move(joined);
            continue;
        }
        if (!current.empty())
            lines.push_back(std::move(current));
        current = paint_fit(word, width, measure);
    }
    if (!current.empty())
        lines.push_back(std::move(current));
    return lines;
}

/// Shortens a line to a width, ending it with the ellipsis.
///
/// @param text the line
/// @param width the room
/// @param measure a text's width
/// @return the line when it fits; empty when not even the ellipsis fits
std::string files_fit(std::string_view text, int32_t width, const Measure& measure) {
    if (measure(text) <= width)
        return std::string(text);
    if (measure(ellipsis) > width)
        return {};
    std::vector<std::size_t> ends;
    for (std::size_t at = 0; at < text.size();)
        ends.push_back(at = next_character(text, at));
    std::size_t low = 0;
    std::size_t high = ends.size();
    std::string best(ellipsis);
    while (low < high) {
        const std::size_t middle = (low + high + 1) / 2;
        std::string candidate(text.substr(0, ends[middle - 1]));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate += ellipsis;
        if (measure(candidate) <= width) {
            best = std::move(candidate);
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return best;
}

/// Takes the longest start of a word that fits a width, at least one character.
///
/// @param word the word
/// @param width the room
/// @param measure a text's width
/// @return the bytes of the start
std::size_t fitting_start(std::string_view word, int32_t width, const Measure& measure) {
    std::size_t fits = next_character(word, 0);
    for (std::size_t at = fits; at < word.size();) {
        const std::size_t next = next_character(word, at);
        if (measure(word.substr(0, next)) > width)
            break;
        fits = next;
        at = next;
    }
    return fits;
}

/// Finds where to break a word wider than a line: after the last path separator
/// that fits, else after the last character that fits, at least one character.
///
/// @param word the word
/// @param width the room
/// @param measure a text's width
/// @return the bytes of the first piece
std::size_t breaking_point(std::string_view word, int32_t width, const Measure& measure) {
    std::size_t fits = next_character(word, 0);
    std::size_t separator = 0;
    for (std::size_t at = fits; at < word.size();) {
        const std::size_t next = next_character(word, at);
        if (measure(word.substr(0, next)) > width)
            break;
        fits = next;
        if (word[next - 1] == '/' || word[next - 1] == '\\')
            separator = next;
        at = next;
    }
    if (fits < word.size() && separator > 0)
        return separator;
    return fits;
}

/// Breaks a text as the Game files screen does: a new line starts a paragraph,
/// a wide word breaks between characters, and the lines kept are limited.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @param most_lines the most lines; 0 keeps every line
/// @return the lines
std::vector<std::string>
files_wrap(std::string_view text, int32_t width, const Measure& measure, int32_t most_lines) {
    std::vector<std::string> lines;
    if (width <= 0 || text.empty())
        return lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view paragraph = text.substr(start, end - start);
        std::string current;
        std::size_t at = 0;
        while (at < paragraph.size()) {
            const std::size_t space = std::min(paragraph.find(' ', at), paragraph.size());
            std::string_view word = paragraph.substr(at, space - at);
            at = space + 1;
            if (word.empty())
                continue;
            if (!current.empty()) {
                const std::string joined = current + " " + std::string(word);
                if (measure(joined) <= width) {
                    current = joined;
                    continue;
                }
                lines.push_back(current);
                current.clear();
            }
            while (measure(word) > width) {
                const std::size_t piece = fitting_start(word, width, measure);
                lines.emplace_back(word.substr(0, piece));
                word.remove_prefix(piece);
            }
            current = std::string(word);
        }
        if (!current.empty())
            lines.push_back(current);
        if (end >= text.size())
            break;
        start = end + 1;
    }
    if (most_lines > 0 && lines.size() > static_cast<std::size_t>(most_lines)) {
        std::string rest = lines[static_cast<std::size_t>(most_lines) - 1];
        for (std::size_t index = static_cast<std::size_t>(most_lines); index < lines.size();
             ++index)
            rest += " " + lines[index];
        lines.resize(static_cast<std::size_t>(most_lines));
        lines.back() = files_fit(rest + std::string(ellipsis), width, measure);
    }
    return lines;
}

/// Breaks a text as the folder chooser does: as the Game files screen, with a
/// wide word broken at a path separator when one fits.
///
/// @param text the text
/// @param width the room
/// @param measure a text's width
/// @param most_lines the most lines; 0 keeps every line
/// @return the lines
std::vector<std::string>
chooser_wrap(std::string_view text, int32_t width, const Measure& measure, int32_t most_lines) {
    std::vector<std::string> lines;
    if (width <= 0 || text.empty())
        return lines;
    std::size_t start = 0;
    while (start <= text.size()) {
        const std::size_t end = std::min(text.find('\n', start), text.size());
        const std::string_view paragraph = text.substr(start, end - start);
        std::string current;
        std::size_t at = 0;
        while (at < paragraph.size()) {
            const std::size_t space = std::min(paragraph.find(' ', at), paragraph.size());
            std::string_view word = paragraph.substr(at, space - at);
            at = space + 1;
            if (word.empty())
                continue;
            if (!current.empty()) {
                std::string joined = current + " " + std::string(word);
                if (measure(joined) <= width) {
                    current = std::move(joined);
                    continue;
                }
                lines.push_back(std::move(current));
                current.clear();
            }
            while (measure(word) > width) {
                const std::size_t piece = breaking_point(word, width, measure);
                lines.emplace_back(word.substr(0, piece));
                word.remove_prefix(piece);
            }
            current = std::string(word);
        }
        if (!current.empty())
            lines.push_back(std::move(current));
        if (end >= text.size())
            break;
        start = end + 1;
    }
    if (most_lines > 0 && lines.size() > static_cast<std::size_t>(most_lines)) {
        std::string rest = lines[static_cast<std::size_t>(most_lines) - 1];
        for (std::size_t index = static_cast<std::size_t>(most_lines); index < lines.size();
             ++index)
            rest += " " + lines[index];
        lines.resize(static_cast<std::size_t>(most_lines));
        lines.back() = files_fit(rest + std::string(ellipsis), width, measure);
    }
    return lines;
}

// The closing line stays as written so this file names the namespace once.
// clang-format off
} // the six wraps copied above
// clang-format on

/// Prints two wraps that disagree, then records the failed check.
///
/// @param site which wrap
/// @param measure_name which measure
/// @param width the room
/// @param most_lines the line limit, 0 for the wraps that have none
/// @param text the text
/// @param got the kit's lines
/// @param expected the old wrap's lines
void report_mismatch(
    const char* site,
    const char* measure_name,
    int32_t width,
    int32_t most_lines,
    std::string_view text,
    const std::vector<std::string>& got,
    const std::vector<std::string>& expected
) {
    if (got == expected)
        return;
    std::fprintf(
        stderr,
        "mismatch site %s measure %s width %d most %d text bytes %zu\n",
        site,
        measure_name,
        static_cast<int>(width),
        static_cast<int>(most_lines),
        text.size()
    );
    std::fprintf(stderr, "  kit:\n");
    for (const auto& line : got)
        std::fprintf(stderr, "    [%s]\n", line.c_str());
    std::fprintf(stderr, "  old:\n");
    for (const auto& line : expected)
        std::fprintf(stderr, "    [%s]\n", line.c_str());
    OA_CHECK(got == expected);
}

/// Returns a text's width at 6 columns a byte, and 12 for a wide character.
///
/// @param text the text
/// @return the width
int32_t columns_width(std::string_view text) {
    int32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto read = oa::base::text::break_character(text.substr(at));
        if (oa::base::text::is_wide_script(read.character))
            width += 12;
        else
            width += 6 * static_cast<int32_t>(read.bytes);
        at += read.bytes;
    }
    return width;
}

/// Returns a text's width with narrow letters at 3, wide Latin letters at 9,
/// a wide-script character at 12 and every other character at 6.
///
/// @param text the text
/// @return the width
int32_t proportional_width(std::string_view text) {
    int32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto read = oa::base::text::break_character(text.substr(at));
        if (oa::base::text::is_wide_script(read.character))
            width += 12;
        else if (
            read.character == U'i' || read.character == U'l' || read.character == U'.' ||
            read.character == U','
        )
            width += 3;
        else if (read.character == U'W' || read.character == U'M')
            width += 9;
        else
            width += 6;
        at += read.bytes;
    }
    return width;
}

/// Counts a text's characters, one each, the measure the notice's own check uses.
///
/// @param text the text
/// @return the characters
int32_t one_each(std::string_view text) {
    int32_t count = 0;
    for (const char byte : text)
        if ((static_cast<unsigned char>(byte) & 0xC0U) != 0x80U)
            ++count;
    return count;
}

/// Checks the UTF-8 helpers and the estimated width.
void characters_and_estimates() {
    OA_CHECK(kit::character_bytes("") == 0);
    OA_CHECK(kit::character_bytes("A") == 1);
    OA_CHECK(kit::character_bytes("\xC3\xA9") == 2);
    OA_CHECK(kit::character_bytes("\xE2\x80\xA6") == 3);
    OA_CHECK(kit::character_bytes("\x80") == 1);
    OA_CHECK(kit::continuation_byte(static_cast<char>(0x80)));
    OA_CHECK(kit::continuation_byte(static_cast<char>(0xBF)));
    OA_CHECK(!kit::continuation_byte('A'));
    OA_CHECK(!kit::continuation_byte(static_cast<char>(0xC0)));
    OA_CHECK(kit::character_count("") == 0);
    OA_CHECK(kit::character_count("Hello") == 5);
    OA_CHECK(kit::character_count("a\u2026b") == 3);
    OA_CHECK(kit::character_count("建造完成，单位已就绪") == 10);

    OA_CHECK(kit::estimated_width("") == 0);
    OA_CHECK(kit::estimated_width("Hello") == 35);
    OA_CHECK(kit::estimated_width("建造完成，单位已就绪") == 140);
    OA_CHECK(kit::estimated_character_width == 7);
}

/// Checks that a game font lacking the ellipsis and the folder mark draws the
/// characters that stand in for them, and a font with no glyphs does not.
void stand_ins_replace_what_the_font_lacks() {
    kit::Fonts empty;
    OA_CHECK(kit::with_stand_ins(empty, kit::FontRole::regular, "a\u2026b") == "a\u2026b");
    OA_CHECK(kit::with_stand_ins(empty, kit::FontRole::small, "x\u203Ay") == "x\u203Ay");

    kit::Fonts fonts;
    auto glyph = [](uint16_t width) {
        oa::formats::fnt::Glyph drawn;
        drawn.width = width;
        drawn.height = 8;
        return drawn;
    };
    fonts.regular.font.glyphs[static_cast<unsigned char>('a')] = glyph(4);
    fonts.regular.font.glyphs[static_cast<unsigned char>('b')] = glyph(5);
    fonts.regular.font.glyphs[static_cast<unsigned char>('.')] = glyph(2);
    fonts.regular.font.glyphs[static_cast<unsigned char>('>')] = glyph(3);
    fonts.regular.font.nominal_height = 11;
    OA_CHECK(kit::with_stand_ins(fonts, kit::FontRole::regular, "a\u2026b") == "a...b");
    OA_CHECK(kit::with_stand_ins(fonts, kit::FontRole::regular, "a\u203Ab") == "a>b");
    OA_CHECK(kit::text_width(fonts, kit::FontRole::regular, "a\u2026b") == 15);
    OA_CHECK(kit::text_width(fonts, kit::FontRole::regular, "a\u203Ab") == 12);
    OA_CHECK(kit::tracked_width(fonts, kit::FontRole::regular, "a\u2026b", 1) == 17);
    OA_CHECK(kit::capitals(fonts, kit::FontRole::regular) == 11);
    OA_CHECK(kit::with_stand_ins(fonts, kit::FontRole::small, "a\u2026b") == "a\u2026b");

    // A no-break space is drawn and measured as the font's own space, so it
    // keeps the gap between the words it joins.
    fonts.regular.font.glyphs[static_cast<unsigned char>(' ')] = glyph(3);
    OA_CHECK(kit::with_stand_ins(fonts, kit::FontRole::regular, "a\u00a0b") == "a b");
    OA_CHECK(kit::text_width(fonts, kit::FontRole::regular, "a\u00a0b") == 12);
    OA_CHECK(
        kit::text_width(fonts, kit::FontRole::regular, "a\u00a0b") ==
        kit::text_width(fonts, kit::FontRole::regular, "a b")
    );
    // A line the modern fonts draw whole, as one with a letter the font
    // lacks, keeps its own marks, which those fonts draw.
    OA_CHECK(
        kit::with_stand_ins(fonts, kit::FontRole::regular, "\u0436\u00a0a\u203Ab") ==
        "\u0436\u00a0a\u203Ab"
    );
    // A wrap never breaks a line at a no-break space, and measures the words
    // it joins with it: "b\u00a0a" goes whole to the next line.
    const kit::Measure measure = [&fonts](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::regular, text);
    };
    const auto lines = kit::wrap("a a b\u00a0a", 16, measure);
    OA_CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        OA_CHECK(lines[0] == "a a");
        OA_CHECK(lines[1] == "b\u00a0a");
    }
    for (const std::string& line : lines)
        OA_CHECK(measure(line) <= 16);
}

/// Checks the notice lines the dialog's test already names.
void the_known_notice_lines_stay() {
    const auto measure = [](std::string_view text) { return one_each(text); };
    const std::string deep = "/home/player/" + std::string(25, 'x') + "/Open Annihilation/Saves";
    const auto lines = kit::wrap_path(deep, 20, measure);
    std::string joined;
    for (const auto& line : lines) {
        OA_CHECK(one_each(line) <= 20);
        joined += line;
    }
    OA_CHECK(joined == deep);
    OA_CHECK(lines.size() >= 4 && lines[0] == "/home/player/");
    OA_CHECK(before_kit::notice_path(deep, 20, measure) == lines);

    const auto words = kit::wrap("Screenshots, films and mods now go here.", 16, measure);
    OA_CHECK(words.size() == 3);
    OA_CHECK(
        words[0] == "Screenshots," && words[1] == "films and mods" && words[2] == "now go here."
    );
    OA_CHECK(
        before_kit::notice_wrap("Screenshots, films and mods now go here.", 16, measure) == words
    );
    OA_CHECK(kit::wrap("", 16, measure).empty());
    const std::string wides(40, 'w');
    OA_CHECK(kit::wrap(wides, 16, measure).size() == 3);
    OA_CHECK(
        (kit::wrap("建造完成，单位已就绪", 4, measure) ==
         std::vector<std::string>{"建造完", "成，单位", "已就绪"})
    );
    OA_CHECK(
        before_kit::notice_wrap("建造完成，单位已就绪", 4, measure) ==
        kit::wrap("建造完成，单位已就绪", 4, measure)
    );
    // Korean, as Settings' hints show it, breaks at its spaces and keeps
    // each word whole, where Chinese would fill the row: "반드시 살" fits
    // five, but 살아남아야 is one word.
    OA_CHECK(
        (kit::wrap("스파이더 한 대는 반드시 살아남아야", 5, measure) ==
         std::vector<std::string>{"스파이더", "한 대는", "반드시", "살아남아야"})
    );
    // A word wider than the line still breaks inside.
    OA_CHECK(
        (kit::wrap("커맨더를 제외한", 3, measure) ==
         std::vector<std::string>{"커맨더", "를", "제외한"})
    );
}

/// The rules a site's call passes to the one wrap.
struct Site {
    const char* name{};      ///< which wrap
    bool wide_scripts{true}; ///< a wide script breaks between characters
    bool newlines{};         ///< a new line starts a paragraph
    bool shorten{};          ///< a wide word is shortened
    bool path_break{};       ///< a wide word breaks at a separator
    bool limit_lines{};      ///< the lines kept are limited
    std::vector<std::string> (*word_wrap)(std::string_view, int32_t, const before_kit::Measure&){};
    std::vector<std::string> (*limited)(
        std::string_view, int32_t, const before_kit::Measure&, int32_t
    ){};
};

/// Checks the one wrap against each of the six old wraps, over the corpus.
void the_wrap_matches_every_old_wrap() {
    const std::string wides(40, 'w');
    const std::string_view texts[] = {
        "",
        " ",
        "word",
        "Screenshots, films and mods now go here.",
        wides,
        "建造完成，单位已就绪",
        "mixed 中文 text with words",
        "Open Annihilation \u203A Total Annihilation",
        "C:\\Users\\player\\Documents\\Open Annihilation\\Saves",
        "/home/player/Documents/Open Annihilation/Saves",
        "line one\nline two three",
        "  leading and  double  spaces ",
        "a\u2026b",
    };
    const before_kit::Measure measures[] = {columns_width, proportional_width};
    const char* measure_names[] = {"columns", "proportional"};
    const int32_t widths[] = {1, 5, 12, 30, 60, 120, 400};
    const Site sites[] = {
        {"notice", true, false, false, false, false, before_kit::notice_wrap, nullptr},
        {"mods", true, false, true, false, false, before_kit::mods_wrap, nullptr},
        {"paint", false, false, true, false, false, before_kit::paint_wrap, nullptr},
        {"files", false, true, false, false, true, nullptr, before_kit::files_wrap},
        {"chooser", false, true, false, true, true, nullptr, before_kit::chooser_wrap},
    };

    for (const Site& site : sites) {
        for (std::size_t measure_index = 0; measure_index < 2; ++measure_index) {
            const before_kit::Measure& measure = measures[measure_index];
            for (const int32_t width : widths) {
                for (const std::string_view text : texts) {
                    const int32_t limits[] = {0, 1, 2};
                    const int32_t limit_count = site.limit_lines ? 3 : 1;
                    for (int32_t index = 0; index < limit_count; ++index) {
                        const int32_t most = limits[index];
                        kit::WrapRules rules;
                        rules.wide_scripts = site.wide_scripts;
                        rules.newlines = site.newlines;
                        rules.most_lines = static_cast<std::size_t>(most);
                        if (site.shorten && site.name[0] == 'm') {
                            rules.shorten_word = [&](std::string_view word) {
                                return before_kit::cut_text(word, width, measure);
                            };
                        } else if (site.shorten) {
                            rules.shorten_word = [&](std::string_view word) {
                                return before_kit::paint_fit(word, width, measure);
                            };
                        }
                        if (site.path_break) {
                            rules.break_word = [&](std::string_view word) {
                                return kit::path_break(word, width, measure);
                            };
                        }
                        if (site.limit_lines) {
                            rules.shorten_last = [&](std::string_view joined) {
                                return before_kit::files_fit(
                                    std::string(joined) + std::string(ellipsis), width, measure
                                );
                            };
                        }
                        const std::vector<std::string> got = kit::wrap(text, width, measure, rules);
                        const std::vector<std::string> expected =
                            site.limited != nullptr ? site.limited(text, width, measure, most)
                                                    : site.word_wrap(text, width, measure);
                        report_mismatch(
                            site.name,
                            measure_names[measure_index],
                            width,
                            most,
                            text,
                            got,
                            expected
                        );
                    }
                }
            }
        }
    }

    const before_kit::Measure measure = columns_width;
    for (const std::string_view text : texts) {
        for (const int32_t width : widths) {
            report_mismatch(
                "path",
                "columns",
                width,
                0,
                text,
                kit::wrap_path(text, width, measure),
                before_kit::notice_path(text, width, measure)
            );
            report_mismatch(
                "path",
                "proportional",
                width,
                0,
                text,
                kit::wrap_path(text, width, proportional_width),
                before_kit::notice_path(text, width, proportional_width)
            );
        }
    }
}

/// Checks the painter's two wrap checks, at the same size, weight and room.
void the_paint_wrap_keeps_its_lines() {
    auto fonts = text_font::FontStack::open(text_font::bundled_font_directory());
    if (!fonts) {
        std::printf("ui-kit: no fonts beside the test, so the paint wrap was not checked\n");
        return;
    }
    const auto measure = [&](std::string_view text) {
        return paint::text_width(*fonts, text, 16, true);
    };
    kit::WrapRules rules;
    rules.wide_scripts = false;
    rules.shorten_word = [&](std::string_view word) {
        return paint::fit_text(*fonts, word, 16, true, 120);
    };
    const auto lines = kit::wrap("Hold to self-destruct the selected units", 120, measure, rules);
    OA_CHECK(lines.size() >= 2);
    for (const auto& wrapped : lines)
        OA_CHECK(paint::text_width(*fonts, wrapped, 16, true) <= 120);
    OA_CHECK(kit::wrap("", 120, measure, rules).empty());
    OA_CHECK(
        lines == before_kit::paint_wrap("Hold to self-destruct the selected units", 120, measure)
    );
}

/// Checks fit and fitting_start against the Game files screen's own copies, which they
/// replace, over the wrap's texts and rooms with both measures.
void fit_matches_the_screens_copies() {
    const std::string wides(40, 'w');
    const std::string_view texts[] = {
        "",
        " ",
        "word",
        "Screenshots, films and mods now go here.",
        wides,
        "建造完成，单位已就绪",
        "trailing spaces   before the cut",
        "Open Annihilation \u203A Total Annihilation",
        "a\u2026b",
    };
    const before_kit::Measure measures[] = {columns_width, proportional_width};
    const int32_t widths[] = {-5, 0, 1, 5, 6, 12, 30, 60, 120, 400};
    for (const before_kit::Measure& measure : measures) {
        for (const int32_t width : widths) {
            for (const std::string_view text : texts) {
                OA_CHECK(
                    kit::fit(text, width, measure) == before_kit::files_fit(text, width, measure)
                );
                const std::string ended = std::string(text) + std::string(ellipsis);
                OA_CHECK(
                    kit::fit(ended, width, measure) == before_kit::files_fit(ended, width, measure)
                );
                OA_CHECK(
                    kit::fitting_start(text, width, measure) ==
                    before_kit::fitting_start(text, width, measure)
                );
            }
        }
    }
    // A line that fits is whole; one too long keeps no space before its ellipsis.
    OA_CHECK(kit::fit("word", 24, columns_width) == "word");
    OA_CHECK(kit::fit("ab cd", 20, proportional_width) == "ab\u2026");
    OA_CHECK(kit::fit("abcd", 5, columns_width).empty());
    OA_CHECK(kit::ellipsis == ellipsis);
}

/// Checks path_break: where it breaks a path, at a separator that fits and not at the one the
/// path starts with, and that it breaks every word as the folder chooser's own copy, which it
/// replaces, did, over the wrap's texts and rooms with both measures.
void a_path_breaks_after_a_separator() {
    // Six pixels a character: the room of ten characters holds "/home/deck".
    OA_CHECK(kit::path_break("/home/deck/Games", 60, columns_width) == 6);
    OA_CHECK(kit::path_break("C:\\Games\\TA", 54, columns_width) == 9);
    // No separator, or only the one it starts with: the characters that fit.
    OA_CHECK(kit::path_break("abcdefghij", 30, columns_width) == 5);
    OA_CHECK(kit::path_break("/abcdefgh", 30, columns_width) == 5);
    // A path that fits is whole; the first character is kept however narrow the room.
    OA_CHECK(kit::path_break("/a/b", 60, columns_width) == 4);
    OA_CHECK(kit::path_break("/abc", 3, columns_width) == 1);
    OA_CHECK(kit::path_break("", 30, columns_width) == 0);
    const kit::WordBreak rule = kit::path_break;
    OA_CHECK(rule("/home/deck/Games", 60, columns_width) == 6);

    const std::string wides(40, 'w');
    const std::string_view words[] = {
        "",
        "/",
        "word",
        wides,
        "建造完成/单位已就绪",
        "C:\\Users\\player\\Documents\\Open_Annihilation\\Saves",
        "/home/player/Documents/Open_Annihilation/Saves",
        "/run/media/deck/SD/SteamLibrary/steamapps/common/Total_Annihilation",
        "//double//separators//",
        "a\u2026b/c\u203Ad",
    };
    const before_kit::Measure measures[] = {columns_width, proportional_width};
    const int32_t widths[] = {-5, 0, 1, 5, 6, 12, 30, 60, 120, 400};
    for (const before_kit::Measure& measure : measures)
        for (const int32_t width : widths)
            for (const std::string_view word : words)
                OA_CHECK(
                    kit::path_break(word, width, measure) ==
                    before_kit::breaking_point(word, width, measure)
                );
}

/// A width hook that counts its calls: 10 pixels a character, 12 in bold.
int measured_calls = 0;

/// Measures a line for the hooks' check.
///
/// @param context unused
/// @param text the line
/// @param pixel_size unused
/// @param bold the weight
/// @return pixels
int counted_width(void* context, std::string_view text, int pixel_size, bool bold) {
    static_cast<void>(context);
    static_cast<void>(pixel_size);
    ++measured_calls;
    return static_cast<int>(text.size()) * (bold ? 12 : 10);
}

/// Gives a line's height for the hooks' check: the size less 20, so a small size gives 0 or less.
///
/// @param context unused
/// @param pixel_size the size
/// @param bold unused
/// @return pixels
int short_line(void* context, int pixel_size, bool bold) {
    static_cast<void>(context);
    static_cast<void>(bold);
    return pixel_size - 20;
}

/// Checks the fit record: a text wider than its box, and a text whose ink
/// crosses another, are named; a text that fits and stays clear is not, and
/// a new frame drops what the last one kept. A release build records
/// nothing.
void a_text_wider_than_its_box_or_across_another_is_recorded() {
    kit::set_text_fit_screen("language");
    kit::begin_text_fit_frame();
#if defined(NDEBUG)
    kit::note_text_fit("Benutzeroberfläche", 126, 0, 20, 116, 12, 0);
    OA_CHECK(kit::text_fit_records().empty());
#else
    kit::note_text_fit("fits", 40, 0, 0, 80, 12, 0);
    OA_CHECK(kit::text_fit_records().empty());
    kit::note_text_fit("Benutzeroberfläche", 126, 0, 20, 116, 12, 0);
    OA_CHECK(kit::text_fit_records().size() == 1);
    const kit::TextFitRecord& wider = kit::text_fit_records().front();
    OA_CHECK(wider.screen == "language");
    OA_CHECK(wider.text == "Benutzeroberfläche");
    OA_CHECK(wider.wider);
    OA_CHECK(!wider.across);
    OA_CHECK(wider.text_width == 126 && wider.box_width == 116);
    OA_CHECK(wider.box_x == 0 && wider.box_y == 20);
    // The next line starts where the wide one ends, so the edges meet and it is clear.
    kit::note_text_fit("beside", 40, 126, 20, 40, 12, 126);
    OA_CHECK(kit::text_fit_records().size() == 1);
    // This one sits on the wide one's ink.
    kit::note_text_fit("across", 40, 100, 20, 80, 12, 100);
    OA_CHECK(kit::text_fit_records().size() == 2);
    OA_CHECK(kit::text_fit_records().back().across);
    OA_CHECK(!kit::text_fit_records().back().wider);
    // A new paint keeps the records and forgets the ink, so the same place is clear.
    kit::text_fit_begin_paint();
    kit::note_text_fit("again", 40, 100, 20, 80, 12, 100);
    OA_CHECK(kit::text_fit_records().size() == 2);
    kit::begin_text_fit_frame();
    OA_CHECK(kit::text_fit_records().empty());
    kit::note_text_fit("", 20, 0, 0, 4, 12, 0);
    OA_CHECK(kit::text_fit_records().empty());
#endif
}

/// Checks the modern fonts' measure: the hooks when they are set, an estimate when not, and
/// an empty line measured as nothing without asking the hook.
void the_measure_uses_its_hooks() {
    const kit::TextMeasureHooks none{};
    OA_CHECK(kit::measured_width(none, "", 10, false) == 0);
    OA_CHECK(kit::measured_width(none, "abc", 10, false) == 17);
    OA_CHECK(kit::measured_width(none, "a\u2026b", 20, true) == 33);
    OA_CHECK(kit::measured_line(none, 10, false) == 13);
    OA_CHECK(kit::measured_line(none, 8, true) == 10);
    kit::TextMeasureHooks hooks{};
    hooks.width = counted_width;
    hooks.line_height = short_line;
    measured_calls = 0;
    OA_CHECK(kit::measured_width(hooks, "", 10, false) == 0);
    OA_CHECK(measured_calls == 0);
    OA_CHECK(kit::measured_width(hooks, "abc", 10, false) == 30);
    OA_CHECK(kit::measured_width(hooks, "abc", 10, true) == 36);
    OA_CHECK(measured_calls == 2);
    OA_CHECK(kit::measured_line(hooks, 30, false) == 10);
    OA_CHECK(kit::measured_line(hooks, 12, false) == 1);
}

} // namespace

int main() {
    characters_and_estimates();
    stand_ins_replace_what_the_font_lacks();
    the_known_notice_lines_stay();
    the_wrap_matches_every_old_wrap();
    the_paint_wrap_keeps_its_lines();
    fit_matches_the_screens_copies();
    a_path_breaks_after_a_separator();
    the_measure_uses_its_hooks();
    a_text_wider_than_its_box_or_across_another_is_recorded();
    return oa::test::check_exit_status();
}
