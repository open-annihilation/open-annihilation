// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen-reader bridge at the game's frame. The preference decides
// whether the adapter is created. The control tree is built only while the
// system says a screen reader is running and one has asked for the tree.
// An unchanged tree is not sent. An action becomes the key or press the
// game already handles.

#include "oa/app/runtime.hpp"

#include "oa/platform/screen_reader.hpp"
#include "oa/platform/screen_reader/speech.hpp"

#include <SDL3/SDL.h>

#include <string>
#include <string_view>
#include <utility>

#if defined(OA_SCREEN_READER_COMPILED_OUT) && defined(_WIN32)
#include "oa/app/automation/view.hpp"
#include "oa/app/check_host.hpp"

#include <string>
#endif

#if !defined(OA_SCREEN_READER_COMPILED_OUT)

#include "device_state.hpp"
#include "input.hpp"
#include "oa/app/automation/view.hpp"
#include "oa/app/screen_reader_tree.hpp"

#include <cstdint>
#include <deque>
#include <vector>

namespace {

namespace bridge = oa::platform::screen_reader;

std::deque<std::string> typed_text;

/// The scancode of a bridge key.
///
/// @param key the key
/// @return the scancode
SDL_Scancode scancode_of(bridge::Key key) {
    switch (key) {
    case bridge::Key::left:
        return SDL_SCANCODE_LEFT;
    case bridge::Key::right:
        return SDL_SCANCODE_RIGHT;
    }
    return SDL_SCANCODE_UNKNOWN;
}

/// Pushes one key down and up.
///
/// @param window_id the window
/// @param scancode the key
/// @param repeats how many taps
void push_key(SDL_WindowID window_id, SDL_Scancode scancode, int32_t repeats) {
    if (scancode == SDL_SCANCODE_UNKNOWN || repeats < 1)
        return;
    for (int32_t tap = 0; tap < repeats; ++tap) {
        oa::app::device_state::hold_key(scancode, true);
        SDL_Event down{};
        down.type = SDL_EVENT_KEY_DOWN;
        down.key.timestamp = SDL_GetTicksNS();
        down.key.windowID = window_id;
        down.key.scancode = scancode;
        down.key.key = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        down.key.mod = oa::app::device_state::modifiers_held();
        down.key.down = true;
        down.key.repeat = false;
        SDL_PushEvent(&down);
        oa::app::device_state::hold_key(scancode, false);
        SDL_Event up = down;
        up.type = SDL_EVENT_KEY_UP;
        up.key.timestamp = SDL_GetTicksNS();
        up.key.down = false;
        up.key.repeat = false;
        up.key.mod = oa::app::device_state::modifiers_held();
        SDL_PushEvent(&up);
    }
}

/// Moves the pointer, and presses when asked.
///
/// @param window_id the window
/// @param x the column, in window points
/// @param y the row, in window points
/// @param press true to click
void push_pointer(SDL_WindowID window_id, float x, float y, bool press) {
    SDL_Event motion{};
    motion.type = SDL_EVENT_MOUSE_MOTION;
    motion.motion.timestamp = SDL_GetTicksNS();
    motion.motion.windowID = window_id;
    motion.motion.x = x;
    motion.motion.y = y;
    if (press)
        oa::app::device_state::hold_buttons(SDL_BUTTON_MASK(SDL_BUTTON_LEFT));
    motion.motion.state = oa::app::device_state::buttons_held();
    SDL_PushEvent(&motion);
    if (!press)
        return;
    SDL_Event down{};
    down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    down.button.timestamp = SDL_GetTicksNS();
    down.button.windowID = window_id;
    down.button.button = SDL_BUTTON_LEFT;
    down.button.down = true;
    down.button.clicks = 1;
    down.button.x = x;
    down.button.y = y;
    SDL_PushEvent(&down);
    oa::app::device_state::hold_buttons(0);
    SDL_Event up = down;
    up.type = SDL_EVENT_MOUSE_BUTTON_UP;
    up.button.timestamp = SDL_GetTicksNS();
    up.button.down = false;
    SDL_PushEvent(&up);
}

/// Pushes typed text. The characters stay until the next frame's actions.
///
/// @param window_id the window
/// @param text the characters
void push_text(SDL_WindowID window_id, const std::string& text) {
    if (text.empty())
        return;
    typed_text.push_back(text);
    SDL_Event typed{};
    typed.type = SDL_EVENT_TEXT_INPUT;
    typed.text.timestamp = SDL_GetTicksNS();
    typed.text.windowID = window_id;
    typed.text.text = typed_text.back().c_str();
    SDL_PushEvent(&typed);
}

/// Pushes one bridge event onto SDL's queue. Text replaces a field: the
/// keys delete what it holds, then the new text is typed.
///
/// @param window_id the window
/// @param event the event
void push_event(SDL_WindowID window_id, const bridge::Event& event) {
    switch (event.kind) {
    case bridge::EventKind::press:
        push_pointer(window_id, static_cast<float>(event.x), static_cast<float>(event.y), true);
        break;
    case bridge::EventKind::motion:
        push_pointer(window_id, static_cast<float>(event.x), static_cast<float>(event.y), false);
        break;
    case bridge::EventKind::key:
        push_key(window_id, scancode_of(event.key), event.repeats);
        break;
    case bridge::EventKind::text:
        push_key(window_id, SDL_SCANCODE_BACKSPACE, event.clear);
        push_text(window_id, event.text);
        break;
    }
}

/// Speaks the focused control when the bridge cannot run.
///
/// @param nodes the controls a reader would be shown
/// @param focused_id the focused id
void speak_focused_node(
    const std::vector<oa::platform::screen_reader::Node>& nodes, uint64_t focused_id
) {
    const oa::platform::screen_reader::Node* chosen = nullptr;
    for (const oa::platform::screen_reader::Node& node : nodes) {
        if (node.id == focused_id) {
            chosen = &node;
            break;
        }
    }
    if (chosen == nullptr || chosen->role == oa::platform::screen_reader::Role::none) {
        oa::platform::screen_reader::speak_focus({}, {}, {}, {});
        return;
    }
    const auto role =
        static_cast<oa::platform::screen_reader::SpokenRole>(static_cast<unsigned>(chosen->role));
    oa::platform::screen_reader::speak_focus(
        std::to_string(chosen->id),
        chosen->label,
        oa::platform::screen_reader::role_words(role),
        chosen->value_text
    );
}

} // namespace

#endif

#if defined(OA_SCREEN_READER_COMPILED_OUT) && defined(_WIN32)
namespace {

static_assert(
    static_cast<unsigned>(oa::app::AutomationControlRole::none) ==
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::none)
);
static_assert(
    static_cast<unsigned>(oa::app::AutomationControlRole::button) ==
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::button)
);
static_assert(
    static_cast<unsigned>(oa::app::AutomationControlRole::label) ==
    static_cast<unsigned>(oa::platform::screen_reader::SpokenRole::label)
);

/// Speaks the focused control from the listing. A masked field's value is
/// how many characters it holds, never the text.
///
/// @param runtime the running game
void speak_focused_control(oa::app::Runtime& runtime) {
    const oa::app::CheckHost check = oa::app::check_host(runtime);
    const oa::app::AutomationHost host = oa::app::automation_host(runtime);
    const oa::app::automation::ControlListing listing =
        oa::app::automation::listing_of(check, host);
    const oa::app::automation::ListedControl* chosen = nullptr;
    for (const oa::app::automation::ListedControl& item : listing.flat) {
        if (!item.control.focused || !item.control.visible || item.control.hidden)
            continue;
        if (item.control.role == oa::app::AutomationControlRole::none)
            continue;
        chosen = &item;
        break;
    }
    if (chosen == nullptr) {
        oa::platform::screen_reader::speak_focus({}, {}, {}, {});
        return;
    }
    const oa::app::AutomationControl& control = chosen->control;
    std::string value = control.value_text;
    if (control.masked || control.role == oa::app::AutomationControlRole::password)
        value = std::to_string(control.text_length);
    const auto role =
        static_cast<oa::platform::screen_reader::SpokenRole>(static_cast<unsigned>(control.role));
    const std::string identity = control.window + "\n" + control.dialog + "\n" + control.name;
    oa::platform::screen_reader::speak_focus(
        identity, control.label, oa::platform::screen_reader::role_words(role), value
    );
}

} // namespace
#endif

namespace oa::app {

bool screen_reader_hides_window(oa::platform::screen_reader::Preference mode) {
    namespace bridge = oa::platform::screen_reader;
    if (mode == bridge::Preference::off)
        return false;
    return mode == bridge::Preference::on || bridge::reader_running();
}

void attach_screen_reader_and_show(
    SDL_Window* window, oa::platform::screen_reader::Preference mode
) {
    namespace bridge = oa::platform::screen_reader;
    if (window == nullptr || (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) == 0)
        return;
    bridge::set_preference(mode);
    bridge::note_attach_finished(window, bridge::attach(window));
    SDL_ShowWindow(window);
}

SDL_WindowFlags screen_reader_window_flags(SDL_WindowFlags flags, bool hide_until_attached) {
#if defined(_WIN32)
    if (hide_until_attached)
        return flags | SDL_WINDOW_HIDDEN;
#else
    (void)hide_until_attached;
#endif
    return flags;
}

namespace {

/// Shows a window that was created hidden so the bridge could attach first.
///
/// @param window the game's window
void show_if_hidden(SDL_Window* window) {
#if defined(_WIN32)
    if (window != nullptr && (SDL_GetWindowFlags(window) & SDL_WINDOW_HIDDEN) != 0)
        SDL_ShowWindow(window);
#else
    (void)window;
#endif
}

} // namespace

void Runtime::ensure_screen_reader() {
    namespace bridge = oa::platform::screen_reader;
    static const std::string key{bridge::preference_key};
    std::string_view stored;
    const auto found = preference_values_.find(key);
    if (found != preference_values_.end())
        stored = found->second;
    const bridge::Preference mode = bridge::preference_of(stored);
    bridge::set_preference(mode);
    SDL_Window* game_window = sdl_.window;
    if (mode == bridge::Preference::off) {
        show_if_hidden(game_window);
        return;
    }
    if (game_window == nullptr)
        return;
    // Automatic waits until the system says a reader is running. The window
    // is shown either way, so a reader that starts later can still attach.
    if (mode == bridge::Preference::automatic && !bridge::reader_running()) {
        show_if_hidden(game_window);
        return;
    }
    if (bridge::attached_to(game_window)) {
        bridge::refresh_window(game_window);
        show_if_hidden(game_window);
        return;
    }
    if (bridge::attach_skipped(game_window)) {
        show_if_hidden(game_window);
        return;
    }
    // On Windows a window created hidden for the reader was attached and
    // shown when it was made. A window that is already showing takes the
    // plain adapter here. Neither path hides it.
    const bool succeeded = bridge::attach(game_window);
    show_if_hidden(game_window);
    bridge::note_attach_finished(game_window, succeeded);
}

void Runtime::take_screen_reader_actions() {
    ensure_screen_reader();
#if !defined(OA_SCREEN_READER_COMPILED_OUT)
    typed_text.clear();
    if (sdl_.window == nullptr)
        return;
    const SDL_WindowID window_id = SDL_GetWindowID(sdl_.window);
    const std::vector<oa::platform::screen_reader::Event> events =
        oa::platform::screen_reader::take_events();
    for (const oa::platform::screen_reader::Event& event : events)
        push_event(window_id, event);
#endif
}

void Runtime::publish_screen_reader() {
#if !defined(OA_SCREEN_READER_COMPILED_OUT)
    const bool speaking = oa::platform::screen_reader::speech_where_bridge_cannot();
    const bool publishing =
        oa::platform::screen_reader::reader_running() && oa::platform::screen_reader::active();
    if (!speaking && !publishing)
        return;
    const CheckHost check = check_host(*this);
    const AutomationHost host = automation_host(*this);
    const oa::app::automation::ControlListing listing =
        oa::app::automation::listing_of(check, host);
    const oa::app::automation::WindowPlacement placement =
        oa::app::automation::window_placement(check);
    uint64_t focused_id = oa::platform::screen_reader::window_node;
    std::vector<oa::platform::screen_reader::Node> nodes = nodes_of(listing, placement, focused_id);
    if (speaking)
        speak_focused_node(nodes, focused_id);
    if (publishing)
        oa::platform::screen_reader::submit_tree(std::move(nodes), focused_id);
#elif defined(_WIN32)
    if (oa::platform::screen_reader::speech_where_bridge_cannot())
        speak_focused_control(*this);
#endif
}

} // namespace oa::app
