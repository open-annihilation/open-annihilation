// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Central Command login dialog ("BONEYARDS LOGIN").
#pragma once

#include "oa/boneyards/service.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// The session keeps 15 characters of the name and of the password.
inline constexpr std::size_t kLoginFieldCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kPlayerNameCharacters));
// Server list text Rover takes from a login (it copies at most 23 characters).
inline constexpr std::size_t kLoginServerCapacity = 0x18;
inline constexpr int32_t kLoginLogoFrames = 3;

// The login result for a password that does not match the name, as the
// session reports it. The dialog adds the reset address before showing it.
inline constexpr const char* kWrongPasswordStatus =
    "The password you provided was not correct for the username.  Check the values and try again.";

// That result as the login status shows it, in the status list.
inline constexpr const char* kWrongPasswordResetStatus =
    "The password you provided was not correct for the username.  Check the values and try again. "
    "Forgot it? Reset it at coreprime.net/forgot";

// Dialog flow states; the service owns the separate session connection level.
enum class LoginState : int32_t {
    idle = 0,
    connecting = 3,
    planetary = 4, // logged in; the Main Frame loads the planetary map
};

// Controls of the login dialog.
enum class LoginControl : int32_t {
    cancel_button,
    login_button,
    register_button,
    member_radio, // password required
    guest_radio,  // no password
};

struct LoginDialog {
    const BoneyardsService* service;
    void* ui_user;
    void (*message_box)(void* user, const char* text);
    LoginState state;
    char name[0x40];              // name entry
    char password[0x40];          // password entry
    int32_t password_field_value; // widget value (0 disabled, 1 enabled)
    int32_t member_value;         // radio value (1 = selected)
    int32_t guest_value;          // radio value
    int32_t login_value;          // button value
    int32_t logo_frame;
    char status[0x180];                         // status text, reset address included
    char session_name[kLoginFieldCapacity];     // the session's user name
    char session_password[kLoginFieldCapacity]; // the session's password
    // Selected server list entry, empty when there are no rows.
    char server[kLoginServerCapacity];
    bool closed;
    void* status_user{};
    void (*append_status)(void* user, const char* text){};
    // 1 when REGISTER offers the in-game form beside the web page.
    int32_t registration_form{};
    // When set, REGISTER opens the registration console instead of naming it.
    void* registration_user{};
    void (*open_registration)(void* user){};
};

/// Opens the login dialog in the member (password) mode.
///
/// @param[out] dialog dialog to reset
/// @param service service the login goes to
/// @param ui_user context handed to `message_box`
/// @param message_box shows a message box; may be null
void login_init(
    LoginDialog* dialog,
    const BoneyardsService* service,
    void* ui_user,
    void (*message_box)(void* user, const char* text)
);

/// Shows a status line in the dialog and in its status list.
///
/// A wrong-password line gains "Forgot it? Reset it at coreprime.net/forgot"
/// before it is shown. A catalogue translation of that whole line is used
/// when the player is reading another language.
///
/// @param[in,out] dialog login dialog
/// @param text status line the session reported
void login_set_status(LoginDialog* dialog, const char* text);

/// Handles a click on one of the dialog's controls.
///
/// LOGIN copies the name and (for a member) the password into the session
/// fields and asks the service to log in; CANCEL aborts the attempt;
/// REGISTER opens the registration console when one is attached, and otherwise
/// names that console. The in-game route is offered when `registration_form`
/// is set. The radio buttons switch between member and guest mode.
///
/// @param[in,out] dialog login dialog
/// @param control control clicked
void login_on_control(LoginDialog* dialog, LoginControl control);

/// Records whether REGISTER offers the in-game form.
///
/// The form is offered when its release constant is set or `beta_login_ux`
/// is set. The web page stays available either way.
///
/// @param[in,out] dialog login dialog
/// @param beta_login_ux stored BetaLoginUX preference, 0 or 1
void login_set_registration_access(LoginDialog* dialog, int32_t beta_login_ux);

/// Disables LOGIN while the name, or a member's password, is empty, and enables a disabled LOGIN
/// once they are filled.
///
/// @param[in,out] dialog login dialog
void login_update_button(LoginDialog* dialog);

/// Presses LOGIN on Enter in the password field when the button is enabled.
///
/// @param[in,out] dialog login dialog
/// @param password_focused whether the password field has the focus
/// @return whether the key was consumed
bool login_on_enter(LoginDialog* dialog, bool password_focused);

/// Ends a login attempt on the dialog.
///
/// Success moves to the planetary state and closes the dialog; a failure or
/// cancellation says so and returns to idle with LOGIN enabled again. The
/// dossier callback that ends a real login lives with the client session.
///
/// @param[in,out] dialog login dialog
/// @param connected whether the login succeeded
/// @param cancelled whether the player cancelled it (chooses the status line)
void login_on_result(LoginDialog* dialog, bool connected, bool cancelled);

/// Returns the dialog to idle with LOGIN enabled, keeping the status lines.
///
/// The client has already reported why the attempt ended.
///
/// @param[in,out] dialog login dialog
void login_on_reset(LoginDialog* dialog);

} // namespace oa::boneyards::screens
