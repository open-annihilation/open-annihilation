// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Game text in the game's own fonts and in the modern fonts, as the
// player's Language settings choose (oa/present/game_text.hpp): what
// a GUI or FNT font draws, where its baseline lies and which modern face
// stands in for it, and a line drawn on the RGB screens.

#include "oa/formats/fnt.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/present/gaf_sprites.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::ui::frontend_renderer {

/// Gives the characters a GUI font draws: every glyph that is not the
/// picture of glyph 0, the font's box for a missing character. They are
/// worked out once per font and kept.
///
/// @param font the GUI font
/// @return the characters
[[nodiscard]] present::FontCharacters gui_font_characters(const present::GafSprites& font);

/// Forgets the characters gui_font_characters kept. They are kept by the
/// address of each font's glyphs, so they go when the fonts do: a font
/// loaded later may come at the same address with other glyphs.
void forget_gui_font_characters();

/// Gives the rows from a GUI font's pen down to its baseline: the height of
/// its 'I', as gadget text hangs the glyphs from the pen.
///
/// @param font the GUI font
/// @return the rows; 0 without an 'I'
[[nodiscard]] int32_t gui_font_baseline(const present::GafSprites& font);

/// Gives the modern face a GUI font stands for: the message face for
/// hattfont12's height, the status face for a smaller one.
///
/// @param font the GUI font
/// @return the face
[[nodiscard]] present::TextFace gui_font_face(const present::GafSprites& font);

/// Gives the characters an FNT font draws: the bytes it has glyphs for. A
/// GUI font read as one (formats::fnt::Font::gaf_backed) draws its GUI
/// font's characters: every glyph that is not the picture of glyph 0, its
/// box for a missing character.
///
/// @param font the font
/// @return the characters
[[nodiscard]] present::FontCharacters fnt_font_characters(const formats::fnt::Font& font);

/// Gives the rows from the pen row raster_text takes down to an FNT font's
/// baseline: one below the last row of its 'H'.
///
/// @param font the font
/// @return the rows; the line height less two without an 'H'
[[nodiscard]] int32_t fnt_font_baseline(const formats::fnt::Font& font);

/// Gives the palette entry a font's letters are drawn in, which the modern
/// fonts draw its text in too: the lightest of the entries its 'H' is drawn
/// in through a palette. A GUI font's letters are shaded and outlined in
/// darker entries than their face; an FNT font draws every letter in
/// formats::fnt::foreground_index.
///
/// @param font the font
/// @param palette the palette the font is drawn through
/// @return the entry; formats::fnt::foreground_index for a font without an 'H'
[[nodiscard]] uint8_t fnt_font_ink(const formats::fnt::Font& font, const PaletteBytes& palette);

/// Gives the modern face an FNT font stands for: the message face for a
/// font whose 'H' stands 9 rows or more, as COMIX.FNT's does, the label
/// face for the smaller ones, such as CONSOLE.FNT and SMLFONT.FNT.
///
/// @param font the font
/// @return the face
[[nodiscard]] present::TextFace fnt_font_face(const formats::fnt::Font& font);

/// Tells whether a text needs more than an 8-bit font's own bytes: it has a
/// byte from 0x80 up, or the settings draw the whole of game text in the
/// modern fonts.
///
/// @param text the game text
/// @param game_text the text is game text, which the settings may draw in
///        the modern fonts
/// @return true when the text goes through split_game_text
[[nodiscard]] bool needs_text_runs(std::string_view text, bool game_text);

/// Splits game text into what a game font draws and what the modern fonts
/// draw.
///
/// The bytes are read as the settings say (decode_game_text). Game text the
/// settings draw in the modern fonts is one modern run at the settings'
/// text size; other text keeps the font for every character it draws, and
/// the modern fonts draw the rest at the font's own size.
///
/// @param text the game text
/// @param font the characters the game font draws
/// @param game_text the text is game text
/// @return the runs
[[nodiscard]] std::vector<present::TextRun>
split_game_text(std::string_view text, const present::FontCharacters& font, bool game_text);

/// Gives the size a run is drawn at on the menus' and lobbies' screens,
/// whose gadgets are laid out for the game's fonts: the run's size, held to
/// the game fonts' own. Smaller text keeps the font's baseline.
///
/// @param run the run
/// @return the size, in percent
[[nodiscard]] int32_t screen_text_size(const present::TextRun& run) noexcept;

/// Gives a colour through a row of the display's light table, as lit
/// glyphs are drawn.
///
/// @param color the colour
/// @param palette the palette the table's entries index
/// @param level the light-table row; 0 leaves the colour
/// @return the lit colour; the colour itself without a light table
[[nodiscard]] std::array<uint8_t, 3>
lit_text_color(std::array<uint8_t, 3> color, std::span<const uint8_t> palette, int32_t level);

/// The pixels of a screen text may change, inclusive.
struct TextClip {
    int32_t left{};
    int32_t top{};
    int32_t right{};
    int32_t bottom{};
};

/// Draws FNT-font glyphs on an RGB screen as formats::fnt::raster_text
/// places them, each covered pixel inside the clip in the palette's colour
/// of its glyph pixel's index. Only the pixels the glyphs can cover are
/// visited.
///
/// @param[in,out] surface the screen
/// @param font the FNT font
/// @param text the glyphs, one byte each
/// @param x the pen column
/// @param y the pen row raster_text takes
/// @param palette the screen's palette
/// @param clip the pixels that may change
/// @param light_row the light-table row each index is lit through, or nullptr
///        to draw the indices as they are
void draw_fnt_glyphs(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    const PaletteBytes& palette,
    const TextClip& clip,
    const uint8_t* light_row = nullptr
);

/// Draws a line of FNT-font game text on an RGB screen, the modern fonts
/// drawing what the settings give them.
///
/// The game font's part is drawn in the palette's colours of its glyphs'
/// pixels; each modern run is placed on the font's baseline in `color` and
/// reduced to the palette. Glyphs, and modern characters, are drawn up to
/// the first one that does not wholly fit left of the clip's right edge.
///
/// @param[in,out] surface the screen
/// @param font the FNT font
/// @param text the game text
/// @param x the pen column
/// @param y the pen row raster_text takes
/// @param color the text's colour
/// @param palette the screen's palette
/// @param clip the pixels that may change
/// @param game_text the text is game text, which the settings may draw in the
///        modern fonts
/// @param most_size the most size, in percent of the game font's, its modern
///        runs are drawn at, as a caption shrunk to fit its gadget's
///        (present::fitted_text_size)
/// @return the pen column after the text
int32_t draw_fnt_game_text(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    std::array<uint8_t, 3> color,
    const PaletteBytes& palette,
    const TextClip& clip,
    bool game_text,
    int32_t most_size = present::game_font_text_size
);

/// Draws the input method's composition at the end of a line of FNT-font
/// game text: wholly in the modern fonts, as the characters it composes
/// are drawn, the Latin letters of its spelling among them, and underlined
/// in its colour on the line's lowest rows (present::modern_text_underline).
/// Where the modern fonts cannot draw it the font draws it, underlined on
/// its line's last row. As much of it as fits left of the clip's right edge
/// is drawn.
///
/// @param[in,out] surface the screen
/// @param font the FNT font the line is drawn in
/// @param composition the composition, as game text
/// @param x the pen column
/// @param y the pen row raster_text takes
/// @param color the text's colour
/// @param palette the screen's palette
/// @param clip the pixels that may change
/// @return the pen column after the composition
int32_t draw_fnt_composition(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view composition,
    int32_t x,
    int32_t y,
    std::array<uint8_t, 3> color,
    const PaletteBytes& palette,
    const TextClip& clip
);

/// Measures FNT-font game text as draw_fnt_game_text draws it.
///
/// @param font the FNT font
/// @param text the game text
/// @param game_text the text is game text
/// @param most_size the most size, in percent of the game font's, its modern
///        runs are drawn at
/// @return the width in pixels
[[nodiscard]] int32_t measure_fnt_game_text(
    const formats::fnt::Font& font,
    std::string_view text,
    bool game_text,
    int32_t most_size = present::game_font_text_size
);

/// Returns the most size a caption's modern runs are drawn at so that the
/// caption, measured as measure_fnt_game_text measures it, fits its room
/// (present::fitted_text_size): the game font's own size when it fits, as
/// every caption the game font draws whole keeps.
///
/// @param font the FNT font
/// @param text the game text
/// @param room the room, in pixels
/// @param game_text the text is game text
/// @return the size, in percent of the game font's
[[nodiscard]] int32_t fitted_fnt_text_size(
    const formats::fnt::Font& font, std::string_view text, int32_t room, bool game_text
);

/// The rows of a line, counted from its pen row.
struct LineRows {
    int32_t top{};    ///< the first row; negative above the pen row
    int32_t bottom{}; ///< the last row, inclusive
};

/// Gives the rows the modern fonts draw on in a line of FNT-font game text,
/// as draw_fnt_game_text draws it: their letters, outline and shadow, which
/// may stand taller over the font's baseline than the font's own letters,
/// as ideographs do, and reach further below it.
///
/// @param font the FNT font
/// @param text the game text
/// @param game_text the text is game text
/// @param most_size the most size, in percent of the game font's, its modern
///        runs are drawn at
/// @return the rows from the first modern run's top to the last's bottom;
///         none when the modern fonts draw nothing of it, as for text the
///         font draws whole
[[nodiscard]] std::optional<LineRows> fnt_game_text_modern_rows(
    const formats::fnt::Font& font,
    std::string_view text,
    bool game_text,
    int32_t most_size = present::game_font_text_size
);

/// Where a caption of FNT-font game text stands in a box's rows, and the
/// size its modern runs are drawn at.
struct CaptionRows {
    int32_t pen_row{}; ///< the pen row raster_text takes
    /// the most size, in percent of the game font's, its modern runs are
    /// drawn at
    int32_t most_size{present::game_font_text_size};
};

/// Keeps what the modern fonts draw of a caption to a box's rows, as the
/// font's own letters keep to them: drawn smaller, a step of
/// present::fitted_text_size_step at a time down to the lowest text size,
/// while it stands taller than the box (present::fitted_text_size), then
/// raised by what it would draw below the box, such as descenders and the
/// shadow, and lowered by what it would draw above it, such as ideographs
/// standing taller than the font's letters. A caption still taller than
/// the box keeps to its top. A caption the font draws whole stands on the
/// box's top row at the game font's size, where 3.1c puts it.
///
/// @param font the FNT font
/// @param text the game text
/// @param game_text the text is game text
/// @param top the box's first row
/// @param height the box's rows
/// @return the pen row and the most size
[[nodiscard]] CaptionRows fnt_caption_rows(
    const formats::fnt::Font& font,
    std::string_view text,
    bool game_text,
    int32_t top,
    int32_t height
);

} // namespace oa::ui::frontend_renderer
