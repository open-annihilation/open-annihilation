// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Boneyards Main Frame: the full-screen console shown after "Enter
// Boneyards", with its navigation buttons, activity light and advert banner.
#include "oa/boneyards/utf8.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "ad_banner.hpp"
#include "battle_console.hpp"
#include "console_layer.hpp"
#include "console_state.hpp"
#include "oa/boneyards/console/feature_consoles.hpp"
#include "oa/boneyards/message_box.hpp"
#include "oa/boneyards/release.hpp"
#include "widgets.hpp"

namespace oa::boneyards::console {

namespace {

namespace layout = main_frame_layout;

constexpr int32_t k_login_delay = 0xf;
constexpr int32_t k_keepalive_period = 0x1c2;

// Main_Console.Gaf frames of a console button.
constexpr int32_t k_button_frame_lit = 0;
constexpr int32_t k_button_frame_pressed = 1;
constexpr int32_t k_button_frame_normal = 2;
constexpr int32_t k_button_frame_disabled = 3;

const char* const k_frame_name = "BYMainFrame";
const char* const k_frame_sequence = "Main_Frame";
const char* const k_exit_name = "ExitBYBtn";
const char* const k_exit_sequence = "MF_Exit_Button";
const char* const k_quickplay_sequence = "MF_Quickplay_Button";
const char* const k_light_name = "MFActivityLight";
const char* const k_planet_sound = "PLANET_BUTTON";
const char* const k_galactic_sound = "GALACTIC_BUTTON";
const char* const k_join_sound = "JOINGAME";
const char* const k_message_sound = "MESSAGE";
const char* const k_promoted_sound = "PROMOTED";

// The news ticker: its label under the exit button in the small font, the
// text it starts with, the spacer that follows a text short enough for it
// and the characters of the text the label is given at each step, the last
// replaced by the terminator.
constexpr int32_t k_ticker_x = 0x2d;
constexpr int32_t k_ticker_y = 0x5f;
constexpr int32_t k_ticker_width = 0x7c;
constexpr int32_t k_ticker_alignment = 4;
const char* const k_ticker_welcome = "    Welcome to the Boneyards ...                   ";
const char* const k_ticker_spacer = "    ";
constexpr std::size_t k_ticker_spacer_limit = 0xfb;
constexpr int32_t k_ticker_window = 100;

// A page's user_id with this bit set is a guest's.
constexpr uint32_t k_guest_user_bit = 0x80000000U;

struct ConsoleSound {
    const char* category;
    const char* file; // sounds/ WAV name
};

// Sound names the Boneyards windows play that allsound.tdf does not define.
constexpr ConsoleSound k_console_sounds[] = {
    {"PROMOTED", "cdoggy"},
    {"GALACTIC_BUTTON", "button4"},
    {"PLANET_BUTTON", "butmain"},
    {"GREEN_BUTTON", "beep2"},
    {"INFO_PANE_OPEN", "button11"},
    {"INFO_PANE_CLOSE", "cancel1"},
    {"GREEN_SLIDE", "hoverok1"},
    {"MESSAGE", "hovsmsl1"},
};

struct PaneSpec {
    const char* title;
    InfoPane* (*build)(MainFrame* frame, const char* title);
};

// Title and builder of each pane kind, in PaneKind order.
constexpr PaneSpec k_panes[k_pane_kind_count] = {
    {"HOST BATTLE", host_battle_pane_create},
    {"BATTLE INTELLIGENCE", join_summary_pane_create},
    {"BATTLE INTELLIGENCE", scoreboard_pane_create},
    {"", player_pane_create},
    {"CREATE FREQUENCY", create_frequency_pane_create},
    {"CHANGE FREQUENCY", frequency_info_pane_create},
};

struct ConsoleButtonSpec {
    const char* sequence;
    int32_t x, y, width, height;
};

// Main_Console.Gaf sequence and bounds of each console button, in
// ConsoleButton order.
constexpr ConsoleButtonSpec k_console_buttons[k_console_button_count] = {
    {"MF_Release_Button", 8, 404, 15, 43},
    {"MF_Recon_Button", 525, 188, 98, 20},
    {"MF_Chat_Button", 525, 212, 98, 20},
    {"MF_Battle_Button", 525, 236, 98, 20},
    {"MF_News_Button", 538, 270, 86, 20},
    {"MF_Ladders_Button", 538, 306, 86, 20},
    {"MF_Locator_Button", 538, 341, 86, 20},
    {"MF_Profile_Button", 538, 378, 86, 20},
    {"MF_Help_Button", 538, 414, 86, 20},
};

MainFrame* as_frame(Window* window) {
    return reinterpret_cast<MainFrame*>(window);
}

Window* button(const MainFrame* frame, ConsoleButton which) {
    return frame->console_buttons[static_cast<int32_t>(which)];
}

void play_sound(const MainFrame* frame, const char* name) {
    const MainMenuHost& host = frame->owner->config.host;
    if (host.play_sound != nullptr)
        host.play_sound(host.user, name);
}

// ConsoleSystem.chat_console_pending: a battle launched from the Boneyards is running.
bool battle_launched(const MainFrame* frame) {
    return frame->owner->system.chat_console_pending != 0;
}

void battle_message(const MainFrame* frame, const char* text) {
    const MainMenuHost& host = frame->owner->config.host;
    if (host.battle_message != nullptr)
        host.battle_message(host.user, text);
}

/// Sets the ticker's text, with the spacer after it when it is short enough.
///
/// The text is cut to the ticker field on a character boundary. A result
/// shorter than 0xFB bytes keeps the spacer.
///
/// @param[in,out] frame the Main Frame whose ticker changes
/// @param text the new ticker text
void set_ticker(MainFrame* frame, const char* text) {
    oa::boneyards::utf8::copy(frame->ticker, sizeof frame->ticker, text, 0);
    if (std::strlen(frame->ticker) < k_ticker_spacer_limit)
        std::snprintf(
            frame->ticker + std::strlen(frame->ticker),
            sizeof frame->ticker - std::strlen(frame->ticker),
            "%s",
            k_ticker_spacer
        );
    boneyards_console_driver_note("ticker", frame->ticker);
}

/// Scrolls the news ticker one character every sixth pulse.
///
/// The ticker label gets the next 99 characters of the ticker's text, read
/// around the text but for its last character, one character further on
/// than the last time.
///
/// @param[in,out] frame Main Frame
/// @param label ticker label; null skips the label but still scrolls
void main_frame_tick_ticker(MainFrame* frame, Window* label) {
    ConsoleSystem& system = frame->owner->system;
    if (system.ticker_countdown != 0) {
        --system.ticker_countdown;
        return;
    }
    system.ticker_countdown = k_ticker_period;
    const auto span = static_cast<int32_t>(std::strlen(frame->ticker)) - 1;
    char shown[k_ticker_window]{};
    for (int32_t i = 0; i < k_ticker_window - 1 && span > 0; ++i)
        shown[i] = frame->ticker[(i + system.ticker_offset) % span];
    if (label != nullptr)
        screens::lobby_label_set_text(label, shown);
    ++system.ticker_offset;
}

ConsoleNavButton* as_nav_button(Window* window) {
    return reinterpret_cast<ConsoleNavButton*>(window);
}

/// Releases the lit and normal faces, then destroys the button.
///
/// @param self the console button
void console_button_destroy(Window* self) {
    ConsoleNavButton* button = as_nav_button(self);
    image_release(button->lit);
    image_release(button->normal);
    k_button_ops.destroy(self);
}

const WindowOps* console_button_ops() {
    static const WindowOps ops = [] {
        WindowOps o = k_button_ops;
        o.destroy = console_button_destroy;
        return o;
    }();
    return &ops;
}

/// Creates a Main Frame console navigation button, disabled until the service enables it.
///
/// State 0 shows frame 3, state 1 frame 1 and state 2 the normal face
/// (frame 2); frame 0 is kept as the lit face its open console shows.
///
/// @param owner status widget whose image store loads the faces
/// @param[in,out] parent the Main Frame window
/// @param spec Main_Console.Gaf sequence and bounds of the button
/// @return the button
Window* console_button_create(StatusWidget* owner, Window* parent, const ConsoleButtonSpec& spec) {
    ConsoleNavButton* button =
        as_nav_button(console_window_alloc(sizeof(ConsoleNavButton), console_button_ops()));
    Window* w = &button->base;
    // The Button base: three button states, unpressed.
    window_set_state_count(w, button_state::count);
    window_set_state(w, button_state::unpressed);
    window_create(w, spec.sequence, spec.x, spec.y, spec.width, spec.height, parent, true);
    ConsoleImages* images = owner->images;
    const auto load = [&](int32_t frame) {
        return console_images_load(
            images, resource::main_console_gaf, spec.sequence, frame, resource::console_key
        );
    };
    const auto show = [&](int32_t state, int32_t frame) {
        const Handle image = load(frame);
        window_set_state_image(w, state, image);
        image_release(image);
    };
    show(button_state::disabled, k_button_frame_disabled);
    show(button_state::pressed, k_button_frame_pressed);
    button->lit = load(k_button_frame_lit);
    button->normal = load(k_button_frame_normal);
    window_set_state_image(w, button_state::unpressed, button->normal);
    window_set_state(w, button_state::disabled);
    return w;
}

// A console screen in the green layer's rectangle, from the factory or the
// built-in views.
Window* create_screen(MainFrame* frame, int32_t screen) {
    const Window& console = frame->console->base;
    const ConsoleScreenFactory& factory = frame->owner->config.screens;
    if (factory.create != nullptr) {
        return factory.create(
            factory.user, screen, &frame->base, console.x, console.y, console.width, console.height
        );
    }
    if (screen == console_screen::quickplay) {
        return quickplay_console_create(
            frame->owner, &frame->base, console.x, console.y, console.width, console.height
        );
    }
    if (screen == console_screen::locator) {
        return locator_console_create(
            frame->owner, &frame->base, console.x, console.y, console.width, console.height
        );
    }
    const ConsoleScreenSlots slots = console_screen_slots(screen);
    frame->screen_open = slots.open;
    frame->screen_close = slots.close;
    return console_create_screen_view(
        frame->owner, screen, &frame->base, console.x, console.y, console.width, console.height
    );
}

/// True for a console screen that keeps its window between openings.
///
/// The login console is built every time it opens.
///
/// @param screen console_screen id
/// @return true when a closed screen of that id is shown again
bool screen_is_kept(int32_t screen) {
    return screen >= 0 && screen < console_screen::none && screen != console_screen::login;
}

/// Returns the window kept for `screen`.
///
/// @param frame Main Frame
/// @param screen console_screen id
/// @return the kept window, or null when that screen is built each time or has not been built
Window* kept_screen(const MainFrame* frame, int32_t screen) {
    if (!screen_is_kept(screen))
        return nullptr;
    return frame->kept_screens[screen];
}

/// Remembers `window` as the console `screen` shows the next time.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
/// @param window the console, or null to forget it
void remember_screen(MainFrame* frame, int32_t screen, Window* window) {
    if (screen_is_kept(screen))
        frame->kept_screens[screen] = window;
}

/// Hides `window` and destroys it on the next cycle.
///
/// @param[in,out] window window to drop; null does nothing
void discard_screen(Window* window) {
    if (window == nullptr)
        return;
    window->visible = 0;
    window->owned = 1;
}

/// Installs the screen's open and close slots.
///
/// A screen from the factory has none: the factory does not name them.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
void install_screen_slots(MainFrame* frame, int32_t screen) {
    if (frame->owner->config.screens.create != nullptr) {
        frame->screen_open = nullptr;
        frame->screen_close = nullptr;
        return;
    }
    const ConsoleScreenSlots slots = console_screen_slots(screen);
    frame->screen_open = slots.open;
    frame->screen_close = slots.close;
}

/// Returns the console `screen` shows: the one kept from last time, or a new one.
///
/// The update console's auto-update state starts only when that console is
/// first built.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
/// @return the console window, or null when it cannot be built
Window* acquire_screen(MainFrame* frame, int32_t screen) {
    if (Window* kept = kept_screen(frame, screen)) {
        install_screen_slots(frame, screen);
        return kept;
    }
    if (screen == console_screen::update)
        maia_update_init(&frame->update_console);
    Window* created = create_screen(frame, screen);
    remember_screen(frame, screen, created);
    return created;
}

/// Ends a QuickPlay search kept on this frame.
///
/// Called when that console is hidden and when the Boneyards session ends, so
/// the cancel is sent while the session can still carry it. A search that
/// already stopped is left as it is.
///
/// @param frame Main Frame; null does nothing
void end_quickplay_search(MainFrame* frame) {
    if (frame == nullptr)
        return;
    Window* screen = kept_screen(frame, console_screen::quickplay);
    if (screen == nullptr && frame->console_screen == console_screen::quickplay)
        screen = frame->active_screen;
    quickplay_console_end_search(screen);
}

/// Closes the active screen: its own close slot runs where it has one, then
/// the screen is hidden. Every console but the login console is kept, so
/// the next opening shows the same one; the login console is destroyed on
/// the next cycle. Hiding QuickPlay ends a running search, so the player is
/// not matched to a battle while looking at another console.
///
/// @param[in,out] frame the Main Frame whose active screen closes
void close_active_screen(MainFrame* frame) {
    Window* screen = frame->active_screen;
    if (screen == nullptr)
        return;
    if (frame->console_screen == console_screen::quickplay)
        end_quickplay_search(frame);
    if (frame->screen_close != nullptr)
        frame->screen_close(screen);
    screen->visible = 0;
    Desktop* desktop = services().desktop;
    Window* back = screen->focus_return;
    const bool hidden_focus =
        desktop != nullptr && desktop->focus != nullptr && !window_is_shown(desktop->focus);
    if (frame->console_screen == console_screen::login)
        discard_screen(screen);
    else
        remember_screen(frame, frame->console_screen, screen);
    if (hidden_focus) {
        if (desktop_holds(desktop, back) && window_is_shown(back))
            services_desktop_focus(back);
        else
            services_desktop_focus(nullptr);
    }
    frame->active_screen = nullptr;
    frame->screen_open = nullptr;
    frame->screen_close = nullptr;
}

// The screens' open slot: the screen's own open slot where it has one,
// otherwise the screen is shown; the update console starts its auto-update
// step.
void open_active_screen(MainFrame* frame) {
    Window* screen = frame->active_screen;
    if (screen == nullptr)
        return;
    if (frame->screen_open != nullptr)
        frame->screen_open(screen);
    else
        screen->visible = 1;
    if (frame->console_screen == console_screen::update) {
        screens::update_pass_reset(&frame->update_pass);
        maia_update_open(&frame->update_console);
    }
}

/// Shows `screen` at once, without a slide.
///
/// The open console closes first unless it is already `screen`. A launch
/// leaves the battle console active and hidden, so the return opens that
/// same console in place.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
void show_screen_now(MainFrame* frame, int32_t screen) {
    const Window* kept = kept_screen(frame, screen);
    const bool current = frame->active_screen != nullptr && frame->console_screen == screen &&
                         (kept == nullptr || frame->active_screen == kept);
    if (!current) {
        close_active_screen(frame);
        frame->active_screen = acquire_screen(frame, screen);
        frame->console_screen = screen;
        if (frame->active_screen != nullptr)
            frame->active_screen->visible = 0;
    }
    if (Window* shown = frame->active_screen) {
        shown->ops->raise(shown);
        open_active_screen(frame);
    }
}

/// Puts every console button, Quickplay included, in its disabled state.
///
/// @param[in,out] frame Main Frame
void main_frame_disable_buttons(MainFrame* frame) {
    window_set_state(button(frame, ConsoleButton::release), button_state::disabled);
    window_set_state(frame->quickplay_button, button_state::disabled);
    for (int32_t i = static_cast<int32_t>(ConsoleButton::recon); i < k_console_button_count; ++i)
        window_set_state(frame->console_buttons[i], button_state::disabled);
}

/// Starts covering the map with the green layer.
///
/// Sounds GREEN_SLIDE, disables the console buttons and locks the map until
/// the slide ends.
///
/// @param[in,out] frame Main Frame
void main_frame_begin_slide(MainFrame* frame) {
    play_sound(frame, console_sound::green_slide);
    main_frame_disable_buttons(frame);
    main_frame_lock_map(frame);
    console_layer_start(frame->console);
}

/// Opens console screen `screen` in the green layer's rectangle, closing the open one.
///
/// A console already built for `screen` is shown again, with what it held.
/// The login console is built every time. The screen starts hidden and is
/// shown once the green covers the map: at once when it already does,
/// otherwise when the slide it starts ends. The screen's id is recorded for
/// every screen, where 3.1c leaves it unchanged for the login console.
///
/// @param[in,out] frame Main Frame
/// @param screen console_screen id
void main_frame_switch_screen(MainFrame* frame, int32_t screen) {
    close_active_screen(frame);
    frame->active_screen = acquire_screen(frame, screen);
    if (frame->active_screen != nullptr)
        frame->active_screen->visible = 0;
    frame->console_screen = screen;
    if (frame->console->covering == 0)
        main_frame_begin_slide(frame);
    else
        main_frame_present_screen(frame, false);
    if (screen == console_screen::login) {
        const screens::SettingsStore& settings = frame->owner->config.settings;
        int32_t beta = 0;
        if (settings.read_number != nullptr)
            beta = settings.read_number(settings.user, kBetaLoginUxKey, 0);
        screens::login_set_registration_access(&frame->owner->navigation->login, beta);
    }
}

/// Replaces the update console with the login console.
///
/// @param[in,out] frame Main Frame
void main_frame_open_login(MainFrame* frame) {
    main_frame_switch_screen(frame, console_screen::login);
}

void main_frame_toggle(MainFrame* frame, const char* sound, int32_t screen) {
    play_sound(frame, sound);
    if (frame->console_screen != screen)
        main_frame_switch_screen(frame, screen);
    else
        main_frame_close_screen(frame);
}

/// Handles the magnification slider and the Main Frame buttons.
///
/// The slider sets the map's zoom level. The buttons: exit, QuickPlay and
/// Locator (each opens its console, or closes it when that console is
/// already open, for every account), release (which
/// leaves the Boneyards while the login console is open and otherwise
/// closes the open console screen) and the console screen toggles.
///
/// @param self the Main Frame
/// @param notify control notice
void main_frame_on_notify(Window* self, const Notify* notify) {
    MainFrame* frame = as_frame(self);
    const Window* source = notify->source;
    if (source == &frame->mag_slider->base) {
        screens::galaxy_set_scale(
            &frame->owner->navigation->galaxy, slider_index(frame->mag_slider)
        );
        galaxy_panel_sync_hotspots(frame);
        return;
    }
    if (source == frame->exit_button) {
        end_quickplay_search(frame);
        console_reset_to_main_menu(frame->owner);
        return;
    }
    if (source == button(frame, ConsoleButton::release)) {
        if (frame->console_screen == console_screen::login) {
            end_quickplay_search(frame);
            console_reset_to_main_menu(frame->owner);
        } else
            main_frame_close_screen(frame);
        return;
    }
    if (source == frame->quickplay_button || source == button(frame, ConsoleButton::locator)) {
        const bool quickplay = source == frame->quickplay_button;
        const int32_t screen = quickplay ? console_screen::quickplay : console_screen::locator;
        main_frame_toggle(frame, quickplay ? k_join_sound : k_galactic_sound, screen);
        return;
    }

    struct Toggle {
        ConsoleButton which;
        const char* sound;
        int32_t screen;
    };

    static const Toggle toggles[] = {
        {ConsoleButton::recon, k_planet_sound, console_screen::recon},
        {ConsoleButton::chat, k_planet_sound, console_screen::chat},
        {ConsoleButton::battle, k_planet_sound, console_screen::battle},
        {ConsoleButton::news, k_galactic_sound, console_screen::news},
        {ConsoleButton::ladders, k_galactic_sound, console_screen::ladders},
        {ConsoleButton::profile, k_galactic_sound, console_screen::profile},
        {ConsoleButton::help, k_galactic_sound, console_screen::help},
    };
    for (const Toggle& toggle : toggles) {
        if (source == button(frame, toggle.which)) {
            main_frame_toggle(frame, toggle.sound, toggle.screen);
            return;
        }
    }
}

/// Makes Escape act as the release button while it is enabled.
///
/// @param self the Main Frame
/// @param msg key message
/// @return true when the key acted as the release button
bool main_frame_key_down(Window* self, const Message* msg) {
    MainFrame* frame = as_frame(self);
    Window* release = button(frame, ConsoleButton::release);
    if (msg->key != key::escape || release->states.current == button_state::disabled)
        return false;
    Notify notify{};
    notify.code = NotifyCode::activated;
    notify.source = release;
    self->ops->on_notify(self, &notify);
    return true;
}

/// Opens a notice the session left for this cycle.
///
/// Ok on the box asks the following cycle to leave for the main menu. A
/// notice that is already open, or none, is left as it is. The text has to
/// stay alive until this cycle copies it into the box.
///
/// @param[in,out] frame Main Frame
void main_frame_open_pending_notice(MainFrame* frame) {
    const char* text = frame->pending_notice;
    if (text == nullptr || text == k_pending_notice_shown)
        return;
    frame->notice_box = message_box_create(
        text, message_box_buttons::ok, &frame->return_requested, nullptr, nullptr
    );
    frame->pending_notice = k_pending_notice_shown;
}

/// Runs the Main Frame's per-cycle upkeep while shown.
///
/// Leaves for the main menu when the open notice was dismissed, then opens
/// a notice the session left for this cycle. Opens the update console once
/// the client is active, steps the war-map animation and the news ticker,
/// unlocks the map (enabling Recon, Chat and Battle for a selected planet)
/// once the galaxy data has loaded, and while no unlock is pending asks for
/// the current area's census every 0x1C2 cycles as a keep-alive. The shown
/// update console steps its session stand-in here; the login console opens
/// once the placeholder pass has played.
///
/// @param self the Main Frame
void main_frame_update(Window* self) {
    if (!window_is_shown(self))
        return;
    MainFrame* frame = as_frame(self);
    StatusWidget* owner = frame->owner;
    if (frame->return_requested) {
        frame->return_requested = false;
        frame->pending_notice = nullptr;
        end_quickplay_search(frame);
        console_reset_to_main_menu(owner);
        return;
    }
    main_frame_open_pending_notice(frame);
    // One frame of the placeholder pass per clock tick while the console is
    // shown. The cycle that finishes the pass leaves it up so the last frame
    // can draw; the next cycle opens the login console. The session stand-in
    // completes with nothing to fetch.
    if (frame->console_screen == console_screen::update && frame->active_screen != nullptr &&
        window_is_shown(frame->active_screen)) {
        (void)maia_update_idle(&frame->update_console);
        StatusWidget::UpdateDrive& real_drive = owner->update_drive;
        if (real_drive.real) {
            // Leave the finished picture up for one cycle, so its last line
            // can draw, then open the login console. A stop stays here.
            const bool go_on = real_drive.finished && !real_drive.stopped &&
                               frame->update_console.start_result == kMaiaStartOk &&
                               frame->update_console.completed != 0;
            if (!go_on)
                real_drive.hold = false;
            else if (!real_drive.hold)
                real_drive.hold = true;
            else
                main_frame_open_login(frame);
        } else if (!frame->update_pass.finished)
            screens::update_pass_step(&frame->update_pass);
        else if (
            frame->update_console.start_result == kMaiaStartOk &&
            frame->update_console.completed != 0
        )
            main_frame_open_login(frame);
    }
    if (owner->system.connection_level == 1 && frame->login_countdown-- == 0)
        main_frame_switch_screen(frame, console_screen::update);
    screens::BoneyardsScreens* nav = owner->navigation;
    const GalaxyGraphics* graphics = owner->galaxy_graphics.get();
    screens::galaxy_step_animation(
        &nav->galaxy, graphics != nullptr ? graphics->map_static_frames : 0
    );
    main_frame_tick_ticker(frame, frame->ticker_label);
    if (frame->unlock_pending != 0) {
        if (nav->galaxy.loading)
            return;
        if (nav->galaxy.selected != screens::kNoPlanet) {
            for (ConsoleButton which :
                 {ConsoleButton::recon, ConsoleButton::chat, ConsoleButton::battle})
                window_set_state(button(frame, which), button_state::unpressed);
        }
        frame->unlock_pending = 0;
        main_frame_unlock_map(frame);
    }
    if (--frame->keepalive_countdown < 0) {
        frame->keepalive_countdown = k_keepalive_period;
        main_frame_request_census(frame);
    }
}

/// Draws the Main Frame.
///
/// Paints the planet summary's picture into the background and refreshes
/// its labels, then draws the frame and its children. QuickPlay and Locator
/// are drawn uncovered for every account.
///
/// @param self the Main Frame
/// @return true always
bool main_frame_draw(Window* self) {
    MainFrame* frame = as_frame(self);
    if (frame->battle_summary != nullptr) {
        battle_summary_draw(frame->battle_summary);
        battle_summary_update(frame->battle_summary);
    }
    window_draw_background(self);
    return true;
}

/// Sounds the end of a slide of the green layer: every playing sound stops, BIGBUTTON plays and,
/// while the background sound is on, BGM drones on the alternate route again.
///
/// @param frame Main Frame
void sound_slide_end(const MainFrame* frame) {
    console_stop_sounds(frame->owner);
    play_sound(frame, console_sound::big_button);
    console_play_background(frame->owner);
}

/// Adds the Boneyards sound names to the named-sound cache.
///
/// The Main Frame build does this before the welcome alert, so the ticker's
/// MESSAGE chime is a named sound rather than a missing sounds/message.wav.
///
/// @param frame Main Frame
void main_frame_register_sounds(const MainFrame* frame) {
    boneyards_status_widget_register_sounds(frame->owner);
}

/// Builds the Main Frame tree when its bounds are set.
///
/// Adds the background, exit and Quickplay buttons, the console buttons, the
/// Mag_Slider, the activity light, the advert banner, the news ticker's
/// label (the ticker starts with its welcome through the alert handler), the
/// map and the planet summary; the map starts locked. The Boneyards sounds
/// are named before that welcome, so its chime is already a named sound.
/// Then every playing sound stops and, while the background sound is on,
/// BGM drones on the alternate route.
///
/// @param self the Main Frame
void main_frame_build(Window* self) {
    MainFrame* frame = as_frame(self);
    StatusWidget* owner = frame->owner;
    ConsoleImages* images = owner->images;
    const Handle background = console_images_load(
        images, resource::main_console_gaf, k_frame_sequence, 0, resource::console_key
    );
    window_set_state_image(self, 1, background);
    image_release(background);

    frame->exit_button = button_create();
    window_create(
        frame->exit_button,
        k_exit_name,
        layout::exit_x,
        layout::exit_y,
        layout::exit_width,
        layout::exit_height,
        self,
        true
    );
    console_bind_images(
        frame->exit_button, resource::main_console_gaf, k_exit_sequence, 0, resource::console_key
    );

    frame->quickplay_button = button_create();
    window_create(
        frame->quickplay_button,
        k_quickplay_sequence,
        layout::quickplay_x,
        layout::quickplay_y,
        layout::quickplay_width,
        layout::quickplay_height,
        self,
        true
    );
    console_bind_images(
        frame->quickplay_button,
        resource::main_console_gaf,
        k_quickplay_sequence,
        0,
        resource::console_key
    );

    for (int32_t i = 0; i < k_console_button_count; ++i)
        frame->console_buttons[i] = console_button_create(owner, self, k_console_buttons[i]);
    frame->mag_slider = main_frame_create_mag_slider(frame);

    auto* light = reinterpret_cast<ActivityLight*>(
        console_window_alloc(sizeof(ActivityLight), main_menu_activity_light_ops())
    );
    frame->activity_light = light;
    window_create(
        &light->base,
        k_light_name,
        layout::light_x,
        layout::light_y,
        layout::light_width,
        layout::light_height,
        self,
        true
    );

    frame->ad_banner = make_ad_banner(frame, images, self);
    frame->ticker_label = screens::lobby_label_create(
        self,
        k_ticker_x,
        k_ticker_y,
        "",
        owner->system.hat_fonts.small,
        k_ticker_alignment,
        0,
        k_ticker_width
    );
    window_set_name(frame->ticker_label, "Ticker");
    // Name MESSAGE (and the other console sounds) before the welcome plays it.
    // Nothing else in this build plays a console sound: the map, summary and
    // lock are silent, and BGM on the alternate route is not one of them.
    main_frame_register_sounds(frame);
    const AlertReport welcome{static_cast<int32_t>(AlertKind::ticker), k_ticker_welcome, 0, ""};
    main_frame_on_alert(frame, &welcome);
    main_frame_create_map(frame);
    frame->battle_summary = battle_summary_create(frame);
    if (frame->battle_summary != nullptr)
        battle_summary_update(frame->battle_summary);
    main_frame_lock_map(frame);
    console_stop_sounds(owner);
    console_play_background(owner);
}

// Destroys the panes the frame keeps, wherever a console has put them,
// before its children go.
void main_frame_release_panes(Window* self) {
    for (InfoPane*& pane : as_frame(self)->panes) {
        if (pane != nullptr)
            window_destroy(&pane->base);
        pane = nullptr;
    }
}

// Frees the planet summary, then the frame.
void main_frame_destroy(Window* self) {
    MainFrame* frame = as_frame(self);
    if (frame->battle_summary != nullptr) {
        battle_summary_release(frame->battle_summary);
        std::free(frame->battle_summary);
        frame->battle_summary = nullptr;
    }
    console_window_free(self);
}

WindowOps make_frame_ops() {
    WindowOps ops = console_panel_ops();
    ops.destroy = main_frame_destroy;
    ops.on_destroy = main_frame_release_panes;
    ops.on_bounds = main_frame_build;
    ops.pulse = main_frame_update;
    ops.on_notify = main_frame_on_notify;
    ops.draw = main_frame_draw;
    ops.key_down = main_frame_key_down;
    return ops;
}

const WindowOps k_frame_ops = make_frame_ops();

} // namespace

void main_frame_show_screen(MainFrame* frame, int32_t screen) {
    main_frame_switch_screen(frame, screen);
}

void boneyards_status_widget_register_sounds(const StatusWidget* widget) {
    if (widget == nullptr)
        return;
    const MainMenuHost& host = widget->config.host;
    if (host.register_sound == nullptr)
        return;
    for (const ConsoleSound& sound : k_console_sounds)
        host.register_sound(host.user, sound.category, sound.file);
}

void main_frame_request_census(MainFrame* frame) {
    const screens::BoneyardsScreens* nav = frame->owner->navigation;
    ++frame->census_requests;
    nav->service.request_census(nav->service.user, nav->session.area_id);
}

void main_frame_on_alert(MainFrame* frame, const AlertReport* alert) {
    const char* text = alert->text != nullptr ? alert->text : "";
    switch (static_cast<AlertKind>(alert->kind)) {
    case AlertKind::ticker:
        play_sound(frame, k_message_sound);
        if (battle_launched(frame))
            battle_message(frame, text);
        else
            set_ticker(frame, text);
        return;
    case AlertKind::message_box:
        play_sound(frame, k_message_sound);
        if (battle_launched(frame)) {
            battle_message(frame, text);
            return;
        }
        if (frame->notice_box != nullptr) {
            if (desktop_holds(services().desktop, &frame->notice_box->base))
                frame->notice_box->base.owned = 1;
            frame->notice_box = nullptr;
        }
        frame->notice_box =
            message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
        return;
    case AlertKind::page: {
        if ((alert->sender_id & k_guest_user_bit) != 0)
            return;
        screens::BoneyardsScreens* nav = frame->owner->navigation;
        const BoneyardsService& service = nav->service;
        if (service.peoplelist_contains(service.user, kSilencedList, alert->sender_id))
            return;
        boneyards_console_driver_note("page", text);
        play_sound(frame, k_message_sound);
        if (battle_launched(frame)) {
            battle_message(frame, text);
            return;
        }
        const ChatView* chat = console_open_chat();
        if (chat != nullptr && window_is_shown(&chat->base)) {
            screens::chat_print_line(&nav->chat, text, screens::kDefaultTextColor, false);
            return;
        }
        message_box_create(text, message_box_buttons::ok, nullptr, nullptr, nullptr);
        return;
    }
    case AlertKind::notification:
        if (std::strcmp(
                alert->notification_type != nullptr ? alert->notification_type : "",
                kPromotionNotification
            ) != 0)
            return;
        play_sound(frame, k_promoted_sound);
        if (battle_launched(frame))
            battle_message(frame, text);
        else
            set_ticker(frame, text);
        return;
    }
}

InfoPane*
main_frame_pane(MainFrame* frame, PaneKind kind, Window* parent, const InfoPaneLayout& layout) {
    const auto index = static_cast<int32_t>(kind);
    if (index < 0 || index >= k_pane_kind_count)
        return nullptr;
    InfoPane*& pane = frame->panes[index];
    const PaneSpec& spec = k_panes[index];
    if (pane == nullptr && spec.build != nullptr)
        pane = spec.build(frame, spec.title);
    if (pane == nullptr)
        return nullptr;
    info_pane_set_layout(pane, layout);
    window_set_parent(&pane->base, parent);
    return pane;
}

void main_frame_present_screen(MainFrame* frame, bool play_sound_effect) {
    if (play_sound_effect)
        sound_slide_end(frame);
    if (Window* screen = frame->active_screen) {
        screen->ops->raise(screen);
        open_active_screen(frame);
    }
    if (frame->console_screen != console_screen::update &&
        frame->console_screen != console_screen::login)
        main_frame_enable_buttons(frame);
}

void main_frame_release_map(MainFrame* frame, bool play_sound_effect) {
    if (play_sound_effect)
        sound_slide_end(frame);
    main_frame_enable_buttons(frame);
    window_set_state(button(frame, ConsoleButton::release), button_state::disabled);
    if (frame->owner->navigation->galaxy.loading) {
        frame->unlock_pending = 1;
        return;
    }
    main_frame_unlock_map(frame);
    frame->unlock_pending = 0;
}

void main_frame_close_screen(MainFrame* frame) {
    play_sound(frame, console_sound::green_slide);
    close_active_screen(frame);
    frame->console_screen = console_screen::none;
    main_frame_disable_buttons(frame);
    Window* release = button(frame, ConsoleButton::release);
    window_set_state_image(release, button_state::unpressed, as_nav_button(release)->normal);
    console_layer_reverse(frame->console);
}

void main_frame_reopen_chat(MainFrame* frame) {
    console_stop_sounds(frame->owner);
    console_play_background(frame->owner);
    screens::ChatSession& session = frame->owner->navigation->session;
    if (session.debrief_room_id == 0) {
        show_screen_now(frame, console_screen::battle);
        return;
    }
    session.area_id = session.debrief_area_id;
    show_screen_now(frame, console_screen::chat);
}

void main_frame_enable_buttons(MainFrame* frame) {
    const screens::GalaxyView& galaxy = frame->owner->navigation->galaxy;
    for (ConsoleButton which :
         {ConsoleButton::release,
          ConsoleButton::recon,
          ConsoleButton::chat,
          ConsoleButton::battle,
          ConsoleButton::news,
          ConsoleButton::ladders,
          ConsoleButton::help,
          ConsoleButton::profile}) {
        window_set_state(button(frame, which), button_state::unpressed);
    }
    window_set_state(frame->quickplay_button, button_state::unpressed);
    window_set_state(button(frame, ConsoleButton::locator), button_state::unpressed);
    if (galaxy.loading || galaxy.selected == screens::kNoPlanet) {
        window_set_state(button(frame, ConsoleButton::recon), button_state::disabled);
        window_set_state(button(frame, ConsoleButton::chat), button_state::disabled);
        window_set_state(button(frame, ConsoleButton::battle), button_state::disabled);
    }
}

MainFrame* main_frame_create(StatusWidget* owner) {
    auto* frame =
        reinterpret_cast<MainFrame*>(console_window_alloc(sizeof(MainFrame), &k_frame_ops));
    frame->console_screen = console_screen::none;
    frame->login_countdown = k_login_delay;
    frame->keepalive_countdown = k_keepalive_period;
    frame->owner = owner;
    window_create(&frame->base, k_frame_name, 0, 0, 640, 480, nullptr, false);
    return frame;
}

} // namespace oa::boneyards::console
