// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What Open Annihilation's own screens on the OA layer draw alike: Settings
// and the Library drawn with the game's art (game_menu_art.hpp), the Add
// registry window over them and the selection bar. They share the colours,
// the outlined captions, the steel: bolted frames round dark panels,
// bevelled plates and bolts, lines, rings and arrows, the pictures made of
// GAF frames and the interface catalogue's words.
#pragma once

#include "oa/app/window_icon.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::menu_paint {

/// The window height the menus' measures are given for.
inline constexpr double design_rows = 1080.0;

inline constexpr oa::ui::kit::Colour caption_colour{236, 230, 198};       ///< a caption, cream
inline constexpr oa::ui::kit::Colour caption_lit_colour{255, 250, 222};   ///< a highlighted caption
inline constexpr oa::ui::kit::Colour caption_faded_colour{150, 152, 144}; ///< a disabled caption
inline constexpr oa::ui::kit::Colour outline_colour{18, 20, 16};          ///< round every caption
inline constexpr oa::ui::kit::Colour key_colour{70, 190, 70};             ///< the quick key's underline
inline constexpr oa::ui::kit::Colour rule_colour{168, 172, 160};          ///< the heading's rules, metal
inline constexpr oa::ui::kit::Colour plate_colour{14, 20, 26};            ///< the panels
inline constexpr oa::ui::kit::Colour plate_edge_colour{92, 104, 112};     ///< their edge
/// The panels' edge inside their own.
inline constexpr oa::ui::kit::Colour panel_inner_edge_colour{40, 46, 50};
inline constexpr oa::ui::kit::Colour steel_colour{64, 70, 72};         ///< a steel plate's face
inline constexpr oa::ui::kit::Colour steel_light_colour{164, 170, 166}; ///< its lit edge
inline constexpr oa::ui::kit::Colour steel_dark_colour{18, 20, 22};     ///< its shaded edge
inline constexpr oa::ui::kit::Colour bolt_colour{126, 132, 126};        ///< a bolt's head
inline constexpr oa::ui::kit::Colour bolt_light_colour{214, 218, 206};  ///< the light on it
/// The lit lower edge of what is sunk into a panel.
inline constexpr oa::ui::kit::Colour sunk_light_colour{70, 78, 76};
/// The ground inside a steel frame.
inline constexpr oa::ui::kit::Colour panel_ground_colour{9, 13, 17};
/// The ground's opacity inside a steel frame, in 256ths.
inline constexpr uint32_t panel_ground_opacity = 228;
/// The panels' text.
inline constexpr oa::ui::kit::Colour plate_text_colour{196, 202, 204};
/// The panels' opacity, in 256ths.
inline constexpr uint32_t plate_opacity = 210;

/// Returns a text in the language shown, from the interface catalogue.
///
/// @param english the text in English
/// @return the text
[[nodiscard]] std::string worded(std::string_view english);

/// Returns a text with a word in braces replaced.
///
/// @param pattern the text, holding the word
/// @param token the word, braces included
/// @param value what replaces it
/// @return the text
[[nodiscard]] std::string filled(std::string pattern, std::string_view token, std::string_view value);

/// Returns a GAF frame as a picture, through a palette; a pixel the frame
/// leaves uncovered is clear.
///
/// @param frame the frame
/// @param palette 4 bytes per colour
/// @return the picture; empty when the frame does not render
[[nodiscard]] WindowIcon picture_of_frame(
    const oa::formats::gaf::Frame& frame, const oa::PaletteBytes& palette
);

/// Rounds a measure to the nearest point.
///
/// @param value the measure, in points
/// @return the point
[[nodiscard]] int32_t nearest(double value) noexcept;

/// Pictures of the art filtered to the pixels they are drawn on, each made
/// the first time it is drawn there and kept until clear: a redraw copies
/// the pixels and does not filter the pictures again.
class FittedArt {
  public:

    /// Draws a picture scaled to fill a rectangle.
    ///
    /// @param canvas where it draws
    /// @param rect the rectangle, in points
    /// @param picture the picture; it stays as long as the copies made of it
    void draw(
        const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, const WindowIcon& picture
    ) const;

    /// Forgets every copy, as when the screen's size changes.
    void clear() noexcept { fitted_.clear(); }

  private:

    /// One picture filtered to a size.
    struct Fitted {
        const WindowIcon* picture{};   ///< the picture it is made from
        int32_t columns{};             ///< its width, in canvas pixels
        int32_t rows{};                ///< its height, in canvas pixels
        std::vector<uint8_t> pixels{}; ///< four bytes a pixel; empty when it cannot be made
    };

    mutable std::vector<Fitted> fitted_{}; ///< the copies made
};

/// Draws a text outlined a point all round, as the captions are.
///
/// @param canvas where it draws
/// @param role which font
/// @param text the text
/// @param box the box it is centred in
/// @param align where the text sits along the box's width
/// @param ink the text's colour
/// @param tracking extra points after each character but the last
void outlined_text(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::FontRole role,
    std::string_view text,
    const oa::ui::kit::Rect& box,
    oa::ui::kit::Align align,
    oa::ui::kit::Colour ink,
    int32_t tracking = 0
);

/// Draws an arrow pointing left or right, its tip at a point, with a shadow
/// a point right and down.
///
/// @param canvas where it draws
/// @param tip the arrow's tip
/// @param reach how far it reaches up and down from the tip, and back from it
/// @param left it points left; otherwise right
/// @param ink its colour
void draw_arrow(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point tip,
    int32_t reach,
    bool left,
    oa::ui::kit::Colour ink
);

/// Draws an arrow pointing up or down, its tip at a point, with a shadow a
/// point right and down.
///
/// @param canvas where it draws
/// @param tip the arrow's tip
/// @param reach how far it reaches left and right of the tip, and back from it
/// @param down it points down; otherwise up
/// @param ink its colour
void draw_upright_arrow(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point tip,
    int32_t reach,
    bool down,
    oa::ui::kit::Colour ink
);

/// Draws a small steel button: a bevelled steel plate with a bolt in each
/// corner when its shorter side is long enough for them.
///
/// @param canvas where it draws
/// @param rect the button
/// @param face its face's colour
void draw_steel_button(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, oa::ui::kit::Colour face
);

/// Draws what is sunk into a panel: a shaded edge over and left of it, a lit
/// one under and right, and a dark outline inside them.
///
/// @param canvas where it draws
/// @param rect what is sunk, its edges included
void draw_sunk_edge(const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect);

/// Draws a ring of a width round a rectangle, inside it.
///
/// @param canvas where it draws
/// @param rect the rectangle
/// @param width how many points wide
/// @param ink its colour
void draw_ring(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& rect,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a line of a width between two points, as squares along it.
///
/// @param canvas where it draws
/// @param from where it starts
/// @param to where it ends
/// @param width how thick it is, in points
/// @param ink its colour
void draw_line(
    const oa::ui::kit::Canvas& canvas,
    oa::ui::kit::Point from,
    oa::ui::kit::Point to,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a ring: a circle's edge.
///
/// @param canvas where it draws
/// @param box the square it fills
/// @param width how thick it is, in points
/// @param ink its colour
void draw_circle(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& box,
    int32_t width,
    oa::ui::kit::Colour ink
);

/// Draws a round bolt head: a dark ring, the head and the light on its
/// upper left.
///
/// @param canvas where it draws
/// @param centre its centre
/// @param radius its head's radius, in points; under 1 draws nothing
void draw_bolt(const oa::ui::kit::Canvas& canvas, oa::ui::kit::Point centre, int32_t radius);

/// Draws a bevelled steel plate: its face lit towards the top and shaded
/// towards the bottom, a dark outline, a lit edge over and left of it and a
/// shaded one under and right.
///
/// @param canvas where it draws
/// @param rect the plate, in points
/// @param face its face's colour
void draw_steel(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& rect, oa::ui::kit::Colour face
);

/// Draws a glow round the outside of a rectangle, fading out from its edge.
///
/// @param canvas where it draws
/// @param rect the rectangle it lies round, in points
/// @param ink the glow's colour
/// @param reach how many points it reaches out
void draw_glow(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& rect,
    oa::ui::kit::Colour ink,
    int32_t reach
);

/// Draws a panel in a steel frame: the dark ground inside, a rim of steel
/// with a groove along it, lit along its top and shaded along its bottom,
/// the inside sunk into it, a bolt in each corner and vents on its sides.
///
/// @param canvas where it draws
/// @param panel the panel, its rim included, in points
/// @param rim the rim's width, in points; under 3, or too wide for the
///     panel, draws a dark panel with a plain edge
void draw_panel_frame(
    const oa::ui::kit::Canvas& canvas, const oa::ui::kit::Rect& panel, int32_t rim
);

/// Returns how wide a label plate is for a text.
///
/// @param canvas where it is drawn
/// @param text the label
/// @param height the plate's height
/// @param tracking extra points after each character but the last
/// @return the width, in points
[[nodiscard]] int32_t label_plate_width(
    const oa::ui::kit::Canvas& canvas, std::string_view text, int32_t height, int32_t tracking
);

/// Draws a steel label plate, as wide as label_plate_width, with a bolt at
/// each end and its label spaced out between them.
///
/// @param canvas where it draws
/// @param plate the plate, in points
/// @param text the label, in the language shown
/// @param tracking extra points after each character but the last
void draw_label_plate(
    const oa::ui::kit::Canvas& canvas,
    const oa::ui::kit::Rect& plate,
    std::string_view text,
    int32_t tracking
);

} // namespace oa::app::menu_paint
