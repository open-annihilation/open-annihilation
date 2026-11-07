// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mission briefing text: word wrap, colour-span reflow and the paged
// TextRegion layout of MSNBRIEF.GUI.
//
// Briefing files mark highlighted words as `&Xword&`, where X selects the
// highlight colour (G green, Y yellow, R and anything else red). The pager
// emits plain rows plus highlight entries drawn over them as blinking words.
#pragma once

#include <cstddef>
#include <cstdint>

namespace oa::ui::campaign {

// Text byte that ends a briefing buffer as well as NUL.
inline constexpr char kTextEnd = static_cast<char>(0xff);
inline constexpr char kSpanMarker = '&';
/// The characters a row, or a highlight, holds at most: 3.1c's row of 128
/// bytes holds 127 characters of its one-byte code page and its end.
inline constexpr std::size_t kRowCharacters = 0x7f;
/// The bytes a row's or a highlight's text takes: kRowCharacters characters
/// of up to four bytes each in UTF-8, and the end. A row of the game's
/// one-byte code page still ends at 127 characters, so 3.1c's own briefings
/// lay out as they do there, while a language whose letters take two bytes
/// (Cyrillic) or three (CJK) holds as many characters as English does.
inline constexpr std::size_t kRowTextBytes = kRowCharacters * 4 + 1;
inline constexpr std::size_t kMaxRows = 64;
// MSNBRIEF allocates a 15-entry blink-word table for the highlights.
inline constexpr std::size_t kMaxHighlights = 15;
// Extra bytes the reflow buffer reserves over the source length.
inline constexpr std::size_t kReflowSlack = 0x32;

// Highlight colour slots; the palette index is side_colors[side * 4 + slot].
enum class HighlightColor : uint8_t { green = 1, yellow = 2, red = 3 };

using MeasureText = int32_t (*)(void* context, const char* text);

/// Wraps text at spaces, hyphens and newlines so no line measures `width` or more.
///
/// Copies `text` into `out`, breaking before the word that first makes a line
/// reach `width`; breaks are found only at a following space, hyphen or
/// newline and are written as CR LF, the CR replacing the separator (so a
/// hyphen at a break is lost, as in 3.1c). A word longer than the line is
/// left unbroken.
///
/// With `keep_hyphen`, as for rows the modern fonts draw, a hyphen a row
/// breaks at stays at the end of its row, before the CR LF, and a row that
/// would reach `width` once that hyphen joined it breaks before its word
/// instead, as it would for a longer word. A space at a break is still
/// dropped. A text with Chinese, Japanese or Korean characters never breaks
/// at a hyphen, so `keep_hyphen` leaves it alone.
///
/// A text with Chinese, Japanese or Korean characters breaks where the line
/// breaker lets it (oa::base::text::first_row), before a row reaches
/// `width`: at its spaces, between Chinese or Japanese characters, which are
/// written without spaces, never starting a row with a closing mark or
/// ending one with an opening mark, and inside a word, a Korean one among
/// them, only when it is wider than a row; never parting a highlight's
/// markers from the words they mark; its output holds whole characters
/// only.
///
/// @param text Source text, ended by NUL or kTextEnd.
/// @param width Line width in pixels.
/// @param measure Measures a line in pixels.
/// @param context Context passed to `measure`.
/// @param[out] out Destination buffer; cleared first.
/// @param capacity Size of `out` in bytes; the output stops short of it.
/// @param keep_hyphen Whether a hyphen at a break stays at the end of its row.
/// @return Output length in bytes; 0 for no capacity.
std::size_t wrap_text(
    const char* text,
    int32_t width,
    MeasureText measure,
    void* context,
    char* out,
    std::size_t capacity,
    bool keep_hyphen = false
);

/// Closes and reopens a colour span around every line break inside it so each row carries balanced markers.
///
/// A break inside a span becomes "&\\r\\n&X", X being the span's colour code.
///
/// @param text Wrapped briefing text, ended by NUL or kTextEnd.
/// @param[out] out Destination buffer, strlen(text) + kReflowSlack bytes; cleared first.
/// @param capacity Size of `out` in bytes; the output stops short of it.
/// @return Output length in bytes; 0 for no capacity.
std::size_t reflow_span_text(const char* text, char* out, std::size_t capacity);

/// Finds the start of a page of text.
///
/// @param text Wrapped briefing text, ended by NUL or kTextEnd.
/// @param lines_per_page Rows per page.
/// @param page Page index; 0 is the text itself.
/// @return The character after newline number lines_per_page * page, or null
///         past the end (or for empty text).
[[nodiscard]] const char* find_text_page(const char* text, int32_t lines_per_page, int32_t page);

/// The pixels a briefing's rows start right of its TextRegion.
inline constexpr int32_t kRowInset = 5;

struct BriefingRegion {
    int32_t x{};
    int32_t y{};
    int32_t height{};
    int32_t line_height{}; // font height + 2
    /// The rows from a row's pen row to the last its line may draw on, and
    /// one more, for rows the modern fonts draw, whose letters, outline and
    /// shadow reach further below the font's baseline than the font's own
    /// letters; 0 for rows the game's font draws, which the region holds as
    /// many of as its height has whole lines.
    int32_t row_reach{};
    /// Whether a hyphen a row breaks at stays at the end of its row, as it
    /// does in rows the modern fonts draw; the game's font loses it, as in
    /// 3.1c (wrap_text).
    bool keep_hyphen{};
};

struct BriefingRow {
    char text[kRowTextBytes]{};
    int32_t x{};
    int32_t y{};
};

struct BriefingHighlight {
    char text[kRowTextBytes]{};
    int32_t x{};
    int32_t y{};
    HighlightColor color{};
};

enum class MoreLabel : uint8_t { none, more, back_to_start };

struct BriefingPage {
    BriefingRow rows[kMaxRows];
    uint32_t row_count{};
    BriefingHighlight highlights[kMaxHighlights];
    uint32_t highlight_count{};
    MoreLabel more{};
    int32_t page{};
};

// Paging state kept between MOREBAR clicks.
struct BriefingPager {
    const char* text{}; // wrapped and reflowed briefing
    int32_t page{};
    bool fresh{}; // next layout restarts at page 0
};

/// Starts paging a briefing text; the next layout shows page 0.
///
/// @param[out] pager Paging state.
/// @param text Wrapped and reflowed briefing text.
void briefing_pager_reset(BriefingPager* pager, const char* text);

/// Advances to the next page of the TextRegion (wrapping to the first) and lays it out.
///
/// Rows start kRowInset pixels right of the region and half a line down,
/// one per line; every colour span adds a blinking highlight positioned
/// after the row text before it. A page holds as many rows as the region's
/// height has whole lines; with a row_reach, only those whose line ends
/// inside the region, and at least one. A line feed ends its row wherever it
/// stands, right after a highlight's closing marker included, so a briefing
/// with line feeds alone, not CR LF, lays the highlighted line out as a row
/// of its own. A line feed after a CR that a marker took as its colour code
/// (a highlight left open across a blank line) is laid out as 3.1c lays it
/// out. The MOREBAR label becomes MORE... while a further page exists, BACK
/// TO START on the last of several pages, or nothing.
///
/// @param[in,out] pager Paging state; its page advances.
/// @param region TextRegion position, height and line height in pixels.
/// @param measure Measures text in pixels.
/// @param context Context passed to `measure`.
/// @param[out] page Rows, highlights and MOREBAR label of the page; empty
///                  without text or a line height.
void briefing_next_page(
    BriefingPager* pager,
    const BriefingRegion* region,
    MeasureText measure,
    void* context,
    BriefingPage* page
);

/// Names the MOREBAR caption of a label.
///
/// @param label MOREBAR label.
/// @return "MORE...", "BACK TO START" or "".
[[nodiscard]] const char* more_label_text(MoreLabel label);

} // namespace oa::ui::campaign
