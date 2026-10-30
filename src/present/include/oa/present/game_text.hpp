// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game text as the player sees it: the game's 8-bit code page read as
// Unicode and written back, which characters a game font draws with which
// of its bytes, and lines drawn in the modern fonts at the text size, with
// the outline, shadow and background the player's Language
// settings choose, broken into rows, laid on 8-bit or RGB pixels and
// reduced to the palette in use. The fonts themselves are reached through
// hooks the application installs; nothing here changes the simulation, a
// saved game or what a shared game sends.
#pragma once

#include "oa/present/surface.hpp"
#include "oa/present/text_style.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::present {

// ---- Characters and the game's code page ----

/// One character at the start of a text.
struct TextCharacter {
    std::size_t bytes{};   ///< bytes it takes; 0 at the end of the text
    char32_t code_point{}; ///< the character
    bool utf8_sequence{};  ///< two to four bytes of well-formed UTF-8
};

/// Reads the well-formed UTF-8 sequence of two to four bytes at the start of a text.
///
/// A sequence is well formed when it is not overlong, not a surrogate and
/// not past U+10FFFF.
///
/// @param text bytes of the text
/// @return the sequence's length and character; no bytes when the text does
///         not start with such a sequence
[[nodiscard]] TextCharacter utf8_sequence(std::string_view text) noexcept;

/// Gives the character a byte of the game's 8-bit code page stands for.
///
/// The code page is Windows-1252: Latin-1 with typographic marks, the euro
/// sign and a few letters at 0x80-0x9F. The five bytes it leaves undefined
/// stand for the control characters of the same value.
///
/// @param byte the byte
/// @return the character
[[nodiscard]] char32_t code_page_character(uint8_t byte) noexcept;

/// Gives the byte of the game's 8-bit code page that holds a character.
///
/// @param character the character
/// @return the byte; none for a character the code page lacks
[[nodiscard]] std::optional<uint8_t> code_page_byte(char32_t character) noexcept;

/// Appends a character to UTF-8 text.
///
/// @param[in,out] text the text
/// @param character a code point up to U+10FFFF; others append nothing
void append_utf8(std::string& text, char32_t character);

/// Reads game text as UTF-8.
///
/// Game text is what saves, game data and other players' machines hold:
/// bytes of the game's code page, and UTF-8 where a profile's text
/// rendering lets players send it.
///
/// @param bytes the game text
/// @param utf8 read each well-formed UTF-8 sequence as its character; every
///        other byte, and every byte without it, is read in the code page
/// @return the text as UTF-8
[[nodiscard]] std::string decode_game_text(std::string_view bytes, bool utf8);

/// Writes UTF-8 text as game text.
///
/// @param text UTF-8 text, as typed
/// @param utf8 keep the text as UTF-8; without it each character becomes its
///        code-page byte, and one the code page lacks, or a byte that is not
///        UTF-8, becomes '?'
/// @return the game text
[[nodiscard]] std::string encode_game_text(std::string_view text, bool utf8);

/// Returns where the last character of a text starts, for an erase key.
///
/// @param text bytes of the text
/// @return the start of a trailing well-formed UTF-8 sequence, else the last
///         byte's position; 0 for an empty text
[[nodiscard]] std::size_t last_character_start(std::string_view text) noexcept;

// ---- What a game font draws ----

/// The characters a game font draws, each with the byte of its glyph.
///
/// ASCII, from the space up, is its own byte. Above it, the game's fonts
/// hold letters of two code pages: the GUI fonts (hattfont12 and hattfont11)
/// hold the DOS code page's letters at 0x80-0xAF and part of the game's code
/// page at 0xB0 and 0xBF-0xFC; the FNT fonts hold the game's code page from
/// 0xA1 to 0xFE, and some of its marks at 0x80-0x9F. A character in both
/// places takes the game's code page slot. Only the slots that hold a real
/// glyph count.
class FontCharacters {
  public:

    /// Tells whether a font has a real glyph at a byte.
    using HasGlyph = std::function<bool(uint8_t byte)>;

    /// Builds the characters of a GUI font.
    ///
    /// @param has_glyph whether the glyph at a byte is a real one, not the
    ///        font's box for a missing character
    /// @return the characters
    [[nodiscard]] static FontCharacters gui_font(const HasGlyph& has_glyph);

    /// Builds the characters of an FNT font.
    ///
    /// @param has_glyph whether the font has a glyph at a byte
    /// @return the characters
    [[nodiscard]] static FontCharacters fnt_font(const HasGlyph& has_glyph);

    /// Gives the byte a character is drawn with.
    ///
    /// @param character the character
    /// @return the byte; none for a character the font cannot draw
    [[nodiscard]] std::optional<uint8_t> byte_for(char32_t character) const noexcept;

  private:

    /// The characters above ASCII, by character.
    std::vector<std::pair<char32_t, uint8_t>> extra_{};
};

/// Tells whether a GUI font's glyph is the same picture as another, such as
/// the font's box for a missing character.
///
/// @param glyph a glyph; null matches nothing
/// @param other the other glyph; null matches nothing
/// @return true when both have the same size, hotspot and pixels
[[nodiscard]] bool same_glyph(const Sprite* glyph, const Sprite* other);

/// One run of a line of text: bytes the game font draws, or characters the
/// modern fonts draw.
struct TextRun {
    bool modern{};      ///< the modern fonts draw it
    std::string text{}; ///< the font's bytes, or UTF-8 for the modern fonts
    /// The size the modern fonts draw it at, in percent of the game font's:
    /// the player's text size for game text they draw whole, the game
    /// font's own for the characters it lacks.
    int32_t size{game_font_text_size};
};

/// Splits UTF-8 text into the runs a game font draws and the runs it leaves
/// to the modern fonts.
///
/// A text that holds a letter the game font lacks (a Cyrillic or Greek
/// letter, a Latin letter beyond its code page) is drawn whole by the modern
/// fonts, its digits, Latin words and marks with it, so that neither a word
/// such as "будь-якого" nor its line changes typeface part way. Otherwise
/// each character is drawn by the game font when it has it, and only the
/// others by the modern fonts: all of English keeps the game font, and so
/// do the digits and Latin letters beside a symbol the font lacks (→) or a
/// Chinese, Japanese or Korean character.
///
/// @param text UTF-8 text
/// @param font the characters the game font draws
/// @return the runs, in order; adjacent characters of one kind share a run
[[nodiscard]] std::vector<TextRun> split_text(std::string_view text, const FontCharacters& font);

/// The step a caption too wide for its gadget shrinks its modern text by,
/// in percent of the game font's size.
inline constexpr int32_t fitted_text_size_step = 5;

/// Returns the most size, in percent of the game font's, a caption's modern
/// runs may be drawn at so that the caption is no wider than its room: the
/// game font's own size when it fits, else the largest size, a step of
/// fitted_text_size_step at a time, down to lowest_text_size, at which it
/// fits, else lowest_text_size. A caption whose width does not change with
/// the size, as one the game font draws whole, keeps the game font's size.
///
/// @param width_at the caption's width with its modern runs drawn at most a
///        size, in pixels
/// @param room the room, in pixels
/// @return the size, from lowest_text_size to game_font_text_size
[[nodiscard]] int32_t
fitted_text_size(const std::function<int32_t(int32_t most_size)>& width_at, int32_t room);

/// Tells whether split_text draws a text whole in the modern fonts: it holds
/// a letter the game font lacks.
///
/// @param text UTF-8 text
/// @param font the characters the game font draws
/// @return true when the modern fonts draw all of it
[[nodiscard]] bool drawn_whole_in_modern_fonts(std::string_view text, const FontCharacters& font);

// ---- Modern text ----

/// The game font a line of modern text stands in for, which sets its size
/// and weight.
enum class TextFace : uint8_t {
    message, ///< hattfont12 and COMIX.FNT: the message log and the GUI's text, bold
    status,  ///< hattfont11: the status readouts, bold and smaller
    label,   ///< CONSOLE.FNT and SMLFONT.FNT: labels and the chat line, regular
};

/// A line drawn in the modern fonts: how much of each pixel its letters
/// cover.
struct TextMask {
    int32_t width{};    ///< columns
    int32_t height{};   ///< rows
    int32_t baseline{}; ///< the row just below the capitals
    int32_t origin{};   ///< the column the pen starts at
    int32_t advance{};  ///< pixels the pen moves
    /// width * height bytes, top row first: 0 untouched, 255 fully covered
    std::vector<uint8_t> alpha{};
    /// Where the pen stands after each character of the text, in order.
    std::vector<int32_t> character_ends{};
};

/// How game text is drawn now.
struct TextSettings {
    /// The player's Language settings; modern_fonts only while the
    /// modern fonts can be drawn.
    TextStyle style{false, true, true, false, default_text_size};
    /// Game text may hold UTF-8 (a profile's text rendering, unicode).
    bool utf8{};
};

/// The hooks game text reaches the modern fonts and the player's settings
/// through.
struct GameTextHooks {
    void* context{};
    /// Gives the settings in effect.
    TextSettings (*settings)(void* context){};
    /// Draws a line of UTF-8 in the modern font of a face, `scale` screen
    /// pixels to a game pixel, at `size` percent of the face's own size;
    /// null when it cannot.
    std::shared_ptr<const TextMask> (*draw)(
        void* context, std::string_view text, TextFace face, int32_t scale, int32_t size
    ){};
    /// Gives the palette 8-bit GUI surfaces are drawn in: entries of red,
    /// green, blue and one more byte.
    std::span<const uint8_t> (*palette)(void* context){};
};

/// Installs the hooks game text draws through.
///
/// @param hooks kept until the next call; default hooks remove them
void set_game_text_hooks(const GameTextHooks& hooks) noexcept;

/// Returns the installed game-text hooks.
///
/// @return the hooks; draw is null when none are installed
[[nodiscard]] const GameTextHooks& game_text_hooks() noexcept;

/// Returns the settings game text is drawn with now.
///
/// @return the hooks' settings; without hooks, the game's fonts alone and
///         no UTF-8
[[nodiscard]] TextSettings game_text_settings();

/// Holds a text size to the sizes the setting offers.
///
/// @param size a text size, in percent
/// @return the size, lowest_text_size to highest_text_size
[[nodiscard]] constexpr int32_t held_text_size(int32_t size) noexcept {
    return size < lowest_text_size    ? lowest_text_size
           : size > highest_text_size ? highest_text_size
                                      : size;
}

/// Gives a length at a text size.
///
/// @param length a length at the game fonts' size, 0 or more, in pixels
/// @param size the text size, in percent
/// @return length times size over 100, to the nearest pixel, a half up
[[nodiscard]] constexpr int32_t sized_length(int32_t length, int32_t size) noexcept {
    return (length * size + game_font_text_size / 2) / game_font_text_size;
}

/// Returns the size game text is drawn at now.
///
/// @return the settings' text size, held to the sizes offered, while the
///         modern fonts draw game text; else game_font_text_size, the game
///         fonts' own
[[nodiscard]] int32_t game_text_size();

/// The smallest pixel size the modern fonts are drawn at, however small the
/// text size: below it their letters stop being readable.
inline constexpr int32_t least_text_pixel_size = 7;

/// Gives the pixel size of a modern face at a scale and text size.
///
/// @param game_font_pixels the face's pixel size beside the game font it
///        stands in for, at a scale of 1
/// @param scale screen pixels to a game pixel, 1 or more
/// @param size the text size, in percent
/// @return the face's size at the text size, to the nearest pixel, times
///         the scale; least_text_pixel_size at the least
[[nodiscard]] int32_t
face_pixel_size(int32_t game_font_pixels, int32_t scale, int32_t size) noexcept;

/// Gives how thick the outline of modern text is, and how far its shadow
/// falls.
///
/// @param scale screen pixels to a game pixel, 1 or more
/// @param size the text size, in percent
/// @return the scale at the size, to the nearest pixel, a half down; 1 at
///         the least
[[nodiscard]] int32_t text_border(int32_t scale, int32_t size) noexcept;

/// The colour of a letter's outline: the dark grey of the game's own font
/// outlines.
inline constexpr std::array<uint8_t, 3> text_outline_color{43, 43, 43};
/// The colour of the outline over pixels darker than text_outline_color.
inline constexpr std::array<uint8_t, 3> text_dark_outline_color{0, 0, 0};
/// The colour of the shadow and of the background box: black.
inline constexpr std::array<uint8_t, 3> text_shade_color{0, 0, 0};
/// How much of the shadow's colour covers the pixels under it: half.
inline constexpr uint8_t text_shadow_alpha = 128;
/// The colour hattfont12's and hattfont11's letters are drawn in.
inline constexpr std::array<uint8_t, 3> gui_font_color{195, 195, 155};

/// A line of modern text with its outline, shadow and background, ready to
/// lay on pixels.
struct TextLayers {
    int32_t width{};    ///< columns
    int32_t height{};   ///< rows
    int32_t baseline{}; ///< the row just below the capitals
    int32_t pen{};      ///< the column the pen starts at
    int32_t advance{};  ///< pixels the pen moves past the line
    /// width * height bytes each, top row first: the letters' coverage, 0 to
    /// 255; and 1 where the outline and the shadow lie
    std::vector<uint8_t> fill{};
    std::vector<uint8_t> outline{};
    std::vector<uint8_t> shadow{};
    /// A black box lies under the line first, over every column and the
    /// rows from background_top to background_bottom, inclusive.
    bool background{};
    int32_t background_top{};
    int32_t background_bottom{};
    /// The first and last rows the letters, the outline or the shadow lie
    /// on, inclusive; both -1 for a line with nothing drawn.
    int32_t drawn_top{-1};
    int32_t drawn_bottom{-1};
};

/// Adds the borders a style asks for to a line.
///
/// The outline covers every pixel within `thickness` of a letter, on all
/// eight sides, that the letters leave; the shadow is the letters and their
/// outline moved `thickness` down and right. The line grows by the borders'
/// room on each side, and the pen moves `thickness` further past a line
/// with either border. The background box spans the rows the letters and
/// their borders reach, so that close lines keep each other's letters; a
/// line with nothing drawn has none.
///
/// @param mask the line's letters
/// @param style which borders
/// @param thickness pixels of the outline and of the shadow's offset, 1 or more
/// @return the layers
[[nodiscard]] TextLayers
build_text_layers(const TextMask& mask, const TextStyle& style, int32_t thickness);

/// Draws a line in the modern fonts with the borders the settings choose.
///
/// The layers of the lines drawn last are kept, at most 256 lines and
/// 16 MiB of them, so a line drawn again with the same letters and borders
/// is laid from them rather than built anew; forget_undrawn_text_layers
/// lets go those of the lines no longer drawn.
///
/// @param text UTF-8 text
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel, 1 or more: the size and the
///        borders grow with it
/// @param size the text size, in percent of the face's own, held to the
///        sizes offered: the size and the borders (text_border) grow with it
/// @param allow_background false leaves out the background box whatever the
///        settings say, for text whose box its caller lays
/// @return the layers; none when the modern fonts cannot draw it
[[nodiscard]] std::optional<TextLayers> modern_text(
    std::string_view text, TextFace face, int32_t scale, int32_t size, bool allow_background = true
);

/// Gives how far the pen moves past a line drawn in the modern fonts with
/// the borders the settings choose, without building the line's layers.
///
/// @param text UTF-8 text
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel, 1 or more
/// @param size the text size, in percent of the face's own, held to the
///        sizes offered
/// @return the advance modern_text gives the same line; none when the
///         modern fonts cannot draw it
[[nodiscard]] std::optional<int32_t>
modern_text_advance(std::string_view text, TextFace face, int32_t scale, int32_t size);

/// Drops the layers modern_text keeps of the lines it drew last, so that
/// each line is built anew when it is next drawn. Installing hooks drops
/// them too.
void forget_text_layers() noexcept;

/// Drops the layers modern_text keeps of the lines it has not drawn since
/// the last call, so that a caller that draws every line it shows on each
/// of its frames, as a match does, keeps the layers of those lines alone
/// and none of the lines it showed before: a readout's earlier numbers or
/// the screens left behind.
void forget_undrawn_text_layers() noexcept;

/// Gives the longest start of a text whose modern line fits a width.
///
/// @param text UTF-8 text
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @param width the room, in screen pixels
/// @return the bytes of the whole characters that fit
[[nodiscard]] std::size_t
modern_text_fit(std::string_view text, TextFace face, int32_t scale, int32_t size, int32_t width);

/// Gives where the longest end of a text whose modern line fits a width
/// starts, as a line being typed shows its end.
///
/// @param text UTF-8 text
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @param width the room, in screen pixels
/// @return the byte of the first whole character that fits with all after
///         it; 0 when the whole text fits or the modern fonts cannot draw it
[[nodiscard]] std::size_t
modern_text_tail(std::string_view text, TextFace face, int32_t scale, int32_t size, int32_t width);

/// Where a stretch of a modern line is underlined, as the input method's
/// composition is marked apart from the text already typed.
struct TextUnderline {
    int32_t left{};      ///< its first column, from the line's pen
    int32_t width{};     ///< columns
    int32_t row{};       ///< its first row, from the line's baseline
    int32_t thickness{}; ///< rows
};

/// Gives where a stretch of a modern line is underlined: under its
/// characters, on the line's lowest rows, as thick as its outline.
///
/// @param text UTF-8 text, the whole line
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @param from the stretch's first byte
/// @param to the byte past the stretch's last
/// @return the underline; none when the modern fonts cannot draw the line
///         or the stretch holds no character
[[nodiscard]] std::optional<TextUnderline> modern_text_underline(
    std::string_view text,
    TextFace face,
    int32_t scale,
    int32_t size,
    std::size_t from,
    std::size_t to
);

/// Where one row of a broken line lies in its text.
struct TextRowSpan {
    std::size_t offset{}; ///< the row's first byte
    std::size_t bytes{};  ///< the row's bytes, the spaces it was broken at left out
};

/// Breaks a line of modern text into rows no wider than a width, as
/// modern_text draws them with the borders the settings choose.
///
/// A row ends at the last place that lets it fit where it may break
/// (oa::base::text::first_row): at a space, whose spaces belong to neither
/// row, or between two Chinese or Japanese characters, never starting a row
/// with a closing mark or ending one with an opening mark; a Korean word,
/// like a Latin one, is kept whole. A word wider than a row is broken after
/// its last character that fits, and every row holds at least one
/// character.
///
/// @param text UTF-8 text
/// @param face the game font it stands in for
/// @param scale screen pixels to a game pixel
/// @param size the text size, in percent
/// @param width the room, in screen pixels
/// @return the rows, in order; one row of the whole text when the modern
///         fonts cannot draw it, none for an empty text
[[nodiscard]] std::vector<TextRowSpan>
modern_text_rows(std::string_view text, TextFace face, int32_t scale, int32_t size, int32_t width);

/// What a run of see-through pixels asks of the picture under a canvas
/// (TextCanvas::world_run).
enum class TextWorldLayer : uint8_t {
    shadow,  ///< darkened toward text_shade_color by text_shadow_alpha
    outline, ///< each channel held to text_outline_color's at the most
    letter,  ///< the letters' colour over it at the run's coverage
};

/// A run of one row's see-through pixels a line hands to the picture under
/// the canvas.
struct TextWorldRun {
    TextWorldLayer layer{};
    int32_t x{}; ///< the run's first column
    int32_t y{}; ///< its row
    int32_t width{};
    std::array<uint8_t, 3> color{}; ///< the letters' colour, for a letter run
    /// For a letter run, how much of each pixel the letters cover, in
    /// 255ths, below the whole.
    uint8_t coverage{};

    friend bool operator==(const TextWorldRun&, const TextWorldRun&) = default;
};

/// Pixels text is laid on: 8-bit palette indices or RGB.
struct TextCanvas {
    uint8_t* pixels{};
    int32_t width{};
    int32_t height{};
    std::ptrdiff_t pitch{}; ///< bytes from one row to the next
    bool indexed{};         ///< one palette index a pixel; else red, green and blue
    /// The pixels drawn may change, inclusive.
    int32_t clip_left{};
    int32_t clip_top{};
    int32_t clip_right{};
    int32_t clip_bottom{};
    /// Entries of red, green, blue and one more byte; every drawn colour is
    /// reduced to its nearest entry. Empty on an RGB canvas keeps the colours.
    std::span<const uint8_t> palette{};
    /// On an RGB canvas laid over a picture drawn elsewhere, such as the
    /// overlay canvas of the battlefield the graphics card draws, the colour
    /// of the pixels that hold nothing of the canvas's own, where that
    /// picture shows through; none on a canvas that holds its whole
    /// picture. The canvas cannot tell what the picture is there, so over
    /// such a pixel a letter covering it whole takes the text's colour, and
    /// every other layer is left to world_run.
    std::optional<std::array<uint8_t, 3>> see_through{};
    /// Called, once a line is laid, for each run of a row's see-through
    /// pixels a layer falls on that no letter covers whole, which stay as
    /// they were: first every shadow run, then every outline run, then every
    /// letter run, so that the caller draws the layers over the picture in
    /// the order lay_text lays them. Runs of one layer that touch in a row
    /// are one run, as are letter runs of one coverage. Null leaves those
    /// pixels to the picture alone.
    void (*world_run)(void* user, const TextWorldRun& run){};
    void* world_user{}; ///< handed to world_run
};

/// Views an 8-bit surface as a canvas, within its clip rectangle.
///
/// @param surface the surface
/// @param palette its palette
/// @return the canvas
[[nodiscard]] TextCanvas indexed_canvas(Surface& surface, std::span<const uint8_t> palette);

/// Views RGB pixels as a canvas.
///
/// @param rgb width * height * 3 bytes, top row first
/// @param width columns
/// @param height rows
/// @param palette the palette colours are reduced to; empty keeps them
/// @return the canvas
[[nodiscard]] TextCanvas
rgb_canvas(std::span<uint8_t> rgb, int32_t width, int32_t height, std::span<const uint8_t> palette);

/// Lays a line on a canvas: the background box, then the shadow at half
/// strength, then the outline, then the letters blended in their colour by
/// their coverage. The outline is text_outline_color, or
/// text_dark_outline_color over a pixel darker than that. Over a pixel the
/// picture under the canvas shows through (TextCanvas::see_through) only a
/// letter covering it whole is laid; the shadow, the outline and the
/// letters' partial coverage there go to the canvas's world_run, for the
/// picture's drawing.
///
/// @param[in,out] canvas the pixels
/// @param layers the line
/// @param x the column of the pen
/// @param baseline_y the row just below the capitals
/// @param color the letters' colour
void lay_text(
    TextCanvas& canvas,
    const TextLayers& layers,
    int32_t x,
    int32_t baseline_y,
    std::array<uint8_t, 3> color
);

/// Returns the colour a text colour makes over another at a coverage.
///
/// @param under colour under the text
/// @param over text colour
/// @param alpha coverage, 0 (under) to 255 (over)
/// @return each channel under + (over - under) * alpha / 255, rounded to nearest
[[nodiscard]] std::array<uint8_t, 3>
blend_color(std::array<uint8_t, 3> under, std::array<uint8_t, 3> over, uint8_t alpha) noexcept;

/// Finds the palette entry nearest a colour by squared distance over every entry.
///
/// @param palette entries of `entry_bytes` bytes, red, green and blue first
/// @param entry_bytes bytes of one entry, 3 or more
/// @param color colour looked for
/// @return the first nearest entry; 0 for an empty palette
[[nodiscard]] uint8_t nearest_palette_index(
    std::span<const uint8_t> palette, std::size_t entry_bytes, std::array<uint8_t, 3> color
) noexcept;

} // namespace oa::present
