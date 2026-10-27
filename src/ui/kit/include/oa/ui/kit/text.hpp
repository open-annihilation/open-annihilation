// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Provisional: this header changes whenever OA's screens need it to, until the kit is declared stable (src/ui/kit/README.md).

// The OA UI kit's text: the classic look's two game fonts and the modern
// faces that draw what they lack, the modern look's Barlow, one measure,
// one wrap, and the stand-in characters. The kit never looks a word up in
// the interface catalogue; a caller passes the text to show.
#pragma once

#include "oa/formats/hpi.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::kit {

/// Which of the two game fonts a text is drawn in.
enum class FontRole : uint8_t {
    regular, ///< labels, values and buttons
    small,   ///< the section heading, the hints and the version
};

/// Returns the game font the classic look draws a level of the type scale
/// in: a title and a label in the regular font, body and small text in the
/// small one.
///
/// @param level the level
/// @return the font
[[nodiscard]] constexpr FontRole classic_font(TextLevel level) noexcept {
    return level == TextLevel::title || level == TextLevel::label ? FontRole::regular
                                                                  : FontRole::small;
}

/// Returns a level's size in the modern look at a size class's metrics.
///
/// @param metrics the class's metrics
/// @param level the level
/// @return the size, in points
[[nodiscard]] constexpr int32_t text_size(const Metrics& metrics, TextLevel level) noexcept {
    switch (level) {
    case TextLevel::title:
        return metrics.title_size;
    case TextLevel::label:
        return metrics.label_size;
    case TextLevel::body:
        return metrics.body_size;
    case TextLevel::small:
        return metrics.small_size;
    }
    return metrics.body_size;
}

/// Returns the modern face that draws a font's characters the game font
/// lacks, and every character where the game's fonts are missing: the one
/// place the kit chooses its modern face. The regular font's is the
/// message face and the small font's the status face.
///
/// @param role which font
/// @return the face
[[nodiscard]] constexpr oa::present::TextFace modern_face(FontRole role) noexcept {
    return role == FontRole::small ? oa::present::TextFace::status : oa::present::TextFace::message;
}

/// The fonts a screen draws its texts in.
struct Fonts {
    oa::ui::frontend_renderer::TextFont regular; ///< labels, values and buttons
    oa::ui::frontend_renderer::TextFont small;   ///< the section heading, the hints and the version
    /// The characters each font draws, for UTF-8 texts such as a language's
    /// name in itself; the modern fonts draw the others.
    oa::present::FontCharacters regular_characters{};
    oa::present::FontCharacters small_characters{}; ///< the small font's
    /// The modern look's fonts. While set, every text is measured and drawn
    /// in Barlow from this stack at its role's type (modern_type), and what
    /// Barlow lacks in the faces after it; the game's fonts above are not
    /// used. Null keeps the classic look. The host owns the stack, and one
    /// thread at a time uses it.
    oa::platform::text_font::FontStack* modern{};
};

/// Tells whether fonts draw the modern look.
///
/// @param fonts the fonts
/// @return true while they hold the modern look's stack
[[nodiscard]] constexpr bool modern_look(const Fonts& fonts) noexcept {
    return fonts.modern != nullptr;
}

/// The type the modern look draws a line in.
struct ModernType {
    int32_t points{}; ///< its size, in points: pixels per em at a scale of 1
    oa::platform::text_font::Weight weight{}; ///< its weight
    oa::platform::text_font::Family family{}; ///< the faces it looks in first
};

/// Returns the type the modern look draws a font's text in: the type
/// scale's Compact steps at every size class, as the game's fonts keep one
/// size at every class. The regular font's text is a label, Barlow
/// SemiBold at label_size (14); the small font's is small text, Barlow
/// Regular at small_size (12). A tracked line, a window's title or a
/// section's heading in capitals, is Barlow Condensed: the regular font's
/// Bold at title_size (17), the small font's SemiBold at small_size.
///
/// @param role which font
/// @param tracked the line has tracking
/// @return the type
[[nodiscard]] constexpr ModernType modern_type(FontRole role, bool tracked) noexcept {
    using oa::platform::text_font::Family;
    using oa::platform::text_font::Weight;
    if (tracked)
        return role == FontRole::regular
                   ? ModernType{compact_metrics.title_size, Weight::bold, Family::interface_condensed}
                   : ModernType{
                         compact_metrics.small_size, Weight::semibold, Family::interface_condensed
                     };
    return role == FontRole::regular
               ? ModernType{compact_metrics.label_size, Weight::semibold, Family::interface}
               : ModernType{compact_metrics.small_size, Weight::regular, Family::interface};
}

/// Returns a line's width in the modern look: each character's advance at
/// a scale of 1, and the tracking after each character but the last. The
/// line is as wide in points at every scale it is drawn at (modern_line).
///
/// @param fonts the modern look's stack
/// @param text the text, in UTF-8
/// @param type its type
/// @param tracking extra points after each character but the last
/// @return the width, in points; 0 for text the stack refuses
[[nodiscard]] int32_t modern_width(
    oa::platform::text_font::FontStack& fonts,
    std::string_view text,
    const ModernType& type,
    int32_t tracking
);

/// One character of a line in the modern look, drawn at a scale.
struct ModernGlyph {
    oa::platform::text_font::Coverage coverage{}; ///< the character, antialiased
    int32_t pen{}; ///< the pen's column, in pixels from the line's start
};

/// A line in the modern look drawn at a scale.
struct ModernLine {
    int32_t width{}; ///< points, as modern_width gives it
    /// Its characters, or the whole line as one where nothing needs them
    /// apart: at a scale of 1 without tracking.
    std::vector<ModernGlyph> glyphs{};
};

/// Draws a line in the modern look at a scale. Each character is drawn at
/// the type's size times the scale, its pen at its pen at a scale of 1
/// times the scale, rounded, so the line keeps the width its measure gave
/// at every scale. A line in an alphabet Barlow lacks is drawn whole in
/// the sans faces (oa::platform::text_font::Family).
///
/// @param fonts the modern look's stack
/// @param text the text, in UTF-8
/// @param type its type
/// @param tracking extra points after each character but the last
/// @param scale_percent pixels a point, in percent: 100, 150, 200 and so on
/// @return the line; empty for text the stack refuses
[[nodiscard]] ModernLine modern_line(
    oa::platform::text_font::FontStack& fonts,
    std::string_view text,
    const ModernType& type,
    int32_t tracking,
    int32_t scale_percent
);

/// Returns the rows a type's capitals stand above its baseline in the
/// modern look at a scale: those of its capital H at least half covered.
///
/// @param fonts the modern look's stack
/// @param type the type
/// @param scale_percent pixels a point, in percent
/// @return the rows, in pixels; 0 when the stack cannot draw it
[[nodiscard]] int32_t modern_capitals(
    oa::platform::text_font::FontStack& fonts, const ModernType& type, int32_t scale_percent
);

/// Loads the game's fonts: the button font as the regular one and the label
/// font as the small one, each readied for text in one colour. Their letters
/// keep their shading and lose the dark outline round them.
///
/// Throws std::runtime_error when a font or the game's palette is missing, or a
/// font is malformed.
///
/// @param assets the game's files
/// @return the fonts
[[nodiscard]] Fonts load_game_fonts(oa::AssetStore& assets);

/// Returns a UTF-8 text's width as it is drawn: the characters a font draws
/// at its glyphs' widths, and the others in the modern fonts. A character the
/// game font lacks, of the ones it stands in for, is measured as the
/// characters drawn in its place. In the modern look, its width in the
/// role's type (modern_width).
///
/// @param fonts the fonts
/// @param role which of them
/// @param text the text, in UTF-8
/// @return the width, in points
[[nodiscard]] int32_t text_width(const Fonts& fonts, FontRole role, std::string_view text);

/// Returns a text's width with extra columns after each character but the last.
///
/// The extra columns are counted on the text as given. The width itself counts
/// a stood-in character as the characters drawn in its place. In the modern
/// look a line with tracking is measured in the tracked type (modern_type).
///
/// @param fonts the fonts
/// @param role which of them
/// @param text the text, in UTF-8
/// @param tracking the extra columns
/// @return the width, in points
[[nodiscard]] int32_t
tracked_width(const Fonts& fonts, FontRole role, std::string_view text, int32_t tracking);

/// Returns how far a tracked text drawn wholly in the modern face moves the
/// pen, one character at a time at a scale, as draw_boxed_text draws it. In
/// the modern look, tracked_width.
///
/// @param fonts the fonts
/// @param role which of them
/// @param text the text, in UTF-8, as it is drawn
/// @param tracking the extra columns after each character but the last
/// @param scale the placement's scale
/// @return the width, in points
[[nodiscard]] int32_t modern_tracked_width(
    const Fonts& fonts, FontRole role, std::string_view text, int32_t tracking, int32_t scale
);

/// Returns how many rows a font's capitals stand: the game font's nominal
/// height, or, for a font the modern face stands in for wholly, the rows its
/// capital H stands above its baseline. In the modern look, the rows of the
/// role's type at a scale of 1 (modern_capitals).
///
/// @param fonts the fonts
/// @param role which of them
/// @return the rows, in points
[[nodiscard]] int32_t capitals(const Fonts& fonts, FontRole role);

/// Returns a text as a font shows it. Where the game font lacks the ellipsis
/// or the mark between a location's folders, the characters it draws in their
/// place are written instead: "..." and ">". A font the modern face stands in
/// for wholly, and every text in the modern look, is returned unchanged.
///
/// @param fonts the fonts
/// @param role which of them
/// @param text the text, in UTF-8
/// @return the text to draw and measure
[[nodiscard]] std::string with_stand_ins(const Fonts& fonts, FontRole role, std::string_view text);

/// Draws a UTF-8 text: the characters the font draws with its glyphs, the
/// others in the modern fonts on the font's baseline, at the placement's
/// scale and within its clip. In the modern look the whole text is drawn in
/// the role's type (modern_line), antialiased, with no outline or shadow.
///
/// @param[in,out] target the surface
/// @param placement where the picture lands
/// @param fonts the fonts
/// @param role which of them
/// @param text the text, in UTF-8, as it is drawn
/// @param pen the pen column, in points
/// @param pen_row the pen row, in points
/// @param colour the colour, red, green and blue
/// @return the pen column after the text, in points
int32_t draw_text(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    const Fonts& fonts,
    FontRole role,
    std::string_view text,
    int32_t pen,
    int32_t pen_row,
    std::array<uint8_t, 3> colour
);

/// Where a text sits along a box's width.
enum class Align : uint8_t {
    left,   ///< at the box's left
    centre, ///< in the box's middle
    right,  ///< ending at the box's right
};

/// Draws a text in a box, its capitals centred on the box's height. The text
/// is what the caller wants shown: the kit does not look it up. A character
/// the game font lacks, of the ones it stands in for, is drawn as the
/// characters in its place. In the modern look a line with tracking is drawn
/// in the tracked type (modern_type).
///
/// @param[in,out] target the surface
/// @param placement where the picture lands
/// @param fonts the fonts
/// @param role which of them
/// @param shown the text to draw, in UTF-8
/// @param box the box, in points
/// @param align where the text sits along the box's width
/// @param colour the text's colour, red, green and blue
/// @param tracking extra columns after each glyph but the last
void draw_boxed_text(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    const Fonts& fonts,
    FontRole role,
    std::string_view shown,
    const oa::ui::frontend_renderer::SourceRect& box,
    Align align,
    std::array<uint8_t, 3> colour,
    int32_t tracking = 0
);

/// Returns the bytes of the UTF-8 character a text starts with.
///
/// A well-formed sequence of two to four bytes returns its length. Anything
/// else, an ASCII byte included, returns 1. An empty text returns 0.
///
/// @param text the text, in UTF-8
/// @return the character's bytes
[[nodiscard]] std::size_t character_bytes(std::string_view text) noexcept;

/// Tells whether a byte continues a UTF-8 character rather than starting one.
///
/// @param byte the byte
/// @return true for 10xxxxxx
[[nodiscard]] bool continuation_byte(char byte) noexcept;

/// Returns how many characters a text holds, counting a byte that continues
/// a character as part of it.
///
/// @param text the text, in UTF-8
/// @return the characters
[[nodiscard]] std::size_t character_count(std::string_view text) noexcept;

/// A text's width, in points.
using Measure = std::function<int32_t(std::string_view)>;

/// The width counted for one character of a text measured without a font, in points.
inline constexpr int32_t estimated_character_width = 7;

/// Returns a text's estimated width: estimated_character_width a character,
/// twice that for a Chinese, Japanese or Korean character.
///
/// @param text the text, in UTF-8
/// @return the width, in points
[[nodiscard]] int32_t estimated_width(std::string_view text);

/// How one wrap differs from another. Unset, a word wider than a line is
/// broken between characters, a line has no limit, and a new line is not a
/// paragraph of its own.
struct WrapRules {
    /// A text with Chinese, Japanese or Korean characters breaks where the
    /// line breaker lets it (oa::base::text::first_row): at its spaces and
    /// between Chinese or Japanese characters, never starting a line with a
    /// closing mark or ending one with an opening mark, and inside a word,
    /// a Korean one among them, only when the word is wider than a line.
    bool wide_scripts{true};
    /// A new line starts a paragraph. An empty paragraph adds no blank line.
    bool newlines{};
    /// A word wider than a line becomes what this returns. Unset, the word is
    /// broken instead.
    std::function<std::string(std::string_view)> shorten_word{};
    /// Where a word wider than a line breaks, in bytes, at least one character.
    /// Unset, the break is after the whole characters that fit, and a character
    /// that does not fit takes a line of its own.
    std::function<std::size_t(std::string_view)> break_word{};
    /// The most lines kept. 0 keeps every line.
    std::size_t most_lines{};
    /// When more lines are made than most_lines, the last kept line becomes
    /// what this returns of that line and every dropped line joined by single
    /// spaces. The ellipsis, when one is wanted, is this function's.
    std::function<std::string(std::string_view)> shorten_last{};
};

/// Breaks a text into lines no wider than a width.
///
/// Words break at spaces. The rules say how a wide script, a new line, a word
/// wider than a line and a limit on the lines differ from that. One loop
/// wraps every caller.
///
/// @param text the text, in UTF-8
/// @param width the room, in points
/// @param measure a text's width in the font it is drawn in
/// @param rules how this call differs
/// @return the lines; none for an empty text
[[nodiscard]] std::vector<std::string>
wrap(std::string_view text, int32_t width, const Measure& measure, const WrapRules& rules = {});

/// Breaks a folder's path into lines no wider than a width: after a separator
/// ('/' or '\\'), each line as many whole components as fit, a component wider
/// than a line broken between its characters.
///
/// @param path the path, in UTF-8
/// @param width the room, in points
/// @param measure a text's width in the font it is drawn in
/// @return the lines, which put together give the path
[[nodiscard]] std::vector<std::string>
wrap_path(std::string_view path, int32_t width, const Measure& measure);

/// Returns how many bytes of a word's start fit a width: the longest run of
/// whole characters no wider than it, and at least the first character, so a
/// character wider than the width still takes a line of its own. This is
/// where wrap breaks a word wider than a line when its rules give no
/// break_word.
///
/// @param word the word, in UTF-8
/// @param width the room, in the measure's units
/// @param measure a text's width
/// @return the bytes of the start; 0 for an empty word
[[nodiscard]] std::size_t
fitting_start(std::string_view word, int32_t width, const Measure& measure);

/// Where a word wider than a line breaks, as WrapRules::break_word says, with
/// the room and the measure the line is wrapped to: the bytes of the word's
/// first piece, at least its first character.
using WordBreak = std::size_t (*)(std::string_view word, int32_t width, const Measure& measure);

/// Returns where a folder's path, a word wider than a line, breaks: after the
/// last path separator ('/' or '\\') among the whole characters of its start
/// that fit a width, so that a line ends with a folder's name and its
/// separator; with no such separator, where fitting_start breaks it. A
/// separator the path starts with is no place to break, and a word that fits
/// whole is not broken. The folder chooser breaks its texts this way.
///
/// @param word the word, in UTF-8
/// @param width the room, in the measure's units
/// @param measure a text's width
/// @return the bytes of the first piece; 0 for an empty word
[[nodiscard]] std::size_t path_break(std::string_view word, int32_t width, const Measure& measure);

/// The ellipsis that ends a shortened line.
inline constexpr std::string_view ellipsis = "\xE2\x80\xA6";

/// Returns a line shortened to a width with an ellipsis.
///
/// A line that fits is returned whole. Otherwise the longest start of whole
/// characters that, with its spaces at the end dropped and the ellipsis
/// added, fits; the ellipsis alone when no character does. The start is found
/// by halving, which takes a measure that grows with the text.
///
/// @param text the line, in UTF-8
/// @param width the room, in the measure's units
/// @param measure a text's width
/// @return the line as it fits; empty when not even the ellipsis fits
[[nodiscard]] std::string fit(std::string_view text, int32_t width, const Measure& measure);

/// Measures a screen's text in the modern fonts, at a size in canvas pixels,
/// for a screen laid out in canvas pixels (the Game files screen and the
/// folder chooser). The app fills it from the bundled fonts; a hook left null
/// is estimated.
struct TextMeasureHooks {
    void* context{}; ///< passed back to every hook
    /// Width in pixels of a line at a pixel size; null: 0.55 em a character.
    int (*width)(void* context, std::string_view text, int pixel_size, bool bold){};
    /// Height of a line in pixels; null: 1.25 em.
    int (*line_height)(void* context, int pixel_size, bool bold){};
};

/// Returns a line's width in canvas pixels as the hooks measure it.
///
/// An empty line is 0 wide and is not measured. Without a width hook, each
/// character counts 0.55 of the pixel size, and the whole is rounded up.
///
/// @param hooks the measure
/// @param text the line, in UTF-8
/// @param pixel_size the text's size, in canvas pixels
/// @param bold true for bold
/// @return the width, in canvas pixels
[[nodiscard]] int32_t
measured_width(const TextMeasureHooks& hooks, std::string_view text, int32_t pixel_size, bool bold);

/// Returns the height of a line in canvas pixels as the hooks measure it.
///
/// With a hook, at least 1. Without one, 1.25 times the pixel size, rounded up.
///
/// @param hooks the measure
/// @param pixel_size the text's size, in canvas pixels
/// @param bold true for bold
/// @return the height, in canvas pixels
[[nodiscard]] int32_t measured_line(const TextMeasureHooks& hooks, int32_t pixel_size, bool bold);

/// One text drawn wider than its box, or across another text. A release build
/// keeps none: recording is off there.
struct TextFitRecord {
    std::string screen;   ///< the screen named for this frame
    std::string text;     ///< the text drawn
    int32_t text_width{}; ///< how wide the text is, in the box's units
    int32_t box_x{};      ///< the box's left, in the same units
    int32_t box_y{};      ///< the box's top, in the same units
    int32_t box_width{};  ///< the box's width, in the same units
    bool wider{};         ///< the text is wider than its box
    bool across{};        ///< its ink crosses another text drawn in this paint
};

/// Names the screen later records belong to. The name stays until the next call.
///
/// @param screen the screen, as the check should report it
void set_text_fit_screen(std::string_view screen);

/// Clears the records and this paint's ink. The screen name stays.
void begin_text_fit_frame();

/// Puts later notes in a space of their own. A drawing copied onto the screen
/// from another buffer is not compared with the screen's text. Each end undoes
/// one begin. Release builds record nothing.
void begin_text_fit_space();

/// Ends the space begin_text_fit_space opened.
void end_text_fit_space();

/// Starts one paint: the ink used to notice text crossing other text is
/// cleared, and the records stay, so a check can read every text of a screen
/// after several paints.
void text_fit_begin_paint();

/// Notes a text drawn in a box. A text wider than the box, or whose ink
/// crosses another text of this paint, is recorded. Empty text is not.
/// Ink outside a clip is not drawn, so it crosses nothing; a clip with a
/// negative width hides every ink. Release builds record nothing.
///
/// @param text the text drawn
/// @param text_width how wide it is, in the box's units
/// @param box_x the box's left
/// @param box_y the box's top
/// @param box_width the box's width
/// @param box_height the box's height
/// @param ink_x the ink's left, after alignment
/// @param clip_x the clip's left; ignored when the clip has no area
/// @param clip_y the clip's top
/// @param clip_width the clip's width; 0 leaves the ink whole, and less than 0 hides it
/// @param clip_height the clip's height
void note_text_fit(
    std::string_view text,
    int32_t text_width,
    int32_t box_x,
    int32_t box_y,
    int32_t box_width,
    int32_t box_height,
    int32_t ink_x,
    int32_t clip_x = 0,
    int32_t clip_y = 0,
    int32_t clip_width = 0,
    int32_t clip_height = 0
);

/// Forgets the ink an opaque fill covers. Text drawn over that fill does not
/// count as crossing text the fill has hidden. Release builds record nothing.
///
/// @param x the fill's left, in the same units as the ink
/// @param y the fill's top
/// @param width the fill's width
/// @param height the fill's height
void cover_text_fit(int32_t x, int32_t y, int32_t width, int32_t height);

/// The records since begin_text_fit_frame. Empty in a release build.
///
/// @return the records
[[nodiscard]] const std::vector<TextFitRecord>& text_fit_records();

} // namespace oa::ui::kit
