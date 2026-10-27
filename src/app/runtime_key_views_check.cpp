// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-key-views: the key editor's keyboard and mouse at 640x480, 1280x720
// and 1920x1080, each modifier layer, the shape drop-down, a free key bound
// through the capture prompt, the mouse's Forward, and Left-click saved as
// Interface Type. With --snapshot it writes key-keyboard-<w>x<h>.ppm and
// key-mouse-<w>x<h>.ppm. The pictures are the shape the system reports.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/key_editor/screen.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace editor = oa::ui::key_editor;
namespace first_run = oa::ui::first_run;
namespace layout = oa::ui::display_layout;
namespace input = oa::sim::gameplay_input;

/// The preference that remembers the keyboard picture's shape.
constexpr std::string_view kKeyboardShape = "open-annihilation.keys.keyboard";

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
        throw std::runtime_error("key views check: " + std::string(what));
}

/// Names one step, so the log lists step 7.
///
/// @param line the step
void note(std::string_view line) {
    std::cout << "key views check: " << line << '\n';
}

} // namespace

void Runtime::check_key_views() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    // Start on the right-click interface, so SAVE to left-click is a real write.
    // The engine stores left-click as 0. The ticket's "Interface Type 1" is the
    // right-click value, which is where this run begins.
    preferences_.interface_type = input::interface_right_click;
    preferences_dirty_ = true;
    save_preferences();
    load_key_bindings();
    require(
        preferences_.interface_type == input::interface_right_click,
        "the run did not start on the right-click interface"
    );

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
        frames(2);
    };
    const auto dismiss_welcome = [&] {
        if (LayerScreen* welcome = oa_layer().find("first-run")) {
            oa_layer().close(welcome);
            frames(1);
        }
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    frames(2);
    dismiss_welcome();
    require(open_key_editor({}), "the editor did not open");
    frames(2);
    require(oa_layer().find(editor::screen_name) != nullptr, "the editor is not shown");

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        resize(window);
        click_named("oa.key-editor.view.keyboard");
        note("Keyboard view at " + size);
        snapshot("key-keyboard", window);
        click_named("oa.key-editor.view.mouse");
        note("Mouse view at " + size);
        snapshot("key-mouse", window);
    }

    resize(kLarge);
    click_named("oa.key-editor.view.keyboard");
    const std::string_view layers[] = {"Nothing", "Shift", "Ctrl", "Alt"};
    const std::string_view layer_names[] = {
        "oa.key-editor.layer.none",
        "oa.key-editor.layer.shift",
        "oa.key-editor.layer.ctrl",
        "oa.key-editor.layer.alt",
    };
    for (int index = 0; index < 4; ++index) {
        click_named(layer_names[index]);
        note(std::string("layer ") + std::string(layers[index]));
    }
    if (named("oa.key-editor.layer.cmd").has_value()) {
        click_named("oa.key-editor.layer.cmd");
        note("layer Cmd");
    } else {
        note("Cmd is absent because Primary is Ctrl");
    }

    note("the drop-down to JIS writes open-annihilation.keys.keyboard");
    click_named("oa.key-editor.keyboard-shape");
    click_named("oa.key-editor.keyboard-shape.jis");
    const auto stored = preference_values_.find(std::string(kKeyboardShape));
    require(
        stored != preference_values_.end() && stored->second == "jis",
        "the drop-down did not write open-annihilation.keys.keyboard as jis"
    );

    note("selecting Repair, then the free F9 inspects it, and Use F9 for Repair… gives F9");
    click_named("oa.key-editor.view.list");
    type_search("repair");
    show_row("oa.key-editor.row.order.repair");
    click_named("oa.key-editor.row.order.repair");
    click_named("oa.key-editor.view.keyboard");
    click_named("oa.key-editor.layer.none");
    click_named("oa.key-editor.keyboard.f9");
    require(!named("oa.key-editor.capture").has_value(), "a click on F9 opened the capture");
    require(
        require_named("oa.key-editor.key-card", "the inspector is not shown").text.find("F9") !=
            std::string::npos,
        "the inspector does not name F9"
    );
    click_named("oa.key-editor.key-card.use");
    const AutomationControl prompt =
        require_named("oa.key-editor.capture", "the capture prompt did not open for F9");
    require(prompt.text.find("F9") != std::string::npos, "the prompt does not show F9");
    require(
        require_named("oa.key-editor.capture.ok", "OK is not shown").enabled,
        "OK is disabled for the free F9"
    );
    click_named("oa.key-editor.capture.ok");
    require(!named("oa.key-editor.capture").has_value(), "OK left the capture open");
    click_named("oa.key-editor.view.list");
    bool repair_f9 = false;
    {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls) {
            if (control.name.starts_with("oa.key-editor.detail.chord.") &&
                control.text.find("F9") != std::string::npos)
                repair_f9 = true;
        }
    }
    require(repair_f9, "Repair's details do not show F9");

    note("the Mouse view's Forward opens the prompt for its role");
    click_named("oa.key-editor.view.mouse");
    click_named("oa.key-editor.mouse.mouse-forward");
    const AutomationControl forward =
        require_named("oa.key-editor.capture", "Forward did not open the prompt");
    require(forward.text.find("Repair") != std::string::npos, "Forward's prompt is not for Repair");
    click_named("oa.key-editor.capture.cancel");
    require(!named("oa.key-editor.capture").has_value(), "CANCEL left the prompt open");

    note("Left-click redraws the roles and SAVE writes Interface Type 1");
    note("the engine's left-click Interface Type is 0, which is the value SAVE writes");
    click_named("oa.key-editor.scheme.left-click");
    bool command_on_left = false;
    {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls) {
            if (!control.name.starts_with("oa.key-editor.mouse-table.playing"))
                continue;
            if (control.text.find("Left") != std::string::npos &&
                control.text.find("Command") != std::string::npos)
                command_on_left = true;
        }
    }
    require(command_on_left, "Left-click did not put Command on the left button");
    require(
        preferences_.interface_type == input::interface_right_click,
        "the scheme strip wrote Interface Type before SAVE"
    );
    click_named("oa.key-editor.save");
    require(
        preferences_.interface_type == input::interface_left_click,
        "SAVE did not write Interface Type 0 for left-click"
    );
    require(oa_layer().find(editor::screen_name) == nullptr, "SAVE left the editor open");
    note("key views check done");
}

} // namespace oa::app
