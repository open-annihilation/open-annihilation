// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The cards the settings dialog lays among a section's rows (cards.cpp):
// Controls' key layout card, which names the layout in use, counts the
// player's own changes and says where it applies, with Change layout… and
// Edit keys…; Accessibility's practice pad, where the player tries the
// helpers the dialog shows before playing; and Updates' card, the update
// centre's one state with its actions. The rows' placing
// (geometry::place_section), the drawing (dialog_draw.cpp) and the events
// (dialog.cpp) all place them through here.
#pragma once

#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/theme.hpp"

#include "geometry.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::engine_settings::cards {

using geometry::SourceRect;
using geometry::TextWidth;

/// One line of text a card shows, placed.
struct Line {
    SourceRect rect{};                                  ///< its box, one line high
    std::string text;                                   ///< the words, as shown
    DialogFont font{};                                  ///< the font it is drawn in
    oa::ui::kit::Colour colour{};                       ///< its colour
    oa::ui::kit::Align align{oa::ui::kit::Align::left}; ///< where it sits in its box
};

/// The key layout card's eyebrow, over the layout's name.
inline constexpr std::string_view key_layout_eyebrow_text = "Key layout";
/// Change layout…'s caption: the Library on Keys.
inline constexpr std::string_view change_layout_text = "Change layout…";
/// Edit keys…'s caption: the key editor.
inline constexpr std::string_view edit_keys_text = "Edit keys…";
/// The badge while the player's own layout changes one action.
inline constexpr std::string_view one_own_change_text = "1 change of your own";
/// The badge while it changes more; {count} is how many.
inline constexpr std::string_view own_changes_text = "{count} changes of your own";
/// Where a layout applies when it is the game's own.
inline constexpr std::string_view applies_everywhere_text = "Applies to every game.";
/// Where it applies when it is the one chosen for the mod played; {mod} is
/// the mod's name.
inline constexpr std::string_view applies_with_mod_text = "Applies with {mod} only.";
/// How a match shows the keys; {primary} is Ctrl, or Cmd on a Mac.
inline constexpr std::string_view keys_in_match_text = "{primary}+/ in a match shows the keys.";

/// Controls' key layout card, placed: a keyboard tile, the eyebrow, the
/// layout's name and the count of the player's own changes, where it
/// applies, and the card's two buttons, beside the words where the section
/// is wide enough and under them where it is not.
struct KeyCard {
    SourceRect card{};          ///< the card's face
    SourceRect tile{};          ///< the keyboard tile
    std::vector<Line> lines;    ///< the eyebrow, the name's lines and where it applies
    SourceRect badge{};         ///< the count of the player's own changes; empty for none
    std::string badge_text;     ///< the badge's words
    SourceRect change{};        ///< Change layout…; empty where the Library cannot open
    std::string change_caption; ///< its caption, as shown
    SourceRect edit{};          ///< Edit keys…
    std::string edit_caption;   ///< its caption, as shown
    int32_t height{};           ///< the card's height
};

/// Places the key layout card from a row: under the row's line, across the
/// section's width. The name wraps; nothing of it is cut. Change layout…
/// shows only where the Library opens over the dialog (Dialog::library_link).
///
/// @param dialog the dialog
/// @param sized the dialog's sizes
/// @param top the card's top row
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @return the card
[[nodiscard]] KeyCard key_card(
    const Dialog& dialog,
    const geometry::Sizes& sized,
    int32_t top,
    const TextWidth& regular,
    const TextWidth& small
);

/// The practice pad's eyebrow.
inline constexpr std::string_view try_it_text = "Try it here";
/// The caption of the pad's button: every helper off.
inline constexpr std::string_view helpers_off_text = "Turn every helper off";

/// Accessibility's practice pad, placed: the eyebrow, what to try with the
/// helpers the dialog shows, the keys held or latched as keycaps, what the
/// last key did, Hold to confirm's bar while that helper is on, and Turn
/// every helper off.
struct Pad {
    /// One keycap of the pad's well.
    struct Cap {
        SourceRect rect{}; ///< the keycap
        std::string label; ///< its legend
        bool down{};       ///< held, latched or locked: drawn pressed
    };

    SourceRect card{};       ///< the pad's face
    SourceRect well{};       ///< the well the keycaps stand in
    std::vector<Cap> caps;   ///< the keycaps, left to right
    std::vector<Line> lines; ///< every line of text, the "+" between keycaps among them
    SourceRect bar{};        ///< Hold to confirm's bar; empty while that helper is off
    int32_t bar_done{};      ///< how far it has filled, in milliseconds
    int32_t bar_whole{};     ///< the hold's time, in milliseconds
    SourceRect off{};        ///< Turn every helper off
    std::string off_caption; ///< its caption, as shown
    bool off_enabled{};      ///< a helper it turns off is on
    int32_t height{};        ///< the pad's height
};

/// Returns how wide the practice pad stands beside Accessibility's rows: a
/// column at the section's right at Regular and Large, 0 at Compact, where
/// the pad follows the rows.
///
/// @param sized the dialog's sizes
/// @return the column's width, in points; 0 when the pad follows the rows
[[nodiscard]] int32_t pad_column(const geometry::Sizes& sized) noexcept;

/// Places the practice pad.
///
/// @param dialog the dialog, whose chosen helpers the pad shows
/// @param sized the dialog's sizes
/// @param left the pad's left column
/// @param top the pad's top row
/// @param width the pad's width
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @return the pad
[[nodiscard]] Pad practice_pad(
    const Dialog& dialog,
    const geometry::Sizes& sized,
    int32_t left,
    int32_t top,
    int32_t width,
    const TextWidth& regular,
    const TextWidth& small
);

/// Updates' card, placed: the version installed, the update centre's
/// heading and detail, a bar while a download is under way, the state's
/// actions as buttons, left to right, the first in the accent, and its note.
struct UpdateCard {
    SourceRect card{};                 ///< the card's face
    std::vector<Line> lines;           ///< every line of text, top to bottom
    SourceRect bar{};                  ///< the bar; empty for none
    int32_t bar_done{-1};              ///< how far the bar has filled, 0 to 100
    bool bar_paused{};                 ///< the bar is drawn paused
    std::vector<SourceRect> buttons;   ///< one for each action, left to right
    std::vector<std::string> captions; ///< each button's caption, as shown
    int32_t height{};                  ///< the card's height
};

/// Tells whether Updates shows its card: the host has filled the update
/// centre's heading (UpdatesOffer::card_status).
///
/// @param dialog the dialog
/// @return true while the card stands in place of the status row
[[nodiscard]] bool update_card_shown(const Dialog& dialog) noexcept;

/// Places Updates' card from a row: under the row's line, across the
/// section's width. Every line wraps; nothing is cut. A button that does not
/// fit after the one before it starts a line of its own.
///
/// @param dialog the dialog
/// @param sized the dialog's sizes
/// @param top the card's top row
/// @param regular a text's width in the regular font
/// @param small a text's width in the small font
/// @return the card
[[nodiscard]] UpdateCard update_card(
    const Dialog& dialog,
    const geometry::Sizes& sized,
    int32_t top,
    const TextWidth& regular,
    const TextWidth& small
);

/// Tells whether Turn every helper off has a helper to turn off: Sticky
/// modifiers, Hold to confirm, Slow keys, Bounce keys, the Order wheel or
/// Pointer keys on.
///
/// @param settings the settings shown
/// @return true when one of them is on
[[nodiscard]] bool any_helper_on(const EngineSettings& settings) noexcept;

/// Turns every helper Turn every helper off names off, and lets go of every
/// key the pad holds. The timings, Edge scrolling and the latches stay.
///
/// @param[in,out] dialog the dialog
void turn_helpers_off(Dialog& dialog) noexcept;

} // namespace oa::ui::engine_settings::cards
