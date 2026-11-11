// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registers the Boneyards status widget as an overlay on the native main menu.
//
// The overlay's Single/Multi/Intro/Exit buttons and credits hotspot sit on
// the identically placed MAINMENU.GUI gadgets, whose built-in handler already
// performs their actions; pointer input over them is shown on the overlay but
// left unconsumed so that handler still runs. "Enter Boneyards" (and the B
// key) is handled here with the Central Command session once oa-game has
// configured it (the offline service otherwise); its messages are Boneyards
// message boxes that take all input until dismissed. Every overlay cycle
// pumps the session.
//
// Entering the Boneyards takes the main menu down and sets frontend state 7,
// in which the Boneyards desktop draws the whole frame and takes all input;
// typed characters then arrive as text events. Leaving reloads the main menu
// and returns to state 2.
//
// The "-y" switch has the main menu's first update enter the Boneyards as
// ENTER BONEYARDS does, without its network check (dispatcher step 0x10b).
// The game runs the frontend's dispatcher on input or when asked, so the
// overlay's first tick after "-y" asks for the pass that holds that update.
// Leaving the Boneyards after "-y" (its exit, back, Escape, or a notice
// dismissed back to the main menu) ends the run. Closing the window still
// asks "Exit to the system?", and YES ends it with a real disconnect reason
// when there is one. Without "-y", leaving returns to the main menu.
//
// A battle the Boneyards launches goes on in the multiplayer screens, which
// a frontend pass opens from the connection selection's initialize. Coming
// back, dispatcher step 0x109 (and the main menu's own return, when that
// pass still runs on the game list) asks for the main menu. The overlay's
// next tick on the main menu finishes the return, so a failed join does not
// stay on the game list. The application
// runs the Boneyards' idle work every frame (boneyards_console_idle), which
// pumps the session whenever the overlay has not.
//
// The overlay keeps the host and services its create is given, which stay
// the same while the runtime lives: the sound stops, the background drone,
// the frontend pass and the application's entries below reach them outside
// the overlay's own callbacks too.
//
// The strip is drawn where it fits. As the strip is first built, the
// overlay measures how much of the strip's rows the menu's FrontendX
// changes against the base game's, and the mod's profile may name the mode
// instead (media.boneyards-entry). On the base game's art the strip is drawn
// and takes every pointer event inside its rows, Credits still mirrored to
// the gadget under it. On a mod's own art the main menu keeps the layout it
// has without the strip, and only the strip's ENTER plate is shown, where the
// profile's x and y put it or at the top right: the strip still draws it,
// lamps, pressed ENTER and all, and the pointer over it is passed to the
// strip at the plate's own place. B, "-y" and the strip's message boxes work as before. "none" shows
// neither.
#include <array>
#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <optional>
#include <utility>
#include <vector>

#include "oa/formats/hpi.hpp"
#include "oa/boneyards/console/images.hpp"
#include "oa/boneyards/console/menu_entry.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/screens/after_battle.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/ui/frontend_state/dispatcher.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/main_menu_art.hpp"
#include "oa/ui/frontend_renderer/menu_buttons.hpp"
#include "oa/ui/screen_registry.hpp"

namespace oa::app {
namespace {

namespace console = oa::boneyards::console;
namespace renderer = oa::ui::frontend_renderer;
using oa::boneyards::Message;
using oa::boneyards::MessageCode;
using oa::boneyards::Window;

// app::Screen::main_menu.
constexpr ScreenId k_main_menu_screen = 0;
// The dispatcher step the multiplayer states' main menu runs to return to
// the Boneyards after a launched battle.
constexpr uint32_t k_return_to_boneyards_step = 0x109;
// The dispatcher step the multiplayer states' main menu runs on its first
// update after a setup request ("-y").
constexpr uint32_t k_enter_boneyards_step = 0x10b;
constexpr int16_t k_overlay_z = 0;
constexpr const char* k_active_palette = "bitmaps/frontendx.pcx";
constexpr const char* k_shade_table = "palettes/palette.shd";
constexpr const char* k_light_table = "palettes/palette.lht";
constexpr uint8_t k_sdl_left = 1;
constexpr uint8_t k_sdl_middle = 2;
constexpr uint8_t k_sdl_right = 3;
constexpr uint32_t k_ascii_limit = 0x80;
constexpr uint32_t k_sdl_delete = 0x7F;
// Key values of typed characters; keycodes in the printable ASCII part of
// that range are left to the text events while those are on.
constexpr int32_t k_first_character = 0x20;
constexpr int32_t k_last_printable_keycode = 0x7E;
constexpr int32_t k_frame_width = 640;
// Shown when the overlay graphic cannot be read, and listed for translation.
constexpr const char* k_files_missing = "The Boneyards needs by.ccx and the Boneyards folder.";
constexpr int32_t k_frame_height = 480;
constexpr std::size_t k_frame_pixels = static_cast<std::size_t>(k_frame_width) * k_frame_height;
// The rows a label authored with no height still draws its line of text in.
constexpr int32_t k_label_line = 16;

// SDL keycodes of the navigation and modifier keys the Boneyards widgets use.
struct KeyMapping {
    uint32_t sdl;
    int32_t boneyards;
};

constexpr KeyMapping k_key_map[] = {
    {k_sdl_delete, oa::boneyards::key::del},
    {0x4000004AU, oa::boneyards::key::home},
    {0x4000004BU, oa::boneyards::key::page_up},
    {0x4000004DU, oa::boneyards::key::end},
    {0x4000004EU, oa::boneyards::key::page_down},
    {0x4000004FU, oa::boneyards::key::right},
    {0x40000050U, oa::boneyards::key::left},
    {0x40000051U, oa::boneyards::key::down},
    {0x4000003FU, oa::boneyards::key::f6}, // SDLK_F6
    {0x40000052U, oa::boneyards::key::up},
    {0x400000E0U, oa::boneyards::key::control},
    {0x400000E1U, oa::boneyards::key::shift},
    {0x400000E2U, oa::boneyards::key::alt},
    {0x400000E3U, oa::boneyards::key::system},
    {0x400000E4U, oa::boneyards::key::control},
    {0x400000E5U, oa::boneyards::key::shift},
    {0x400000E6U, oa::boneyards::key::alt},
    {0x400000E7U, oa::boneyards::key::system},
};
// Modifier bits of either Shift key. Ctrl, Alt and the system key have named
// constants; Shift is the low two bits of the same word.
constexpr uint16_t k_modifier_shift = 0x0003;
// A named key (the arrows, Home, End, the page keys, the function keys) has
// this bit set. A key with it set types no character.
constexpr uint32_t k_position_key = 0x40000000U;
// How many focus keys can be down at once. A press is remembered until its
// release, so the release delivers the same move the press did.
constexpr int k_held_focus = 8;
// Remembered when a press is not delivered: the layout moved its command
// onto another key, so the release is not delivered either.
constexpr int32_t k_focus_dropped = -1;

// One focus key that is down, and the move its press was delivered as.
struct HeldFocus {
    uint32_t sdl = 0;
    int32_t sent = 0;
    bool held = false;
    // The press was Previous button, so a repeat walks back too.
    bool previous = false;
};

struct OverlayState {
    ScreenContext* ctx = nullptr; // valid during a hook call only
    // ScreenContext::host and ::services, kept from the overlay's create.
    void* host = nullptr;
    const ScreenServices* services = nullptr;
    console::StatusWidget* widget = nullptr;
    std::vector<uint8_t> shade;
    std::vector<uint8_t> light;
    std::optional<oa::PaletteBytes> palette;
    oa::Palette widget_palette{};
    uint32_t held_buttons = 0;
    bool mirrored_event = false; // a click the Credits gadget under the strip takes
    // A menu button asks for two frontend passes: the first takes the signal,
    // and the next one opens the screen that signal names. One is spent by
    // the click; the tick spends the other.
    int menu_passes_left = 0;
    uint8_t frontend_state = console::frontend_value::state_main_menu;
    std::vector<uint8_t> pass_a;
    std::vector<uint8_t> pass_b;
    // Hears every sound the Boneyards plays (checks); null when none does.
    void (*sound_observer)(void* user, const char* name) = nullptr;
    void* sound_observer_user = nullptr;
    // Host faces for the markup pages; none sets them in the FNT fonts.
    oa::boneyards::html::FontService text_fonts{};
    // The application's part of a launched battle.
    console::LaunchBridge bridge{};
    // The overlay's tick has pumped the session since the last idle call.
    bool ticked = false;
    // "-y" was given: the overlay's next tick asks for a frontend pass, whose
    // main-menu update enters the Boneyards.
    bool entry_pass_pending = false;
    // A return to the Boneyards was asked for from another screen. The
    // overlay's next tick on the main menu finishes it.
    bool battle_return_pending = false;
    // What the mod's profile asks for (media.boneyards-entry): auto, and the
    // plate at the default place at the top right, without a profile.
    console::MenuEntryAsk entry_ask = console::MenuEntryAsk::automatic;
    int32_t entry_x = console::entry_plate_default_place.x;
    int32_t entry_y = console::entry_plate_default_place.y;
    // The played mod's folder; empty when no mod is played. Only its files
    // are the mod's own: an expansion's folder is the game's.
    std::filesystem::path mod_folder;
    // How the way in stands on this runtime's main menu, chosen once as the
    // strip is first built.
    std::optional<console::MenuEntryMode> mode;
    // The strip is drawn across the menu's bottom rows and takes the pointer
    // there: strip mode, with the active palette to draw it in.
    bool strip_drawn = false;
    // The strip's ENTER plate is shown at its place: button mode,
    // with both palettes to draw it in.
    bool plate_shown = false;
    // The plate's place in button mode, in canvas pixels, without its outline.
    renderer::SourceRect plate_place{};
    // The base game's main-menu palette, which the plate is shown in.
    std::vector<uint8_t> plate_palette;
    // A press on the plate is held: the strip has every pointer input until
    // it is let go.
    bool plate_held = false;
    // A press that began outside the strip's rows is the menu's: the strip
    // lets its moves and its release through.
    bool press_outside_strip = false;
    // Focus keys that are down, so each release delivers the move its press did.
    HeldFocus held_focus[k_held_focus]{};
    // The character a focus command took, dropped from the text that follows
    // the press, until the key is let go. Zero when the next typed character
    // is kept.
    int32_t swallow_character = 0;
};

OverlayState g_state;

// A headless --load never shows the main menu. The strip stays unbuilt for
// that run, so the saved game does not carry it.
bool g_defer_strip = false;

// "-y": leaving the Boneyards ends the run instead of returning to the menu.
bool g_end_on_leave = false;

// --check-boneyards-shortcut: once the login is up, leave it with no input.
bool g_check_shortcut = false;
bool g_shortcut_done = false;

bool in_boneyards(const OverlayState* state);

// The extension installs this so the focused field's place reaches the
// system. Null in a test, which has no system window.
void (*g_text_field_place)(void* host, int x, int y, int width, int height, int cursor) = nullptr;

// Passes the focused edit's place, or a non-positive size when there is none,
// so characters being composed can be offered beside the caret.
void publish_text_field(OverlayState* state) {
    if (g_text_field_place == nullptr || state == nullptr || state->host == nullptr)
        return;
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t cursor = 0;
    if (in_boneyards(state) && console::boneyards_status_widget_text_field(
                                   state->widget, &x, &y, &width, &height, &cursor
                               ))
        g_text_field_place(state->host, x, y, width, height, cursor);
    else
        g_text_field_place(state->host, 0, 0, 0, 0, 0);
}

bool read_resource(void* user, const char* path, std::vector<uint8_t>* out) {
    auto* assets = static_cast<oa::AssetStore*>(user);
    try {
        *out = assets->read(path).bytes;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

// A base-disc map that failed to load is tried again after the game
// directory's archives are scanned once more, as a disc put in after start-up
// is then found. A scan that cannot run leaves the second read to fail.
void rescan_archives(void* user) {
    auto* assets = static_cast<oa::AssetStore*>(user);
    if (assets == nullptr)
        return;
    try {
        (void)assets->discover("31");
    } catch (const std::exception&) {
        // The map is read again either way; a failed scan changes nothing.
    }
}

std::vector<uint8_t> read_or_empty(oa::AssetStore* assets, const char* path) {
    std::vector<uint8_t> bytes;
    if (!read_resource(assets, path, &bytes))
        bytes.clear();
    return bytes;
}

// A sound asked for outside the overlay's own hook calls, as the entry's
// frame pump opens the Boneyards, goes through the host saved at create, as
// host_set_frontend_state does.
void host_play_sound(void* user, const char* name) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->sound_observer != nullptr)
        state->sound_observer(state->sound_observer_user, name);
    if (state->ctx != nullptr) {
        screen_play_sound(state->ctx, name);
        return;
    }
    if (state->services != nullptr && state->services->play_sound != nullptr)
        state->services->play_sound(state->host, name);
}

// A name registered outside a hook call, as the Main Frame build does when
// the pump opens the Boneyards, also goes to the host saved at create.
void host_register_sound(void* user, const char* category, const char* file) {
    auto* state = static_cast<OverlayState*>(user);
    const ScreenServices* services = state->services;
    void* host = state->host;
    if (state->ctx != nullptr) {
        services = state->ctx->services;
        host = state->ctx->host;
    }
    if (services != nullptr && services->register_sound != nullptr)
        services->register_sound(host, category, file);
}

void host_set_signal(void* user, uint8_t signal) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx != nullptr && state->ctx->services->set_frontend_signal != nullptr)
        state->ctx->services->set_frontend_signal(state->ctx->host, signal);
}

// Writes the signal the next main-menu pass acts on, and asks for that pass.
// The service writes both signal bytes; the pass then treats them as already
// taken up. A press the overlay handles does not reach the menu's own
// handler, so the pass has to be asked for here.
void host_set_pending_signal(void* user, uint8_t signal) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx == nullptr)
        return;
    state->menu_passes_left = 2;
    const ScreenServices& services = *state->ctx->services;
    if (services.set_frontend_signal != nullptr)
        services.set_frontend_signal(state->ctx->host, signal);
    if (services.run_frontend != nullptr)
        services.run_frontend(state->ctx->host);
}

// Rescans the archives before the multiplayer menu, so a map added after
// start-up is found.
void host_prepare_multiplayer(void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx != nullptr)
        rescan_archives(state->ctx->assets);
}

// True when maps/multiplay.tdf can be read.
bool host_load_multiplayer_document(void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx == nullptr || state->ctx->assets == nullptr)
        return false;
    try {
        return !state->ctx->assets->read("maps/multiplay.tdf").bytes.empty();
    } catch (const std::exception&) {
        return false;
    }
}

bool in_boneyards(const OverlayState* state) {
    return state->frontend_state == console::frontend_value::state_boneyards;
}

// Places the on-screen keyboard clear of the focused field, then opens it.
// A null place, or one with no area, closes the keyboard.
void host_screen_keyboard(void* user, const oa::boneyards::TextFieldPlace* place) {
    auto* state = static_cast<OverlayState*>(user);
    if (state == nullptr || state->services == nullptr || !in_boneyards(state))
        return;
    void* host = state->host;
    if (host == nullptr && state->ctx != nullptr)
        host = state->ctx->host;
    const bool open = place != nullptr && place->width > 0 && place->height > 0;
    if (g_text_field_place != nullptr && host != nullptr) {
        if (open)
            g_text_field_place(
                host, place->x, place->y, place->width, place->height, place->cursor
            );
        else
            g_text_field_place(host, 0, 0, 0, 0, 0);
    }
    if (state->services->set_text_input != nullptr)
        state->services->set_text_input(host, open ? 1 : 0);
}

// Notes Ctrl, Alt, the system key and Shift on the next key, from the
// engine's modifier bits. A bit that is clear leaves a key the player is
// holding.
void apply_key_modifiers(OverlayState* state, const ScreenInput& input) {
    if (state->widget == nullptr)
        return;
    if (input.kind != ScreenInputKind::key_down && input.kind != ScreenInputKind::key_up)
        return;
    const uint16_t mods = input.modifiers;
    console::boneyards_status_widget_event_modifiers(
        state->widget,
        (mods & k_modifier_shift) != 0,
        (mods & kInputModifierCtrl) != 0,
        (mods & kInputModifierAlt) != 0,
        (mods & kInputModifierSystem) != 0
    );
}

// "-y" ends the run when the Boneyards returns to the main menu. A launched
// battle, which goes on to the connection selection, is left alone. Without
// "-y" this does nothing, so exit, back and Escape still return to the menu.
void end_shortcut(OverlayState* state) {
    if (!g_end_on_leave || state == nullptr)
        return;
    const ScreenServices* services = state->services;
    void* host = state->host;
    if (state->ctx != nullptr && state->ctx->services != nullptr) {
        services = state->ctx->services;
        host = state->ctx->host;
    }
    if (services != nullptr && services->quit != nullptr)
        services->quit(host, nullptr, 0);
}

// Frontend state 7 hands the frame and the keyboard to the Boneyards. The
// on-screen keyboard opens when a text field is focused, and closes on leaving.
void host_set_frontend_state(void* user, uint8_t value) {
    auto* state = static_cast<OverlayState*>(user);
    const bool was_boneyards = in_boneyards(state);
    state->frontend_state = value;
    const bool back_to_menu = was_boneyards && value == console::frontend_value::state_main_menu;
    // A pass that runs outside the overlay's own callback still has the host
    // saved at create. The engine's state has to move with the overlay's.
    if (state->ctx == nullptr) {
        if (state->services != nullptr && state->services->set_frontend_state != nullptr &&
            state->host != nullptr)
            state->services->set_frontend_state(state->host, value);
        if (back_to_menu)
            end_shortcut(state);
        return;
    }
    const ScreenServices& services = *state->ctx->services;
    if (services.set_frontend_state != nullptr)
        services.set_frontend_state(state->ctx->host, value);
    if (was_boneyards && !in_boneyards(state) && services.set_text_input != nullptr)
        services.set_text_input(state->ctx->host, 0);
    // Intro is the movie state. A press the overlay handles does not reach
    // the menu's own handler, so the pass that plays the movie is asked for
    // here, and the tick asks for the one that returns to the menu.
    if (value == console::frontend_value::state_intro && services.run_frontend != nullptr) {
        state->menu_passes_left = 2;
        services.run_frontend(state->ctx->host);
    }
    publish_text_field(state);
    if (back_to_menu)
        end_shortcut(state);
}

void host_close_main_menu_panel(void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx != nullptr && state->ctx->services->close_main_menu_panel != nullptr)
        state->ctx->services->close_main_menu_panel(state->ctx->host);
}

// Credits is the one strip control the main menu's own gadget still acts on.
bool host_press_taken_below(void* user) {
    return static_cast<const OverlayState*>(user)->mirrored_event;
}

void host_reload_main_menu_panel(void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx != nullptr && state->ctx->services->reload_main_menu_panel != nullptr)
        state->ctx->services->reload_main_menu_panel(state->ctx->host);
}

// Read only from the overlay draw, which supplies the context.
bool host_pointer_position(void* user, int32_t* x, int32_t* y) {
    const auto* state = static_cast<OverlayState*>(user);
    if (state->ctx == nullptr || state->ctx->services->pointer_position == nullptr)
        return false;
    state->ctx->services->pointer_position(state->ctx->host, x, y);
    return true;
}

// Read only from the overlay tick, which supplies the context.
uint32_t host_current_tick(void* user) {
    const auto* state = static_cast<OverlayState*>(user);
    return state->ctx->services->current_tick(state->ctx->host);
}

bool host_launch_permitted(void* user, const char** text) {
    const auto& bridge = static_cast<OverlayState*>(user)->bridge;
    if (bridge.launch_permitted == nullptr)
        return true;
    return bridge.launch_permitted(bridge.user, text);
}

void host_battle_launched(
    void* user, const oa::boneyards::LaunchReport* launch, const char* user_name
) {
    const auto& bridge = static_cast<OverlayState*>(user)->bridge;
    if (bridge.launched != nullptr)
        bridge.launched(bridge.user, launch, user_name);
}

void host_launch_ended(void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->bridge.launch_ended != nullptr)
        state->bridge.launch_ended(state->bridge.user);
    oa::boneyards::screens::show_after_battle_notice(state->services, state->host);
}

void host_run_frontend(void* user) {
    const auto* state = static_cast<OverlayState*>(user);
    if (state->services != nullptr && state->services->run_frontend != nullptr)
        state->services->run_frontend(state->host);
}

void host_stop_sounds(void* user) {
    const auto* state = static_cast<OverlayState*>(user);
    if (state->services != nullptr && state->services->stop_sounds != nullptr)
        state->services->stop_sounds(state->host);
}

void host_play_sound_alternate(void* user, const char* name) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->sound_observer != nullptr)
        state->sound_observer(state->sound_observer_user, name);
    if (state->services != nullptr && state->services->play_sound_alternate != nullptr)
        state->services->play_sound_alternate(state->host, name);
}

void host_leave_game(void* user) {
    const auto& bridge = static_cast<OverlayState*>(user)->bridge;
    if (bridge.leave_game != nullptr)
        bridge.leave_game(bridge.user);
}

void host_left_game(void*, uint32_t area_id, const char* game_id) {
    (void)oa::boneyards::rover_call_send_left_game(area_id, game_id);
}

void host_set_latency_reply_flags(void*, uint32_t flags) {
    oa::boneyards::rover_call_set_latency_reply_flags(flags);
}

void host_empty_chat_ring(void* user) {
    const auto& bridge = static_cast<OverlayState*>(user)->bridge;
    if (bridge.empty_chat_ring != nullptr)
        bridge.empty_chat_ring(bridge.user);
}

void host_battle_message(void* user, const char* text) {
    const auto& bridge = static_cast<OverlayState*>(user)->bridge;
    if (bridge.battle_message != nullptr)
        bridge.battle_message(bridge.user, text);
}

// The overlay's own box. It restores the buttons when it closes. The screen
// services have no separate modal.
void host_show_message_box(void* user, const char* text) {
    console::boneyards_status_widget_open_message(static_cast<OverlayState*>(user)->widget, text);
}

/// Tells whether the main menu returns to the Boneyards as it opens.
///
/// It does when no connection type waits, the Boneyards launched the battle
/// that ended and the network the Boneyards needs is there.
///
/// @param state overlay state
/// @return true to return to the Boneyards
bool returns_to_boneyards(const OverlayState* state) {
    const auto& bridge = state->bridge;
    const int32_t connection_type =
        bridge.connection_type != nullptr ? bridge.connection_type(bridge.user) : 0;
    return connection_type == 0 && console::boneyards_status_widget_launched_game(state->widget) &&
           console::boneyards_status_widget_network_available(state->widget);
}

bool settings_read_string(void* user, const char* key, char* out, std::size_t capacity) {
    auto* state = static_cast<OverlayState*>(user);
    return state->ctx != nullptr && state->ctx->services->read_string != nullptr &&
           state->ctx->services->read_string(state->ctx->host, "Boneyards", key, out, capacity) !=
               0;
}

bool settings_save_string(void* user, const char* key, const char* value) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx == nullptr || state->ctx->services->write_string == nullptr)
        return false;
    state->ctx->services->write_string(state->ctx->host, "Boneyards", key, value);
    return true;
}

int32_t settings_read_number(void* user, const char* key, int32_t fallback) {
    auto* state = static_cast<OverlayState*>(user);
    uint32_t value{};
    if (state->ctx == nullptr || state->ctx->services->read_number == nullptr ||
        !state->ctx->services->read_number(state->ctx->host, "Boneyards", key, &value))
        return fallback;
    return static_cast<int32_t>(value);
}

bool settings_save_number(void* user, const char* key, int32_t value) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->ctx == nullptr || state->ctx->services->write_number == nullptr)
        return false;
    state->ctx->services->write_number(
        state->ctx->host, "Boneyards", key, static_cast<uint32_t>(value)
    );
    return true;
}

void build_strip(ScreenContext* ctx, OverlayState* state);

// The strip comes back when the main menu does, unless this run never shows
// it. A battle the Boneyards launched keeps the widget, so this does nothing.
void ensure_strip(ScreenContext* ctx, OverlayState* state) {
    if (g_defer_strip || state->widget != nullptr)
        return;
    build_strip(ctx, state);
}

// The dispatcher's main-menu steps reach the Boneyards system object.
//
// The strip, its pictures and the parsed menu graphic go with the menu. A
// battle the Boneyards launched keeps the widget: the return opens the
// Boneyards again from it. Any other leave frees the widget. The passes are
// scratch either way. The menu keeps the strip's layout either way: the
// pictures are built again when the menu returns, and the old columns would
// come back if the layout forgot the strip while it was away.
void step_leave_main_menu(ScreenContext*, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    const bool keep =
        state->widget != nullptr && console::boneyards_status_widget_launched_game(state->widget);
    if (keep) {
        console::boneyards_status_widget_leave_main_menu(state->widget);
    } else if (state->widget != nullptr) {
        console::boneyards_status_widget_destroy(state->widget);
        state->widget = nullptr;
        std::vector<uint8_t>().swap(state->shade);
        std::vector<uint8_t>().swap(state->light);
        state->palette.reset();
    }
    std::vector<uint8_t>().swap(state->pass_a);
    std::vector<uint8_t>().swap(state->pass_b);
    console::console_images_release_embedded_menu();
}

/// Returns from a battle the Boneyards launched to the Boneyards, over the main menu.
///
/// @param ctx screen context of the main menu
/// @param state the overlay state
void return_to_boneyards(ScreenContext* ctx, OverlayState* state) {
    state->ctx = ctx;
    console::boneyards_status_widget_return_from_battle(state->widget);
    state->ctx = nullptr;
}

/// Finishes a return that was asked for while another screen was showing.
///
/// The pass that leaves the game list runs before the main menu is current,
/// so the return itself waits for this overlay's tick on the main menu.
/// Without a battle still counting as launched, the wait is forgotten.
///
/// @param ctx screen context of the tick
/// @param state the overlay state
void finish_battle_return(ScreenContext* ctx, OverlayState* state) {
    if (!state->battle_return_pending || ctx->screen != k_main_menu_screen)
        return;
    state->battle_return_pending = false;
    if (state->widget == nullptr || !console::boneyards_status_widget_launched_game(state->widget))
        return;
    ensure_strip(ctx, state);
    return_to_boneyards(ctx, state);
}

/// Returns from a battle the Boneyards launched to the Boneyards (dispatcher step 0x109).
///
/// The Boneyards draws over the main menu. From another screen (the game
/// list a failed join left, or the battle room's back button) the main menu
/// is asked for, and the overlay's next tick on that menu takes the player
/// to the Boneyards.
///
/// @param ctx screen context of the step
/// @param user the overlay state
void step_return_to_boneyards(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (ctx->screen != k_main_menu_screen) {
        state->battle_return_pending = true;
        screen_request(ctx, k_main_menu_screen);
        return;
    }
    state->battle_return_pending = false;
    ensure_strip(ctx, state);
    return_to_boneyards(ctx, state);
}

// ENTER, B and "-y" share this when the extension installed it. Null keeps
// the open that does not check for an update or a missing expansion.
void (*g_entry_flow)(console::StatusWidget*) = nullptr;

/// Enters the Boneyards from the main menu's first update after "-y" (dispatcher step 0x10b).
///
/// With an entry flow installed, "-y" runs that flow. Otherwise the entry is
/// ENTER BONEYARDS without its network check (boneyards_status_widget_enter).
///
/// @param ctx screen context of the step
/// @param user the overlay state
void step_enter_boneyards(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    state->ctx = ctx;
    if (g_entry_flow != nullptr)
        g_entry_flow(state->widget);
    else
        console::boneyards_status_widget_enter(state->widget);
    state->ctx = nullptr;
}

// The main menu shows the overlay again, unless a battle the Boneyards
// launched has ended: then it returns to the Boneyards, as its initialize
// does.
void step_return_to_main_menu(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    // The pass can run while the game list is still the screen. A launched
    // battle asks for the main menu and finishes the return on its tick,
    // instead of staying on the game list.
    if (ctx->screen != k_main_menu_screen && state->widget != nullptr &&
        console::boneyards_status_widget_launched_game(state->widget)) {
        state->battle_return_pending = true;
        screen_request(ctx, k_main_menu_screen);
        return;
    }
    ensure_strip(ctx, state);
    if (returns_to_boneyards(state)) {
        state->battle_return_pending = false;
        return_to_boneyards(ctx, state);
        return;
    }
    const bool was_boneyards = in_boneyards(state);
    state->frontend_state = console::frontend_value::state_main_menu;
    state->ctx = ctx;
    console::boneyards_status_widget_return_to_main_menu(state->widget);
    if (was_boneyards)
        end_shortcut(state);
    state->ctx = nullptr;
}

bool is_mirrored_point(console::StatusWidget* widget, int32_t x, int32_t y) {
    // Single, Multi, Intro and Exit act on the strip. Credits stays with the
    // gadget underneath it.
    const Window* credits =
        console::boneyards_status_widget_part(widget, console::OverlayPart::credits);
    return credits != nullptr && x >= credits->x && x <= credits->x + credits->width &&
           y >= credits->y && y <= credits->y + credits->height;
}

uint32_t button_bit(uint8_t button) {
    switch (button) {
    case k_sdl_left:
        return oa::boneyards::button_left;
    case k_sdl_right:
        return oa::boneyards::button_right;
    case k_sdl_middle:
        return oa::boneyards::button_middle;
    default:
        return 0;
    }
}

// Pointer and key messages. With `typing` the printable keycodes are
// dropped: their characters come from the text events instead.
bool translate(const ScreenInput& input, uint32_t held, bool typing, Message* msg) {
    *msg = Message{};
    msg->x = static_cast<int32_t>(input.x);
    msg->y = static_cast<int32_t>(input.y);
    msg->buttons = held;
    switch (input.kind) {
    case ScreenInputKind::key_down:
    case ScreenInputKind::key_up: {
        int32_t key = input.key < k_ascii_limit ? static_cast<int32_t>(input.key) : -1;
        for (const KeyMapping& mapping : k_key_map) {
            if (mapping.sdl == input.key)
                key = mapping.boneyards;
        }
        if (key < 0)
            return false;
        if (typing && key >= k_first_character && key <= k_last_printable_keycode)
            return false;
        msg->code =
            input.kind == ScreenInputKind::key_down ? MessageCode::key_down : MessageCode::key_up;
        msg->key = key;
        return true;
    }
    case ScreenInputKind::pointer_move:
        msg->code = MessageCode::mouse_move;
        return true;
    case ScreenInputKind::pointer_down:
    case ScreenInputKind::pointer_up: {
        // The game's window procedure queues only the left and right button
        // messages; the middle and extra ones go to DefWindowProc. Its window
        // class takes double clicks, so Windows reports every second press of
        // a run as WM_xBUTTONDBLCLK in place of its down. A queued button
        // record reaches the windows with its held-button word cleared.
        const bool left = input.button == k_sdl_left;
        if (!left && input.button != k_sdl_right)
            return false;
        msg->buttons = 0;
        if (input.kind == ScreenInputKind::pointer_up) {
            msg->code = left ? MessageCode::left_up : MessageCode::right_up;
            return true;
        }
        const bool twice = input.clicks >= 2 && input.clicks % 2 == 0;
        if (left)
            msg->code = twice ? MessageCode::left_double : MessageCode::left_down;
        else
            msg->code = twice ? MessageCode::right_double : MessageCode::right_down;
        return true;
    }
    default:
        return false;
    }
}

// The one character a printable key types, in lower case for a letter, or 0
// when the key types nothing.
int32_t typed_byte(uint32_t key) {
    if ((key & k_position_key) != 0 || key < static_cast<uint32_t>(k_first_character) ||
        key > static_cast<uint32_t>(k_last_printable_keycode))
        return 0;
    if (key >= static_cast<uint32_t>('A') && key <= static_cast<uint32_t>('Z'))
        return static_cast<int32_t>(key - static_cast<uint32_t>('A') + static_cast<uint32_t>('a'));
    return static_cast<int32_t>(key);
}

// True when the key types into a field. Ctrl, Alt and the system key do not,
// and neither does a place key. Shift still types, so a command moved onto a
// letter stays with the field.
bool key_types_into_field(const ScreenInput& input) {
    const uint16_t command = kInputModifierCtrl | kInputModifierAlt | kInputModifierSystem;
    if ((input.modifiers & command) != 0)
        return false;
    return typed_byte(input.key) != 0;
}

// True when this text is the one character a focus command already took.
bool swallowed_text(const char* text, int32_t stored) {
    if (text == nullptr || text[0] == '\0' || text[1] != '\0' || stored == 0)
        return false;
    const auto byte = static_cast<unsigned char>(text[0]);
    int32_t typed = static_cast<int32_t>(byte);
    if (byte >= 'A' && byte <= 'Z')
        typed = static_cast<int32_t>(byte - 'A' + 'a');
    return typed == stored;
}

// Gives the focused edit the typed characters, whole, including those no
// single key value can name. A character a focus command already took is
// dropped, so it is not typed into the field the command lands on.
void route_text(OverlayState* state, const ScreenInput& input) {
    if (input.text == nullptr)
        return;
    if (swallowed_text(input.text, state->swallow_character)) {
        state->swallow_character = 0;
        return;
    }
    state->swallow_character = 0;
    console::boneyards_status_widget_take_text(state->widget, input.text);
}

// The key the consoles, the frame and the war map already understand for a move.
int32_t focus_message_key(MenuMove move) {
    switch (move) {
    case MenuMove::next:
    case MenuMove::previous:
        return oa::boneyards::key::tab;
    case MenuMove::left:
        return oa::boneyards::key::left;
    case MenuMove::right:
        return oa::boneyards::key::right;
    case MenuMove::up:
        return oa::boneyards::key::up;
    case MenuMove::down:
        return oa::boneyards::key::down;
    case MenuMove::home:
        return oa::boneyards::key::home;
    case MenuMove::end:
        return oa::boneyards::key::end;
    case MenuMove::page_up:
        return oa::boneyards::key::page_up;
    case MenuMove::page_down:
        return oa::boneyards::key::page_down;
    case MenuMove::accept:
    case MenuMove::press:
    case MenuMove::none:
        return 0;
    }
    return 0;
}

// Tab, the arrows, Home, End and the page keys. With the default layout each
// of these fires one focus command. F6 is not one: it moves between regions
// on its own.
bool raw_focus_key(uint32_t key) {
    switch (key) {
    case 0x09U:
    case 0x4000004AU:
    case 0x4000004BU:
    case 0x4000004DU:
    case 0x4000004EU:
    case 0x4000004FU:
    case 0x40000050U:
    case 0x40000051U:
    case 0x40000052U:
        return true;
    default:
        return false;
    }
}

void remember_focus(OverlayState* state, uint32_t sdl, int32_t sent, bool previous) {
    HeldFocus* slot = nullptr;
    for (HeldFocus& held : state->held_focus) {
        if (held.held && held.sdl == sdl) {
            slot = &held;
            break;
        }
        if (slot == nullptr && !held.held)
            slot = &held;
    }
    if (slot == nullptr)
        slot = &state->held_focus[0];
    slot->sdl = sdl;
    slot->sent = sent;
    slot->held = true;
    slot->previous = previous;
}

// The focus key that is down on this key, or null.
const HeldFocus* held_focus_of(const OverlayState* state, uint32_t sdl) {
    for (const HeldFocus& held : state->held_focus) {
        if (held.held && held.sdl == sdl)
            return &held;
    }
    return nullptr;
}

bool take_focus(OverlayState* state, uint32_t sdl, int32_t* sent) {
    for (HeldFocus& held : state->held_focus) {
        if (!held.held || held.sdl != sdl)
            continue;
        *sent = held.sent;
        held = {};
        return true;
    }
    return false;
}

// What a key in the Boneyards does once the player's layout has been read.
enum class FocusRoute : uint8_t { raw, send, drop };

// Reads the focus command this key fires and fills `msg` when that command
// decides the key.
//
// Next button, Previous button, the arrows, Home, End and the page keys are
// the commands the layout binds, not the keys those commands use by default.
// A letter typed into a field is never one of them, and neither is Tab, an
// arrow, Home, End or a page key held with Ctrl. F6 is left as itself unless
// a focus command was moved onto it. The release delivers the same move the
// press did.
FocusRoute focus_route(OverlayState* state, const ScreenInput& input, uint32_t held, Message* msg) {
    if (state->services == nullptr || state->services->bound_focus_action == nullptr ||
        state->host == nullptr)
        return FocusRoute::raw;
    if (input.kind == ScreenInputKind::key_up) {
        int32_t sent = 0;
        if (!take_focus(state, input.key, &sent))
            return FocusRoute::raw;
        // The press's character, when it comes, comes before the release.
        // One that comes later was typed by other means (an input method's
        // or an on-screen keyboard's text) and is kept.
        state->swallow_character = 0;
        if (sent == k_focus_dropped)
            return FocusRoute::drop;
        *msg = Message{};
        msg->x = static_cast<int32_t>(input.x);
        msg->y = static_cast<int32_t>(input.y);
        msg->buttons = held;
        msg->code = MessageCode::key_up;
        msg->key = sent;
        return FocusRoute::send;
    }
    if (input.kind != ScreenInputKind::key_down)
        return FocusRoute::raw;
    state->swallow_character = 0;
    // A chord matches Ctrl only where it names Ctrl, so Ctrl with Tab, an
    // arrow, Home, End or a page key is no focus command of the default
    // layout. It goes to the consoles as before, unasked, so the ring
    // follows the key as it always has, and Ctrl+Left and Ctrl+Right still
    // move a field's caret by a word.
    if ((input.modifiers & kInputModifierCtrl) != 0 && raw_focus_key(input.key))
        return FocusRoute::raw;
    const bool field =
        state->widget != nullptr && console::boneyards_status_widget_text_field(
                                        state->widget, nullptr, nullptr, nullptr, nullptr, nullptr
                                    );
    // A character typed into a field is the field's, even when the layout
    // binds that key to a focus command. Asking with no action keeps the
    // ring from showing for the character.
    if (field && key_types_into_field(input)) {
        static_cast<void>(
            state->services->bound_focus_action(state->host, &input, nullptr, 0, nullptr)
        );
        return FocusRoute::raw;
    }
    char action[96]{};
    static_cast<void>(
        state->services->bound_focus_action(state->host, &input, action, sizeof action, nullptr)
    );
    const MenuMove move = menu_move_of(action);
    int32_t sent = focus_message_key(move);
    bool previous = move == MenuMove::previous;
    if (move == MenuMove::none || sent == 0) {
        // A press that fires nothing while this key is already down is not
        // another step. The bindings repeat a held focus key themselves, and
        // each of those repeats arrives as a press that fires the action.
        // The release still delivers the move the first press made.
        const HeldFocus* down = held_focus_of(state, input.key);
        if (down != nullptr && down->sent != k_focus_dropped)
            return FocusRoute::drop;
        if (!raw_focus_key(input.key))
            return FocusRoute::raw;
        remember_focus(state, input.key, k_focus_dropped, false);
        return FocusRoute::drop;
    }
    remember_focus(state, input.key, sent, previous);
    if (sent == oa::boneyards::key::tab) {
        console::boneyards_status_widget_event_modifiers(
            state->widget,
            previous,
            (input.modifiers & kInputModifierCtrl) != 0,
            (input.modifiers & kInputModifierAlt) != 0,
            (input.modifiers & kInputModifierSystem) != 0
        );
    }
    const int32_t typed = typed_byte(input.key);
    if (typed != 0 && key_types_into_field(input))
        state->swallow_character = typed;
    *msg = Message{};
    msg->x = static_cast<int32_t>(input.x);
    msg->y = static_cast<int32_t>(input.y);
    msg->buttons = held;
    msg->code = MessageCode::key_down;
    msg->key = sent;
    return FocusRoute::send;
}

// The overlay graphic is missing, so the player is told which files to add.
// The game keeps running.
void report_files_missing(ScreenContext* ctx) {
    const std::string missing_sentence(oa::boneyards::shown_text(k_files_missing));
    std::cerr << missing_sentence << '\n';
    if (ctx == nullptr || ctx->services == nullptr || ctx->services->show_notice == nullptr)
        return;
    const std::string missing_title(oa::boneyards::shown_text("The Boneyards"));
    const char* missing_paragraphs[] = {missing_sentence.c_str()};
    NoticeRequest missing_notice{};
    missing_notice.title = missing_title.c_str();
    missing_notice.paragraphs = missing_paragraphs;
    missing_notice.paragraph_count = 1;
    static_cast<void>(ctx->services->show_notice(ctx->host, &missing_notice));
}

/// Names a mode for the log.
///
/// @param mode the mode
/// @return its name
const char* mode_name(console::MenuEntryMode mode) {
    switch (mode) {
    case console::MenuEntryMode::strip:
        return "strip";
    case console::MenuEntryMode::button:
        return "button";
    case console::MenuEntryMode::none:
        return "none";
    }
    return "strip";
}

/// Decodes a FrontendX.
///
/// @param bytes the file
/// @return the picture, or nullopt when it does not decode
std::optional<oa::Image> decode_picture(const std::vector<uint8_t>& bytes) {
    if (bytes.empty())
        return std::nullopt;
    auto decoded = oa::decode_pcx(bytes);
    if (!decoded.ok())
        return std::nullopt;
    return std::move(decoded.value);
}

/// Chooses, once a runtime, how the way in stands on the main menu, and logs it.
///
/// The menu's FrontendX is set against the base game's over the strip's
/// rows. A FrontendX the played mod's folder does not hold is the base
/// game's own. The plate is shown in the base game's palette.
///
/// @param ctx screen context, with the assets
/// @param state overlay state
/// @param shown_bytes the FrontendX the menu shows
void choose_mode(ScreenContext* ctx, OverlayState* state, const std::vector<uint8_t>& shown_bytes) {
    if (state->mode)
        return;
    const std::optional<oa::Image> shown = decode_picture(shown_bytes);
    std::optional<oa::Image> base;
    bool own_art = false;
    try {
        own_art = renderer::from_mod_folder(
            *ctx->assets, renderer::main_menu_background_path, state->mod_folder
        );
        if (own_art) {
            if (auto found =
                    renderer::base_game_main_menu_background(*ctx->assets, state->mod_folder))
                base = decode_picture(found->bytes);
        }
    } catch (const std::exception&) {
        base.reset();
    }
    // The share of the strip's rows the menu's art changes, in thousandths:
    // none for the base game's own picture, and all of it when a mod's
    // picture has no base to be set against.
    uint32_t share = 0;
    if (own_art)
        share = shown && base
                    ? renderer::compare_menu_art(
                          *shown, *base, renderer::main_menu_band_top, renderer::main_menu_band_rows
                      )
                          .per_mille()
                    : 1000U;
    state->mode = console::choose_menu_entry_mode(state->entry_ask, share);
    const std::optional<oa::Image>& palette_source = base ? base : shown;
    if (palette_source && palette_source->palette)
        state->plate_palette.assign(
            palette_source->palette->begin(), palette_source->palette->end()
        );
    std::cout << "boneyards menu: " << mode_name(*state->mode) << " mode; the menu's art differs "
              << "from the base game's in " << share / 10U << '.' << share % 10U
              << "% of the strip's rows"
              << (state->entry_ask == console::MenuEntryAsk::automatic
                      ? ""
                      : "; the mod's profile names the mode")
              << '\n';
    std::cout.flush();
}

/// Places the strip's ENTER plate where the mod's profile puts it
/// (media.boneyards-entry x and y), or at the default place at the top right,
/// and logs where it stands and when it covers a gadget of the menu or one
/// of Open Annihilation's buttons, which stay where they are.
///
/// @param ctx screen context, with the assets
/// @param state overlay state
void place_plate(ScreenContext* ctx, OverlayState* state) {
    state->plate_place = console::entry_plate_place(state->entry_x, state->entry_y);
    std::vector<renderer::SourceRect> covered;
    try {
        const auto menu = renderer::load_main_menu(
            *ctx->assets, renderer::MainMenuLayout::base_game, state->mod_folder
        );
        for (std::size_t index = 1; index < menu.layout.gadgets.size(); ++index) {
            const auto& common = menu.layout.gadgets[index].common;
            if (common.active == 0 || common.width <= 0)
                continue;
            // A label authored with no height (MAINMENU's DebugString) still
            // draws a line of text.
            const int32_t height = common.height > 0 ? common.height : k_label_line;
            covered.push_back({common.x, common.y, common.width, height});
        }
    } catch (const std::exception&) {
        covered.clear();
    }
    const renderer::MenuPairPlaces pair =
        renderer::menu_pair_places(renderer::MenuCorner::bottom_right);
    covered.push_back(pair.library);
    covered.push_back(pair.settings);
    const renderer::SourceRect& place = state->plate_place;
    const bool asked = place.x != console::entry_plate_default_place.x ||
                       place.y != console::entry_plate_default_place.y;
    std::cout << "boneyards menu: the ENTER plate stands at " << place.x << ',' << place.y
              << (asked ? ", where the mod's profile puts it (media.boneyards-entry)"
                        : ", the default place at the top right")
              << '\n';
    if (console::entry_plate_covers(place, covered))
        std::cout << "boneyards menu: the ENTER plate at " << place.x << ',' << place.y
                  << " covers a gadget of the mod's menu or one of Open Annihilation's buttons; "
                     "media.boneyards-entry x and y in the mod's profile move it\n";
    std::cout.flush();
}

/// Logs that the profile's place for the plate is not used, the strip being
/// drawn instead.
///
/// @param state overlay state, its mode chosen
void note_unused_place(const OverlayState* state) {
    if (!state->mode || *state->mode == console::MenuEntryMode::button)
        return;
    if (state->entry_x == console::entry_plate_default_place.x &&
        state->entry_y == console::entry_plate_default_place.y)
        return;
    std::cout << "boneyards menu: media.boneyards-entry x " << state->entry_x << " and y "
              << state->entry_y << " place the ENTER plate in button mode only; this menu is in "
              << mode_name(*state->mode) << " mode\n";
    std::cout.flush();
}

void build_strip(ScreenContext* ctx, OverlayState* state) {
    if (ctx == nullptr || ctx->assets == nullptr)
        return;
    state->host = ctx->host;
    state->services = ctx->services;
    state->menu_passes_left = 0;
    state->ctx = ctx;
    state->frontend_state = console::frontend_value::state_main_menu;
    state->held_buttons = 0;
    state->shade = read_or_empty(ctx->assets, k_shade_table);
    state->light = read_or_empty(ctx->assets, k_light_table);
    const std::vector<uint8_t> pcx = read_or_empty(ctx->assets, k_active_palette);
    const bool first_build = !state->mode;
    choose_mode(ctx, state, pcx);
    if (first_build && *state->mode == console::MenuEntryMode::button)
        place_plate(ctx, state);
    else if (first_build)
        note_unused_place(state);
    if (!pcx.empty()) {
        auto decoded = oa::decode_pcx(pcx);
        if (decoded.ok())
            state->palette = std::move(decoded.value->palette);
        else
            state->palette.reset();
    }
    if (state->palette) {
        for (std::size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
            const uint8_t* rgb = state->palette->data() + i * oa::palette_entry_bytes;
            state->widget_palette.entries[i] = oa::PaletteEntry{rgb[0], rgb[1], rgb[2], 0};
        }
    }
    console::StatusWidgetConfig config{};
    config.files = console::FileReader{ctx->assets, read_resource, rescan_archives};
    config.shade = console::ShadeTables{
        state->shade.size() == console::k_shade_table_bytes ? state->shade.data() : nullptr,
        state->light.size() == console::k_shade_table_bytes ? state->light.data() : nullptr
    };
    config.palette = state->palette ? &state->widget_palette : nullptr;
    config.text_fonts = state->text_fonts;
    config.settings = {
        state,
        settings_save_number,
        settings_read_number,
        settings_read_string,
        settings_save_string
    };
    config.host.user = state;
    config.host.play_sound = host_play_sound;
    config.host.register_sound = host_register_sound;
    config.host.prepare_multiplayer = host_prepare_multiplayer;
    config.host.load_multiplayer_document = host_load_multiplayer_document;
    // Clears the highlighted main-menu gadget once the multiplayer map loads.
    config.host.after_multiplayer_selection = host_close_main_menu_panel;
    config.host.set_pending_signal = host_set_pending_signal;
    config.host.set_signal = host_set_signal;
    config.host.set_frontend_state = host_set_frontend_state;
    config.host.update_gadgets = host_close_main_menu_panel;
    config.host.show_message_box = host_show_message_box;
    config.host.init_main_menu_panel = host_reload_main_menu_panel;
    config.host.press_taken_below = host_press_taken_below;
    if (ctx->services->current_tick != nullptr)
        config.host.current_tick = host_current_tick;
    config.host.pointer_position = host_pointer_position;
    config.host.battle_message = host_battle_message;
    config.host.battle_launched = host_battle_launched;
    config.host.launch_permitted = host_launch_permitted;
    config.host.launch_ended = host_launch_ended;
    config.host.run_frontend = host_run_frontend;
    config.host.left_game = host_left_game;
    config.host.set_latency_reply_flags = host_set_latency_reply_flags;
    config.host.empty_chat_ring = host_empty_chat_ring;
    config.host.stop_sounds = host_stop_sounds;
    config.host.play_sound_alternate = host_play_sound_alternate;
    config.host.leave_game = host_leave_game;
    config.host.request_entry = g_entry_flow;
    config.defer_models = true;
    config.service = oa::boneyards::rover_session_configured()
                         ? oa::boneyards::rover_session_service()
                         : oa::boneyards::boneyards_offline_service(nullptr);
    state->widget = console::boneyards_status_widget_create(config);
    if (state->widget == nullptr) {
        report_files_missing(ctx);
    } else {
        console::boneyards_status_widget_set_screen_keyboard(
            state->widget, host_screen_keyboard, state
        );
        // The consoles' sound names are known from here on, however the
        // Boneyards are later opened.
        console::boneyards_status_widget_register_sounds(state->widget);
    }
    state->ctx = nullptr;
    state->plate_held = false;
    state->press_outside_strip = false;
    // The strip draws only with the active palette; without it the menu is
    // left with no Boneyards buttons over its own, and nothing of the strip
    // takes the pointer. Outside strip mode the menu keeps the layout it has
    // without the strip, and the plate needs the base game's palette too.
    const bool drawable = state->widget != nullptr && state->palette.has_value();
    state->strip_drawn = drawable && *state->mode == console::MenuEntryMode::strip &&
                         ctx->services->set_main_menu_overlay != nullptr;
    state->plate_shown = drawable && *state->mode == console::MenuEntryMode::button &&
                         state->plate_palette.size() >= OA_PALETTE_COLORS * oa::palette_entry_bytes;
    if (state->strip_drawn)
        ctx->services->set_main_menu_overlay(ctx->host, 1);
}

void overlay_create(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    state->host = ctx->host;
    state->services = ctx->services;
    state->menu_passes_left = 0;
    state->battle_return_pending = false;
    // The saved-game run never draws this menu. Building the strip here would
    // keep it for the whole load.
    if (g_defer_strip || ctx->assets == nullptr)
        return;
    build_strip(ctx, state);
}

/// Tells whether an input is the pointer's.
///
/// @param kind the input's kind
/// @return true for a move, a press, a release or the wheel
bool pointer_input(ScreenInputKind kind) {
    return kind == ScreenInputKind::pointer_move || kind == ScreenInputKind::pointer_down ||
           kind == ScreenInputKind::pointer_up || kind == ScreenInputKind::wheel;
}

/// Tells whether a canvas point lies in a rectangle.
///
/// @param rect the rectangle
/// @param x the point's column
/// @param y the point's row
/// @return true inside it
bool inside(const renderer::SourceRect& rect, float x, float y) {
    return x >= static_cast<float>(rect.x) && y >= static_cast<float>(rect.y) &&
           x < static_cast<float>(rect.x + rect.width) &&
           y < static_cast<float>(rect.y + rect.height);
}

/// Tells whether a canvas point lies in the strip's rows.
///
/// @param x the point's column
/// @param y the point's row
/// @return true inside the strip's 640 by 102 area
bool on_strip(float x, float y) {
    return inside(
        {console::overlay_layout::x,
         console::overlay_layout::y,
         console::overlay_layout::width,
         console::overlay_layout::height},
        x,
        y
    );
}

/// Passes the pointer over the plate to the strip, at the plate's own place.
///
/// Over the plate, each pointer input reaches the strip's windows as if the
/// pointer were on the strip's own plate, so ENTER shows its press and
/// answers its release as it does on the strip. A press made on the plate
/// keeps every pointer input until it is let go, wherever the pointer goes,
/// so a release away from ENTER presses nothing. Off the plate the menu has
/// the pointer, including a point under it. Nothing takes it while the plate
/// is not shown.
///
/// @param ctx screen context of the event
/// @param state overlay state
/// @param input the pointer input
/// @return 1 when the plate took the input
int plate_event(ScreenContext* ctx, OverlayState* state, const ScreenInput& input) {
    if (!state->plate_shown) {
        state->plate_held = false;
        return 0;
    }
    const bool over = inside(state->plate_place, input.x, input.y);
    if (!over && !state->plate_held)
        return 0;
    ScreenInput moved = input;
    moved.x = input.x - static_cast<float>(state->plate_place.x - console::entry_plate.x);
    moved.y = input.y - static_cast<float>(state->plate_place.y - console::entry_plate.y);
    if (moved.kind == ScreenInputKind::pointer_down)
        state->held_buttons |= button_bit(moved.button);
    Message msg{};
    const bool routed = translate(moved, state->held_buttons, false, &msg);
    if (moved.kind == ScreenInputKind::pointer_up)
        state->held_buttons &= ~button_bit(moved.button);
    if (routed) {
        state->ctx = ctx;
        static_cast<void>(console::boneyards_status_widget_event(state->widget, msg));
        state->ctx = nullptr;
    }
    if (moved.kind == ScreenInputKind::pointer_down && over)
        state->plate_held = true;
    if (moved.kind == ScreenInputKind::pointer_up && state->held_buttons == 0)
        state->plate_held = false;
    return 1;
}

/// Tells whether the drawn strip keeps a pointer input from the menu under it.
///
/// The strip takes every pointer input inside its own rows, so a gadget of
/// the menu under its art takes no click. Credits stays with the gadget
/// under the strip's credits spot. A press that began outside the strip's
/// rows is the menu's: its moves and its release reach the menu wherever
/// they land, so a button pressed on its visible part is let go. Outside
/// the rows the strip keeps what its windows took.
///
/// @param[in,out] state overlay state; notes where a press began
/// @param input the pointer input
/// @param consumed the strip's windows took it
/// @param mirrored it was on the credits spot, which the gadget under it answers
/// @return true when the menu does not see it
bool strip_pointer_taken(
    OverlayState* state, const ScreenInput& input, bool consumed, bool mirrored
) {
    const bool over = on_strip(input.x, input.y);
    if (input.kind == ScreenInputKind::pointer_down)
        state->press_outside_strip = !over;
    const bool menu_press = state->press_outside_strip;
    if (input.kind == ScreenInputKind::pointer_up && state->held_buttons == 0)
        state->press_outside_strip = false;
    if (mirrored)
        return false;
    if (menu_press &&
        (input.kind == ScreenInputKind::pointer_up || input.kind == ScreenInputKind::pointer_move))
        return consumed && !over;
    return over || consumed;
}

// + and − zoom the war map. They are printable, so with the Boneyards open
// they would be kept for a text field. A field that has the keyboard still
// takes them as text; otherwise the planet that has it does.
bool map_zoom_key(const ScreenInput& input) {
    if (input.kind != ScreenInputKind::key_down && input.kind != ScreenInputKind::key_up)
        return false;
    return input.key == static_cast<uint32_t>('-') || input.key == static_cast<uint32_t>('+') ||
           input.key == static_cast<uint32_t>('=');
}

// The layer's ring, copied onto the desktop before this key is routed.
//
// Tab, F6, Return and a pointer move still update the desktop for the key
// in hand. The copy is what the consoles read on the next key, which is the
// ring the family is drawing, including when focus is always shown.
void note_service_ring(OverlayState* state) {
    if (state->widget == nullptr || state->services == nullptr ||
        state->services->bound_focus_action == nullptr || state->host == nullptr)
        return;
    // A key-up asks only for the ring. It does not count as the screen
    // reading the key.
    ScreenInput probe{};
    probe.kind = ScreenInputKind::key_up;
    int shown = 0;
    static_cast<void>(state->services->bound_focus_action(state->host, &probe, nullptr, 0, &shown));
    console::boneyards_status_widget_note_ring(state->widget, shown);
}

// In frontend state 7 no TA panel takes input, so every input is consumed.
int overlay_event(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    if (state->widget == nullptr || ctx->input == nullptr)
        return 0;
    const ScreenInput& input = *ctx->input;
    const bool boneyards = in_boneyards(state);
    if (boneyards)
        note_service_ring(state);
    if (input.kind == ScreenInputKind::text) {
        if (!boneyards)
            return 0;
        state->ctx = ctx;
        route_text(state, input);
        state->ctx = nullptr;
        publish_text_field(state);
        return 1;
    }
    const bool modal = console::boneyards_status_widget_message_open(state->widget);
    // Only what is drawn takes the pointer: the strip in strip mode, the
    // plate in button mode, and nothing in none or without a palette.
    if (!boneyards && !modal && pointer_input(input.kind)) {
        if (state->mode && *state->mode == console::MenuEntryMode::button)
            return plate_event(ctx, state, input);
        if (!state->strip_drawn)
            return 0;
    }
    if (input.kind == ScreenInputKind::pointer_down)
        state->held_buttons |= button_bit(input.button);
    Message msg{};
    apply_key_modifiers(state, input);
    // The host's order owns Tab and Shift+Tab on the main menu, and, while
    // its ring is showing, the arrows and the bound Accept and Press. The
    // strip drops its focus and passes those keys on. With no ring the
    // strip keeps every other key it takes, and a message box keeps its own.
    if (!boneyards && !modal &&
        (input.kind == ScreenInputKind::key_down || input.kind == ScreenInputKind::key_up) &&
        state->services != nullptr && state->services->main_menu_order_owns_key != nullptr &&
        state->services->main_menu_order_owns_key(state->host, &input) != 0) {
        console::boneyards_status_widget_clear_focus(state->widget);
        return 0;
    }
    bool typing = boneyards;
    if (typing && map_zoom_key(input) &&
        !console::boneyards_status_widget_text_field(
            state->widget, nullptr, nullptr, nullptr, nullptr, nullptr
        ))
        typing = false;
    // In the Boneyards the focus keys are the player's. The consoles, the
    // frame and the war map then see the same moves they see with the
    // default layout. On the main menu the host's order already owns those
    // keys, and the strip keeps the keys it has today.
    bool routed = false;
    const bool focus_key = boneyards && (input.kind == ScreenInputKind::key_down ||
                                         input.kind == ScreenInputKind::key_up);
    if (focus_key) {
        const FocusRoute decided = focus_route(state, input, state->held_buttons, &msg);
        if (decided == FocusRoute::raw)
            routed = translate(input, state->held_buttons, typing, &msg);
        else
            routed = decided == FocusRoute::send;
    } else {
        routed = translate(input, state->held_buttons, typing, &msg);
    }
    if (input.kind == ScreenInputKind::pointer_up)
        state->held_buttons &= ~button_bit(input.button);
    if (!routed) {
        console::boneyards_status_widget_event_modifiers(state->widget, false, false, false, false);
        publish_text_field(state);
        // The middle button and the wheel on the strip's rows reach no
        // gadget under it either.
        const bool on_drawn_strip = state->strip_drawn && !modal && pointer_input(input.kind) &&
                                    on_strip(input.x, input.y) && !state->press_outside_strip;
        return boneyards || on_drawn_strip ? 1 : 0;
    }
    state->ctx = ctx;
    state->mirrored_event = !boneyards && !modal && msg.code != MessageCode::key_down &&
                            msg.code != MessageCode::key_up &&
                            is_mirrored_point(state->widget, msg.x, msg.y);
    const bool consumed = console::boneyards_status_widget_event(state->widget, msg);
    const bool mirrored = state->mirrored_event;
    state->mirrored_event = false;
    state->ctx = nullptr;
    publish_text_field(state);
    if (boneyards || modal)
        return 1;
    if (state->strip_drawn && pointer_input(input.kind))
        return strip_pointer_taken(state, input, consumed, mirrored) ? 1 : 0;
    return consumed && !mirrored ? 1 : 0;
}

void overlay_tick(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    // A menu button asks for two frontend passes. The click's pass leaves
    // the menu and releases the strip. This tick asks for the pass that
    // opens the next screen, and that request still has to be made once the
    // strip is gone.
    const bool opening_pass = state->menu_passes_left > 1;
    if (opening_pass)
        --state->menu_passes_left;
    if (state->widget == nullptr) {
        if (opening_pass)
            host_run_frontend(state);
        return;
    }
    finish_battle_return(ctx, state);
    if (state->entry_pass_pending) {
        state->entry_pass_pending = false;
        host_run_frontend(state);
    }
    if (opening_pass)
        host_run_frontend(state);
    state->ctx = ctx;
    // The shortcut check leaves the way Exit does, once the login is up and
    // before this tick pumps the session.
    const bool shortcut_left = g_check_shortcut && !g_shortcut_done && in_boneyards(state);
    if (shortcut_left) {
        g_shortcut_done = true;
        if (!g_end_on_leave) {
            std::cout << "boneyards shortcut check: -y was not given\n";
            std::cout.flush();
        } else {
            std::cout << "boneyards shortcut check: -y entered the login and left with no reason\n";
            std::cout.flush();
            host_set_frontend_state(state, console::frontend_value::state_main_menu);
        }
    }
    if (!shortcut_left) {
        if (in_boneyards(state))
            note_service_ring(state);
        console::boneyards_status_widget_tick(state->widget);
    }
    state->ctx = nullptr;
    state->ticked = true;
    if (in_boneyards(state))
        publish_text_field(state);
}

/// Fills `pixels` with one pass of the windows.
///
/// `origin_y` is the screen row the buffer's top stands for, and `height`
/// is the rows the buffer holds. The strip's buffer is only the strip. A
/// message box, and the Boneyards window, use the whole frame.
///
/// @param state overlay
/// @param pixels pass buffer; a larger one is released
/// @param fill colour the windows did not write
/// @param origin_y screen row of the buffer's first row
/// @param height rows in the buffer
void draw_pass(
    OverlayState* state,
    std::vector<uint8_t>& pixels,
    uint8_t fill,
    int32_t origin_y,
    int32_t height
) {
    const std::size_t count =
        static_cast<std::size_t>(k_frame_width) * static_cast<std::size_t>(height);
    if (pixels.capacity() > count) {
        std::vector<uint8_t>(count, fill).swap(pixels);
    } else {
        pixels.assign(count, fill);
    }
    oa::Surface surface{};
    surface.width = k_frame_width;
    surface.height = height;
    surface.pitch = k_frame_width;
    surface.pixels = pixels.data();
    surface.clip = oa::Rect32{0, 0, k_frame_width - 1, height - 1};
    console::boneyards_status_widget_draw(state->widget, &surface, origin_y);
}

/// Writes one palette colour as RGB into a frontend surface.
///
/// @param[in,out] target frontend surface with 3 bytes per pixel
/// @param palette 256-entry palette
/// @param pixel pixel index in the surface
/// @param index palette index of the colour
void put_pixel(
    oa::ui::frontend_renderer::Surface* target,
    const oa::PaletteBytes& palette,
    std::size_t pixel,
    uint8_t index
) {
    const std::size_t color = static_cast<std::size_t>(index) * oa::palette_entry_bytes;
    target->rgb[pixel * 3] = palette[color];
    target->rgb[pixel * 3 + 1] = palette[color + 1];
    target->rgb[pixel * 3 + 2] = palette[color + 2];
}

/// The line drawn round the plate on a mod's own art, so its edge holds on
/// light or busy art: the strip's darkest metal.
constexpr uint8_t k_plate_outline_rgb[3] = {12, 12, 12};

/// Shows the strip's ENTER plate at its place on the main menu.
///
/// The strip draws its rows as it does in strip mode, lamps, greyed or
/// pressed ENTER and all, and the plate's rectangle of them is copied to the
/// plate's place in the base game's palette, every pixel, inside a one-pixel
/// dark line.
///
/// @param state overlay state, its context set
/// @param target the frame
void draw_plate(OverlayState* state, oa::ui::frontend_renderer::Surface* target) {
    if (!state->plate_shown)
        return;
    const int32_t top = console::overlay_layout::y;
    draw_pass(state, state->pass_a, 0x00, top, console::overlay_layout::height);
    const auto put = [&](int32_t x, int32_t y, const uint8_t* rgb) {
        if (x < 0 || y < 0 || x >= k_frame_width || y >= k_frame_height)
            return;
        const std::size_t at =
            (static_cast<std::size_t>(y) * k_frame_width + static_cast<std::size_t>(x)) * 3U;
        target->rgb[at] = rgb[0];
        target->rgb[at + 1] = rgb[1];
        target->rgb[at + 2] = rgb[2];
    };
    const renderer::SourceRect& place = state->plate_place;
    const renderer::SourceRect& plate = console::entry_plate;
    for (int32_t row = 0; row < plate.height; ++row) {
        for (int32_t column = 0; column < plate.width; ++column) {
            const std::size_t from = static_cast<std::size_t>(plate.y - top + row) * k_frame_width +
                                     static_cast<std::size_t>(plate.x + column);
            const std::size_t colour =
                static_cast<std::size_t>(state->pass_a[from]) * oa::palette_entry_bytes;
            put(place.x + column, place.y + row, state->plate_palette.data() + colour);
        }
    }
    for (int32_t step = -1; step <= plate.width; ++step) {
        put(place.x + step, place.y - console::entry_plate_outline, k_plate_outline_rgb);
        put(place.x + step, place.y + plate.height, k_plate_outline_rgb);
    }
    for (int32_t step = 0; step < plate.height; ++step) {
        put(place.x - console::entry_plate_outline, place.y + step, k_plate_outline_rgb);
        put(place.x + plate.width, place.y + step, k_plate_outline_rgb);
    }
}

void overlay_draw(ScreenContext* ctx, void* user) {
    auto* state = static_cast<OverlayState*>(user);
    auto* target = ctx->surface;
    if (state->widget == nullptr || !state->palette || target == nullptr ||
        target->width != k_frame_width || target->height != k_frame_height ||
        target->rgb.size() != k_frame_pixels * 3U)
        return;
    const oa::PaletteBytes& palette = *state->palette;
    state->ctx = ctx;
    // In state 7 the Boneyards desktop is the whole frame.
    if (in_boneyards(state)) {
        draw_pass(state, state->pass_a, 0x00, 0, k_frame_height);
        state->ctx = nullptr;
        for (std::size_t i = 0; i < state->pass_a.size(); ++i)
            put_pixel(target, palette, i, state->pass_a[i]);
        return;
    }
    // A message box sits above the strip, so that draw keeps the whole frame.
    // The idle strip writes only inside its own rows. Outside strip mode the
    // strip's rows are not shown: in button mode only its plate is, at the
    // plate's place, and a box only above those rows.
    const bool whole = console::boneyards_status_widget_message_open(state->widget);
    const bool strip_shown = state->strip_drawn;
    if (!strip_shown) {
        draw_plate(state, target);
        if (!whole) {
            state->ctx = nullptr;
            return;
        }
    }
    const int32_t origin_y = whole ? 0 : console::overlay_layout::y;
    const int32_t height = whole ? k_frame_height : console::overlay_layout::height;
    // Two passes over different fills find the pixels the overlay wrote.
    draw_pass(state, state->pass_a, 0x00, origin_y, height);
    draw_pass(state, state->pass_b, 0xFF, origin_y, height);
    state->ctx = nullptr;
    const std::size_t width = static_cast<std::size_t>(k_frame_width);
    const std::size_t top = static_cast<std::size_t>(origin_y);
    // Without the strip a box draws only above the strip's rows.
    const std::size_t rows_shown =
        strip_shown ? state->pass_a.size()
                    : static_cast<std::size_t>(console::overlay_layout::y) * width;
    for (std::size_t i = 0; i < state->pass_a.size() && i < rows_shown; ++i) {
        if (state->pass_a[i] != state->pass_b[i])
            continue;
        const std::size_t pixel = (top + i / width) * width + i % width;
        put_pixel(target, palette, pixel, state->pass_a[i]);
    }
}

} // namespace

// Test access to the overlay's status widget; null before the overlay is
// created.
console::StatusWidget* boneyards_console_status_widget() {
    return g_state.widget;
}

// Where a focused text field is reported. A null place stops reporting.
// The host is the one the overlay's create was given.
void boneyards_console_set_text_field_place(
    void (*place)(void* host, int x, int y, int width, int height, int cursor)
) {
    g_text_field_place = place;
}

// The text service the markup pages of overlays created from now on use; a
// service with no `open` leaves them in the FNT fonts.
void boneyards_console_set_text_fonts(const oa::boneyards::html::FontService& fonts) {
    g_state.text_fonts = fonts;
}

// The analogue atlas beside the bundled fonts. The console opens it when the
// Boneyards window opens. A null path clears the one set earlier.
void boneyards_console_set_analogue_atlas_path(const char* path) {
    console::boneyards_status_widget_set_analogue_atlas_path(path);
}

// Check access to the sounds the overlay asks the host to play; a null
// observer stops listening.
void boneyards_console_observe_sounds(void (*observer)(void* user, const char* name), void* user) {
    g_state.sound_observer = observer;
    g_state.sound_observer_user = user;
}

// Has the overlay's next tick ask for a frontend pass, whose main-menu
// update enters the Boneyards after "-y" (dispatcher step 0x10b).
void boneyards_console_request_entry_pass() {
    g_state.entry_pass_pending = true;
}

// "-y": the next return from the Boneyards to the main menu ends the run.
void boneyards_console_end_on_leave() {
    g_end_on_leave = true;
}

// --check-boneyards-shortcut leaves the login once it is up.
void boneyards_console_check_shortcut() {
    g_check_shortcut = true;
}

/// ENTER, the B key and "-y" call `flow` instead of opening at once.
///
/// A null flow restores the open that does not check for an update or a
/// missing expansion. An overlay created after this copies the flow onto
/// its host; one created before keeps the flow it was given.
///
/// @param flow the entry, or null
void boneyards_console_set_entry_flow(void (*flow)(console::StatusWidget*)) {
    g_entry_flow = flow;
}

// Installs the application's part of a battle the Boneyards launches.
void boneyards_console_set_launch_bridge(const console::LaunchBridge& bridge) {
    g_state.bridge = bridge;
}

// Runs the Boneyards' idle work of one frame: the session is pumped unless
// the overlay's tick pumped it since the last call.
void boneyards_console_idle(uint32_t now_ms) {
    console::boneyards_status_widget_idle(g_state.widget, now_ms, !g_state.ticked);
    g_state.ticked = false;
}

// Leaves the Boneyards for the main menu. A launch already ended is not ended again.
void boneyards_console_exit_to_menu() {
    console::boneyards_status_widget_exit_to_menu(g_state.widget);
}

// Opens a Boneyards message box, shown with the Boneyards screens.
void boneyards_console_show_notice(const char* text) {
    console::boneyards_status_widget_show_notice(g_state.widget, text);
}

// Tells whether the Boneyards is up (boneyards_status_widget_up).
bool boneyards_console_up() {
    return console::boneyards_status_widget_up(g_state.widget);
}

// Opens the Boneyards box "Exit to the system?"; false when none opens.
bool boneyards_console_ask_exit() {
    return console::boneyards_status_widget_ask_exit(g_state.widget);
}

// Writes one of the Boneyards' settings (the preferences' Boneyards section)
// through the services the overlay's create kept; false before the overlay
// is created.
bool boneyards_console_write_setting(const char* key, const char* value) {
    if (g_state.services == nullptr || g_state.services->write_string == nullptr)
        return false;
    g_state.services->write_string(g_state.host, "Boneyards", key, value);
    return true;
}

// Stops every playing sound, as the Boneyards does when a joiner's host is
// not found.
void boneyards_console_stop_sounds() {
    console::boneyards_status_widget_stop_sounds(g_state.widget);
}

// A headless --load asks for this before the screens register. The strip is
// then not built. The next runtime builds its own.
void boneyards_console_defer_main_menu() {
    g_defer_strip = true;
}

/// Frees the overlay and the menu tables a runtime built.
///
/// The status widget and the shade, light and frame buffers live for the
/// runtime that created them; the next runtime builds its own. Host calls
/// run only while a screen hook is in progress, and none is, so nothing of
/// the runtime is used. A runtime that built no overlay frees nothing.
void boneyards_console_release() {
    // A host call checks this and does nothing, so tearing the widget down
    // cannot reach the runtime.
    g_state.ctx = nullptr;
    g_defer_strip = false;
    console::boneyards_status_widget_destroy(g_state.widget);
    g_state.widget = nullptr;
    g_state.host = nullptr;
    g_state.services = nullptr;
    g_state.held_buttons = 0;
    g_state.mirrored_event = false;
    g_state.frontend_state = console::frontend_value::state_main_menu;
    g_state.entry_pass_pending = false;
    g_state.battle_return_pending = false;
    g_state.ticked = false;
    g_state.sound_observer = nullptr;
    g_state.sound_observer_user = nullptr;
    std::vector<uint8_t>().swap(g_state.shade);
    std::vector<uint8_t>().swap(g_state.light);
    std::vector<uint8_t>().swap(g_state.pass_a);
    std::vector<uint8_t>().swap(g_state.pass_b);
    g_state.palette.reset();
    g_state.entry_ask = console::MenuEntryAsk::automatic;
    g_state.entry_x = console::entry_plate_default_place.x;
    g_state.entry_y = console::entry_plate_default_place.y;
    g_state.mod_folder.clear();
    g_state.mode.reset();
    g_state.strip_drawn = false;
    g_state.plate_shown = false;
    g_state.plate_place = {};
    std::vector<uint8_t>().swap(g_state.plate_palette);
    g_state.plate_held = false;
    g_state.press_outside_strip = false;
    for (HeldFocus& held : g_state.held_focus)
        held = {};
    g_state.swallow_character = 0;
    console::console_images_release_embedded_menu();
}

/// What the mod's profile asks of the main menu's way in
/// (media.boneyards-entry), and the played mod's folder.
///
/// The mode and the place are read when the strip is first built; a mode
/// that names none of the four is auto. The place is the ENTER plate's
/// top-left corner in button mode; the profile's baseline is the default
/// place at the top right. Only the mod's folder holds the mod's own art: an
/// expansion's folder is the game's.
///
/// @param mode auto, strip, button or none; null is auto
/// @param x the plate's left column
/// @param y the plate's top row
/// @param mod_folder the played mod's folder; empty when no mod is played
void boneyards_console_set_menu_entry(
    const char* mode, int32_t x, int32_t y, const std::filesystem::path& mod_folder
) {
    g_state.entry_ask = console::menu_entry_ask_named(mode != nullptr ? mode : "");
    g_state.entry_x = x;
    g_state.entry_y = y;
    g_state.mod_folder = mod_folder;
}

/// Reports how the way in stands on the main menu, and the plate's place.
///
/// @param[out] mode the mode; strip until the strip is first built
/// @param[out] x the plate's left column while it is shown; 0 otherwise
/// @param[out] y its top row
/// @param[out] width its width; 0 while it is not shown
/// @param[out] height its height
/// @return false until the strip has been built once
bool boneyards_console_menu_entry(
    console::MenuEntryMode* mode, int32_t* x, int32_t* y, int32_t* width, int32_t* height
) {
    const bool chosen = g_state.mode.has_value();
    const console::MenuEntryMode shown = chosen ? *g_state.mode : console::MenuEntryMode::strip;
    const bool button = shown == console::MenuEntryMode::button && g_state.plate_shown;
    if (mode != nullptr)
        *mode = shown;
    if (x != nullptr)
        *x = button ? g_state.plate_place.x : 0;
    if (y != nullptr)
        *y = button ? g_state.plate_place.y : 0;
    if (width != nullptr)
        *width = button ? g_state.plate_place.width : 0;
    if (height != nullptr)
        *height = button ? g_state.plate_place.height : 0;
    return chosen;
}

void register_boneyards_console_screens(ScreenRegistry* registry) {
    OverlayDesc desc{};
    desc.name = "boneyards-status-widget";
    desc.screen = k_main_menu_screen;
    desc.z = k_overlay_z;
    desc.create = overlay_create;
    desc.event = overlay_event;
    desc.tick = overlay_tick;
    desc.draw = overlay_draw;
    desc.state = &g_state;
    overlay_register(registry, &desc);
    step_register(
        registry, oa::ui::frontend_state::Step::shut_down_resource, step_leave_main_menu, &g_state
    );
    step_register(
        registry,
        oa::ui::frontend_state::Step::return_to_main_menu,
        step_return_to_main_menu,
        &g_state
    );
    // The main menu's return to the Boneyards after a launched battle (the
    // multiplayer states' return_after_launch step).
    step_register(
        registry,
        oa::ui::frontend_state::Step{k_return_to_boneyards_step},
        step_return_to_boneyards,
        &g_state
    );
    // The entry "-y" takes (the multiplayer states' setup_requested step).
    step_register(
        registry,
        oa::ui::frontend_state::Step{k_enter_boneyards_step},
        step_enter_boneyards,
        &g_state
    );
}

} // namespace oa::app
