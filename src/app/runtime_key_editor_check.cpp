// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-key-editor: the key editor over Settings at 640x480, 1280x720 and
// 1920x1080, then the list's edits, the save, a paused skirmish and Developer
// Mode. With --snapshot it writes key-editor-<w>x<h>.ppm for the three menu
// windows, key-editor-match-<w>x<h>.ppm for the three match windows, and
// key-editor-draft.ppm and key-editor-close-question.ppm at 1280x720.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/theme.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
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
namespace settings = oa::ui::engine_settings;
namespace layout = oa::ui::display_layout;
namespace oakeys = oa::formats::oakeys;
namespace keys = oa::data::key_library;
namespace first_run = oa::ui::first_run;

/// The modifier Settings' shortcut takes on this platform.
#ifdef SDL_PLATFORM_MACOS
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_GUI;
#else
constexpr SDL_Keymod kShortcutModifier = SDL_KMOD_CTRL;
#endif

/// One window the check opens the editor in, and the class the menus give it.
struct CheckWindow {
    int32_t width{};             ///< the window's width, in pixels
    int32_t height{};            ///< its height
    kit::SizeClass size_class{}; ///< the class on the menus
    int32_t scale{};             ///< window pixels a point, on the menus
};

/// The menu windows, in the order the check opens them.
constexpr std::array<CheckWindow, 3> kWindows{{
    {640, 480, kit::SizeClass::compact, 1},
    {1280, 720, kit::SizeClass::large, 1},
    {1920, 1080, kit::SizeClass::large, 1},
}};

/// The filter pills, in the order the header draws them.
constexpr std::array<std::string_view, 5> kFilters{{
    "oa.key-editor.filter.all",
    "oa.key-editor.filter.changed",
    "oa.key-editor.filter.clashes",
    "oa.key-editor.filter.unbound",
    "oa.key-editor.filter.latchable",
}};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("key editor check: " + std::string(what));
}

/// The count in a filter's caption, "All (164)". -1 when the caption has none.
///
/// @param text the caption
/// @return the count
int32_t count_of(const std::string& text) {
    const std::size_t open = text.rfind('(');
    const std::size_t close = text.rfind(')');
    if (open == std::string::npos || close == std::string::npos || close < open + 2)
        return -1;
    int32_t value = 0;
    for (std::size_t index = open + 1; index < close; ++index) {
        const char digit = text[index];
        if (digit < '0' || digit > '9')
            return -1;
        value = value * 10 + static_cast<int32_t>(digit - '0');
    }
    return value;
}

/// The path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    return snapshot.parent_path() / (snapshot.stem().string() + '-' + std::string(step) + ".ppm");
}

/// Puts a stream's buffer back when this goes.
struct RestoreStream {
    std::ostream& stream;       ///< the stream
    std::streambuf* previous{}; ///< its buffer

    ~RestoreStream() { stream.rdbuf(previous); }
};

} // namespace

void Runtime::check_key_editor() {
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
    const auto listed = [this](std::string_view name) {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name)
                return true;
        return false;
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
    const auto type_text = [&](std::string_view words) {
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
    const auto show_row = [&](std::string_view name) {
        for (int32_t step = 0; step < 30 && !named(name); ++step) {
            require(named("oa.key-editor.list").has_value(), "the list is not shown");
            if (step == 0)
                click_named("oa.key-editor.list");
            PadAccess::send_key(*this, SDLK_PAGEDOWN);
            frames(1);
        }
        require(named(name).has_value(), std::string(name) + " is not shown");
    };
    const auto snapshot = [this](const std::string& step) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto dismiss_welcome = [&] {
        if (LayerScreen* welcome = oa_layer().find("first-run")) {
            oa_layer().close(welcome);
            frames(1);
        }
    };
    const auto close_editor = [&] {
        if (LayerScreen* open = oa_layer().find(editor::screen_name)) {
            oa_layer().close(open);
            frames(1);
        }
        require(oa_layer().find(editor::screen_name) == nullptr, "the editor stayed open");
    };
    const auto close_settings = [&] {
        if (engine_settings_dialog() != nullptr)
            take_settings_screen_action(settings::DialogAction::cancelled);
        else if (LayerScreen* settings_screen = oa_layer().find("settings"))
            oa_layer().close(settings_screen);
        frames(1);
        require(
            engine_settings_dialog() == nullptr && oa_layer().find("settings") == nullptr,
            "Settings stayed open"
        );
    };
    const auto resize = [&](const CheckWindow& window) {
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not take ") + std::to_string(window.width) + 'x' +
                std::to_string(window.height)
        );
        apply_output_mode();
    };
    const auto prepare_menu = [&] {
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        frames(2);
        dismiss_welcome();
        if (oa_layer().find(editor::screen_name) != nullptr)
            close_editor();
        if (oa_layer().find("settings") == nullptr) {
            PadAccess::send_key(*this, SDLK_COMMA, kShortcutModifier);
            frames(2);
        }
        require(
            engine_settings_dialog() != nullptr && oa_layer().find("settings") != nullptr &&
                oa_layer().top() != nullptr && oa_layer().top()->name() == "settings",
            "Settings did not open"
        );
    };
    const auto open_over_settings = [&] {
        KeyEditorRequest request;
        request.over_settings = true;
        require(open_key_editor(request), "the editor did not open over Settings");
        frames(1);
        require(
            oa_layer().find(editor::screen_name) != nullptr &&
                oa_layer().find("settings") != nullptr && oa_layer().top() != nullptr &&
                oa_layer().top()->name() == editor::screen_name,
            "the editor is not over Settings"
        );
    };
    const auto check_filters = [&](std::string_view where) {
        int32_t all = -1;
        for (const std::string_view filter : kFilters) {
            const AutomationControl control =
                require_named(filter, std::string(filter) + " is not shown " + std::string(where));
            const int32_t count = count_of(control.text);
            require(count >= 0, std::string(filter) + " has no count " + std::string(where));
            if (filter == "oa.key-editor.filter.all")
                all = count;
            else
                require(
                    all >= count, std::string(filter) + " counts past All " + std::string(where)
                );
            std::cout << "key editor check: " << filter << " " << control.text << ' ' << where
                      << '\n';
        }
        require(all > 0, std::string("All counts nothing ") + std::string(where));
    };

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        resize(window);
        prepare_menu();
        require(
            oa_layer().screen_class() == window.size_class &&
                oa_layer().screen_scale() == window.scale,
            size + " is not " + std::string(size_class_name(window.size_class))
        );
        open_over_settings();
        const LayerPlacement placed = oa_layer().placement_of(editor::screen_name);
        const kit::Metrics& metrics = kit::metrics_of(window.size_class);
        require(
            placed.points_width == metrics.dialog_width &&
                placed.points_height == metrics.dialog_height &&
                placed.scale_percent == window.scale * 100,
            size + " is not the class dialog"
        );
        std::cout << "key editor check: " << size << " is " << size_class_name(window.size_class)
                  << " at " << window.scale << "x, " << placed.points_width << 'x'
                  << placed.points_height << '\n';
        snapshot(size);
        if (window.size_class == kit::SizeClass::compact) {
            click_named("oa.key-editor.category");
            PadAccess::send_key(*this, SDLK_END);
            frames(1);
            check_filters("in the category menu");
            PadAccess::send_key(*this, SDLK_ESCAPE);
            frames(1);
            require(
                oa_layer().find(editor::screen_name) != nullptr &&
                    !named("oa.key-editor.filter.all").has_value(),
                "Escape did not close the category menu"
            );
        } else {
            check_filters(std::string("on ") + size);
        }
        PadAccess::send_key(*this, SDLK_ESCAPE);
        frames(1);
        require(oa_layer().find(editor::screen_name) == nullptr, "Escape did not close the editor");
    }

    const CheckWindow large = kWindows[1];
    resize(large);
    prepare_menu();
    open_over_settings();
    std::cout << "key editor check: search attack\n";
    type_text("attack");
    show_row("oa.key-editor.row.order.attack");
    require(
        require_named("oa.key-editor.row.order.attack", "Attack is not shown").text == "Attack A",
        "Attack is not shown as Attack A"
    );
    click_named("oa.key-editor.row.order.attack");
    click_named("oa.key-editor.detail.chord.1.remove");
    require(
        require_named("oa.key-editor.row.order.attack", "Attack left the list").text == "Attack",
        "removing A did not leave Attack unbound"
    );
    click_named("oa.key-editor.filter.changed");
    require(named("oa.key-editor.row.order.attack").has_value(), "Attack is not among the changes");
    click_named("oa.key-editor.filter.unbound");
    require(named("oa.key-editor.row.order.attack").has_value(), "Attack is not among the unbound");
    click_named("oa.key-editor.filter.all");
    std::cout << "key editor check: Attack unbound\n";

    PadAccess::send_key(*this, SDLK_ESCAPE);
    frames(1);
    require(
        oa_layer().find(editor::screen_name) != nullptr, "Escape left the editor with a search"
    );
    require(
        require_named("oa.key-editor.search", "the search is not shown").text.empty(),
        "Escape did not clear the search"
    );
    std::cout << "key editor check: Queue latch\n";
    type_text("queue");
    show_row("oa.key-editor.row.order.queue-modifier");
    click_named("oa.key-editor.row.order.queue-modifier");
    click_named("oa.key-editor.detail.more");
    click_named("oa.key-editor.detail.latch.on");

    std::cout << "key editor check: number keys\n";
    click_named("oa.key-editor.nav.groups");
    click_named("oa.key-editor.number-keys.on");
    std::cout << "key editor check: save\n";
    click_named("oa.key-editor.save");
    require(oa_layer().find(editor::screen_name) == nullptr, "SAVE did not close the editor");
    require(engine_settings().switch_alt, "SAVE did not set SwitchAlt");
    const fs::path own_file = user_folder_ / std::string(keys::keys_folder_name) /
                              keys::own_folder_name("oa-default") /
                              std::string(oakeys::manifest_file);
    std::ifstream in(own_file, std::ios::binary);
    require(static_cast<bool>(in), "the own layout was not written");
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    oakeys::Manifest manifest;
    std::vector<oakeys::Problem> problems;
    require(oakeys::read_manifest(bytes, manifest, problems), "the own layout cannot be read");
    bool attack_empty = false;
    for (const oakeys::BindingEntry& binding : manifest.bindings) {
        if (binding.action == "order.attack") {
            attack_empty = binding.chords.empty();
            break;
        }
    }
    require(attack_empty, "order.attack was not saved with no chord");
    bool queue_latched = false;
    if (manifest.helpers.toggle) {
        for (const std::string& id : *manifest.helpers.toggle)
            if (id == "order.queue-modifier")
                queue_latched = true;
    }
    require(queue_latched, "Queue was not saved as latched");
    std::cout << "key editor check: own layout written\n";

    close_settings();
    std::cout << "key editor check: A arms nothing\n";
    options_.trace_input = true;
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    require(!match_paused_, "the skirmish started paused");
    std::stringstream caught;
    {
        RestoreStream restore{std::cerr, std::cerr.rdbuf(caught.rdbuf())};
        PadAccess::send_key(*this, SDLK_A);
    }
    options_.trace_input = false;
    require(caught.str().find("order.attack") == std::string::npos, "A still arms Attack");
    // load alone leaves the match object and its pause. The next screen is
    // the menus, so the match is left first.
    leave_match();
    load(Screen::main_menu);
    frames(2);
    dismiss_welcome();

    std::cout << "key editor check: reopen\n";
    resize(large);
    prepare_menu();
    open_over_settings();
    const AutomationControl summary =
        require_named("oa.key-editor.summary", "the summary is not shown");
    require(
        summary.text.find("and 2 changes of your own") != std::string::npos,
        "the summary does not name 2 changes"
    );
    click_named("oa.key-editor.nav.groups");
    click_named("oa.key-editor.number-keys.off");
    require(
        require_named("oa.key-editor.draft", "the draft line is not shown").text ==
            "1 unsaved edit",
        "the draft line does not say 1 unsaved edit"
    );
    snapshot("draft");
    click_named("oa.key-editor.cancel");
    require(
        require_named("oa.prompt.body", "Cancel did not ask before closing")
                .text.find("before closing") != std::string::npos,
        "the close question does not ask"
    );
    require(named("oa.prompt.save").has_value(), "Save is not shown");
    require(named("oa.prompt.discard").has_value(), "Discard is not shown");
    require(named("oa.prompt.keep-editing").has_value(), "Keep editing is not shown");
    std::cout << "key editor check: close question\n";
    snapshot("close-question");
    click_named("oa.prompt.keep-editing");
    require(oa_layer().find(editor::screen_name) != nullptr, "Keep editing closed the editor");
    click_named("oa.key-editor.undo");
    require(
        require_named("oa.key-editor.draft", "the draft line is not shown").text ==
            "No unsaved edits",
        "Undo did not take the edit back"
    );
    click_named("oa.key-editor.number-keys.off");
    click_named("oa.key-editor.close");
    click_named("oa.prompt.discard");
    require(oa_layer().find(editor::screen_name) == nullptr, "Discard did not close the editor");
    require(engine_settings().switch_alt, "Discard wrote the switch");
    std::cout << "key editor check: Discard writes nothing\n";
    open_over_settings();
    click_named("oa.key-editor.nav.groups");
    click_named("oa.key-editor.number-keys.off");
    click_named("oa.key-editor.cancel");
    click_named("oa.prompt.save");
    require(oa_layer().find(editor::screen_name) == nullptr, "Save did not close the editor");
    require(!engine_settings().switch_alt, "Save did not write the switch");
    std::cout << "key editor check: Save from the close question\n";

    std::cout << "key editor check: developer off\n";
    open_over_settings();
    require(
        !listed("oa.key-editor.nav.developer"), "Developer is listed while Developer Mode is off"
    );
    close_editor();
    const settings::EngineSettings opened = engine_settings();
    settings::EngineSettings chosen = opened;
    chosen.developer_mode = true;
    apply_engine_settings(chosen);
    open_over_settings();
    require(
        named("oa.key-editor.nav.developer").has_value(),
        "Developer is not listed in Developer Mode"
    );
    std::cout << "key editor check: developer on\n";
    close_editor();
    apply_engine_settings(opened);
    require(!engine_settings().developer_mode, "Developer Mode stayed on");

    std::cout << "key editor check: unbind Settings\n";
    open_over_settings();
    type_text("settings");
    show_row("oa.key-editor.row.ui.settings");
    click_named("oa.key-editor.row.ui.settings");
    click_named("oa.key-editor.detail.chord.1.remove");
    click_named("oa.key-editor.save");
    require(oa_layer().find(editor::screen_name) == nullptr, "SAVE did not close the editor");
    close_settings();
    PadAccess::send_key(*this, SDLK_COMMA, kShortcutModifier);
    frames(2);
    require(
        oa_layer().find("settings") == nullptr && engine_settings_dialog() == nullptr,
        "Primary+comma still opens Settings"
    );
    std::cout << "key editor check: Settings stays closed\n";
    require(open_key_editor({}), "the editor did not open");
    frames(1);
    type_text("chat.cancel");
    show_row("oa.key-editor.row.chat.cancel");
    click_named("oa.key-editor.row.chat.cancel");
    require(named("oa.key-editor.detail.reset").has_value(), "Cancel has no details");
    {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            require(
                !control.name.starts_with("oa.key-editor.detail.chord.") ||
                    !control.name.ends_with(".remove"),
                "Escape on chat.cancel offers a remove"
            );
    }
    std::cout << "key editor check: Escape stays fixed\n";
    PadAccess::send_key(*this, SDLK_ESCAPE);
    frames(1);
    require(
        oa_layer().find(editor::screen_name) != nullptr,
        "Escape left instead of clearing the search"
    );
    PadAccess::send_key(*this, SDLK_ESCAPE);
    frames(1);
    require(oa_layer().find(editor::screen_name) == nullptr, "Escape did not close the editor");

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        resize(window);
        load(Screen::main_menu);
        frames(2);
        dismiss_welcome();
        start_benchmark_skirmish();
        require(screen_ == Screen::match && match_ != nullptr, "the match did not start");
        const uint32_t tick = match_timing_.tick;
        require(open_key_editor({}), "the editor did not open in the match");
        frames(3);
        require(match_paused_ || !match_clock_steps(), "the match is not paused");
        require(match_timing_.tick == tick, "the clock moved while the editor was open");
        const LayerView seen = oa_layer().view();
        require(seen.match && seen.match_layout != nullptr, "the match has no layout");
        const MatchDialogPlace place =
            OaLayer::match_dialog(*seen.match_layout, seen.interface_percent, seen.fit_safe_area);
        const LayerPlacement placed = oa_layer().placement_of(editor::screen_name);
        const kit::Metrics& metrics = kit::metrics_of(place.size_class);
        require(
            placed.shown.x == place.shown.x && placed.shown.y == place.shown.y &&
                placed.shown.width == place.shown.width &&
                placed.shown.height == place.shown.height,
            "the editor is not beside the side column"
        );
        require(
            placed.points_width == metrics.dialog_width &&
                placed.points_height == metrics.dialog_height &&
                placed.scale_percent == place.scale_percent,
            "the match editor is not the dialog's class"
        );
        std::cout << "key editor check: match " << size << " holds the clock\n";
        snapshot(std::string("match-") + size);
        PadAccess::send_key(*this, SDLK_ESCAPE);
        frames(1);
        require(
            oa_layer().find(editor::screen_name) == nullptr, "Escape did not close the match editor"
        );
        // The pause the editor opened belongs to this match. The next size
        // starts another, which must not inherit that pause.
        leave_match();
        load(Screen::main_menu);
        frames(1);
    }

    std::cout << "key editor check passed\n";
}

} // namespace oa::app
