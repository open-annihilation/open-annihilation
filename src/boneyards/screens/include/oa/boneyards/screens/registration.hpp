// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration console: the web address, the route the player picked, and the
// call that opens that address in the system browser.
#pragma once

#include "oa/boneyards/utf8.hpp"

#include <cstddef>
#include <cstdint>

// The address drawn as the QR code. The build sets it; the default is Core
// Prime's registration page.
#ifndef OA_BONEYARDS_REGISTRATION_URL
#define OA_BONEYARDS_REGISTRATION_URL "https://coreprime.net/register"
#endif

namespace oa::boneyards::screens {

inline constexpr const char kRegistrationAddress[] = OA_BONEYARDS_REGISTRATION_URL;

// How the console reaches the system browser. Both calls may be null, which
// leaves the browser unavailable. `open` receives the address unchanged.
struct WebAddressEntry {
    bool (*available)();
    bool (*open)(const char* address);
};

/// Installs the calls that ask whether a browser can open and that open an address.
///
/// An entry of null calls leaves the browser unavailable. The console reads
/// the entry when it is opened and when OPEN IN BROWSER is pressed.
///
/// @param entry browser calls; null function pointers are allowed
void registration_set_web_entry(WebAddressEntry entry);

/// Reports whether OPEN IN BROWSER can be offered.
///
/// @return true when an availability call is installed and it reports that a browser can open
bool registration_browser_available();

// What the registration console is showing. `route` is 0 for the web page and
// 1 for enlisting in the game.
struct RegistrationConsole {
    char address[160];
    char host[160];
    int32_t browser_available;
    int32_t in_game_route;
    int32_t route;
    int32_t back;
    char opened_address[160];
    int32_t open_count;
};

/// Fills a registration console.
///
/// A null or empty address uses the build's registration address. The host is
/// that address without its scheme and without anything from the first `/`,
/// `?` or `#`. The web route starts selected.
///
/// @param[out] console console to fill
/// @param address page address; null uses the build setting
/// @param browser_available 1 when OPEN IN BROWSER is offered
/// @param in_game_route 1 when the in-game route is offered beside the web page
void registration_init(
    RegistrationConsole* console,
    const char* address,
    int32_t browser_available,
    int32_t in_game_route
);

/// Selects the web route (0) or the in-game route (1).
///
/// The in-game route is ignored while it is not offered.
///
/// @param[in,out] console registration console
/// @param route 0 for the web page, 1 for enlisting in the game
void registration_select(RegistrationConsole* console, int32_t route);

/// Records that the player left the registration console.
///
/// @param[in,out] console registration console
void registration_back(RegistrationConsole* console);

/// Opens the console's address in the system browser.
///
/// Nothing happens when the browser is unavailable or no open call is
/// installed. Otherwise the address is kept, the open count increases, and
/// the open call receives that same address.
///
/// @param[in,out] console registration console
void registration_open_browser(RegistrationConsole* console);

/// Opens an address in the system browser through the same entry the registration page uses.
///
/// Nothing happens when the address is empty, when no browser is available, or when no open call
/// is installed.
///
/// @param address page to open; null opens nothing
void registration_open_address(const char* address);

// A name is 3 to 15 characters. The status list holds four lines.
inline constexpr int32_t kRegistrationNameMin = 3;
inline constexpr int32_t kRegistrationNameMax = 15;
inline constexpr int32_t kRegistrationStatusLines = 4;
inline constexpr int32_t kRegistrationStatusBytes = 80;

// The side the player enlists with. Core starts selected.
namespace registration_side {
inline constexpr int32_t arm = 0;
inline constexpr int32_t core = 1;
inline constexpr int32_t undecided = 2;
} // namespace registration_side

// Why a form was not sent. `ok` means it was.
namespace registration_check {
inline constexpr int32_t ok = 0;
inline constexpr int32_t empty_name = 1;
inline constexpr int32_t name_too_long = 2;
inline constexpr int32_t name_too_short = 3;
inline constexpr int32_t password_mismatch = 4;
inline constexpr int32_t password_mark = 5;
inline constexpr int32_t bad_email = 6;
inline constexpr int32_t rules = 7;
inline constexpr int32_t password_empty = 8;
} // namespace registration_check

// What sending answered. This server only answers `unavailable`.
namespace registration_reply {
inline constexpr int32_t none = 0;
inline constexpr int32_t unavailable = 1;
inline constexpr int32_t registered = 2;
inline constexpr int32_t name_taken = 3;
} // namespace registration_reply

// The in-game registration form: the fields, the check that last ran, and the
// status lines the form is showing. `sending` is 1 while a request is out.
// `status_bright` is the line drawn in the bright colour, or -1.
struct RegistrationForm {
    char name[oa::boneyards::utf8::bytes_for(static_cast<std::size_t>(kRegistrationNameMax))];
    char password[48];
    char confirm[48];
    char email[80];
    int32_t side;
    int32_t rules;
    int32_t rules_page;
    int32_t sending;
    int32_t reply;
    int32_t check;
    int32_t status_count;
    int32_t status_bright;
    char status[kRegistrationStatusLines][kRegistrationStatusBytes];
};

/// Clears a registration form.
///
/// Core starts selected, the Rules of Conduct are not accepted, and the
/// status lines ask for a name. Nothing is being sent.
///
/// @param[out] form form to clear; null is ignored
void registration_form_init(RegistrationForm* form);

/// Rewrites the idle status lines from the fields.
///
/// A form that can be sent says it is ready. Any other form names the first
/// problem and is not sent. A form that is already sending is left as it is.
///
/// @param[in,out] form registration form; null is ignored
void registration_form_review(RegistrationForm* form);

/// Sends the form when every field passes.
///
/// A problem is stored in `check` and named in the status lines, and nothing
/// is sent. A form that passes stores `ok`, shows the sending lines and
/// disables the send until an answer or an abandon. Sending again while a
/// request is out leaves that request as it is.
///
/// @param[in,out] form registration form; null checks as a bad e-mail and sends nothing
/// @return the check that decided this call
int32_t registration_form_submit(RegistrationForm* form);

/// Records an answer to the request that is out.
///
/// Nothing is recorded when nothing is being sent. The sending lines stay.
///
/// @param[in,out] form registration form; null is ignored
/// @param reply registration_reply value
void registration_form_answer(RegistrationForm* form, int32_t reply);

/// Answers that this server does not take registrations from the game.
///
/// @param[in,out] form registration form; null is ignored
void registration_form_receive(RegistrationForm* form);

/// Drops the request that is out and returns the form to its idle lines.
///
/// @param[in,out] form registration form; null is ignored
void registration_form_abandon(RegistrationForm* form);

/// Writes the message for an answer, with "\\n" where a line breaks.
///
/// `registered` names the form's name. Any other reply, including `none`,
/// writes nothing when `reply` is `none`.
///
/// @param reply registration_reply value
/// @param name name to welcome; null is an empty name
/// @param[out] out buffer; null writes nothing
/// @param capacity bytes available in `out`, including the terminator
void registration_reply_text(int32_t reply, const char* name, char* out, std::size_t capacity);

} // namespace oa::boneyards::screens
