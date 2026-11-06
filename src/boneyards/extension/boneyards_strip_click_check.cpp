// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-strip-buttons. A real pointer click, button down then up, on the
// strip's SINGLE, MULTI and EXIT, and on the part of MULTI the strip does
// not cover. --check-boneyards-menu-modes: the way in a mod's main menu
// shows, the strip's art over a hidden button, and the strip's ENTER plate. The
// running game is driven through the check host. No button is activated
// by its name.
#include "boneyards_strip_click_check.hpp"

#include "boneyards_check.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/boneyards/console/menu_entry.hpp"
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
bool boneyards_console_menu_entry(
    oa::boneyards::console::MenuEntryMode* mode,
    int32_t* x,
    int32_t* y,
    int32_t* width,
    int32_t* height
);

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

/// The passes the plate's ENTER has to open the Boneyards in.
constexpr int k_entry_passes = 90;
/// Where the base game's MAINMENU.GUI has SINGLE: a menu with art of its own
/// and no layout of its own keeps it there, with or without the expansion.
constexpr int32_t k_base_single_x = 139;
constexpr int32_t k_base_single_y = 393;
/// A point of the strip's plain art, at its bottom right, over no button.
constexpr int32_t k_strip_art_x = 620;
constexpr int32_t k_strip_art_y = 470;

/// What the failures of the check that runs say first.
std::string_view g_check_name = "strip buttons check";

/// Fails the strip check.
///
/// @param what Failure text.
[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error(std::string(g_check_name) + ": " + std::string(what));
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

int check_boneyards_menu_modes(const CheckHost& host, bool button) {
    g_check_name = "boneyards menu modes check";
    void* const game = host.context;
    require(on_main_menu(host), "not on the main menu");
    by_console::MenuEntryMode mode = by_console::MenuEntryMode::strip;
    int32_t enter_x = 0;
    int32_t enter_y = 0;
    int32_t enter_width = 0;
    int32_t enter_height = 0;
    require(
        boneyards_console_menu_entry(&mode, &enter_x, &enter_y, &enter_width, &enter_height),
        "the main menu never built the strip"
    );
    // A motion over nothing, which reaches the run only while it goes on.
    const auto still_running = [&host] {
        return host.pointer(host.context, SDL_EVENT_MOUSE_MOTION, k_empty_x, k_empty_y, 1);
    };
    if (!button) {
        require(
            mode == by_console::MenuEntryMode::strip, "the menu on the base game's art has no strip"
        );
        int32_t x = 0;
        int32_t y = 0;
        int32_t width = 0;
        int32_t height = 0;
        require(host.gadget(game, "EXIT", &x, &y, &width, &height), "the mod's EXIT is missing");
        require(
            y >= by_console::overlay_layout::y &&
                y + height <= by_console::overlay_layout::y + by_console::overlay_layout::height,
            "the mod's EXIT is not under the strip"
        );
        click_at(host, x + width / 2, y + height / 2, "the strip's art over the mod's EXIT");
        require(still_running(), "the strip's art over the mod's EXIT ended the run");
        require(on_main_menu(host), "the strip's art over the mod's EXIT left the main menu");
        click_at(host, k_strip_art_x, k_strip_art_y, "the strip's plain art");
        require(still_running(), "the strip's plain art ended the run");
        require(on_main_menu(host), "the strip's plain art left the main menu");
        std::cout << "boneyards menu modes check: strip: the menu on the base game's art shows "
                     "the strip, and its art over the mod's hidden EXIT takes the click\n";
        return 0;
    }
    require(
        mode == by_console::MenuEntryMode::button, "the menu with art of its own has no button"
    );
    require(enter_width > 0 && enter_height > 0, "the ENTER plate has no place");
    int32_t single_x = 0;
    int32_t single_y = 0;
    int32_t single_width = 0;
    int32_t single_height = 0;
    require(
        host.gadget(game, "SINGLE", &single_x, &single_y, &single_width, &single_height),
        "the mod's SINGLE is missing"
    );
    require(
        single_x == k_base_single_x && single_y == k_base_single_y,
        "the menu does not keep the base game's layout: SINGLE is at " + std::to_string(single_x) +
            ',' + std::to_string(single_y)
    );
    click_named(host, "SINGLE", "the mod's SINGLE");
    require(
        host.screen(game) == screen_id(Screen::single_player) &&
            host.frontend_state(game) == frontend::state_id::single_player_menu,
        "the mod's SINGLE did not open single player"
    );
    click_named(host, "PrevMenu", "single player's back button");
    require(on_main_menu(host), "single player's back button did not return");
    // ENTER where it stands on the plate, as on the strip.
    const int32_t press_x = enter_x + by_console::overlay_layout::enter_x -
                            by_console::entry_plate.x + by_console::overlay_layout::enter_width / 2;
    const int32_t press_y = enter_y + by_console::overlay_layout::enter_y -
                            by_console::entry_plate.y +
                            by_console::overlay_layout::enter_height / 2;
    click_at(host, press_x, press_y, "the plate's ENTER");
    host.hold_clock(game, host.clock(game));
    // Button mode only shows on a mod's own art, so a mod is always on here.
    // The entry then asks about it, or, for this check's command-line mod in
    // a quiet run, logs that the mod must be left off the command line and
    // stays on the menu. The test reads that line, which only the entry
    // writes, as the proof that the plate's ENTER was pressed.
    const auto entry_asked = [] {
        return by_console::boneyards_status_widget_message_open(boneyards_console_status_widget());
    };
    for (int pass = 0;
         pass < k_entry_passes && !host.frame_owned_by_package(game) && !entry_asked();
         ++pass)
        static_cast<void>(boneyards_pass(host));
    const bool opened = host.frame_owned_by_package(game);
    const bool asked = entry_asked();
    std::cout << "boneyards menu modes check: button: the menu with art of its own keeps the "
                 "base game's SINGLE at "
              << single_x << ',' << single_y << ", and ENTER on the plate at " << enter_x << ','
              << enter_y << " was pressed"
              << (opened  ? "; the Boneyards opened\n"
                  : asked ? "; the entry asked about the mod\n"
                          : "; the menu stayed\n");
    return 0;
}

} // namespace oa::app
