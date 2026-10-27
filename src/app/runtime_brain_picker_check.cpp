// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-brain-picker: the brain picker over the main menu, on a feed of
// made-up brains. At 640x480, 1280x720, 1920x1080 and 2560x1440 it requires
// the size class and the centred window, the rows in name order with the
// built-in player first, Ridge Raider locked with its reason, Firm, a press
// on Calm then USE THIS, the built-in player's Difficulty note with no level
// strip, every computer seat, and CANCEL handing nothing. The keyboard alone
// and the controller alone each reach a level and USE THIS. At 1280x720 the
// header's close mark is taken once. Then the runtime's own feed lists two
// packages written into the check's Brains folder. Each size's opening frame
// is written as brain-picker-<w>x<h>.png in the working folder. The feed
// needs no network.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace brains = oa::ui::brain_picker;
namespace mutators = oa::ui::mutator_picker;
namespace seats = oa::data::brain_library;
namespace layout = oa::ui::display_layout;
namespace dialogs = oa::ui::frontend_dialogs;
namespace oabrain = oa::formats::oabrain;
namespace pad = oa::ui::pad_controls;

/// One window the check opens the picker in.
struct CheckWindow {
    int32_t width{};             ///< the window's width, in pixels
    int32_t height{};            ///< its height
    kit::SizeClass size_class{}; ///< the size class
    int32_t scale{};             ///< window pixels a point
};

/// The windows, in the order the check opens them.
constexpr std::array<CheckWindow, 4> kWindows{{
    {640, 480, kit::SizeClass::compact, 1},
    {1280, 720, kit::SizeClass::large, 1},
    {1920, 1080, kit::SizeClass::large, 1},
    {2560, 1440, kit::SizeClass::large, 2},
}};

/// The rows, built-in first, then each brain by its folded name.
constexpr std::array<std::string_view, 6> kRowOrder{
    "oa.brains.row.built-in",
    "oa.brains.row.lantern-scout",
    "oa.brains.row.quiet-garrison",
    "oa.brains.row.ridge-raider",
    "oa.brains.row.steady-commander",
    "oa.brains.row.tide-keeper",
};

/// The feed's stand-in. The list does not change.
struct Fixture {
    uint64_t generation{1};
};

/// The one fixture. The feed's functions share it.
///
/// @return the fixture
Fixture& fixture() {
    static Fixture held;
    return held;
}

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("brain picker check: " + std::string(what));
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

/// One level.
///
/// @param id the level id
/// @param name the name a player sees
/// @param difficulty the income
/// @return the level
brains::Level make_level(std::string id, std::string name, seats::Difficulty difficulty) {
    brains::Level made;
    made.id = std::move(id);
    made.name = std::move(name);
    made.difficulty = difficulty;
    return made;
}

/// A choice option. The values are also the captions.
///
/// @param id the option id
/// @param label the label
/// @param values the values
/// @param fallback the default
/// @return the option
mutators::OptionSpec choice_option(
    std::string id, std::string label, std::vector<std::string> values, std::string fallback
) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::choice;
    spec.label = std::move(label);
    spec.values = std::move(values);
    spec.captions = spec.values;
    spec.fallback = std::move(fallback);
    return spec;
}

/// A toggle, off then on.
///
/// @param id the option id
/// @param label the label
/// @param fallback the default
/// @return the option
mutators::OptionSpec toggle_option(std::string id, std::string label, std::string fallback) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::toggle;
    spec.label = std::move(label);
    spec.values = {"off", "on"};
    spec.captions = spec.values;
    spec.fallback = std::move(fallback);
    return spec;
}

/// A number option.
///
/// @param id the option id
/// @param label the label
/// @return the option
mutators::OptionSpec number_option(std::string id, std::string label) {
    mutators::OptionSpec spec;
    spec.id = std::move(id);
    spec.type = mutators::OptionSpec::Type::number;
    spec.label = std::move(label);
    spec.least = "0";
    spec.most = "4";
    spec.step = "2";
    spec.fallback = "0";
    return spec;
}

/// Steady Commander. Calm sits after Firm, so Right from Firm lands on it.
///
/// @return the brain
brains::Brain steady() {
    brains::Brain brain;
    brain.id = "steady-commander";
    brain.name = "Steady Commander";
    brain.version = "1.2";
    brain.author = "Ada";
    brain.summary = "Holds the line.";
    brain.description = "A commander that holds the ridge and waits.";
    brain.works_with_text = "Any mod";
    brain.levels = {
        make_level("gentle", "Gentle", seats::Difficulty::easy),
        make_level("firm", "Firm", seats::Difficulty::medium),
        make_level("calm", "Calm", seats::Difficulty::medium),
        make_level("fierce", "Fierce", seats::Difficulty::hard),
    };
    brain.options.push_back(choice_option("aggression", "Aggression", {"low", "high"}, "low"));
    brain.options.push_back(choice_option(
        "signal", "Signal", {"North", "South", "East", "West", "Near", "Far"}, "North"
    ));
    brain.options.push_back(toggle_option("hold", "Hold", "off"));
    brain.options.push_back(number_option("focus", "Focus"));
    return brain;
}

/// Lantern Scout sees the whole map and carries a network.
///
/// @return the brain
brains::Brain lantern() {
    brains::Brain brain;
    brain.id = "lantern-scout";
    brain.name = "Lantern Scout";
    brain.version = "0.4";
    brain.author = "Bea";
    brain.summary = "Scouts ahead.";
    brain.description = "Sees the map.";
    brain.full_awareness = true;
    brain.network_bytes = 1048576;
    brain.levels = {
        make_level("dim", "Dim", seats::Difficulty::easy),
        make_level("bright", "Bright", seats::Difficulty::hard),
    };
    return brain;
}

/// A fitting brain whose name is written in lower case.
///
/// @return the brain
brains::Brain quiet() {
    brains::Brain brain;
    brain.id = "quiet-garrison";
    brain.name = "quiet garrison";
    brain.summary = "Stays home.";
    brain.description = "One level.";
    brain.levels = {make_level("only", "Only", seats::Difficulty::hard)};
    return brain;
}

/// Ridge Raider cannot play this seat.
///
/// @return the brain
brains::Brain ridge() {
    brains::Brain brain;
    brain.id = "ridge-raider";
    brain.name = "Ridge Raider";
    brain.version = "3.0";
    brain.author = "Cara";
    brain.summary = "Raids.";
    brain.description = "Made for one mod.";
    brain.works_with_text = "Ridge";
    brain.unfit = "Made for Ridge";
    brain.levels = {make_level("raid", "Raid", seats::Difficulty::hard)};
    return brain;
}

/// Tide Keeper cannot play this seat.
///
/// @return the brain
brains::Brain tide() {
    brains::Brain brain;
    brain.id = "tide-keeper";
    brain.name = "Tide Keeper";
    brain.summary = "Holds the shore.";
    brain.description = "Needs an expansion.";
    brain.unfit = "Needs Core Contingency";
    brain.levels = {make_level("ebb", "Ebb", seats::Difficulty::easy)};
    return brain;
}

/// The five brains, not in name order. The model sorts them.
///
/// @return the brains
std::vector<brains::Brain> roster() {
    return {steady(), tide(), lantern(), ridge(), quiet()};
}

/// The fixture's brains. The model adds the built-in player.
///
/// @return the brains
std::vector<brains::Brain> fixture_brains(void* /*context*/) {
    return roster();
}

/// The fixture's generation. It does not move.
///
/// @return the generation
uint64_t fixture_generation(void* /*context*/) {
    return fixture().generation;
}

/// One option's value, or empty when the choice does not name it.
///
/// @param choice the choice
/// @param id the option id
/// @return the value
std::string option_value(const brains::Choice& choice, std::string_view id) {
    for (const auto& pair : choice.options)
        if (pair.first == id)
            return pair.second;
    return {};
}

/// Writes one made-up brain: the manifest only. The listing does not need a
/// script, an icon or a network.
///
/// @param folder the brain's folder
/// @param id the brain's id
/// @param name the name a player sees
void write_brain_package(const fs::path& folder, std::string id, std::string name) {
    oabrain::Manifest manifest;
    manifest.format = oabrain::format_version;
    manifest.id = std::move(id);
    manifest.name = std::move(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ada";
    manifest.description = "Holds the line.";
    manifest.tags = {"classic"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    oabrain::Level level;
    level.id = "steady";
    level.name = "Steady";
    level.difficulty = oabrain::Difficulty::easy;
    manifest.levels.push_back(std::move(level));
    write_text(folder / "oabrain.yaml", oabrain::write_manifest(manifest));
}

} // namespace

void Runtime::install_brain_picker_check_feed() {
    BrainPickerFeed feed;
    feed.context = &fixture();
    feed.brains = fixture_brains;
    feed.generation = fixture_generation;
    set_brain_picker_feed(feed);
}

void Runtime::check_brain_picker() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto controls_of = [this]() {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        return controls;
    };
    const auto named = [&](std::string_view name) -> std::optional<AutomationControl> {
        for (const AutomationControl& control : controls_of())
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(why));
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
    const auto tap = [&](SDL_Keycode code) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = code;
            event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
            event.key.mod = SDL_KMOD_NONE;
            event.key.down = down;
            dispatch_event(event, running);
            require(running, "a key ended the run");
        }
        frames(1);
    };
    const auto picker_screen = [this]() -> LayerScreen* {
        return oa_layer().find(brains::screen_name);
    };
    const auto list_has = [&](std::string_view needle) {
        LayerScreen* screen = picker_screen();
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
    const auto has_locked = [&] {
        LayerScreen* screen = picker_screen();
        require(screen != nullptr, "the brain picker is not open");
        const kit::DisplayList list = screen->display_list();
        for (const kit::Item& item : list.items)
            if (item.role == kit::Role::locked_fade)
                return true;
        return false;
    };
    const auto focused_name = [&]() -> std::string {
        for (const AutomationControl& control : controls_of())
            if (control.focused)
                return control.name;
        return {};
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
        if (LayerScreen* picker_open = oa_layer().find(brains::screen_name))
            oa_layer().close(picker_open);
        frames(1);
    };
    const auto row_names = [&]() {
        std::vector<std::string> names;
        for (const AutomationControl& control : controls_of()) {
            constexpr std::string_view prefix = "oa.brains.row.";
            if (control.name.compare(0, prefix.size(), prefix) != 0)
                continue;
            if (control.name.find('.', prefix.size()) != std::string::npos)
                continue;
            names.push_back(control.name);
        }
        return names;
    };

    bool used = false;
    bool cancelled = false;
    brains::Choice handed;
    bool handed_every = false;
    const auto open_picker = [&](brains::Choice current, std::string_view on) {
        used = false;
        cancelled = false;
        handed = {};
        handed_every = false;
        BrainPickerRequest request;
        request.setup = seats::Setup::skirmish;
        request.seat_name = "North";
        request.current = std::move(current);
        request.setup_difficulty = seats::Difficulty::medium;
        request.built_in_level_fixed = true;
        request.offer_every_seat = true;
        request.used = [&](brains::Choice choice, bool every_seat) {
            handed = std::move(choice);
            handed_every = every_seat;
            used = true;
        };
        request.cancelled = [&] { cancelled = true; };
        require(open_brain_picker(std::move(request)), "the picker did not open" + std::string(on));
        frames(1);
        require(picker_screen() != nullptr, "the picker is not on the layer" + std::string(on));
    };
    const auto steady_choice = []() {
        brains::Choice current;
        current.brain = "steady-commander";
        current.level = "firm";
        current.options.emplace_back("aggression", "high");
        return current;
    };
    const auto reveal_details = [&](std::string_view on) {
        if (named("oa.brains.details").has_value())
            click_named("oa.brains.details");
        require(picker_screen() != nullptr, "DETAILS closed the picker" + std::string(on));
    };

    install_brain_picker_check_feed();

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        const bool compact = window.size_class == kit::SizeClass::compact;
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open" + on);
        frames(2);
        close_covering();
        require(!oa_layer().modal_shown(false), "a modal screen is still up" + on);
        require(dialogs::dialog_count() == 0, "a front-end dialog is still up" + on);
        require(
            oa_layer().screen_class() == window.size_class &&
                oa_layer().screen_scale() == window.scale,
            "the layer is not " + std::string(size_class_name(window.size_class)) + " at " +
                std::to_string(window.scale) + "x" + on
        );
        std::cout << "brain picker check: " << size << " is " << size_class_name(window.size_class)
                  << " at " << window.scale << "x\n";

        install_brain_picker_check_feed();
        open_picker(steady_choice(), on);

        const LayerPlacement placed = oa_layer().placement_of(brains::screen_name);
        const LayerView seen = oa_layer().view();
        const kit::Point expected = brains::window_size(seen.frame);
        require(
            placed.points_width == expected.x && placed.points_height == expected.y,
            "the picker is not the size class's window" + on
        );
        const int32_t centre_x = placed.shown.x + placed.shown.width / 2;
        const int32_t centre_y = placed.shown.y + placed.shown.height / 2;
        const int32_t room_x = seen.room.x + seen.room.width / 2;
        const int32_t room_y = seen.room.y + seen.room.height / 2;
        const auto apart = [](int32_t left, int32_t right) {
            return left > right ? left - right : right - left;
        };
        require(
            apart(centre_x, room_x) <= 2 && apart(centre_y, room_y) <= 2,
            "the picker is not centred" + on
        );
        std::cout << "brain picker check: " << placed.points_width << "x" << placed.points_height
                  << " and centred" << on << "\n";

        const std::vector<std::string> rows = row_names();
        require(rows.size() == kRowOrder.size(), "the row count is not six" + on);
        for (std::size_t place = 0; place < kRowOrder.size(); ++place)
            require(rows[place] == kRowOrder[place], "the rows are not in name order" + on);
        std::vector<std::pair<int32_t, std::string>> visible_rows;
        for (const AutomationControl& control : controls_of()) {
            constexpr std::string_view prefix = "oa.brains.row.";
            if (control.name.compare(0, prefix.size(), prefix) != 0)
                continue;
            if (!control.visible || control.window_width <= 0 || control.window_height <= 0)
                continue;
            visible_rows.emplace_back(control.window_y, control.name);
        }
        std::stable_sort(
            visible_rows.begin(), visible_rows.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            }
        );
        std::size_t expected_at = 0;
        for (const auto& visible : visible_rows) {
            while (expected_at < kRowOrder.size() && kRowOrder[expected_at] != visible.second)
                ++expected_at;
            require(expected_at < kRowOrder.size(), "a visible row is out of name order" + on);
            ++expected_at;
        }
        std::cout << "brain picker check: rows are built-in, Lantern Scout, quiet garrison, "
                     "Ridge Raider, Steady Commander, Tide Keeper"
                  << on << "\n";

        require(list_has("Made for Ridge"), "Ridge Raider does not read Made for Ridge" + on);
        require(has_locked(), "no row is locked" + on);
        std::cout << "brain picker check: Ridge Raider is locked and reads Made for Ridge" << on
                  << "\n";

        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the frame is not the window's size" + on
        );
        write_png(fs::path("brain-picker-" + size + ".png"), presented);
        std::cout << "brain picker check: wrote brain-picker-" << size << ".png\n";

        if (compact)
            reveal_details(on);
        const AutomationControl level =
            require_named("oa.brains.level", "the level strip is not shown" + on);
        require(level.text.find("Firm") != std::string::npos, "the level does not read Firm" + on);
        std::cout << "brain picker check: level reads \"" << level.text << "\"" << on << "\n";

        click_named("oa.brains.level.calm");
        click_named("oa.brains.use");
        require(used && !cancelled, "USE THIS did not hand the choice" + on);
        require(picker_screen() == nullptr, "USE THIS left the picker open" + on);
        require(handed.brain == "steady-commander", "USE THIS did not hand Steady Commander" + on);
        require(handed.level == "calm", "USE THIS did not hand calm" + on);
        require(option_value(handed, "aggression") == "high", "aggression was not still high" + on);
        require(!handed_every, "USE THIS handed every computer seat" + on);
        std::cout << "brain picker check: USE THIS handed Steady Commander at calm, "
                     "aggression high, every seat off"
                  << on << "\n";

        open_picker(steady_choice(), on);
        click_named("oa.brains.row.built-in");
        reveal_details(on);
        require(list_has("Set by"), "the Difficulty note is not shown" + on);
        require(
            !named("oa.brains.level").has_value(), "the built-in player shows a level strip" + on
        );
        for (const AutomationControl& control : controls_of())
            require(control.name != "oa.brains.level", "the level strip is still listed" + on);
        click_named("oa.brains.every-seat.on");
        const AutomationControl every =
            require_named("oa.brains.every-seat", "every computer seat is not shown" + on);
        require(every.checked, "every computer seat is not on" + on);
        click_named("oa.brains.use");
        require(used && !cancelled, "USE THIS did not hand the built-in player" + on);
        require(handed.brain.empty(), "USE THIS did not hand the built-in player" + on);
        require(handed_every, "USE THIS did not hand every computer seat" + on);
        require(handed.options.empty(), "the built-in player was handed options" + on);
        require(
            handed.level == "easy" || handed.level == "medium" || handed.level == "hard",
            "the built-in level is not a difficulty" + on
        );
        std::cout << "brain picker check: the built-in player shows the Difficulty note, and "
                     "USE THIS handed every computer seat"
                  << on << "\n";

        open_picker(steady_choice(), on);
        click_named("oa.brains.cancel");
        require(cancelled && !used, "CANCEL handed a choice" + on);
        require(picker_screen() == nullptr, "CANCEL left the picker open" + on);
        std::cout << "brain picker check: CANCEL handed nothing" << on << "\n";

        if (window.width == 1280 && window.height == 720) {
            open_picker(steady_choice(), on);
            click_named("oa.brains.close");
            require(cancelled && !used, "the close mark handed a choice" + on);
            require(picker_screen() == nullptr, "the close mark left the picker open" + on);
            std::cout << "brain picker check: the close mark handed nothing" << on << "\n";
        }

        open_picker(steady_choice(), on);
        bool list_focused = false;
        for (int32_t presses = 0; presses < 24 && !list_focused; ++presses) {
            tap(SDLK_TAB);
            list_focused = focused_name() == "oa.brains.list";
        }
        require(list_focused, "Tab did not focus the list" + on);
        tap(SDLK_RETURN);
        require(focused_name() == "oa.brains.level", "Enter did not focus the level" + on);
        tap(SDLK_RIGHT);
        const AutomationControl keyed =
            require_named("oa.brains.level", "the level strip left after Right" + on);
        require(keyed.text.find("Calm") != std::string::npos, "Right did not reach Calm" + on);
        tap(SDLK_SPACE);
        const AutomationControl held =
            require_named("oa.brains.level", "Space left the level strip" + on);
        require(held.text.find("Calm") != std::string::npos, "Space changed the level" + on);
        require(focused_name() == "oa.brains.level", "Space left the level" + on);
        bool use_focused = false;
        for (int32_t presses = 0; presses < 24 && !use_focused; ++presses) {
            tap(SDLK_TAB);
            use_focused = focused_name() == "oa.brains.use";
        }
        require(use_focused, "Tab did not focus USE THIS" + on);
        tap(SDLK_RETURN);
        require(used && !cancelled, "the keyboard did not hand the choice" + on);
        require(
            handed.brain == "steady-commander", "the keyboard did not hand Steady Commander" + on
        );
        require(handed.level == "calm", "the keyboard did not hand calm" + on);
        require(
            option_value(handed, "aggression") == "high",
            "the keyboard did not keep aggression high" + on
        );
        std::cout << "brain picker check: the keyboard reached Calm and USE THIS" << on << "\n";

        open_picker(steady_choice(), on);
        require(
            oa_layer().take_pad_action(pad::Action::focus_next), "the controller did not focus" + on
        );
        frames(1);
        if (compact) {
            require(
                oa_layer().take_pad_action(pad::Action::press),
                "the controller did not open the details" + on
            );
        } else {
            require(
                oa_layer().take_pad_action(pad::Action::focus_next),
                "the controller did not reach the level" + on
            );
        }
        frames(1);
        require(focused_name() == "oa.brains.level", "the controller is not on the level" + on);
        require(
            oa_layer().take_pad_action(pad::Action::focus_right),
            "the controller did not step the level" + on
        );
        frames(1);
        const AutomationControl stepped =
            require_named("oa.brains.level", "the controller left the level strip" + on);
        require(
            stepped.text.find("Calm") != std::string::npos, "the controller did not reach Calm" + on
        );
        require(
            oa_layer().take_pad_action(pad::Action::default_button),
            "the controller did not press USE THIS" + on
        );
        frames(1);
        require(used && !cancelled, "the controller did not hand the choice" + on);
        require(handed.level == "calm", "the controller did not hand calm" + on);
        require(picker_screen() == nullptr, "the controller left the picker open" + on);
        std::cout << "brain picker check: the controller reached Calm and USE THIS" << on << "\n";
    }

    require(oa_layer().find(brains::screen_name) == nullptr, "the picker is still open");
    restore_brain_picker_feed();
    const fs::path brains_folder = user_folder() / std::string(seats::brains_folder_name);
    write_brain_package(brains_folder / "amber-watch", "amber-watch", "Amber Watch");
    write_brain_package(brains_folder / "cedar-guide", "cedar-guide", "Cedar Guide");
    reload_installed_brains();
    open_picker(brains::Choice{}, " on the runtime's own feed");
    const std::vector<std::string> own_rows = row_names();
    require(own_rows.size() >= 3, "the own feed listed fewer than three rows");
    require(
        own_rows[0] == "oa.brains.row.built-in",
        "the own feed did not list the built-in player first"
    );
    require(
        own_rows[1] == "oa.brains.row.amber-watch", "Amber Watch is not under the built-in player"
    );
    require(own_rows[2] == "oa.brains.row.cedar-guide", "Cedar Guide is not under Amber Watch");
    require(list_has("Amber Watch"), "the list does not read Amber Watch");
    require(list_has("Cedar Guide"), "the list does not read Cedar Guide");
    std::cout << "brain picker check: the own feed lists Amber Watch and Cedar Guide\n";
    std::cout << "brain picker check: done\n";
}

} // namespace oa::app
