// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-brain-seat-buttons: each computer row's brain button on SKIRMISH,
// at 640x480 and 1280x720. Before any brain is written a frame is kept.
// Three made-up brains are then written, and the remembered skirmish seats
// name Steady Commander at firm on place 1. The button reads that label,
// sits inside the frame, meets no gadget and stands 4 frame pixels right of
// Energy. Player2 cycled to a computer reads the built-in player and the
// Difficulty button's word, and cycling Difficulty changes it. Tab from
// Energy1 focuses the button with the GUI's focus none, Tab moves on and
// Shift+Tab returns. Return opens the picker on Steady Commander at Firm;
// the built-in player and USE THIS put that word back. Lantern Scout with
// every computer seat sets both rows. The pointer shows its card, and
// Escape closes it. START logs that players 1 and 2 play lantern-scout.
// Back on SKIRMISH the seats read as remembered. With the Brains folder
// removed, no seat button remains and the frame matches the one kept
// before any brain was written. Each size's frame is written as
// brain-seats-skirmish-<w>x<h>.png.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
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
#include <filesystem>
#include <fstream>
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
    bool full_awareness
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

/// The distance between two edges.
///
/// @param left one edge
/// @param right the other
/// @return how far apart they are
int32_t apart(int32_t left, int32_t right) noexcept {
    return left > right ? left - right : right - left;
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
        require(screen_ == Screen::skirmish, std::string("SKIRMISH did not open") + std::string(on));
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
    const auto list_has = [&](std::string_view needle) {
        LayerScreen* screen = oa_layer().find(brains::screen_name);
        require(screen != nullptr, "the brain picker is not open");
        const kit::DisplayList list = screen->display_list();
        for (const kit::Item& item : list.items) {
            if (item.text.find(needle) != std::string::npos)
                return true;
            for (const std::string& line : item.lines)
                if (line.find(needle) != std::string::npos)
                    return true;
        }
        return false;
    };
    const auto reveal_details = [&] {
        if (!named("oa.brains.every-seat.on").has_value() && named("oa.brains.details").has_value())
            click_named("oa.brains.details");
    };
    const auto seat_focused = [&](std::string_view name) {
        const std::optional<AutomationControl> button = named(name);
        return button.has_value() && button->focused && frontend_focus() < 0;
    };

    for (const CheckWindow& window : kWindows) {
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
            false
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
        const kit::Rect energy = gadget_window_of("Energy1");
        const kit::Rect button = window_rect(seat);
        const kit::Rect picture{
            seen.picture.x, seen.picture.y, seen.picture.width, seen.picture.height
        };
        const int32_t slack = percent / 50 + 1;
        const int32_t expected_gap = 4 * percent / 100;
        const int32_t gap = button.x - (energy.x + energy.width);
        require(inside_window(button, picture), "the button leaves the frame" + on);
        require(
            apart(gap, expected_gap) <= slack,
            "the button is not 4 frame pixels right of Energy1 (" + std::to_string(gap) +
                " window pixels, " + std::to_string(expected_gap) + " expected)" + on
        );
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            const gui::CommonFields& common = gadgets[at].common;
            if (common.width <= 0 || common.height <= 0)
                continue;
            const kit::Rect frame{
                static_cast<int32_t>(origin.x) + common.x,
                static_cast<int32_t>(origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            const kit::Rect gadget_window = kit::to_canvas(frame_to_points(seen, frame), percent);
            require(
                !overlaps(button, gadget_window), "the button meets " + common.name + on
            );
        }
        std::cout << "brain seat check: oa.brain-seat.1 reads Steady Commander · Firm, inside "
                     "the frame, meeting no gadget, "
                  << gap << " window pixels right of Energy1" << on << "\n";

        exercise_click("Player2");
        frames(2);
        const std::string built_in = "Built-in" + std::string(kDot) + difficulty_word();
        const AutomationControl second =
            require_named("oa.brain-seat.2", "oa.brain-seat.2 is not shown" + on);
        require(
            second.text == built_in,
            "oa.brain-seat.2 does not read the built-in player and Difficulty (" + second.text +
                ")" + on
        );
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

        const int32_t energy_focus = gadget_index("Energy1");
        require(energy_focus > 0, "Energy1 is not on the screen" + on);
        set_frontend_focus(energy_focus);
        frames(1);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(seat_focused("oa.brain-seat.1"), "Tab from Energy1 did not focus oa.brain-seat.1" + on);
        require(frontend_focus() < 0, "the GUI kept a focus ring on the button" + on);
        std::cout << "brain seat check: Tab from Energy1 focuses oa.brain-seat.1 and the GUI's "
                     "focus is none"
                  << on << "\n";
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(!seat_focused("oa.brain-seat.1"), "Tab did not move on from oa.brain-seat.1" + on);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_SHIFT);
        frames(1);
        require(seat_focused("oa.brain-seat.1"), "Shift+Tab did not return to oa.brain-seat.1" + on);
        std::cout << "brain seat check: Tab moves on and Shift+Tab returns" << on << "\n";

        require(
            oa_layer().take_pad_action(pad::Action::library_search),
            "Y did not open Mutators" + on
        );
        frames(1);
        require(
            oa_layer().find(mutators::screen_name) != nullptr, "Y left Mutators closed" + on
        );
        click_named("oa.mutators.cancel");
        require(
            oa_layer().find(mutators::screen_name) == nullptr, "CANCEL left Mutators open" + on
        );
        std::cout << "brain seat check: Y still opens Mutators" << on << "\n";

        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        frames(1);
        require(
            oa_layer().find(brains::screen_name) != nullptr, "Return did not open the picker" + on
        );
        require(list_has("Steady Commander"), "the picker did not open on Steady Commander" + on);
        if (!named("oa.brains.level").has_value() && named("oa.brains.details").has_value())
            click_named("oa.brains.details");
        const AutomationControl level =
            require_named("oa.brains.level", "the level is not shown" + on);
        require(level.text.find("Firm") != std::string::npos, "the level does not read Firm" + on);
        std::cout << "brain seat check: Return opens the picker on Steady Commander at Firm" << on
                  << "\n";
        click_named("oa.brains.row.built-in");
        click_named("oa.brains.use");
        require(
            oa_layer().find(brains::screen_name) == nullptr, "USE THIS left the picker open" + on
        );
        const std::string built_back = "Built-in" + std::string(kDot) + difficulty_word();
        const AutomationControl after_use =
            require_named("oa.brain-seat.1", "oa.brain-seat.1 left after USE THIS" + on);
        require(
            after_use.text == built_back,
            "USE THIS did not put the built-in player on the seat (" + after_use.text + ")" + on
        );
        std::cout << "brain seat check: the built-in player and USE THIS give " << after_use.text
                  << on << "\n";

        click_named("oa.brain-seat.1");
        require(
            oa_layer().find(brains::screen_name) != nullptr,
            "the seat button did not open the picker again" + on
        );
        click_named("oa.brains.row.lantern-scout");
        reveal_details();
        click_named("oa.brains.every-seat.on");
        const AutomationControl every =
            require_named("oa.brains.every-seat", "every computer seat is not shown" + on);
        require(every.checked, "every computer seat is not on" + on);
        click_named("oa.brains.use");
        require(
            oa_layer().find(brains::screen_name) == nullptr,
            "USE THIS left the picker open after Lantern Scout" + on
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
        std::cout << "brain seat check: Lantern Scout with every computer seat sets both rows"
                  << on << "\n";

        const layout::Point rest{
            first_lantern.window_x + first_lantern.window_width / 2,
            first_lantern.window_y + first_lantern.window_height / 2
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
        std::cout << "brain seat check: START logs lantern-scout for players 1 and 2" << on
                  << "\n";
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

        exercise_click("Player2");
        frames(2);
        require(!named("oa.brain-seat.2").has_value(), "Player2 stayed a computer seat" + on);
        std::error_code removed;
        fs::remove_all(brains_root, removed);
        require(!removed, "the Brains folder could not be removed" + on);
        open_skirmish(on);
        require(!any_seat(), "a seat button remains with no brain installed" + on);
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
