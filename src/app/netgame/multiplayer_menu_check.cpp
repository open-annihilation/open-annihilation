// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-multiplayer-menu with network play: the main menu's Multiplayer
// path clicked and typed through the SDL presenter as a player does, from the
// provider list through TCP.GUI and the game list to the new game screen.
// The check drives the running game through the engine's check host.
#include "multiplayer_menu_check.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/netgame/menu_check.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <SDL3/SDL.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app::netgame::menu_check {

namespace {

namespace app = oa::app;
namespace mp = oa::ui::frontend_multiplayer;
using app::CheckHost;

// The one DirectPlay service provider the session layer offers.
constexpr std::string_view kTcpipProvider = "Internet TCP/IP Connection For DirectPlay";
// Rows of DPLAY are clicked this far below the list's top edge.
constexpr int32_t kFirstRowY = 4;
constexpr int32_t kButtonY = 10;
// Frames run after a click's release, or typed text, before the check reads
// the game.
constexpr int kSettleFrames = 4;
// The presses in a row a click's press and release count.
constexpr uint8_t kSingleClick = 1;

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

struct CanvasPoint {
    int32_t x{};
    int32_t y{};
};

/// Returns the centre of the main menu's MULTI gadget.
///
/// @param host The running game's check host.
/// @return Canvas point at the gadget's centre; a main menu without it fails the check.
CanvasPoint multi_centre(const CheckHost& host) {
    const std::string name(app::menu::resource_name(app::menu::Button::multiplayer));
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    if (!host.gadget(host.context, name.c_str(), &x, &y, &width, &height))
        fail("the main menu has no MULTI gadget");
    return {x + width / 2, y + height / 2};
}

/// Clicks a control of the shown multiplayer panel, this far below its top edge.
///
/// A stacked dialog's controls are placed from its root record.
///
/// @param host The running game's check host.
/// @param name The control's name.
/// @param dy Rows below the control's top edge.
void click_control(const CheckHost& host, std::string_view name, int32_t dy) {
    const mp::Panel& panel = mp::multiplayer_panel();
    const mp::Control* control = mp::panel_control(panel, name);
    require(control != nullptr, "no " + std::string(name) + " control");
    const bool stacked = mp::multiplayer_modal() != nullptr;
    const int32_t x = stacked ? panel.controls[0].x : 0;
    const int32_t y = stacked ? panel.controls[0].y : 0;
    click(host, x + control->x + control->width / 2, y + control->y + dy);
}

/// Clicks the main menu's MULTI gadget.
///
/// @param host The running game's check host.
void click_multi(const CheckHost& host) {
    const auto centre = multi_centre(host);
    click(host, centre.x, centre.y);
}

} // namespace

bool on_main_menu(const CheckHost& host) {
    return host.screen(host.context) == app::screen_id(app::Screen::main_menu) &&
           host.frontend_state(host.context) == app::frontend::state_id::main_menu;
}

void click(const CheckHost& host, int32_t x, int32_t y) {
    for (const uint32_t type :
         {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        host.pointer(host.context, type, x, y, kSingleClick);
        host.frame(host.context);
    }
    for (int count = 0; count < kSettleFrames; ++count)
        host.frame(host.context);
}

void multiplayer_round(const CheckHost& host) {
    require(on_main_menu(host), "not on the main menu");
    click_multi(host);
    require(host.screen(host.context) == mp::kScreenProviders, "MULTI did not open SELPROV");
    const mp::Control* list = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
    require(list != nullptr, "SELPROV has no DPLAY list");
    require(
        list->items.size() == 1 && list->items[0] == kTcpipProvider,
        "DPLAY does not list only the TCP/IP provider"
    );
    click_control(host, "DPLAY", kFirstRowY);
    click_control(host, "SELECT", kButtonY);
    require(
        host.screen(host.context) == mp::kScreenTcp &&
            mp::multiplayer_modal_kind() == mp::ModalKind::tcp,
        "SELECT on TCP/IP did not open TCP.GUI"
    );
    click_control(host, "PREV", kButtonY);
    require(host.screen(host.context) == mp::kScreenProviders, "Cancel did not return to SELPROV");
    click_control(host, "PREVMENU", kButtonY);
    require(on_main_menu(host), "Main Menu did not return to the main menu");
}

} // namespace oa::app::netgame::menu_check

namespace oa::app {

namespace {

// The loopback address typed into TCP.GUI: the game list then asks this
// machine alone for its games.
constexpr const char* kLoopbackAddress = "127.0.0.1";
constexpr const char* kGameName = "Check Game";
// More Backspaces than any text box holds characters.
constexpr int kClearKeyLimit = 256;
// The drawn cursor may sit up to this far from the pointer after the
// canvas-to-window round trip.
constexpr int32_t kPointerSlack = 1;

/// Returns the path of one step's frame: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot The --snapshot path.
/// @param step Step name.
/// @return The step's frame path.
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

} // namespace

void check_multiplayer_screens(const CheckHost& host, const Options& options) {
    // The steps and helpers network play's own part of the check shares.
    using namespace oa::app::netgame::menu_check;
    void* const game = host.context;
    const uint32_t window_id = host.window_id(game);
    if (window_id == 0)
        fail("needs the SDL renderer");
    SDL_Window* const window = SDL_GetWindowFromID(window_id);
    // Each pointer event is followed by the frame the game runs after it.
    const auto pointer = [&](uint32_t type, int32_t x, int32_t y) {
        host.pointer(game, type, x, y, kSingleClick);
        host.frame(game);
    };
    const auto settle = [&] {
        for (int count = 0; count < kSettleFrames; ++count)
            host.frame(game);
    };

    multiplayer_round(host);
    multiplayer_round(host);
    std::cout << "multiplayer menu check: MULTI lists " << kTcpipProvider
              << ", SELECT opens TCP.GUI, twice\n";

    const auto send = [&](SDL_Event event) { (void)host.dispatch(game, &event); };
    const auto snapshot = [&](std::string_view step) {
        if (!options.snapshot.empty())
            write_ppm(step_snapshot(options.snapshot, step), *host.surface(game));
    };
    // The cursor is drawn where the pointer last moved; a screen that takes
    // the pointer must still move it, or the menu looks frozen.
    const auto move_pointer = [&](std::string_view where, CanvasPoint point) {
        pointer(SDL_EVENT_MOUSE_MOTION, point.x, point.y);
        snapshot(where);
        int32_t x = 0;
        int32_t y = 0;
        host.cursor(game, &x, &y);
        require(
            std::abs(x - point.x) <= kPointerSlack && std::abs(y - point.y) <= kPointerSlack,
            "the cursor stays at (" + std::to_string(x) + ", " + std::to_string(y) +
                ") when the pointer moves to (" + std::to_string(point.x) + ", " +
                std::to_string(point.y) + ") over " + std::string(where)
        );
    };
    const auto press_key = [&](SDL_Keycode key) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = window_id;
        event.key.key = key;
        event.key.down = true;
        send(event);
        event.type = SDL_EVENT_KEY_UP;
        event.key.down = false;
        send(event);
        host.frame(game);
    };
    // Backspace empties the focused text box, then the keys are typed. SDL
    // turns typed keys into text events only while text input is on.
    const auto type_into = [&](std::string_view where, std::string_view field, const char* text) {
        const mp::Panel& panel = mp::multiplayer_panel();
        require(
            panel.focus != mp::kNoControl && panel.focus == mp::panel_find(panel, field),
            std::string(where) + " does not focus " + std::string(field)
        );
        for (int key = 0; key < kClearKeyLimit && !mp::panel_text(panel, field).empty(); ++key)
            press_key(SDLK_BACKSPACE);
        require(
            mp::panel_text(panel, field).empty(), "Backspace did not empty " + std::string(field)
        );
        require(
            SDL_TextInputActive(window),
            "text input is off on " + std::string(where) + ", so nothing typed reaches " +
                std::string(field)
        );
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.windowID = window_id;
        event.text.text = text;
        send(event);
        settle();
        require(
            mp::panel_text(panel, field) == text,
            std::string(field) + " holds \"" + std::string(mp::panel_text(panel, field)) +
                "\" after typing \"" + text + "\""
        );
    };
    const auto on_screen = [&](ScreenId id, std::string_view what) {
        require(host.screen(game) == id, what);
        require(
            mp::multiplayer_message().empty(),
            std::string(what) + ": \"" + mp::multiplayer_message() + "\""
        );
    };

    // MULTI, the TCP/IP provider and the loopback address typed into TCP.GUI
    // lead to the game list; New there opens the new game screen, where a
    // typed game name lands in its field. The cursor follows the pointer on
    // every screen. Previous, Previous Menu and Main Menu lead back. OK on
    // TCP.GUI opens the lobby's sockets on every interface in their usual
    // port ranges and asks 127.0.0.1 for games, so two runs at once share
    // those ports.
    require(on_main_menu(host), "not on the main menu");
    click_multi(host);
    on_screen(mp::kScreenProviders, "MULTI did not open SELPROV");
    move_pointer("selprov", {200, 360});
    click_control(host, "DPLAY", kFirstRowY);
    click_control(host, "SELECT", kButtonY);
    require(mp::multiplayer_modal_kind() == mp::ModalKind::tcp, "SELECT did not open TCP.GUI");
    on_screen(mp::kScreenTcp, "SELECT did not open TCP.GUI");
    move_pointer("tcp", {300, 330});
    type_into("TCP.GUI", "ADDRESS", kLoopbackAddress);
    snapshot("tcp-typed");
    click_control(host, "OK", kButtonY);
    on_screen(mp::kScreenGameList, "OK on TCP.GUI did not open the game list");
    move_pointer("games", {60, 330});
    click_control(host, "STARTNEW", kButtonY);
    on_screen(mp::kScreenNewGame, "New on the game list did not open NEWMULTI");
    move_pointer("new-game", {80, 420});
    type_into("NEWMULTI", "GAMENAME", kGameName);
    snapshot("new-game-typed");
    click_control(host, "CANCEL", kButtonY);
    on_screen(mp::kScreenGameList, "Previous on NEWMULTI did not return to the game list");
    click_control(host, "PREVMENU", kButtonY);
    on_screen(mp::kScreenProviders, "Previous Menu on the game list did not return to SELPROV");
    click_control(host, "PREVMENU", kButtonY);
    require(on_main_menu(host), "Main Menu did not return to the main menu");
    require(!SDL_TextInputActive(window), "text input stays on over the main menu");
    std::cout << "multiplayer menu check: the cursor follows the pointer from SELPROV to NEWMULTI; "
              << kLoopbackAddress << " typed into TCP.GUI lists games and a game name typed into "
              << "NEWMULTI lands\n";
}

} // namespace oa::app
