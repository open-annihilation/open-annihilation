// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The registration console shown from REGISTER: the web page as a QR code and
// an address, OPEN IN BROWSER where a browser can open, and the in-game route
// when that route is offered. The window is the screen gui/registration.gui
// builds; this console chooses the route, the address and which of those
// controls are shown.
#pragma once

#include "oa/boneyards/screens/lobby_widgets.hpp"
#include "oa/boneyards/screens/registration.hpp"

namespace oa::boneyards::console {

struct RegistrationView;

/// Ok-box text when gui/registration.gui is not among the game's files.
inline constexpr const char* kRegistrationNeedsMod =
    "The Registration console needs the Boneyards mod.";

/// Ok-box text when gui/registration_form.gui is not among the game's files.
inline constexpr const char* kRegistrationFormNeedsMod =
    "The registration form needs the Boneyards mod.";

/// Opens a registration console over `parent`.
///
/// The window is built from gui/registration.gui. The address defaults to the
/// build setting. OPEN IN BROWSER is shown when the installed browser entry
/// says a browser can open. The in-game route is shown beside the web page
/// when `in_game_route` is set. Selecting that route shows the form from
/// gui/registration_form.gui. When either file cannot be read, REGISTER shows
/// an Ok box (kRegistrationNeedsMod, or kRegistrationFormNeedsMod for the
/// form) and opens no window for that screen.
///
/// @param[in,out] parent window the console covers; null opens nothing
/// @param fonts Hat fonts for the captions; null leaves captions without a font
/// @param address page address; null uses the build setting
/// @param in_game_route 1 when the in-game route is offered
/// @return the console, or null when `parent` is null or gui/registration.gui
///     cannot be read
RegistrationView* registration_open(
    Window* parent, const screens::HatFonts* fonts, const char* address, int32_t in_game_route
);

/// Calls `closed` once, with `user`, when the player leaves the console.
///
/// The console is hidden then, and destroyed on the desktop's next pass over
/// owned windows. The call is not made when the console is destroyed some
/// other way.
///
/// @param[in,out] view registration console; null is ignored
/// @param closed called once when BACK TO LOGIN is pressed; may be null
/// @param user handed to `closed`
void registration_set_on_closed(RegistrationView* view, void (*closed)(void* user), void* user);

/// Returns the console's window.
///
/// @param view registration console; null has no window
/// @return the window, named Registration
Window* registration_window(RegistrationView* view);

/// Returns the console whose window is `window`.
///
/// @param window a registration console's window; null matches nothing
/// @return that console, or null when `window` is not one
RegistrationView* registration_from_window(Window* window);

/// Returns the console's model.
///
/// @param view registration console; null has no model
/// @return the address, the route and the browser call
const screens::RegistrationConsole* registration_model(const RegistrationView* view);

/// Returns the in-game form's model.
///
/// The model exists once the console is open. Its window is built the first
/// time the in-game route is shown.
///
/// @param view registration console; null has no form
/// @return the form, or null when `view` is null
screens::RegistrationForm* registration_form(RegistrationView* view);

/// Returns the in-game form's window.
///
/// @param view registration console; null has no form
/// @return the window, named Enlist, or null before that route is shown
Window* registration_form_window(RegistrationView* view);

/// Shows the form's fields, status lines and REGISTER from its model.
///
/// The form window is built if the in-game route is selected and it is not
/// built yet. A hidden form stays hidden.
///
/// @param[in,out] view registration console; null is ignored
void registration_form_apply(RegistrationView* view);

/// Shows the form's answer in a message box.
///
/// Nothing is shown when the form has no answer. Ok on "not offered" returns
/// to the web route. Ok on a finished dossier leaves the console. Ok on a
/// taken name returns to the form and gives the name the keyboard.
///
/// @param[in,out] view registration console; null is ignored
void registration_form_show_reply(RegistrationView* view);

} // namespace oa::boneyards::console
