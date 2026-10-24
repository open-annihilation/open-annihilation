// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-brain-campaign: the campaign computer player's brain button on
// NEWGAME and Any Mission, at 640x480 and 1280x720. Before any brain is
// written a frame of each screen is kept. Steady Commander is then written.
// NEWGAME shows one button under Difficulty, inside the frame and meeting
// no gadget, reading the built-in player and the Difficulty button's word.
// Tab reaches it and Up does not. Return opens the picker with no every
// computer seat. Steady Commander at Firm and USE THIS make the label
// "Steady Commander · Firm", and Any Mission shows that same button. Each
// screen's frame is written as brain-campaign-<screen>-<w>x<h>.png. With
// the brain removed, no button remains and each frame matches the one kept
// before any brain was written. The first mission then logs that player 1
// plays steady-commander 1.2 at firm. A save made there, loaded after the
// campaign seat was set back to the built-in player, leaves the campaign
// seat at Steady Commander. A skirmish save leaves that seat as it is.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/persist/save_sections.hpp"
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
namespace persist = oa::data::persist;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace brains = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
namespace kit = oa::ui::kit;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace first_run = oa::ui::first_run;
namespace settings = oa::ui::engine_settings;
namespace gui = oa::ui::gui_layout;

/// The middle dot a seat's label puts between the brain and the level.
constexpr std::string_view kDot = " · ";

/// The campaign button's automation name.
constexpr std::string_view kButton = "oa.brain-seat.campaign";

/// The computer player a campaign mission seats.
constexpr int32_t kPlace = 1;

/// One window the check opens the campaign screens in.
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
        throw std::runtime_error("brain campaign check: " + std::string(what));
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

/// Writes Steady Commander, version 1.2, with the level Firm.
///
/// @param folder the brain's folder
void write_commander(const fs::path& folder) {
    oabrain::Manifest manifest;
    manifest.format = oabrain::format_version;
    manifest.id = "steady-commander";
    manifest.name = "Steady Commander";
    manifest.version.major = 1;
    manifest.version.minor = 2;
    manifest.version.parts = 2;
    manifest.author.name = "Ridge Testers";
    manifest.description = "A made-up brain.";
    manifest.tags = {"teaching"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    oabrain::Level level;
    level.id = "firm";
    level.name = "Firm";
    level.difficulty = oabrain::Difficulty::medium;
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

/// The campaign seat at place 1, when the setup has one.
///
/// @param seats the campaign seats
/// @return that seat, or null
const brain::Seat* campaign_seat(const brain::Seats& seats) noexcept {
    for (const brain::Seat& seat : seats.seats) {
        if (seat.place == kPlace)
            return &seat;
    }
    return nullptr;
}

} // namespace

void Runtime::check_brain_campaign() {
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
    const fs::path commander = brains_root / "steady-commander";

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
    const auto button_shown = [&] { return named(kButton).has_value(); };
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
    const auto wheel_list = [&](float notches) {
        const std::optional<AutomationControl> pane = named("oa.brains.list");
        if (!pane.has_value())
            return false;
        const layout::Point at{
            pane->window_x + pane->window_width / 2, pane->window_y + pane->window_height / 2
        };
        SDL_Event motion =
            window_pointer_event(sdl_.window, sdl_.renderer, SDL_EVENT_MOUSE_MOTION, at, 0);
        dispatch_event(motion, running);
        SDL_Event wheel{};
        wheel.type = SDL_EVENT_MOUSE_WHEEL;
        wheel.wheel.windowID = motion.motion.windowID;
        wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        wheel.wheel.y = notches;
        wheel.wheel.mouse_x = motion.motion.x;
        wheel.wheel.mouse_y = motion.motion.y;
        dispatch_event(wheel, running);
        require(running, "a wheel ended the run");
        frames(1);
        return true;
    };
    const auto show_row = [&](std::string_view row_name) {
        if (named(row_name).has_value())
            return;
        if (named("oa.brains.back").has_value())
            click_named("oa.brains.back");
        for (int32_t turn = 0; turn < 12 && !named(row_name).has_value(); ++turn)
            if (!wheel_list(1.0F))
                break;
        for (int32_t turn = 0; turn < 24 && !named(row_name).has_value(); ++turn)
            if (!wheel_list(-1.0F))
                break;
        require_named(row_name, std::string(row_name) + " is not shown");
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
    const auto open_from_menu = [&](Screen which, std::string_view gadget, std::string_view on) {
        load(Screen::main_menu);
        frames(2);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(gadget);
        frames(2);
        close_covering();
        require(screen_ == which, std::string(gadget) + " did not open" + std::string(on));
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
        LayerScreen* picker = oa_layer().find(brains::screen_name);
        require(picker != nullptr, "the brain picker is not open");
        const kit::DisplayList list = picker->display_list();
        for (const kit::Item& item : list.items) {
            if (item.text.find(needle) != std::string::npos)
                return true;
            for (const std::string& line : item.lines)
                if (line.find(needle) != std::string::npos)
                    return true;
        }
        return false;
    };
    const auto seat_focused = [&] {
        const std::optional<AutomationControl> button = named(kButton);
        return button.has_value() && button->focused && frontend_focus() < 0;
    };
    const auto forget_campaign_seat = [this] {
        preference_values_.erase(std::string(settings::key::brains_campaign));
        preferences_dirty_ = true;
        save_preferences();
        setup_brain_seats(brain::Setup::campaign) = {};
    };
    const auto drop_brains = [&] {
        std::error_code removed;
        fs::remove_all(brains_root, removed);
        require(!removed, "the Brains folder could not be removed");
        reload_installed_brains();
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
        drop_brains();
        forget_campaign_seat();
        const auto kept_preferences = preferences_;
        const auto kept_values = preference_values_;

        open_from_menu(Screen::new_campaign, entry::resource_name(entry::Button::new_campaign), on);
        require(!button_shown(), "a campaign button shows before any brain is written" + on);
        park();
        const int32_t newgame_focus = frontend_focus();
        const std::string opening_word = difficulty_word();
        require(!opening_word.empty(), "Difficulty has no word" + on);
        const renderer::Surface kept_newgame = capture();
        require(
            kept_newgame.width == static_cast<uint32_t>(window.width) &&
                kept_newgame.height == static_cast<uint32_t>(window.height),
            "the first NEWGAME frame is not the window's size" + on
        );
        std::cout << "brain campaign check: kept a frame before any brain was written on NEWGAME"
                  << on << "\n";

        open_from_menu(Screen::any_mission, entry::resource_name(entry::Button::any_mission), on);
        require(!button_shown(), "Any Mission shows a campaign button before any brain" + on);
        park();
        const int32_t mission_focus = frontend_focus();
        const renderer::Surface kept_mission = capture();
        require(
            kept_mission.width == static_cast<uint32_t>(window.width) &&
                kept_mission.height == static_cast<uint32_t>(window.height),
            "the first Any Mission frame is not the window's size" + on
        );
        std::cout << "brain campaign check: kept a frame before any brain was written on Any "
                     "Mission"
                  << on << "\n";

        write_commander(commander);
        reload_installed_brains();
        forget_campaign_seat();
        open_from_menu(Screen::new_campaign, entry::resource_name(entry::Button::new_campaign), on);

        const AutomationControl seat =
            require_named(kButton, std::string(kButton) + " is not shown" + on);
        const std::string built_in = "Built-in" + std::string(kDot) + difficulty_word();
        require(
            seat.text == built_in,
            std::string(kButton) + " does not read the built-in player and Difficulty (" +
                seat.text + ")" + on
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
        const kit::Rect difficulty = gadget_window_of("Difficulty");
        const kit::Rect button = window_rect(seat);
        const kit::Rect picture{
            seen.picture.x, seen.picture.y, seen.picture.width, seen.picture.height
        };
        const int32_t slack = percent / 50 + 1;
        require(inside_window(button, picture), "the button leaves the frame" + on);
        require(
            button.y + slack >= difficulty.y + difficulty.height,
            "the button is not under Difficulty" + on
        );
        require(
            button.x < difficulty.x + difficulty.width + slack &&
                button.x + button.width > difficulty.x - slack,
            "the button is not in Difficulty's column" + on
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
            require(!overlaps(button, gadget_window), "the button meets " + common.name + on);
        }
        std::cout << "brain campaign check: " << kButton << " reads " << seat.text
                  << ", under Difficulty, inside the frame, meeting no gadget" << on << "\n";

        int32_t predecessor = -1;
        for (std::size_t at = 1; at < gadgets.size() && predecessor < 0; ++at) {
            set_frontend_focus(static_cast<int32_t>(at));
            frames(1);
            press_key(SDLK_UP, SDL_SCANCODE_UP, SDL_KMOD_NONE);
            frames(1);
            require(!seat_focused(), "Up focused " + std::string(kButton) + on);
            set_frontend_focus(static_cast<int32_t>(at));
            frames(1);
            press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
            frames(1);
            if (seat_focused())
                predecessor = static_cast<int32_t>(at);
        }
        require(predecessor > 0, "Tab did not reach " + std::string(kButton) + on);
        press_key(SDLK_UP, SDL_SCANCODE_UP, SDL_KMOD_NONE);
        frames(1);
        require(seat_focused(), "Up moved on from " + std::string(kButton) + on);
        std::cout << "brain campaign check: Tab reaches " << kButton << " and Up does not" << on
                  << "\n";

        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        frames(1);
        require(
            oa_layer().find(brains::screen_name) != nullptr, "Return did not open the picker" + on
        );
        require(
            !named("oa.brains.every-seat").has_value() &&
                !named("oa.brains.every-seat.on").has_value(),
            "the picker offers every computer seat" + on
        );
        std::cout << "brain campaign check: Return opens the picker with no every computer seat"
                  << on << "\n";
        show_row("oa.brains.row.steady-commander");
        click_named("oa.brains.row.steady-commander");
        require(list_has("Steady Commander"), "Steady Commander is not in the picker" + on);
        if (!named("oa.brains.level").has_value() && named("oa.brains.details").has_value())
            click_named("oa.brains.details");
        const AutomationControl level =
            require_named("oa.brains.level", "the level is not shown" + on);
        require(level.text.find("Firm") != std::string::npos, "the level does not read Firm" + on);
        click_named("oa.brains.use");
        require(
            oa_layer().find(brains::screen_name) == nullptr, "USE THIS left the picker open" + on
        );
        const std::string chosen = "Steady Commander" + std::string(kDot) + "Firm";
        const AutomationControl after =
            require_named(kButton, std::string(kButton) + " left after USE THIS" + on);
        require(
            after.text == chosen,
            "USE THIS did not give Steady Commander · Firm (" + after.text + ")" + on
        );
        std::cout << "brain campaign check: Steady Commander at Firm and USE THIS gives "
                  << after.text << on << "\n";

        park();
        const renderer::Surface shown = capture();
        require(
            shown.width == static_cast<uint32_t>(window.width) &&
                shown.height == static_cast<uint32_t>(window.height),
            "the NEWGAME frame is not the window's size" + on
        );
        const std::string newgame_picture = "brain-campaign-newgame-" + size + ".png";
        write_png(fs::path(newgame_picture), shown);
        std::cout << "brain campaign check: wrote " << newgame_picture << "\n";

        // From NEWGAME the seat stays in memory. The menu would read the
        // remembered seats, and USE THIS has not remembered this one yet.
        load(Screen::any_mission);
        frames(2);
        close_covering();
        require(screen_ == Screen::any_mission, "Any Mission did not open" + on);
        const AutomationControl mission_button =
            require_named(kButton, "Any Mission does not show " + std::string(kButton) + on);
        require(
            mission_button.text == chosen,
            "Any Mission does not read Steady Commander · Firm (" + mission_button.text + ")" + on
        );
        const LayerView mission_seen = oa_layer().view();
        const auto mission_origin = panel_origin();
        const int32_t mission_percent = std::max(mission_seen.scale_percent, int32_t{100});
        const int32_t mission_slack = mission_percent / 50 + 1;
        const auto mission_gadget = [&](std::string_view name) {
            const int32_t found = gadget_index(name);
            require(found > 0, std::string(name) + " is not on Any Mission" + on);
            const gui::CommonFields& common =
                resources_.layout.gadgets[static_cast<std::size_t>(found)].common;
            const kit::Rect frame{
                static_cast<int32_t>(mission_origin.x) + common.x,
                static_cast<int32_t>(mission_origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            return kit::to_canvas(frame_to_points(mission_seen, frame), mission_percent);
        };
        const kit::Rect mission_difficulty = mission_gadget("Difficulty");
        const kit::Rect mission_rect = window_rect(mission_button);
        require(
            mission_rect.y + mission_slack >= mission_difficulty.y + mission_difficulty.height &&
                mission_rect.x < mission_difficulty.x + mission_difficulty.width + mission_slack &&
                mission_rect.x + mission_rect.width > mission_difficulty.x - mission_slack,
            "Any Mission's button is not under Difficulty" + on
        );
        std::cout << "brain campaign check: Any Mission shows " << kButton << on << "\n";
        park();
        const renderer::Surface mission_shown = capture();
        const std::string mission_picture = "brain-campaign-any-mission-" + size + ".png";
        write_png(fs::path(mission_picture), mission_shown);
        std::cout << "brain campaign check: wrote " << mission_picture << "\n";

        drop_brains();
        preferences_ = kept_preferences;
        preference_values_ = kept_values;
        preference_values_.erase(std::string(settings::key::brains_campaign));
        preferences_dirty_ = true;
        save_preferences();
        setup_brain_seats(brain::Setup::campaign) = {};

        open_from_menu(Screen::new_campaign, entry::resource_name(entry::Button::new_campaign), on);
        require(!button_shown(), "a campaign button remains with no brain installed" + on);
        for (int32_t step = 0; step < 6 && difficulty_word() != opening_word; ++step) {
            exercise_click("Difficulty");
            frames(1);
        }
        require(difficulty_word() == opening_word, "Difficulty did not stay " + opening_word + on);
        if (frontend_focus() != newgame_focus)
            set_frontend_focus(newgame_focus);
        park();
        const renderer::Surface newgame_again = capture();
        const LayerPointer cursor{{0, 0}, layer_cursor_reach};
        require(
            newgame_again.width == kept_newgame.width &&
                newgame_again.height == kept_newgame.height &&
                layer_differences(newgame_again, kept_newgame, cursor) == 0,
            "NEWGAME differs from the frame kept before any brain was written" + on
        );

        open_from_menu(Screen::any_mission, entry::resource_name(entry::Button::any_mission), on);
        require(!button_shown(), "Any Mission keeps a campaign button with no brain" + on);
        if (frontend_focus() != mission_focus)
            set_frontend_focus(mission_focus);
        park();
        const renderer::Surface mission_again = capture();
        require(
            mission_again.width == kept_mission.width &&
                mission_again.height == kept_mission.height &&
                layer_differences(mission_again, kept_mission, cursor) == 0,
            "Any Mission differs from the frame kept before any brain was written" + on
        );
        std::cout << "brain campaign check: no campaign button remains and the frame equals the "
                     "one kept before any brain was written"
                  << on << "\n";
    }

    write_commander(commander);
    reload_installed_brains();
    load(Screen::new_campaign);
    frames(2);
    close_covering();
    require(screen_ == Screen::new_campaign, "NEWGAME did not open for the mission");
    brain::Seat played;
    played.place = kPlace;
    played.brain = "steady-commander";
    played.level = "firm";
    brain::Seats chosen_seats;
    chosen_seats.seats.push_back(std::move(played));
    setup_brain_seats(brain::Setup::campaign) = std::move(chosen_seats);
    if (campaign_labels_.empty())
        discover_campaigns();
    require(!campaign_labels_.empty(), "no campaign is listed");
    load_campaign_missions(selected_campaign_index_);
    require(!campaign_mission_files_.empty(), "the campaign has no mission");
    selected_mission_index_ = 0;
    show_mission_briefing();
    require(screen_ == Screen::briefing, "the briefing did not open (" + status_ + ")");
    std::string logged;
    {
        CaughtErrors caught;
        start_campaign_mission();
        frames(2);
        logged = caught.text.str();
    }
    require(
        screen_ == Screen::match && match_ != nullptr,
        "the first mission did not start (" + status_ + ")"
    );
    require(
        logged.find("player 1 plays steady-commander 1.2 at firm") != std::string::npos,
        "the first mission did not log steady-commander 1.2 at firm"
    );
    std::cout << "brain campaign check: the first mission logs player 1 plays "
                 "steady-commander 1.2 at firm\n";

    const fs::path saves = saves_folder();
    fs::create_directories(saves);
    const fs::path campaign_save = saves / "campaign-seat.sav";
    require(
        save_match_game(
            campaign_save, persist::command_line_description, persist::command_line_game_id
        ),
        "the campaign save failed (" + status_ + ")"
    );
    brain::Seats built_in_seat;
    brain::Seat empty;
    empty.place = kPlace;
    built_in_seat.seats.push_back(std::move(empty));
    setup_brain_seats(brain::Setup::campaign) = std::move(built_in_seat);
    require(load_saved_game(campaign_save), "the campaign save did not load (" + status_ + ")");
    const brain::Seat* restored = campaign_seat(setup_brain_seats(brain::Setup::campaign));
    require(
        restored != nullptr && restored->brain == "steady-commander" && restored->level == "firm",
        "a loaded campaign save did not leave the campaign seat at Steady Commander"
    );
    std::cout << "brain campaign check: a loaded campaign save leaves the campaign seat at "
                 "Steady Commander\n";

    // The main menu does not reload the campaign seat, so the seat the
    // campaign load set is still the one a skirmish save must leave alone.
    leave_match();
    load(Screen::main_menu);
    frames(2);
    close_covering();
    start_benchmark_skirmish();
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    const fs::path skirmish_save = saves / "skirmish-seat.sav";
    require(
        save_match_game(
            skirmish_save, persist::command_line_description, persist::command_line_game_id
        ),
        "the skirmish save failed (" + status_ + ")"
    );
    require(load_saved_game(skirmish_save), "the skirmish save did not load (" + status_ + ")");
    const brain::Seat* still = campaign_seat(setup_brain_seats(brain::Setup::campaign));
    require(
        still != nullptr && still->brain == "steady-commander" && still->level == "firm",
        "a skirmish save changed the campaign seat"
    );
    std::cout << "brain campaign check: a skirmish save leaves the campaign seat as it is\n";

    drop_brains();
}

} // namespace oa::app
