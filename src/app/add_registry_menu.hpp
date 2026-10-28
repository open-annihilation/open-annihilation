// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Add registry window in the look of the Library over the game's own
// main menu (library_menu.hpp): the screen shaded, and in its middle a
// panel in the frame the game draws round its score board, its title at
// its top left in the game's font as the Library's panels are labelled.
// Under the title stand the address field and the signing-key field, the
// game's list box with the words in the game's font, an amber ring round
// the one typed into; then what the check found, its facts' labels faded
// and what to heed amber; and at the foot the big buttons on the Options
// screen's plates, right-aligned, an amber ring round the one Enter
// presses. The words and the controls are the window's own
// (oa::ui::engine_settings::add_registry_content); this look only places
// and draws them.
#pragma once

#include "game_menu_art.hpp"

#include "oa/ui/engine_settings/add_registry.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <optional>

namespace oa::app {

/// The Add registry window drawn with the game's art, over a whole screen.
class AddRegistryGameLook {
  public:

    /// Cuts the art up for drawing.
    ///
    /// @param art the game's art
    explicit AddRegistryGameLook(GameMenuArt art);

    /// Lays the look out on a screen of a size, as the Library is laid out
    /// over the game's art.
    ///
    /// @param size the screen's width and height, in points
    /// @return true when the size changed
    bool lay_out(oa::ui::kit::Point size);

    /// Returns the window's controls, as add_registry_list names them, in
    /// the screen's points: the fields, the buttons and the labels
    /// automation reads.
    ///
    /// @param window the window
    /// @return the display list; its items are empty
    [[nodiscard]] oa::ui::kit::DisplayList list(const oa::ui::engine_settings::AddRegistry& window) const;

    /// Returns the focused field while it takes typing and the focus is
    /// shown, as add_registry_field tells, in the screen's points.
    ///
    /// @param window the window
    /// @return the field; none otherwise
    [[nodiscard]] std::optional<oa::ui::kit::Rect>
    field(const oa::ui::engine_settings::AddRegistry& window) const;

    /// Draws the whole screen: the shade and the window over it.
    ///
    /// @param canvas where it draws, the screen's size
    /// @param window the window
    void draw(const oa::ui::kit::Canvas& canvas, const oa::ui::engine_settings::AddRegistry& window) const;

  private:

    struct Placed;

    /// Places the window in the middle of the screen.
    ///
    /// @param window the window
    /// @return where its parts lie
    [[nodiscard]] Placed place(const oa::ui::engine_settings::AddRegistry& window) const;

    /// Returns how many points each pixel of the game's font covers.
    ///
    /// @return the points a pixel
    [[nodiscard]] int32_t square() const;

    /// Returns a line's height in the game's font.
    ///
    /// @return the height, in points
    [[nodiscard]] int32_t line() const;

    GameMenuPainter painter_;     ///< draws with the game's art
    oa::ui::kit::Point size_{};   ///< the screen's size, in points
    double unit_{1.0};            ///< the points a row of the 1080-row window takes
};

} // namespace oa::app
