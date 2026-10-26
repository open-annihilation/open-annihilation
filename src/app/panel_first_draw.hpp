// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the first draw of a GUI panel a match loads gives it: the face 3.1c
// gives a panel whose GUI file names no picture of its own, the common GUI
// art's BackTile, a nine-frame skin; and the quick keys its buttons' captions
// give them.
#pragma once

#include "oa/formats/gaf.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/ui/frontend/options.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace oa::app {

/// The common GUI art's sequence that faces a panel without a picture of its own.
inline constexpr std::string_view kBackTile = "BackTile";

/// Tiles the BackTile frames over a panel root's rectangle of an RGB image.
///
/// The frames are laid out as `oa::ui::gui_layout::skin_tiles` places them
/// over the root. Frame origins are ignored, and nothing is drawn outside the
/// root.
///
/// @param[in,out] image RGB image the root lies on
/// @param root the panel's root, in the image's pixels
/// @param tile the BackTile sequence
/// @param palette palette the frames' colours index, 4 bytes per colour
void draw_back_tile(
    oa::Image& image,
    const oa::ui::gui_layout::CommonFields& root,
    const oa::formats::gaf::Sequence& tile,
    const oa::PaletteBytes& palette
);

/// Gives a button of a loaded panel the quick key its caption gives it, as
/// the gadget engine does when it sets a caption or first draws the panel
/// (ui::gui_input::caption_quick_key): none with stages, the old one with
/// the no_quick_key attribute or an empty caption, else the first caption
/// letter no other button's key takes (ui::gui_input::free_quick_key). A
/// loaded layout's labels hold no quick key: the gadget engine gives a label
/// one only when a caption is set on a label with a link.
///
/// @param[in,out] gadgets the panel's records, root first
/// @param index the button's record; a record that is no button is left as it is
void assign_button_quick_key(std::vector<oa::ui::gui_layout::Gadget>& gadgets, std::size_t index);

/// Copies a panel button's letter back onto the loaded gadget.
///
/// The panel model holds the letter the caption gave the button. The gadget
/// record is what the first draw underlines.
///
/// @param button the loaded button
/// @param control the panel record the letter was copied from
void copy_button_letter(
    oa::ui::gui_layout::ButtonFields& button, const oa::ui::frontend::Control& control
);

/// The unit info panel's buttons as the first draw underlines them.
///
/// Each button takes the letter its caption gives it, then the presentation
/// carries that letter.
///
/// @param gadgets the panel's records, root first
/// @return one presentation for each button
[[nodiscard]] std::vector<oa::ui::frontend_renderer::ButtonPresentation>
unit_info_button_presentations(std::vector<oa::ui::gui_layout::Gadget>& gadgets);

} // namespace oa::app
