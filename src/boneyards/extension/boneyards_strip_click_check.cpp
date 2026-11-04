// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-strip-buttons. A real pointer click, button down then up, on the
// strip's SINGLE, MULTI and EXIT, and on the part of MULTI the strip does
// not cover. The running game is driven through the check host. No button
// is activated by its name.
#include "boneyards_strip_click_check.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {

oa::boneyards::console::StatusWidget* boneyards_console_status_widget();

namespace {

namespace by_console = oa::boneyards::console;
namespace frontend = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;

// The cross-play driver's MULTI click. It sits on the old column, past the
// strip's own MULTI button.
constexpr int32_t k_former_multi_x = 185;
constexpr int32_t k_former_multi_y = 438;
// Above the strip, and clear of the buttons at the top of the menu. A motion
// here is nobody's, so it reaches the run after EXIT.
constexpr int32_t k_empty_x = 320;
constexpr int32_t k_empty_y = 200;
constexpr uint8_t k_single_click = 1;

/// Fails the strip check.
///
/// @param what Failure text.
[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("strip buttons check: " + std::string(what));
}

/// Fails the strip check unless a condition holds.
///
/// @param condition Condition that must hold.
/// @param what Failure text.
void require(bool condition, std::string_view what) {
    if (!condition)
        fail(what);
}

/// Tells whether the main menu is showing.
///
/// @param host The running game.
/// @return True on the main menu.
bool on_main_menu(const CheckHost& host) {
    return host.screen(host.context) == screen_id(Screen::main_menu) &&
           host.frontend_state(host.context) == frontend::state_id::main_menu;
}

/// Sends one pointer event and the pass that follows a click.
///
/// @param host The running game.
/// @param x Canvas column.
/// @param y Canvas row.
/// @param what What the click is, named when the run ends on it.
void click_at(const CheckHost& host, int32_t x, int32_t y, std::string_view what) {
    for (const uint32_t type :
         {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        if (!host.pointer(host.context, type, x, y, k_single_click))
            fail(std::string(what) + " ended the run");
    }
    host.package_pass(host.context);
}

/// Clicks the centre of one strip button. The button is read again, because
/// leaving a screen and coming back builds the strip anew.
///
/// @param host The running game.
/// @param part SINGLE, MULTI or EXIT.
/// @param what What the click is.
void click_strip(const CheckHost& host, by_console::OverlayPart part, std::string_view what) {
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    require(widget != nullptr, "the main menu has no strip");
    const oa::boneyards::Window* button = by_console::boneyards_status_widget_part(widget, part);
    require(button != nullptr, std::string(what) + " is not on the strip");
    const int32_t x = button->x + button->width / 2;
    const int32_t y = button->y + button->height / 2;
    click_at(host, x, y, what);
}

/// Clicks the centre of a control on the screen that is up.
///
/// @param host The running game.
/// @param name The control's name.
/// @param what What the click is.
void click_control(const CheckHost& host, const char* name, std::string_view what) {
    const mp::Panel& panel = mp::multiplayer_panel();
    const mp::Control* control = mp::panel_control(panel, name);
    require(control != nullptr, std::string(what) + " has no " + name);
    const mp::PanelOffset offset = mp::multiplayer_panel_offset();
    const int32_t x = static_cast<int32_t>(offset.x) + static_cast<int32_t>(control->x) +
                      static_cast<int32_t>(control->width) / 2;
    const int32_t y = static_cast<int32_t>(offset.y) + static_cast<int32_t>(control->y) +
                      static_cast<int32_t>(control->height) / 2;
    click_at(host, x, y, what);
}

/// Clicks the centre of the named button on the screen that is up.
///
/// @param host The running game.
/// @param name The button's name.
/// @param what What the click is.
void click_named(const CheckHost& host, const char* name, std::string_view what) {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    require(
        host.gadget(host.context, name, &x, &y, &width, &height), std::string(what) + " is missing"
    );
    click_at(host, x + width / 2, y + height / 2, what);
}

} // namespace

int check_strip_buttons(const CheckHost& host) {
    require(on_main_menu(host), "not on the main menu");
    require(boneyards_console_status_widget() != nullptr, "the main menu has no strip");

    click_strip(host, by_console::OverlayPart::single, "SINGLE");
    require(
        host.screen(host.context) == screen_id(Screen::single_player) &&
            host.frontend_state(host.context) == frontend::state_id::single_player_menu,
        "SINGLE did not open single player"
    );
    click_named(host, "PrevMenu", "single player's back button");
    require(on_main_menu(host), "single player's back button did not return");

    click_strip(host, by_console::OverlayPart::multi, "MULTI");
    require(
        host.screen(host.context) == mp::kScreenProviders &&
            host.frontend_state(host.context) ==
                by_console::frontend_value::state_connection_selection,
        "MULTI did not open SELECT CONNECTION"
    );
    click_control(host, "PREVMENU", "SELECT CONNECTION's back button");
    require(on_main_menu(host), "SELECT CONNECTION's back button did not return");

    click_at(host, k_former_multi_x, k_former_multi_y, "the old MULTI column");
    require(
        host.screen(host.context) == mp::kScreenProviders &&
            host.frontend_state(host.context) ==
                by_console::frontend_value::state_connection_selection,
        "the old MULTI column did not open SELECT CONNECTION"
    );
    click_control(host, "PREVMENU", "the return from the old MULTI column");
    require(on_main_menu(host), "the old MULTI column did not come back");

    click_strip(host, by_console::OverlayPart::exit, "EXIT");
    if (host.pointer(host.context, SDL_EVENT_MOUSE_MOTION, k_empty_x, k_empty_y, k_single_click))
        fail("EXIT left the game running");

    std::cout << "strip buttons check: SINGLE opens single player, MULTI opens SELECT "
                 "CONNECTION, and EXIT ends the run\n";
    return 0;
}

} // namespace oa::app
