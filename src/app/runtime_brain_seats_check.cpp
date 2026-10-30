// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-brain-seat-buttons: each computer row's brain on SKIRMISH, at
// 640x480 and 1280x720. Before any brain is written a frame is kept. Three
// made-up brains are then written, Steady Commander with two levels, and
// the remembered skirmish seats name Steady Commander at firm on place 1.
// The row's notch, oa.brain-seat.1, reads that label and sits inside the
// right end of the row's own Player button, inside the frame. Player2 is
// put back to Open before the kept frame, because a previous size may have
// saved it as a computer, then cycled until it is a computer. It reads the
// built-in player and the Difficulty button's word, and cycling Difficulty
// changes it. Tab from Player1 focuses the notch with the GUI's focus none,
// Tab moves on and Shift+Tab returns. Return opens the brain menu on
// Steady Commander at Firm; its next level steps the seat to Bold and back,
// its Options... opens the picker, whose USE THIS keeps the brain with its
// options, and the built-in player puts the Difficulty word back; the
// built-in player has no Options.... The right button
// on the Player button opens the menu, and Escape closes it. Use for every
// computer seat and Lantern Scout set both rows. The pointer on the Player
// button shows its card, and Escape closes it. START logs that players 1
// and 2 play lantern-scout. Back on SKIRMISH the seats read as remembered,
// and the left button on Player2's words keeps 3.1c's own press, which
// turns the row Open. With the Brains folder removed, no seat remains and
// the frame matches the one kept before any brain was written. On the
// 640x480 window, 120 frames before a computer seat and 120 frames with two
// are timed: the seated frames must not read the game folder and must stay
// under 5 ms of CPU, and copying then removing a bundle must be followed by
// the new choice. Each size's frame is written as
// brain-seats-skirmish-<w>x<h>.png, and the frame with the menu open as
// brain-seats-menu-<w>x<h>.png.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/content_copy.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/oaexp.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace brain = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace brains = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
namespace kit = oa::ui::kit;
namespace pad = oa::ui::pad_controls;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace first_run = oa::ui::first_run;
namespace settings = oa::ui::engine_settings;
namespace gui = oa::ui::gui_layout;
namespace bundles = oa::data::content_bundles;
namespace oaexp = oa::app::package_install::oaexp;

/// The middle dot a seat's label puts between the brain and the level.
constexpr std::string_view kDot = " · ";

/// One window the check opens SKIRMISH in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr CheckWindow kWindows[] = {{640, 480}, {1280, 720}};

/// The brain's script. start and save are enough for a seat to be taken.
constexpr std::string_view kScript =
    "local brain = {}\n"
    "local starts = 0\n"
    "function brain.start(game, me, saved)\n"
    "  if type(saved) == \"table\" and type(saved.starts) == \"number\" then\n"
    "    starts = saved.starts\n"
    "  end\n"
    "  starts = starts + 1\n"
    "end\n"
    "function brain.save()\n"
    "  return { starts = starts }\n"
    "end\n"
    "return brain\n";

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("brain seat check: " + std::string(what));
}

/// Writes a file whole.
///
/// @param file the file
/// @param bytes its bytes
void write_text(const fs::path& file, std::string_view bytes) {
    if (!file.parent_path().empty())
        fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Writes a frame as a PNG file.
///
/// @param file the file
/// @param frame the frame
void write_png(const fs::path& file, const renderer::Surface& frame) {
    std::vector<uint8_t> bytes;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    require(
        oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &bytes),
        "could not encode " + path_to_utf8(file)
    );
    write_text(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/// Writes one made-up brain the Library lists.
///
/// @param folder the brain's folder
/// @param id its id
/// @param name the name players see
/// @param major the version's first part
/// @param minor the version's second part
/// @param parts how many parts the version writes
/// @param level_id the level's id
/// @param level_name the level's name
/// @param difficulty the level's income
/// @param full_awareness true when the brain sees every unit
/// @param second_level a second level's id, and with it the option
///     Aggression (low or high); empty for neither
/// @param second_name that level's name
void write_brain(
    const fs::path& folder,
    std::string_view id,
    std::string_view name,
    uint16_t major,
    uint16_t minor,
    uint8_t parts,
    std::string_view level_id,
    std::string_view level_name,
    oabrain::Difficulty difficulty,
    bool full_awareness,
    std::string_view second_level = {},
    std::string_view second_name = {}
) {
    oabrain::Manifest manifest;
    manifest.format = oabrain::format_version;
    manifest.id = std::string(id);
    manifest.name = std::string(name);
    manifest.version.major = major;
    manifest.version.minor = minor;
    manifest.version.parts = parts;
    manifest.author.name = "Ridge Testers";
    manifest.description = "A made-up brain.";
    manifest.tags = {"teaching"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    manifest.full_awareness = full_awareness;
    oabrain::Level level;
    level.id = std::string(level_id);
    level.name = std::string(level_name);
    level.difficulty = difficulty;
    manifest.levels.push_back(std::move(level));
    if (!second_level.empty()) {
        oabrain::Level second;
        second.id = std::string(second_level);
        second.name = std::string(second_name);
        second.difficulty = oabrain::Difficulty::hard;
        manifest.levels.push_back(std::move(second));
        // A brain with two levels also has an option, Aggression: low or high.
        oamut::Option aggression;
        aggression.id = "aggression";
        aggression.type = oamut::OptionType::choice;
        aggression.label = "Aggression";
        for (const char* value : {"low", "high"}) {
            oamut::ChoiceValue choice;
            choice.text = value;
            aggression.values.push_back(std::move(choice));
        }
        manifest.options.push_back(std::move(aggression));
    }
    write_text(folder / std::string(oabrain::manifest_file), oabrain::write_manifest(manifest));
    write_text(folder / std::string(oabrain::default_entry), kScript);
}

/// Tells whether two window rectangles share more than a pixel.
///
/// @param first a rectangle
/// @param second another
/// @return true when they overlap
bool overlaps(const kit::Rect& first, const kit::Rect& second) noexcept {
    const int32_t left = std::max(first.x, second.x);
    const int32_t right = std::min(first.x + first.width, second.x + second.width);
    const int32_t top = std::max(first.y, second.y);
    const int32_t bottom = std::min(first.y + first.height, second.y + second.height);
    return right - left > 1 && bottom - top > 1;
}

/// Tells whether a window rectangle lies inside another, allowing one pixel.
///
/// @param inner the rectangle
/// @param outer the room
/// @return true when it does
bool inside_window(const kit::Rect& inner, const kit::Rect& outer) noexcept {
    return inner.x >= outer.x - 1 && inner.y >= outer.y - 1 &&
           inner.x + inner.width <= outer.x + outer.width + 1 &&
           inner.y + inner.height <= outer.y + outer.height + 1;
}

kit::Rect window_rect(const AutomationControl& control) noexcept {
    return {control.window_x, control.window_y, control.window_width, control.window_height};
}

/// Holds standard error for one step, then puts it back.
struct CaughtErrors {
    std::stringstream text{};
    std::streambuf* previous{};

    CaughtErrors() : previous(std::cerr.rdbuf(text.rdbuf())) {}

    ~CaughtErrors() { std::cerr.rdbuf(previous); }

    CaughtErrors(const CaughtErrors&) = delete;
    CaughtErrors& operator=(const CaughtErrors&) = delete;
};

/// The mean CPU milliseconds of a run of frames.
///
/// @param started the clock at the first frame
/// @param ended the clock after the last frame
/// @param count how many frames ran
/// @return the mean, in milliseconds
[[nodiscard]] double mean_cpu_ms(std::clock_t started, std::clock_t ended, int32_t count) {
    const double ticks = static_cast<double>(ended - started);
    return ticks * 1000.0 / (static_cast<double>(CLOCKS_PER_SEC) * static_cast<double>(count));
}

/// Prints a millisecond count.
///
/// @param milliseconds the count
/// @return the text, with three digits after the point
[[nodiscard]] std::string ms_text(double milliseconds) {
    std::ostringstream text;
    text << std::fixed << std::setprecision(3) << milliseconds;
    return text.str();
}

/// Tells whether a choice lists a bundle.
///
/// @param ids the bundle ids
/// @param id the one to find
/// @return true when it is listed
[[nodiscard]] bool lists_id(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& known : ids)
        if (known == id)
            return true;
    return false;
}

/// Runs an action once, and again from the destructor if it has not run.
///
/// A failed step still puts the setup back. The action is a lambda of the
/// check, which can see the runtime's own fields.
template <typename PutBack>
struct ContentRestore {
    PutBack put_back; ///< puts the folder, the keys and the scratch folder back
    bool armed{true}; ///< put_back still has to run

    /// Keeps the action.
    ///
    /// @param action what to run
    explicit ContentRestore(PutBack action) : put_back(std::move(action)) {}

    /// Runs the action once.
    void run() {
        if (!armed)
            return;
        put_back();
        armed = false;
    }

    ~ContentRestore() {
        try {
            run();
        } catch (...) {
        }
    }

    ContentRestore(const ContentRestore&) = delete;
    ContentRestore& operator=(const ContentRestore&) = delete;
};

} // namespace

void Runtime::check_brain_seat_buttons() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();

    const fs::path brains_root = user_folder_ / std::string(brain::brains_folder_name);

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto controls_of = [this] {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        return controls;
    };
    const auto named = [&](std::string_view name) -> std::optional<AutomationControl> {
        for (const AutomationControl& control : controls_of()) {
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        }
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> found = named(name);
        require(found.has_value(), why);
        return *found;
    };
    const auto any_seat = [&] {
        for (const AutomationControl& control : controls_of()) {
            constexpr std::string_view prefix = "oa.brain-seat.";
            if (control.name.compare(0, prefix.size(), prefix) == 0 && control.visible &&
                control.window_width > 0)
                return true;
        }
        return false;
    };
    const auto text_has = [&](std::string_view needle) {
        for (const AutomationControl& control : controls_of())
            if (control.visible && control.text.find(needle) != std::string::npos)
                return true;
        return false;
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
    const auto press_key = [&](SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod mod) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = key;
        event.key.scancode = scancode;
        event.key.mod = mod;
        event.key.down = true;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto park = [&] {
        pointer_at({0, 0}, SDL_EVENT_MOUSE_MOTION, 0);
        frames(1);
    };
    const auto capture = [&] {
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        return presented;
    };
    const auto close_covering = [&] {
        dialogs::reset_dialogs();
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(question);
        if (LayerScreen* welcome = oa_layer().find("first-run"))
            oa_layer().close(welcome);
        if (LayerScreen* library_screen = oa_layer().find("library"))
            oa_layer().close(library_screen);
        if (LayerScreen* brain_picker = oa_layer().find(brains::screen_name))
            oa_layer().close(brain_picker);
        if (LayerScreen* mutator_picker = oa_layer().find(mutators::screen_name))
            oa_layer().close(mutator_picker);
        frames(1);
    };
    const auto open_skirmish = [&](std::string_view on) {
        load(Screen::main_menu);
        frames(2);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(entry::resource_name(entry::Button::skirmish));
        frames(2);
        close_covering();
        require(
            screen_ == Screen::skirmish, std::string("SKIRMISH did not open") + std::string(on)
        );
    };
    const auto gadget_index = [this](std::string_view name) {
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at)
            if (tdf_names_equal(gadgets[at].common.name, name))
                return static_cast<int32_t>(at);
        return int32_t{-1};
    };
    const auto difficulty_word = [this] {
        const auto found = widget_text_stages_.find("Difficulty");
        std::size_t stage = 1;
        if (found != widget_text_stages_.end() && found->second <= 2)
            stage = static_cast<std::size_t>(found->second);
        else if (preferences_.difficulty <= 2)
            stage = static_cast<std::size_t>(preferences_.difficulty);
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            if (!tdf_names_equal(gadgets[at].common.name, "Difficulty"))
                continue;
            const auto* fields = std::get_if<gui::ButtonFields>(&gadgets[at].fields);
            if (fields == nullptr)
                break;
            if (std::optional<std::string> translated =
                    renderer::translated_stage_caption(*fields, stage))
                return *translated;
            return std::string(renderer::staged_caption(fields->text, stage));
        }
        return std::string();
    };
    const auto seat_focused = [&](std::string_view name) {
        const std::optional<AutomationControl> button = named(name);
        return button.has_value() && button->focused && frontend_focus() < 0;
    };

    for (const CheckWindow& window : kWindows) {
        double before_seat_ms = 0;
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        open_skirmish(on);
        require(!any_seat(), "a seat button shows before any brain is written" + on);
        // A previous size saved Player2 as a computer when its match started,
        // and entering the menu reads that back. Open hides the row, which is
        // the frame this size keeps.
        for (int32_t step = 0;
             step < 3 && skirmish_settings_.slots.size() > 2 &&
             skirmish_settings_.slots[2].controller != entry::controller::disabled;
             ++step) {
            exercise_click("Player2");
            frames(2);
        }
        require(
            skirmish_settings_.slots.size() > 2 &&
                skirmish_settings_.slots[2].controller == entry::controller::disabled,
            "Player2 did not start Open" + on
        );
        park();
        const int32_t opening_focus = frontend_focus();
        const std::string opening_word = difficulty_word();
        require(!opening_word.empty(), "Difficulty has no word" + on);
        const renderer::Surface kept = capture();
        require(
            kept.width == static_cast<uint32_t>(window.width) &&
                kept.height == static_cast<uint32_t>(window.height),
            "the first frame is not the window's size" + on
        );
        std::cout << "brain seat check: kept a frame before any brain was written" << on << "\n";

        // No brain is installed, so no computer seat button is on the screen.
        if (window.width == 640) {
            frames(2);
            const std::clock_t started = std::clock();
            frames(120);
            before_seat_ms = mean_cpu_ms(started, std::clock(), 120);
        }

        write_brain(
            brains_root / "steady-commander",
            "steady-commander",
            "Steady Commander",
            1,
            0,
            2,
            "firm",
            "Firm",
            oabrain::Difficulty::medium,
            false,
            "bold",
            "Bold"
        );
        write_brain(
            brains_root / "ridge-raider",
            "ridge-raider",
            "Ridge Raider",
            1,
            0,
            2,
            "raid",
            "Raid",
            oabrain::Difficulty::medium,
            false
        );
        write_brain(
            brains_root / "lantern-scout",
            "lantern-scout",
            "Lantern Scout",
            0,
            3,
            2,
            "scout",
            "Scout",
            oabrain::Difficulty::easy,
            true
        );
        brain::Seat remembered;
        remembered.place = 1;
        remembered.brain = "steady-commander";
        remembered.level = "firm";
        brain::Seats memory;
        memory.seats.push_back(std::move(remembered));
        preference_values_[std::string(settings::key::brains_skirmish)] = brain::seats_text(memory);
        preferences_dirty_ = true;
        save_preferences();
        open_skirmish(on);

        const AutomationControl seat =
            require_named("oa.brain-seat.1", "oa.brain-seat.1 is not shown" + on);
        require(
            seat.text == "Steady Commander" + std::string(kDot) + "Firm",
            "oa.brain-seat.1 does not read Steady Commander · Firm (" + seat.text + ")" + on
        );
        const LayerView seen = oa_layer().view();
        const auto origin = panel_origin();
        const int32_t percent = std::max(seen.scale_percent, int32_t{100});
        const auto gadget_window_of = [&](std::string_view name) {
            const int32_t found = gadget_index(name);
            require(found > 0, std::string(name) + " is not on the screen" + on);
            const gui::CommonFields& common =
                resources_.layout.gadgets[static_cast<std::size_t>(found)].common;
            const kit::Rect frame{
                static_cast<int32_t>(origin.x) + common.x,
                static_cast<int32_t>(origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            return kit::to_canvas(frame_to_points(seen, frame), percent);
        };
        const kit::Rect player = gadget_window_of("Player1");
        const kit::Rect button = window_rect(seat);
        const kit::Rect picture{
            seen.picture.x, seen.picture.y, seen.picture.width, seen.picture.height
        };
        const int32_t slack = 2 * percent / 100 + 2;
        // The notch is the Player button's right end, inside its two-pixel edge.
        const int32_t right_gap = (player.x + player.width) - (button.x + button.width);
        require(inside_window(button, picture), "the notch leaves the frame" + on);
        require(inside_window(button, player), "the notch leaves Player1" + on);
        require(
            right_gap >= 0 && right_gap <= slack + 2 * percent / 100,
            "the notch is not at Player1's right end (" + std::to_string(right_gap) +
                " window pixels from it)" + on
        );
        require(
            button.width * 4 <= player.width, "the notch takes more than a quarter of Player1" + on
        );
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            const gui::CommonFields& common = gadgets[at].common;
            if (common.width <= 0 || common.height <= 0 || tdf_names_equal(common.name, "Player1"))
                continue;
            const kit::Rect frame{
                static_cast<int32_t>(origin.x) + common.x,
                static_cast<int32_t>(origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            const kit::Rect gadget_window = kit::to_canvas(frame_to_points(seen, frame), percent);
            require(!overlaps(button, gadget_window), "the notch meets " + common.name + on);
        }
        std::cout << "brain seat check: oa.brain-seat.1 reads Steady Commander · Firm, at "
                     "Player1's right end, inside the frame, meeting no other gadget"
                  << on << "\n";

        // One click from Open makes a computer. A click on a row that is
        // already a computer turns it Open and hides the seat button.
        for (int32_t step = 0;
             step < 3 && skirmish_settings_.slots.size() > 2 &&
             skirmish_settings_.slots[2].controller != entry::controller::computer;
             ++step) {
            exercise_click("Player2");
            frames(2);
        }
        require(
            skirmish_settings_.slots.size() > 2 &&
                skirmish_settings_.slots[2].controller == entry::controller::computer,
            "Player2 did not become a computer" + on
        );
        const std::string built_in = "Built-in" + std::string(kDot) + difficulty_word();
        const AutomationControl second =
            require_named("oa.brain-seat.2", "oa.brain-seat.2 is not shown" + on);
        require(
            second.text == built_in,
            "oa.brain-seat.2 does not read the built-in player and Difficulty (" + second.text +
                ")" + on
        );
        if (window.width == 640) {
            frames(2);
            const uint64_t walks_at = copied_content_bundles_walks();
            const std::clock_t started = std::clock();
            frames(120);
            const uint64_t walked = copied_content_bundles_walks() - walks_at;
            const double seated_ms = mean_cpu_ms(started, std::clock(), 120);
            std::cout << "brain seat check: 120 frames before a computer seat, mean "
                      << ms_text(before_seat_ms) << " ms; 120 frames with two computer seats, mean "
                      << ms_text(seated_ms) << " ms, folder walks " << walked << "\n";
            require(walked == 0, "the game folder was read while two computer seats showed");
            require(
                seated_ms < 5.0,
                "two computer seats took " + ms_text(seated_ms) + " ms of CPU a frame"
            );

            const bundles::ContentChoice original = content_choice();
            require(
                options_.content.empty(),
                "the run names content for this start, so a copied bundle cannot join the choice"
            );
            const std::string base_key{settings::key::base_game};
            const std::string prefix{settings::key::expansions_prefix};
            std::vector<std::pair<std::string, std::string>> saved_keys;
            for (const auto& entry : preference_values_) {
                if (entry.first == base_key || (entry.first.size() >= prefix.size() &&
                                                entry.first.compare(0, prefix.size(), prefix) == 0))
                    saved_keys.emplace_back(entry.first, entry.second);
            }
            const fs::path scratch{"G18-content"};
            const fs::path saved_folder = options_.game_dir;
            {
                std::error_code failure;
                fs::remove_all(scratch, failure);
                failure.clear();
                fs::create_directories(
                    scratch / std::string(bundles::managed_games_folder), failure
                );
                require(!failure, "the scratch game folder was not made");
                failure.clear();
                fs::create_directories(
                    scratch / std::string(bundles::managed_expansions_folder), failure
                );
                require(!failure, "the scratch expansions folder was not made");
            }
            // The lambda is the check's own, so it can put the private fields back.
            ContentRestore hold([&]() {
                options_.game_dir = saved_folder;
                std::vector<std::string> stale;
                for (const auto& stored : preference_values_) {
                    if (stored.first == base_key ||
                        (stored.first.size() >= prefix.size() &&
                         stored.first.compare(0, prefix.size(), prefix) == 0))
                        stale.push_back(stored.first);
                }
                for (const std::string& stale_key : stale)
                    preference_values_.erase(stale_key);
                for (const auto& stored : saved_keys)
                    preference_values_[stored.first] = stored.second;
                std::error_code removed;
                fs::remove_all(scratch, removed);
            });
            std::vector<std::string> drop_keys;
            for (const auto& entry : preference_values_) {
                if (entry.first == base_key || (entry.first.size() >= prefix.size() &&
                                                entry.first.compare(0, prefix.size(), prefix) == 0))
                    drop_keys.push_back(entry.first);
            }
            for (const std::string& key : drop_keys)
                preference_values_.erase(key);
            options_.game_dir = scratch / std::string(bundles::managed_games_folder) / "totala";

            const bundles::ContentChoice empty_choice = content_choice();
            require(
                !lists_id(empty_choice.expansions, "music"), "music is chosen before it is copied"
            );
            const uint64_t generation = content_bundles_generation();
            const uint64_t walks_held = copied_content_bundles_walks();
            const bundles::ContentChoice held_again = content_choice();
            require(
                copied_content_bundles_walks() == walks_held, "the kept bundles were read again"
            );
            require(
                !lists_id(held_again.expansions, "music"), "music appeared without being copied"
            );

            const bundles::Bundle* music = bundles::find_bundle("music");
            require(
                music != nullptr && !music->extends.empty() && !music->files.empty(),
                "music has no files to copy"
            );
            bundles::ContentRecord record;
            record.bundle_id = std::string(music->id);
            record.source = "G18";
            record.files.reserve(music->files.size());
            for (const bundles::ContentFile& file : music->files) {
                bundles::ContentRecordFile item;
                item.copy_name = std::string(file.copy_name);
                item.size = file.size;
                item.modified = 1;
                record.files.push_back(std::move(item));
            }
            const fs::path music_dir = scratch / std::string(bundles::managed_expansions_folder) /
                                       std::string(music->extends[0]) / std::string(music->id);
            std::error_code made;
            fs::create_directories(music_dir, made);
            require(!made, "the music folder was not made");
            std::string record_error;
            require(
                write_content_record(
                    music_dir / std::string(bundles::content_record_name), record, &record_error
                ),
                record_error.empty() ? std::string("the music record was not written")
                                     : record_error
            );
            require(
                content_bundles_generation() > generation,
                "copying music did not raise the generation"
            );
            const bundles::ContentChoice with_music = content_choice();
            require(
                lists_id(with_music.expansions, "music"),
                "music was copied and is not in the choice"
            );

            const uint64_t after_copy = content_bundles_generation();
            oa::app::package_install::Problem problem;
            require(
                oaexp::uninstall_expansion(scratch, "music", problem),
                problem.detail.empty() ? std::string("music was not removed") : problem.detail
            );
            require(
                content_bundles_generation() > after_copy,
                "removing music did not raise the generation"
            );
            const bundles::ContentChoice gone = content_choice();
            require(
                !lists_id(gone.expansions, "music"),
                "music stayed in the choice after it was removed"
            );

            hold.run();
            const bundles::ContentChoice back = content_choice();
            require(
                back.base_game == original.base_game && back.expansions == original.expansions &&
                    back.error == original.error,
                "the content choice did not return to the one the setup had"
            );
            std::cout << "brain seat check: copying music raised the generation and the choice "
                         "followed; removing it did the same\n";
        }
        exercise_click(skirmish::resource_name(skirmish::Button::difficulty));
        frames(2);
        const std::string changed = "Built-in" + std::string(kDot) + difficulty_word();
        const AutomationControl stepped =
            require_named("oa.brain-seat.2", "oa.brain-seat.2 left after Difficulty" + on);
        require(
            stepped.text == changed && stepped.text != built_in,
            "cycling Difficulty did not change oa.brain-seat.2 (" + stepped.text + ")" + on
        );
        std::cout << "brain seat check: Player2 reads " << built_in
                  << ", and Difficulty changes it to " << changed << on << "\n";
        bool restored = false;
        for (int32_t step = 0; step < 6; ++step) {
            if (difficulty_word() == opening_word) {
                restored = true;
                break;
            }
            exercise_click(skirmish::resource_name(skirmish::Button::difficulty));
            frames(1);
        }
        require(restored, "Difficulty did not return to " + opening_word + on);

        const int32_t player_focus = gadget_index("Player1");
        require(player_focus > 0, "Player1 is not on the screen" + on);
        set_frontend_focus(player_focus);
        frames(1);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(
            seat_focused("oa.brain-seat.1"), "Tab from Player1 did not focus oa.brain-seat.1" + on
        );
        require(frontend_focus() < 0, "the GUI kept a focus ring on the notch" + on);
        std::cout << "brain seat check: Tab from Player1 focuses oa.brain-seat.1 and the GUI's "
                     "focus is none"
                  << on << "\n";
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(!seat_focused("oa.brain-seat.1"), "Tab did not move on from oa.brain-seat.1" + on);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_SHIFT);
        frames(1);
        require(
            seat_focused("oa.brain-seat.1"), "Shift+Tab did not return to oa.brain-seat.1" + on
        );
        std::cout << "brain seat check: Tab moves on and Shift+Tab returns" << on << "\n";

        require(
            oa_layer().take_pad_action(pad::Action::library_search), "Y did not open Mutators" + on
        );
        frames(1);
        require(oa_layer().find(mutators::screen_name) != nullptr, "Y left Mutators closed" + on);
        click_named("oa.mutators.cancel");
        require(
            oa_layer().find(mutators::screen_name) == nullptr, "CANCEL left Mutators open" + on
        );
        std::cout << "brain seat check: Y still opens Mutators" << on << "\n";

        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        frames(1);
        const AutomationControl steady = require_named(
            "oa.brain-menu.row.steady-commander", "Return did not open the menu" + on
        );
        require(steady.checked, "the menu does not tick Steady Commander" + on);
        require(
            steady.text == "Steady Commander" + std::string(kDot) + "Firm",
            "the menu's row does not read Steady Commander · Firm (" + steady.text + ")" + on
        );
        require(
            named("oa.brain-menu.row.built-in").has_value() &&
                named("oa.brain-menu.row.lantern-scout").has_value() &&
                named("oa.brain-menu.every-seat").has_value() &&
                named("oa.brain-menu.get-brains").has_value(),
            "the menu lacks the built-in player, a brain, every seat or Get more brains" + on
        );
        require(oa_layer().find(brains::screen_name) == nullptr, "the menu opened the picker" + on);
        std::cout << "brain seat check: Return opens the brain menu on Steady Commander at Firm"
                  << on << "\n";
        park();
        const renderer::Surface menu_frame = capture();
        const std::string menu_name = "brain-seats-menu-" + size + ".png";
        write_png(fs::path(menu_name), menu_frame);
        std::cout << "brain seat check: wrote " << menu_name << "\n";
        click_named("oa.brain-menu.level.next");
        const AutomationControl bold =
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after the next level" + on);
        require(
            bold.text == "Steady Commander" + std::string(kDot) + "Bold",
            "the next level did not give Bold (" + bold.text + ")" + on
        );
        require(
            named("oa.brain-menu.row.steady-commander").has_value(),
            "the next level closed the menu" + on
        );
        click_named("oa.brain-menu.level.previous");
        require(
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after the level before" + on)
                    .text == "Steady Commander" + std::string(kDot) + "Firm",
            "the level before did not give Firm back" + on
        );
        std::cout << "brain seat check: the menu's arrows step the seat to Bold and back to Firm"
                  << on << "\n";

        // Steady Commander has an option: Options... opens the picker on the
        // seat, and USE THIS keeps the brain with its options.
        require(
            named("oa.brain-menu.options").has_value(),
            "the menu offers no Options... for Steady Commander" + on
        );
        click_named("oa.brain-menu.options");
        require(
            oa_layer().find(brains::screen_name) != nullptr,
            "Options... did not open the picker" + on
        );
        require(
            !named("oa.brain-menu.row.built-in").has_value(), "Options... left the menu open" + on
        );
        click_named("oa.brains.use");
        require(
            oa_layer().find(brains::screen_name) == nullptr,
            "USE THIS left the picker open after Options..." + on
        );
        bool kept_options = false;
        for (const brain::Seat& stored : setup_brain_seats(brain::Setup::skirmish).seats)
            if (stored.place == 1 && stored.brain == "steady-commander")
                for (const auto& option : stored.options)
                    kept_options = kept_options || option.first == "aggression";
        require(kept_options, "the seat did not keep Steady Commander's options" + on);
        require(
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after Options..." + on).text ==
                "Steady Commander" + std::string(kDot) + "Firm",
            "Options... changed the seat's brain or level" + on
        );
        std::cout << "brain seat check: Options... opens the picker on Steady Commander, and USE "
                     "THIS keeps its options"
                  << on << "\n";
        click_named("oa.brain-seat.1");
        require(
            named("oa.brain-menu.row.built-in").has_value(),
            "the notch did not open the menu after Options..." + on
        );
        click_named("oa.brain-menu.row.built-in");
        require(
            !named("oa.brain-menu.row.built-in").has_value(),
            "choosing the built-in player left the menu open" + on
        );
        const std::string built_back = "Built-in" + std::string(kDot) + difficulty_word();
        const AutomationControl after_use =
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after the built-in player" + on);
        require(
            after_use.text == built_back,
            "the menu did not put the built-in player on the seat (" + after_use.text + ")" + on
        );
        std::cout << "brain seat check: the built-in player gives " << after_use.text << on << "\n";

        // The right button on the Player button opens the menu; Escape closes it.
        const kit::Rect player_window = gadget_window_of("Player1");
        const layout::Point on_words{
            player_window.x + player_window.width / 3, player_window.y + player_window.height / 2
        };
        pointer_at(on_words, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(on_words, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_RIGHT);
        pointer_at(on_words, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_RIGHT);
        frames(1);
        require(
            named("oa.brain-menu.row.built-in").has_value(),
            "the right button on Player1 did not open the menu" + on
        );
        require(
            skirmish_settings_.slots[1].controller == entry::controller::computer,
            "the right button turned Player1 from a computer" + on
        );
        require(
            !named("oa.brain-menu.options").has_value(),
            "the menu offers Options... for the built-in player" + on
        );
        press_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE);
        frames(1);
        require(!named("oa.brain-menu.row.built-in").has_value(), "Escape left the menu open" + on);
        std::cout << "brain seat check: the right button on Player1 opens the menu and Escape "
                     "closes it"
                  << on << "\n";

        click_named("oa.brain-seat.1");
        require(
            named("oa.brain-menu.every-seat").has_value(),
            "the notch did not open the menu again" + on
        );
        click_named("oa.brain-menu.every-seat");
        const AutomationControl every =
            require_named("oa.brain-menu.every-seat", "every computer seat left" + on);
        require(every.checked, "every computer seat is not ticked" + on);
        click_named("oa.brain-menu.row.lantern-scout");
        require(
            !named("oa.brain-menu.row.lantern-scout").has_value(),
            "choosing Lantern Scout left the menu open" + on
        );
        const std::string lantern = "Lantern Scout" + std::string(kDot) + "Scout";
        const AutomationControl first_lantern =
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after Lantern Scout" + on);
        const AutomationControl second_lantern =
            require_named("oa.brain-seat.2", "oa.brain-seat.2 left after Lantern Scout" + on);
        require(
            first_lantern.text == lantern && second_lantern.text == lantern,
            "every computer seat did not set both rows (" + first_lantern.text + ", " +
                second_lantern.text + ")" + on
        );
        std::cout << "brain seat check: Lantern Scout with every computer seat sets both rows" << on
                  << "\n";

        const layout::Point rest{
            player_window.x + player_window.width / 3, player_window.y + player_window.height / 2
        };
        pointer_at(rest, SDL_EVENT_MOUSE_MOTION, 0);
        SDL_Delay(450);
        frames(4);
        require(text_has("Version: 0.3"), "the card does not read Version: 0.3" + on);
        require(text_has("Level: Scout"), "the card does not read Level: Scout" + on);
        require(
            text_has("Full awareness: Sees every unit on the map"),
            "the card does not read the full-awareness row" + on
        );
        std::cout << "brain seat check: the card reads Version 0.3, Level Scout and Full "
                     "awareness"
                  << on << "\n";
        press_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE);
        frames(2);
        require(!text_has("Version: 0.3"), "Escape left the card open" + on);
        std::cout << "brain seat check: Escape closes the card" << on << "\n";

        park();
        const renderer::Surface shown = capture();
        require(
            shown.width == static_cast<uint32_t>(window.width) &&
                shown.height == static_cast<uint32_t>(window.height),
            "the skirmish frame is not the window's size" + on
        );
        const std::string picture_name = "brain-seats-skirmish-" + size + ".png";
        write_png(fs::path(picture_name), shown);
        std::cout << "brain seat check: wrote " << picture_name << "\n";

        std::string logged;
        {
            CaughtErrors caught;
            exercise_click(skirmish::resource_name(skirmish::Button::start));
            frames(2);
            logged = caught.text.str();
        }
        require(
            screen_ == Screen::match && match_ != nullptr, "START did not start the skirmish" + on
        );
        require(
            logged.find("player 1 plays lantern-scout") != std::string::npos &&
                logged.find("player 2 plays lantern-scout") != std::string::npos,
            "START did not log lantern-scout for players 1 and 2" + on
        );
        require(
            logged.find("player 0 plays") == std::string::npos,
            "START logged a brain for the human seat" + on
        );
        std::cout << "brain seat check: START logs lantern-scout for players 1 and 2" << on << "\n";
        leave_match();
        open_skirmish(on);
        const AutomationControl remembered_first =
            require_named("oa.brain-seat.1", "the remembered seat 1 is not shown" + on);
        const AutomationControl remembered_second =
            require_named("oa.brain-seat.2", "the remembered seat 2 is not shown" + on);
        require(
            remembered_first.text == lantern && remembered_second.text == lantern,
            "SKIRMISH did not remember Lantern Scout (" + remembered_first.text + ", " +
                remembered_second.text + ")" + on
        );
        std::cout << "brain seat check: back on SKIRMISH the seats read as remembered" << on
                  << "\n";

        // The left button on Player2's words is the room's own press: the
        // row turns Open and its seat goes.
        const kit::Rect player2 = gadget_window_of("Player2");
        const layout::Point player2_words{
            player2.x + player2.width / 3, player2.y + player2.height / 2
        };
        pointer_at(player2_words, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(player2_words, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(player2_words, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(2);
        require(
            !named("oa.brain-seat.2").has_value(),
            "the left button on Player2 did not turn the row from a computer" + on
        );
        std::cout << "brain seat check: the left button on Player2's words keeps 3.1c's press" << on
                  << "\n";
        std::error_code removed;
        fs::remove_all(brains_root, removed);
        require(!removed, "the Brains folder could not be removed" + on);
        open_skirmish(on);
        require(!any_seat(), "a seat button remains with no brain installed" + on);
        // Entering the main menu reads the preferences saved when the match
        // started, which still name Player2 as a computer. Cycle that row
        // back to Open so it matches the frame kept before any brain.
        if (skirmish_settings_.slots.size() > 2 &&
            skirmish_settings_.slots[2].controller != entry::controller::disabled) {
            exercise_click("Player2");
            frames(2);
        }
        require(
            skirmish_settings_.slots.size() > 2 &&
                skirmish_settings_.slots[2].controller == entry::controller::disabled,
            "Player2 did not return to Open" + on
        );
        if (frontend_focus() != opening_focus)
            set_frontend_focus(opening_focus);
        require(difficulty_word() == opening_word, "Difficulty did not stay " + opening_word + on);
        park();
        const renderer::Surface again = capture();
        const LayerPointer cursor{{0, 0}, layer_cursor_reach};
        require(
            again.width == kept.width && again.height == kept.height &&
                layer_differences(again, kept, cursor) == 0,
            "the frame differs from the one kept before any brain was written" + on
        );
        std::cout << "brain seat check: no seat button remains and the frame equals the one "
                     "kept before any brain was written"
                  << on << "\n";
    }
}

} // namespace oa::app
