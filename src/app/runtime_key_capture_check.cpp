// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-key-capture: capturing a chord over a paused skirmish, a clash and
// its swap after its confirm button, the clash question opening and
// following the pointer in under 250 ms, Escape held for a second, a refused
// Escape, Settings' chord, Find by key hearing Primary+, and a draft clash
// settled from Settle…. With --snapshot it writes key-capture-<w>x<h>.ppm and
// key-clash-<w>x<h>.ppm at 640x480, 1280x720 and 1920x1080, and
// key-find-1280x720.ppm.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/key_editor/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace oakeys = oa::formats::oakeys;
namespace library = oa::data::key_library;
namespace first_run = oa::ui::first_run;
namespace layout = oa::ui::display_layout;

/// The modifier Settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
constexpr SDL_Keycode kShortcutKey = SDLK_LGUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
constexpr SDL_Keycode kShortcutKey = SDLK_LCTRL;
#endif

/// One window the check opens the editor in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the pictures are taken. The steps run at the large one.
constexpr std::array<CheckWindow, 3> kWindows{{
    {640, 480},
    {1280, 720},
    {1920, 1080},
}};

/// The large window. The steps run here, not at Compact.
constexpr CheckWindow kLarge = kWindows[1];

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("key capture check: " + std::string(what));
}

/// Puts a stream's buffer back when this goes.
struct RestoreStream {
    std::ostream& stream;       ///< the stream
    std::streambuf* previous{}; ///< its buffer

    ~RestoreStream() { stream.rdbuf(previous); }
};

/// Names one step, so the log lists step 8.
///
/// @param step the step
void note(std::string_view line) {
    std::cout << "key capture check: " << line << '\n';
}

} // namespace

void Runtime::check_key_capture() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), why);
        return *control;
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    const auto click_named = [&](std::string_view name) {
        const AutomationControl control = require_named(name, std::string(name) + " is not shown");
        const layout::Point pixel{
            control.window_x + control.window_width / 2,
            control.window_y + control.window_height / 2
        };
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    const auto show_row = [&](std::string_view name) {
        for (int32_t pass = 0; pass < 30 && !named(name); ++pass) {
            require(named("oa.key-editor.list").has_value(), "the list is not shown");
            if (pass == 0)
                click_named("oa.key-editor.list");
            PadAccess::send_key(*this, SDLK_PAGEDOWN);
            frames(1);
        }
        require(named(name).has_value(), std::string(name) + " is not shown");
    };
    const auto type_search = [&](std::string_view words) {
        click_named("oa.key-editor.search");
        for (const char letter : words) {
            const char text[2] = {letter, '\0'};
            SDL_Event typed{};
            typed.type = SDL_EVENT_TEXT_INPUT;
            typed.text.windowID = SDL_GetWindowID(sdl_.window);
            typed.text.text = text;
            dispatch_event(typed, running);
            require(running, "typed text ended the run");
        }
        frames(1);
    };
    const auto clear_search = [&] {
        if (oa_layer().find(editor::screen_name) == nullptr)
            return;
        if (named("oa.key-editor.capture").has_value() || named("oa.key-editor.clash").has_value())
            return;
        const std::optional<AutomationControl> field = named("oa.key-editor.search");
        if (!field || field->text.empty())
            return;
        PadAccess::send_key(*this, SDLK_ESCAPE);
        frames(1);
    };
    const auto key_event = [&](SDL_EventType type, SDL_Keycode key, SDL_Keymod mods) {
        SDL_Event event{};
        event.type = type;
        event.key.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
        event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
        event.key.key = key;
        event.key.mod = mods;
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        event.key.repeat = false;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto chord = [&](SDL_Keycode key, SDL_Keymod mods, SDL_Keycode modifier_key) {
        key_event(SDL_EVENT_KEY_DOWN, modifier_key, mods);
        key_event(SDL_EVENT_KEY_DOWN, key, mods);
        key_event(SDL_EVENT_KEY_UP, key, mods);
        key_event(SDL_EVENT_KEY_UP, modifier_key, SDL_KMOD_NONE);
        frames(1);
    };
    const auto show_details = [&] {
        if (named("oa.key-editor.detail.add").has_value())
            return;
        PadAccess::send_key(*this, SDLK_RETURN);
        frames(1);
    };
    const auto leave_details = [&] {
        if (named("oa.key-editor.back").has_value())
            click_named("oa.key-editor.back");
    };
    const auto cancel_prompt = [&] {
        if (named("oa.key-editor.capture.cancel").has_value())
            click_named("oa.key-editor.capture.cancel");
        else if (named("oa.key-editor.clash.cancel").has_value())
            click_named("oa.key-editor.clash.cancel");
        else if (named("oa.key-editor.find-key.close").has_value())
            click_named("oa.key-editor.find-key.close");
        require(
            !named("oa.key-editor.capture").has_value() &&
                !named("oa.key-editor.clash").has_value(),
            "the prompt stayed open"
        );
    };
    const auto snapshot = [&](std::string_view stem, const CheckWindow& window) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        const fs::path path = options_.snapshot.parent_path() /
                              (std::string(stem) + '-' + std::to_string(window.width) + 'x' +
                               std::to_string(window.height) + ".ppm");
        write_ppm(path, presented);
    };
    const auto resize = [&](const CheckWindow& window) {
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take the size"
        );
        apply_output_mode();
    };
    const auto dismiss_welcome = [&] {
        if (LayerScreen* welcome = oa_layer().find("first-run")) {
            oa_layer().close(welcome);
            frames(1);
        }
    };
    const auto open_paused = [&] {
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        frames(2);
        dismiss_welcome();
        start_benchmark_skirmish();
        require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
        require(open_key_editor({}), "the editor did not open in the match");
        frames(2);
        require(match_paused_ || !match_clock_steps(), "the skirmish is not paused");
        require(oa_layer().find(editor::screen_name) != nullptr, "the editor is not shown");
    };
    const auto close_editor = [&] {
        if (LayerScreen* open = oa_layer().find(editor::screen_name)) {
            oa_layer().close(open);
            frames(1);
        }
    };
    const auto capture_attack = [&](const CheckWindow& window, bool keep) {
        note("select Attack");
        clear_search();
        type_search("attack");
        show_row("oa.key-editor.row.order.attack");
        click_named("oa.key-editor.row.order.attack");
        show_details();
        note("press oa.key-editor.detail.add");
        click_named("oa.key-editor.detail.add");
        require(named("oa.key-editor.capture").has_value(), "the capture prompt did not open");
        note("Ctrl down and Q down and up");
        chord(SDLK_Q, SDL_KMOD_CTRL, SDLK_LCTRL);
        const AutomationControl prompt =
            require_named("oa.key-editor.capture", "the capture prompt is not shown");
        require(
            prompt.text.find("Ctrl") != std::string::npos &&
                prompt.text.find("Q") != std::string::npos,
            "the prompt does not show Ctrl and Q"
        );
        require(prompt.text.find("is free") != std::string::npos, "the free line is not shown");
        require(
            require_named("oa.key-editor.capture.ok", "OK is not shown").enabled, "OK is disabled"
        );
        snapshot("key-capture", window);
        if (!keep) {
            cancel_prompt();
            leave_details();
            return;
        }
        note("OK gives Attack A and Ctrl+Q");
        click_named("oa.key-editor.capture.ok");
        require(!named("oa.key-editor.capture").has_value(), "OK left the capture open");
        clear_search();
        type_search("attack");
        show_row("oa.key-editor.row.order.attack");
        const std::string attack =
            require_named("oa.key-editor.row.order.attack", "Attack left the list").text;
        require(attack.find("A") != std::string::npos, "Attack lost A");
        require(attack.find("Ctrl+Q") != std::string::npos, "Attack did not gain Ctrl+Q");
    };

    for (const CheckWindow& window : kWindows) {
        if (window.width == kLarge.width)
            continue;
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        note(std::string("paused staged skirmish ") + size);
        resize(window);
        open_paused();
        capture_attack(window, false);
        note(std::string("clash picture ") + size);
        clear_search();
        type_search("select.all");
        show_row("oa.key-editor.row.select.all");
        click_named("oa.key-editor.row.select.all");
        show_details();
        click_named("oa.key-editor.detail.chord.1");
        chord(SDLK_B, SDL_KMOD_CTRL, SDLK_LCTRL);
        click_named("oa.key-editor.capture.ok");
        require(
            require_named("oa.key-editor.clash", "the clash prompt is not shown")
                    .text.find("Select builders") != std::string::npos,
            "the clash prompt does not name Select builders"
        );
        snapshot("key-clash", window);
        cancel_prompt();
        close_editor();
        leave_match();
    }

    note("paused staged skirmish");
    resize(kLarge);
    open_paused();
    note("Find by key hears Primary+, and lists Settings, and Settings stays shut");
    click_named("oa.key-editor.find");
    require(named("oa.key-editor.find-key").has_value(), "Find by key did not open");
    chord(SDLK_COMMA, kShortcutModifier, kShortcutKey);
    require(oa_layer().find("settings") == nullptr, "Settings opened from Find by key");
    require(
        require_named("oa.key-editor.find-key", "Find by key is not shown").text.find("Settings") !=
            std::string::npos,
        "Primary+, did not find Settings"
    );
    snapshot("key-find", kLarge);
    click_named("oa.key-editor.find-key.close");
    require(!named("oa.key-editor.find-key").has_value(), "Close left Find by key open");
    note("typing in the search only searches commands");
    type_search("b");
    require(
        require_named("oa.key-editor.search", "the search is not shown").text == "b",
        "a typed b did not reach the search"
    );
    clear_search();
    capture_attack(kLarge, true);

    note("Enter on Select all's Ctrl+A, then Ctrl+B");
    clear_search();
    type_search("select.all");
    show_row("oa.key-editor.row.select.all");
    click_named("oa.key-editor.row.select.all");
    click_named("oa.key-editor.detail.chord.1");
    chord(SDLK_B, SDL_KMOD_CTRL, SDLK_LCTRL);
    require(
        require_named("oa.key-editor.capture", "the clash capture is not shown")
                .text.find("Select builders") != std::string::npos,
        "the capture does not name Select builders"
    );
    click_named("oa.key-editor.capture.ok");
    note("the clash prompt names Select builders");
    require(
        require_named("oa.key-editor.clash", "the clash prompt is not shown")
                .text.find("Select builders") != std::string::npos,
        "the clash prompt does not name Select builders"
    );
    require(named("oa.key-editor.clash.swap").has_value(), "Swap them is not shown");
    snapshot("key-clash", kLarge);
    note("choosing Swap them changes nothing until its button");
    click_named("oa.key-editor.clash.swap");
    require(named("oa.key-editor.clash").has_value(), "choosing Swap them closed the question");
    require(
        require_named("oa.key-editor.clash.confirm", "the confirm button is not shown").text ==
            "Swap them",
        "the confirm button is not Swap them"
    );
    note("Swap them gives Select all Ctrl+B and Select builders Ctrl+A");
    click_named("oa.key-editor.clash.confirm");
    require(!named("oa.key-editor.clash").has_value(), "Swap them left the question open");
    clear_search();
    type_search("select.all");
    show_row("oa.key-editor.row.select.all");
    require(
        require_named("oa.key-editor.row.select.all", "Select all left the list")
                .text.find("Ctrl+B") != std::string::npos,
        "Select all did not gain Ctrl+B"
    );
    clear_search();
    type_search("select.builders");
    show_row("oa.key-editor.row.select.builders");
    require(
        require_named("oa.key-editor.row.select.builders", "Select builders left the list")
                .text.find("Ctrl+A") != std::string::npos,
        "Select builders did not gain Ctrl+A"
    );

    // The question works out what each outcome leaves as it opens. Pressing
    // Choose what happens… and moving the pointer over the question each
    // stay well inside a frame, in a match with the game's GUI keys and
    // Category words listed.
    note("P for Load names Patrol, and Choose what happens… opens at once");
    clear_search();
    type_search("order.load");
    show_row("oa.key-editor.row.order.load");
    click_named("oa.key-editor.row.order.load");
    click_named("oa.key-editor.detail.chord.1");
    key_event(SDL_EVENT_KEY_DOWN, SDLK_P, SDL_KMOD_NONE);
    key_event(SDL_EVENT_KEY_UP, SDLK_P, SDL_KMOD_NONE);
    frames(1);
    require(
        require_named("oa.key-editor.capture", "the capture for Load is not shown")
                .text.find("Patrol") != std::string::npos,
        "P for Load does not name Patrol"
    );
    const auto milliseconds_since = [](std::chrono::steady_clock::time_point start) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - start
        )
            .count();
    };
    const auto opening = std::chrono::steady_clock::now();
    click_named("oa.key-editor.capture.ok");
    const auto open_ms = milliseconds_since(opening);
    require(named("oa.key-editor.clash").has_value(), "Choose what happens… did not ask");
    int64_t slowest_ms = 0;
    for (const std::string_view part :
         {"oa.key-editor.clash.replace",
          "oa.key-editor.clash.swap",
          "oa.key-editor.clash.another",
          "oa.key-editor.clash.cancel",
          "oa.key-editor.clash.confirm"}) {
        const AutomationControl control = require_named(part, std::string(part) + " is not shown");
        const layout::Point pixel{
            control.window_x + control.window_width / 2,
            control.window_y + control.window_height / 2
        };
        const auto moved = std::chrono::steady_clock::now();
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        frames(1);
        slowest_ms = std::max<int64_t>(slowest_ms, milliseconds_since(moved));
    }
    std::cout << "key capture check: Choose what happens… took " << open_ms
              << " ms, the slowest move over the question " << slowest_ms << " ms\n";
    require(open_ms < 250, "Choose what happens… took " + std::to_string(open_ms) + " ms");
    require(
        slowest_ms < 250, "a move over the question took " + std::to_string(slowest_ms) + " ms"
    );
    cancel_prompt();

    note("Escape held 1100 ms for Attack offers the clash with Back");
    clear_search();
    type_search("attack");
    show_row("oa.key-editor.row.order.attack");
    click_named("oa.key-editor.row.order.attack");
    click_named("oa.key-editor.detail.add");
    key_event(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE, SDL_KMOD_NONE);
    SDL_Delay(1100);
    frames(2);
    key_event(SDL_EVENT_KEY_UP, SDLK_ESCAPE, SDL_KMOD_NONE);
    frames(1);
    require(
        require_named("oa.key-editor.capture", "Escape did not finish the capture")
                .text.find("Back") != std::string::npos,
        "Escape for Attack does not clash with Back"
    );
    require(
        require_named("oa.key-editor.capture.ok", "OK is not shown").enabled,
        "the clash with Back disables OK"
    );
    cancel_prompt();

    note("Escape for Send is refused with OK disabled");
    clear_search();
    type_search("chat.send");
    show_row("oa.key-editor.row.chat.send");
    click_named("oa.key-editor.row.chat.send");
    click_named("oa.key-editor.detail.add");
    key_event(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE, SDL_KMOD_NONE);
    SDL_Delay(1100);
    frames(2);
    key_event(SDL_EVENT_KEY_UP, SDLK_ESCAPE, SDL_KMOD_NONE);
    frames(1);
    const AutomationControl refused =
        require_named("oa.key-editor.capture", "Escape for Send did not finish");
    require(
        refused.text.find("Choose another key") != std::string::npos, "Send did not refuse Escape"
    );
    require(
        !require_named("oa.key-editor.capture.ok", "OK is not shown").enabled,
        "OK is enabled for Send"
    );
    cancel_prompt();

    note("Ctrl+, for Attack is not refused and names Settings");
    clear_search();
    type_search("attack");
    show_row("oa.key-editor.row.order.attack");
    click_named("oa.key-editor.row.order.attack");
    click_named("oa.key-editor.detail.add");
    chord(SDLK_COMMA, kShortcutModifier, kShortcutKey);
    const AutomationControl settings =
        require_named("oa.key-editor.capture", "the Settings chord did not finish");
    require(settings.text.find("Settings") != std::string::npos, "the line does not name Settings");
    require(
        settings.text.find("Choose another key") == std::string::npos,
        "Settings was refused as fixed"
    );
    require(require_named("oa.key-editor.capture.ok", "OK is not shown").enabled, "OK is disabled");
    require(oa_layer().find("settings") == nullptr, "Settings opened while the prompt captured");
    click_named("oa.key-editor.capture.ok");
    require(named("oa.key-editor.clash").has_value(), "OK did not open the clash prompt");
    require(oa_layer().find("settings") == nullptr, "Settings opened from the clash prompt");

    note("while the prompt is open A arms nothing");
    options_.trace_input = true;
    std::stringstream armed;
    {
        RestoreStream restore{std::cerr, std::cerr.rdbuf(armed.rdbuf())};
        key_event(SDL_EVENT_KEY_DOWN, SDLK_A, SDL_KMOD_NONE);
        key_event(SDL_EVENT_KEY_UP, SDLK_A, SDL_KMOD_NONE);
    }
    options_.trace_input = false;
    require(
        armed.str().find("input action") == std::string::npos,
        "A armed an action while the prompt was open"
    );
    cancel_prompt();

    note("SAVE, close, and Ctrl+B selects all");
    click_named("oa.key-editor.save");
    require(oa_layer().find(editor::screen_name) == nullptr, "SAVE did not close the editor");
    options_.trace_input = true;
    std::stringstream selected;
    {
        RestoreStream restore{std::cerr, std::cerr.rdbuf(selected.rdbuf())};
        chord(SDLK_B, SDL_KMOD_CTRL, SDLK_LCTRL);
    }
    options_.trace_input = false;
    require(selected.str().find("select.all") != std::string::npos, "Ctrl+B did not select all");
    leave_match();

    note("a scratch own layout with a clash opens on the Clashes filter with the card");
    oakeys::Manifest manifest;
    manifest.format = oakeys::format_version;
    manifest.id = "own-oa-default";
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oakeys::Base::oa_default;
    // One own binding takes Select builders' Ctrl+B. The draft holds the clash
    // the player would see before SAVE. Two bindings of one chord in this file
    // would be dropped as the layout loads, and the card would never show.
    oakeys::BindingEntry attack;
    attack.action = "order.attack";
    attack.chords.push_back(oakeys::ChordText{"Ctrl+B", 0, 0});
    manifest.bindings.push_back(std::move(attack));
    const fs::path own_dir = user_folder_ / std::string(library::keys_folder_name) /
                             library::own_folder_name("oa-default");
    std::error_code made;
    fs::create_directories(own_dir, made);
    require(!made, "the own layout folder was not made");
    {
        std::ofstream out(own_dir / std::string(oakeys::manifest_file), std::ios::binary);
        require(static_cast<bool>(out), "the own layout was not opened");
        out << oakeys::write_manifest(manifest);
        require(static_cast<bool>(out), "the own layout was not written");
    }
    load(Screen::main_menu);
    frames(2);
    dismiss_welcome();
    require(open_key_editor({}), "the editor did not open on the clash");
    frames(1);
    require(named("oa.key-editor.card.settle").has_value(), "Settle… is not shown");
    click_named("oa.key-editor.card.settle");
    require(named("oa.key-editor.card.keep-here").has_value(), "Keep it for Attack is not shown");
    require(
        named("oa.key-editor.card.keep-there").has_value(), "Keep it for the other is not shown"
    );
    require(named("oa.key-editor.clash.another").has_value(), "Choose another key is not shown");
    require(!named("oa.key-editor.row.select.all").has_value(), "the list did not open on Clashes");
    note("Keep it for Attack enables Save changes");
    click_named("oa.key-editor.card.keep-here");
    click_named("oa.key-editor.clash.confirm");
    require(!named("oa.key-editor.clash").has_value(), "the confirm button left the question open");
    require(
        require_named("oa.key-editor.save", "Save changes is not shown").enabled,
        "Keep it for Attack left Save changes disabled"
    );
    close_editor();
}

} // namespace oa::app
