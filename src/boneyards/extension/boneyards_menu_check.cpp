// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' part of --check-multiplayer-menu (boneyards_menu_check.hpp):
// REGISTER opens the registration console when gui/registration.gui is in the
// game's files, and otherwise the box that says that console needs the
// Boneyards mod. Closing the window asks "Exit to the system?" in the Boneyards
// and after its EXIT.
#include "boneyards_menu_check.hpp"
#include "console_state.hpp"
#include "boneyards_options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/boneyards/console/registration.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/app/netgame/menu_check.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {

// The Boneyards overlay's status widget (console_screens.cpp).
oa::boneyards::console::StatusWidget* boneyards_console_status_widget();

namespace {

namespace by = oa::boneyards;
namespace by_console = oa::boneyards::console;
namespace menu_check = oa::app::netgame::menu_check;

// A point inside REGISTER, which the login console places at (0x6f, 0x7e).
constexpr int32_t kRegisterX = 0x6f + 10;
constexpr int32_t kRegisterY = 0x7e + 8;
constexpr std::size_t kLoginPassLimit = 2000;

/// Fails the multiplayer menu check.
///
/// @param what Failure text; thrown as std::runtime_error "multiplayer menu check: <what>".
[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("multiplayer menu check: " + std::string(what));
}

/// Fails the multiplayer menu check unless a condition holds.
///
/// @param condition Condition that must hold.
/// @param what Failure text.
void require(bool condition, std::string_view what) {
    if (!condition)
        fail(what);
}

/// Runs one pass of the Boneyards on the held frontend clock.
///
/// Steps the clock one frontend tick, ticks the screen packages and composes
/// the frame, which is written to --boneyards-frames when that is given.
///
/// @param host The running game's check host, its clock held.
void boneyards_check_pass(const CheckHost& host) {
    host.hold_clock(host.context, host.clock(host.context) + 1);
    host.package_pass(host.context);
    if (!boneyards_options().boneyards_frames.empty()) {
        char name[0x20];
        std::snprintf(name, sizeof name, "frame-%06u.ppm", host.clock(host.context));
        write_ppm(boneyards_options().boneyards_frames / name, *host.surface(host.context));
    }
}

} // namespace

void check_boneyards_menu(const CheckHost& host) {
    void* const game = host.context;
    if (host.window_id(game) == 0)
        fail("needs the SDL renderer");
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    require(widget != nullptr, "no Boneyards overlay on the main menu");
    // Leaving the main menu frees the strip and coming back builds a new one,
    // so the menu is checked against the strip there now, not the first one.
    const auto on_main_menu = [&] {
        const by_console::StatusWidget* strip = boneyards_console_status_widget();
        return menu_check::on_main_menu(host) && strip != nullptr &&
               by_console::boneyards_status_widget_visible(strip);
    };
    const auto click = [&](int32_t x, int32_t y) { menu_check::click(host, x, y); };
    const auto click_window = [&](const by::Window* part) {
        require(part != nullptr, "missing Boneyards window");
        click(part->x + part->width / 2, part->y + part->height / 2);
    };
    // Cleared for good by an event that ends the run.
    bool running = true;
    const auto send = [&](SDL_Event event) {
        if (!host.dispatch(game, &event))
            running = false;
    };
    require(on_main_menu(), "the main menu does not show the Boneyards strip");

    // The Boneyards console runs on a clock stepped one tick per pass.
    host.hold_clock(game, host.clock(game));

    struct ClockRelease {
        const CheckHost& host;

        ~ClockRelease() { host.release_clock(host.context); }
    } clock_release{host};

    click_window(by_console::boneyards_status_widget_part(widget, by_console::OverlayPart::enter));
    require(host.frame_owned_by_package(game), "Enter Boneyards did not reach frontend state 7");
    by_console::MainFrameProbe probe{};
    const auto login_shown = [&] {
        return by_console::boneyards_status_widget_probe(widget, &probe) &&
               probe.console_screen == by_console::console_screen::login && probe.screen_shown;
    };
    for (std::size_t pass = 0; pass < kLoginPassLimit && !login_shown(); ++pass)
        boneyards_check_pass(host);
    require(login_shown(), "the login console did not open");
    // A request to close the window asks "Exit to the system?", whose NO keeps
    // the game: in the Boneyards, and on the main menu after its EXIT, since
    // the Boneyards' entry installed the question and no application mode
    // has been set since.
    const auto close_asks_exit = [&](std::string_view where) {
        SDL_Event close{};
        close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
        send(close);
        by_console::StatusWidget* strip = boneyards_console_status_widget();
        int32_t x = 0;
        int32_t y = 0;
        require(
            running && by_console::boneyards_status_widget_message_button(
                           strip, by::message_box_name::no, &x, &y
                       ),
            "closing the window " + std::string(where) + " did not ask Exit to the system?"
        );
        click(x, y);
        boneyards_check_pass(host);
        require(
            running && !by_console::boneyards_status_widget_message_open(strip),
            "NO did not close Exit to the system? " + std::string(where)
        );
    };
    close_asks_exit("in the Boneyards");
    namespace frame = by_console::main_frame_layout;
    click(frame::console_x + kRegisterX, frame::console_y + kRegisterY);
    by::Window* screen =
        widget->system.main_frame != nullptr ? widget->system.main_frame->active_screen : nullptr;
    by::Window* registration =
        screen != nullptr ? by::window_find_named(screen, "Registration", nullptr) : nullptr;
    const bool console_open = registration != nullptr && by::window_is_shown(registration);
    const bool box_open = by_console::boneyards_status_widget_message_open(widget);
    if (console_open) {
        require(!box_open, "REGISTER opened a message box");
        by::Window* back = by::window_find_named(registration, "Back", nullptr);
        require(back != nullptr && by::window_is_shown(back), "BACK TO LOGIN is not shown");
        click(back->x + back->width / 2, back->y + back->height / 2);
        // The click's tick may already have destroyed the console, so it is
        // looked up again rather than read through the old pointer.
        screen = widget->system.main_frame != nullptr ? widget->system.main_frame->active_screen
                                                      : nullptr;
        by::Window* after =
            screen != nullptr ? by::window_find_named(screen, "Registration", nullptr) : nullptr;
        require(
            after == nullptr || !by::window_is_shown(after),
            "BACK TO LOGIN did not hide the registration console"
        );
        boneyards_check_pass(host);
        screen = widget->system.main_frame != nullptr ? widget->system.main_frame->active_screen
                                                      : nullptr;
        require(
            screen == nullptr || by::window_find_named(screen, "Registration", nullptr) == nullptr,
            "BACK TO LOGIN did not leave the registration console"
        );
    } else {
        require(box_open, "REGISTER did not open the registration console");
        by::MessageBox* box = by::message_box_find_open(&widget->desktop);
        const by::Window* label =
            box != nullptr ? by::window_find_named(&box->base, by::message_box_name::text, nullptr)
                           : nullptr;
        std::string text;
        {
            // The caption is a text handle of the Boneyards' own text service.
            by_console::ScopedServices scope(widget);
            if (label != nullptr && label->ops != nullptr && label->ops->state_text != nullptr) {
                const char* caption = label->ops->state_text(const_cast<by::Window*>(label), -1);
                if (caption != nullptr)
                    text = caption;
            }
        }
        require(
            text == by_console::kRegistrationNeedsMod,
            "REGISTER did not say the Registration console needs the Boneyards mod"
        );
        int32_t ok_x = 0;
        int32_t ok_y = 0;
        require(
            by_console::boneyards_status_widget_message_button(
                widget, by::message_box_name::ok, &ok_x, &ok_y
            ),
            "the registration fallback has no Ok"
        );
        click(ok_x, ok_y);
        boneyards_check_pass(host);
        require(
            !by_console::boneyards_status_widget_message_open(widget),
            "Ok did not close the registration fallback"
        );
    }
    click(frame::exit_x + frame::exit_width / 2, frame::exit_y + frame::exit_height / 2);
    boneyards_check_pass(host);
    require(on_main_menu(), "EXIT did not return to the main menu");
    require(
        !by_console::boneyards_status_widget_message_open(widget),
        "EXIT left a message box over the main menu"
    );
    close_asks_exit("on the main menu after EXIT");
    require(on_main_menu(), "the main menu does not show the Boneyards strip");
    menu_check::multiplayer_round(host);
    require(on_main_menu(), "Main Menu did not return to the Boneyards strip");
    if (console_open) {
        std::cout << "multiplayer menu check: REGISTER opens the registration console, BACK TO "
                     "LOGIN leaves it, and closing the window asks Exit to the system? in the "
                     "Boneyards and on the main menu after EXIT\n";
    } else {
        std::cout << "multiplayer menu check: REGISTER shows the box that says the Registration "
                     "console needs the Boneyards mod, and closing the window asks Exit to "
                     "the system? in the Boneyards and on the main menu after EXIT\n";
    }
}

} // namespace oa::app
