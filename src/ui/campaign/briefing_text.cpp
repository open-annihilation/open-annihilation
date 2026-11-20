// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mission briefing text layout.
#include "oa/ui/campaign/briefing_text.hpp"

#include "oa/base/text/line_break.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace oa::ui::campaign {
namespace {

bool at_end(char c) {
    return c == '\0' || c == kTextEnd;
}

HighlightColor highlight_color(char code) {
    if (code == 'R')
        return HighlightColor::red;
    if (code == 'Y')
        return HighlightColor::yellow;
    if (code == 'G')
        return HighlightColor::green;
    return HighlightColor::red;
}

/// Tells whether a highlight marker opens its span: the markers before it
/// are even in number.
///
/// @param markers the markers before it
/// @return true when it opens a span
bool opens_span(std::size_t markers) {
    return markers % 2 == 0;
}

/// Wraps a briefing with Chinese, Japanese or Korean characters: a row
/// breaks where the line breaker allows (oa::base::text::first_row), at a
/// space or between two Chinese or Japanese characters, which are written
/// without spaces, before it reaches the width, keeping a Korean word whole
/// unless it is wider than a row; but never between a span's opening marker
/// and its colour code and the word after them, nor before its closing
/// marker. A break at spaces drops them; each break is CR LF, and the
/// text's own line breaks stay.
///
/// @param text the briefing
/// @param width the row width, in pixels
/// @param measure measures a row
/// @param context passed to measure
/// @return the wrapped briefing
std::string
wrap_wide_text(std::string_view text, int32_t width, MeasureText measure, void* context) {
    std::string out;
    std::string line;
    const auto fits = [&](std::string_view start) {
        line.assign(start);
        return measure(context, line.c_str()) < width;
    };
    std::size_t markers = 0;
    for (;;) {
        const auto newline = text.find('\n');
        std::string_view rest = text.substr(0, newline);
        // A CR before the line break stays with it.
        const bool carriage = !rest.empty() && rest.back() == '\r';
        if (carriage)
            rest.remove_suffix(1);
        while (!rest.empty()) {
            const auto row = oa::base::text::first_row(rest, fits);
            std::size_t end = row.bytes;
            std::size_t next = row.next;
            if (end == next && next < rest.size()) {
                // A break between two characters moves back while it would
                // part a marker from the words it marks.
                const auto markers_before = [&](std::size_t at) {
                    return markers + static_cast<std::size_t>(std::count(
                                         rest.begin(),
                                         rest.begin() + static_cast<std::ptrdiff_t>(at),
                                         kSpanMarker
                                     ));
                };
                std::size_t moved = end;
                for (;;) {
                    // After an opening marker and its colour code: before them.
                    if (moved >= 2 && rest[moved - 2] == kSpanMarker &&
                        opens_span(markers_before(moved - 2))) {
                        moved -= 2;
                        continue;
                    }
                    // Before a closing marker: one character earlier.
                    if (moved > 0 && rest[moved] == kSpanMarker &&
                        !opens_span(markers_before(moved))) {
                        --moved;
                        while (moved > 0 &&
                               (static_cast<unsigned char>(rest[moved]) & 0xC0U) == 0x80U)
                            --moved;
                        continue;
                    }
                    break;
                }
                if (moved > 0)
                    end = next = moved;
            }
            out.append(rest.substr(0, end));
            markers += static_cast<std::size_t>(std::count(
                rest.begin(), rest.begin() + static_cast<std::ptrdiff_t>(next), kSpanMarker
            ));
            rest.remove_prefix(next);
            if (!rest.empty())
                out += "\r\n";
        }
        if (carriage)
            out += '\r';
        if (newline == std::string_view::npos)
            return out;
        out += '\n';
        text.remove_prefix(newline + 1);
    }
}

/// What a row's or a highlight's text holds as its bytes are copied in.
struct RowFill {
    std::size_t length{};     ///< the bytes copied
    std::size_t characters{}; ///< the characters begun, at most kRowCharacters
    std::size_t pending{};    ///< bytes of the last character still to come
    bool full{};              ///< a character did not fit; nothing more is copied
};

/// Copies a byte of a row's text into a buffer while the whole character it
/// belongs to fits, so that no character is cut in part. A row holds at most
/// kRowCharacters characters: a well-formed UTF-8 sequence is one, and any
/// other byte, as each byte of the game's one-byte code page, is one of its
/// own (oa::base::text::break_character).
///
/// @param p the byte, within its text
/// @param[out] out the buffer, kRowTextBytes bytes
/// @param[in,out] fill what the buffer holds
void put_whole(const char* p, char* out, RowFill& fill) {
    if (fill.full)
        return;
    if (fill.pending == 0) {
        std::size_t available = 0;
        while (available < 4 && !at_end(p[available]))
            ++available;
        const auto read = oa::base::text::break_character(std::string_view(p, available));
        const std::size_t bytes = std::max<std::size_t>(read.bytes, 1);
        if (fill.characters + 1 > kRowCharacters || fill.length + bytes > kRowTextBytes - 1) {
            fill.full = true;
            return;
        }
        ++fill.characters;
        fill.pending = bytes;
    }
    --fill.pending;
    out[fill.length++] = *p;
}

} // namespace

std::size_t wrap_text(
    const char* text,
    int32_t width,
    MeasureText measure,
    void* context,
    char* out,
    std::size_t capacity,
    bool keep_hyphen
) {
    if (capacity == 0)
        return 0;
    std::memset(out, 0, capacity);
    std::size_t length = 0;
    while (!at_end(text[length]))
        ++length;
    if (const std::string_view whole(text, length); oa::base::text::has_wide_script(whole)) {
        const std::string wrapped = wrap_wide_text(whole, width, measure, context);
        const std::size_t room = capacity > 3 ? capacity - 3 : 0;
        const std::size_t n = oa::base::text::whole_character_bytes(wrapped, room);
        std::memcpy(out, wrapped.data(), n);
        return n;
    }
    const char* src = text;
    std::size_t n = 0;
    std::size_t line_start = 0;
    while (!at_end(*src) && n + 3 < capacity) {
        out[n++] = *src;
        const char next = src[1];
        ++src;
        // A hyphen that comes next joins the row it may end, so a row is held
        // to the width with it. The buffer is cleared past the row's end, and
        // the loop leaves room for it.
        const bool hyphen_joins = keep_hyphen && next == '-';
        if (hyphen_joins)
            out[n] = '-';
        const bool too_wide = (next == ' ' || next == '\n' || next == '-') &&
                              width <= measure(context, out + line_start);
        if (hyphen_joins)
            out[n] = '\0';
        if (too_wide) {
            // Walk back to the separator that ended the previous word.
            std::size_t back = 0;
            while (back + 1 < n - line_start &&
                   src[-1 - static_cast<std::ptrdiff_t>(back)] != ' ' &&
                   src[-1 - static_cast<std::ptrdiff_t>(back)] != '-')
                ++back;
            const char separator = src[-1 - static_cast<std::ptrdiff_t>(back)];
            if (separator == ' ' || separator == '-') {
                for (std::size_t i = 0; i <= back; ++i)
                    out[n - i] = '\0';
                src -= back;
                n -= back;
                // The break replaces its separator, so a space and, as in
                // 3.1c, a hyphen are lost; a kept hyphen ends its row.
                if (separator == '-' && keep_hyphen) {
                    out[n] = '\r';
                    out[n + 1] = '\n';
                    n += 2;
                } else {
                    out[n - 1] = '\r';
                    out[n] = '\n';
                    ++n;
                }
                line_start = n;
            }
        }
        if (*src == '\n')
            line_start = n + 1;
    }
    out[n] = '\0';
    return n;
}

std::size_t reflow_span_text(const char* text, char* out, std::size_t capacity) {
    if (capacity == 0)
        return 0;
    std::memset(out, 0, capacity);
    std::size_t n = 0;
    bool inside = false;
    char code = '\0';
    for (const char* p = text; *p != '\0' && *p != kTextEnd && n + 5 < capacity; ++p) {
        out[n] = *p;
        if (*p == kSpanMarker) {
            inside = !inside;
            code = p[1];
        }
        if (*p == '\n' && inside && n > 0) {
            out[n - 1] = kSpanMarker;
            out[n] = '\r';
            out[n + 1] = '\n';
            out[n + 2] = kSpanMarker;
            n += 3;
            out[n] = code;
        }
        ++n;
    }
    out[n] = '\0';
    return n;
}

const char* find_text_page(const char* text, int32_t lines_per_page, int32_t page) {
    if (page == 0)
        return text;
    if (text == nullptr || *text == '\0')
        return nullptr;
    const int32_t target = lines_per_page * page;
    int32_t lines = 0;
    bool found = false;
    const char* p = text;
    do {
        const char c = *p;
        if (c == kTextEnd)
            break;
        if (found)
            return p;
        ++p;
        if (c == '\n' && ++lines == target)
            found = true;
    } while (*p != '\0');
    return found ? p : nullptr;
}

void briefing_pager_reset(BriefingPager* pager, const char* text) {
    pager->text = text;
    pager->page = 0;
    pager->fresh = true;
}

void briefing_next_page(
    BriefingPager* pager,
    const BriefingRegion* region,
    MeasureText measure,
    void* context,
    BriefingPage* page
) {
    page->row_count = 0;
    page->highlight_count = 0;
    page->more = MoreLabel::none;
    if (pager->text == nullptr || region->line_height <= 0)
        return;
    if (pager->fresh) {
        pager->page = -1;
        pager->fresh = false;
    }
    const int32_t line_height = region->line_height;
    int32_t lines = region->height / line_height;
    // Rows the modern fonts draw reach further below their pen row than the
    // font's letters: a page holds those whose line ends inside the region.
    if (region->row_reach > 0)
        while (lines > 1 &&
               line_height / 2 + (lines - 1) * line_height + region->row_reach > region->height)
            --lines;
    const int32_t x = region->x + kRowInset;
    int32_t y = region->y + line_height / 2;
    ++pager->page;
    const char* p = find_text_page(pager->text, lines, pager->page);
    if (p == nullptr) {
        pager->page = 0;
        p = find_text_page(pager->text, lines, 0);
    }
    if (find_text_page(pager->text, lines, pager->page + 1) != nullptr)
        page->more = MoreLabel::more;
    else if (pager->page != 0)
        page->more = MoreLabel::back_to_start;
    page->page = pager->page;

    bool opens = true;
    for (int32_t row = 0; row < lines && page->row_count < kMaxRows; ++row) {
        BriefingRow& out = page->rows[page->row_count++];
        std::memset(out.text, 0, sizeof(out.text));
        out.x = x;
        out.y = y;
        y += line_height;
        RowFill fill;
        char c = *p;
        while (c != '\n') {
            if (at_end(c))
                break;
            if (c == kSpanMarker) {
                if (!opens) {
                    ++p;
                    opens = true;
                } else {
                    opens = false;
                    const HighlightColor color = highlight_color(p[1]);
                    p += 2;
                    if (page->highlight_count < kMaxHighlights) {
                        BriefingHighlight& highlight = page->highlights[page->highlight_count++];
                        std::memset(highlight.text, 0, sizeof(highlight.text));
                        highlight.x = measure(context, out.text) + x;
                        highlight.y = out.y;
                        highlight.color = color;
                        RowFill kept;
                        for (std::size_t i = 0; !at_end(p[i]) && p[i] != kSpanMarker; ++i)
                            put_whole(p + i, highlight.text, kept);
                    }
                }
                if (at_end(*p))
                    break;
                // The character after a marker is copied as part of the row,
                // which is the CR of a briefing with CR LF line ends. A line
                // feed there with no CR before it still ends the row; else
                // the next line would join it, a row far wider than the
                // region. A line feed with a CR before it is the second half
                // of a CR LF whose CR an opening marker took as its colour
                // code, as a highlight left open across a blank line does
                // (3.1c's German ArmCampaign22 holds one); 3.1c draws that
                // row as it always did, so it does not end here.
                if (*p == '\n' && p[-1] != '\r')
                    break;
            }
            put_whole(p, out.text, fill);
            ++p;
            c = *p;
        }
        if (!at_end(*p))
            ++p;
    }
}

const char* more_label_text(MoreLabel label) {
    switch (label) {
    case MoreLabel::more:
        return "MORE...";
    case MoreLabel::back_to_start:
        return "BACK TO START";
    case MoreLabel::none:
        break;
    }
    return "";
}

} // namespace oa::ui::campaign
