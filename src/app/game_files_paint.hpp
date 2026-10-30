// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Painting the Game files screen: a laid-out step's kit parts drawn into an
// RGBA canvas by role with the touch controls' painter and the bundled fonts,
// at the window's own pixel density, and a canvas written as a picture for
// people. The screen's OA mark is the Open Annihilation icon, drawn as the
// settings dialog draws it. The folder chooser paints its layout here too.
#pragma once

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/game_files.hpp"
#include "oa/ui/paint/painter.hpp"

#include <filesystem>
#include <string>

namespace oa::app {

// Named once, whichever app header that draws with the painter is included first.
#ifndef OA_APP_UI_PAINT
#define OA_APP_UI_PAINT
namespace paint = oa::ui::paint;
#endif

/// The bundled fonts as the screen's text measure (TextMeasureHooks over a FontStack), in
/// the family the screen's text is painted in (paint_game_files).
///
/// @param fonts the bundled fonts; null measures with the hooks' fallbacks
/// @param family the sans family, DejaVu Sans, or in the modern look the interface family,
///     Barlow, a bold line in its SemiBold
/// @return the measure, which keeps `fonts` as its context
[[nodiscard]] oa::ui::game_files::TextMeasureHooks game_files_measure(
    oa::platform::text_font::FontStack* fonts,
    oa::platform::text_font::Family family = oa::platform::text_font::Family::sans
) noexcept;

/// Returns the Open Annihilation icon the screen shows as its OA mark: the game's window icon
/// without its clear margin, the picture the settings dialog is handed, decoded once.
///
/// @return the icon; an empty picture when it cannot be decoded, which draws the OA mark
[[nodiscard]] const oa::ui::frontend_renderer::RgbaPicture& game_files_icon();

/// Paints a laid-out step into a canvas of the viewport's size: the background, every part
/// in order (fills, outlines, the marks, wrapped text in the bundled fonts), the focus ring.
/// The OA mark shows game_files_icon(), and the density is estimated from the smallest
/// button.
///
/// @param[in,out] canvas the canvas, of the viewport's size
/// @param layout the laid-out step
/// @param fonts the bundled fonts; null paints no text
void paint_game_files(
    paint::Canvas& canvas,
    const oa::ui::game_files::Layout& layout,
    oa::platform::text_font::FontStack* fonts
);

/// Paints a laid-out step at a known density: as the paint above, with every size the
/// painter adds of its own (outlines, corners, the focus ring, gaps) in points.
///
/// @param[in,out] canvas the canvas, of the viewport's size
/// @param layout the laid-out step
/// @param fonts the bundled fonts; null paints no text
/// @param px_per_point canvas pixels per point (Viewport::px_per_point)
void paint_game_files(
    paint::Canvas& canvas,
    const oa::ui::game_files::Layout& layout,
    oa::platform::text_font::FontStack* fonts,
    float px_per_point
);

/// Paints a laid-out step at a known density with an icon of the caller's: its parts, as
/// the overload for parts below paints them. The folder chooser paints its layout with it.
///
/// @param[in,out] canvas the canvas, of the viewport's size
/// @param layout the laid-out step
/// @param fonts the bundled fonts; null paints no text
/// @param px_per_point canvas pixels per point (Viewport::px_per_point)
/// @param icon the Open Annihilation icon; empty draws the OA mark
/// @param family the family the text is painted in, as the overload for parts below says
void paint_game_files(
    paint::Canvas& canvas,
    const oa::ui::game_files::Layout& layout,
    oa::platform::text_font::FontStack* fonts,
    float px_per_point,
    const oa::ui::frontend_renderer::RgbaPicture& icon,
    oa::platform::text_font::Family family = oa::platform::text_font::Family::sans
);

/// Paints the kit's parts of a screen drawn in the modern fonts, by role, at a known density
/// with an icon of the caller's: the background, then each part in order, clipped to its
/// clip, and the focus ring round a focused one. The badge and every Glyph::oa mark show
/// the icon scaled to their square at the canvas's own pixels, as the settings dialog's
/// header shows it; an empty picture draws the dialog's OA mark instead. A role the screen
/// does not lay out draws nothing.
///
/// @param[in,out] canvas the canvas, of the viewport's size
/// @param parts the parts, in canvas pixels
/// @param fonts the bundled fonts; null paints no text
/// @param px_per_point canvas pixels per point (Viewport::px_per_point)
/// @param icon the Open Annihilation icon; empty draws the OA mark
/// @param family the family the text is painted in: the sans family, DejaVu Sans, or in the
///     modern look the interface family, Barlow, a bold line in its SemiBold
///     (game_files_measure measures the same)
void paint_game_files(
    paint::Canvas& canvas,
    const oa::ui::kit::DisplayList& parts,
    oa::platform::text_font::FontStack* fonts,
    float px_per_point,
    const oa::ui::frontend_renderer::RgbaPicture& icon,
    oa::platform::text_font::Family family = oa::platform::text_font::Family::sans
);

/// Draws the screen's OA mark in a square centred in an area, as large as its shorter side,
/// with engine_settings::draw_oa_mark: the icon scaled to the square at the canvas's own
/// pixels, or without it green "OA" letters in a green outlined square, each of the mark's
/// pixels a block of one canvas pixel for every 20 of the square's side. Only pixels inside
/// the painter's clip change, and they come out opaque.
///
/// @param painter the painter, whose canvas is opaque under the square
/// @param area where it goes
/// @param icon the Open Annihilation icon; empty draws the OA mark
void paint_game_files_oa_mark(
    paint::Painter& painter, paint::Area area, const oa::ui::frontend_renderer::RgbaPicture& icon
);

/// Draws one of the screen's marks centred in an area, as large as its shorter side, with the
/// painter's strokes; none of them is game art. Glyph::oa draws the OA mark without the icon
/// (paint_game_files_oa_mark), in its own green.
///
/// @param painter the painter
/// @param glyph the mark; none draws nothing
/// @param area where it goes
/// @param colour its colour
void paint_game_files_glyph(
    paint::Painter& painter, oa::ui::game_files::Glyph glyph, paint::Area area, paint::Rgba colour
);

/// Returns a colour of the screen as the painter's colour.
///
/// @param colour the screen's colour
/// @return the same colour with straight alpha
[[nodiscard]] paint::Rgba game_files_rgba(oa::ui::game_files::Colour colour) noexcept;

/// Writes a canvas as an RGB PNG (alpha laid over the background colour).
///
/// @param file the picture to write
/// @param canvas the canvas
/// @param[out] error why it could not be written
/// @return true when it was written
bool write_game_files_png(
    const std::filesystem::path& file, const paint::Canvas& canvas, std::string* error
);

} // namespace oa::app
