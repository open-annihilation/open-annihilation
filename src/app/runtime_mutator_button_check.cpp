// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-button: Mutators on the skirmish setup and on NEWGAME, at
// 640x480 and 1280x720. The button is one of the room's own on and off
// buttons, with no part of the kit, in line with the room's own buttons:
// on SKIRMISH as large as Previous Menu, its edges, centred between
// Difficulty and Previous Menu in their column, and on NEWGAME as large as
// Previous Menu, its edges, at the head of Difficulty's column between the
// side bar and Set Difficulty. It reads "Mutators", and its text for
// automation counts the set chosen: "Mutators · off", or "Mutators · 2 on";
// with four chosen, the pointer resting on it shows its card, one line a
// mutator and what a press does. Tab and
// Down reach the button, a finger presses it by its reach, the pad's
// badges leave it as it is, Return opens the
// picker on the remembered set, USE THESE replaces the setup's set, Y opens
// that set again, SELMAP keeps it, and START hands it to the match. A
// scratch preferences file holds a remembered skirmish set (Gatehouse,
// Gatehouse Keep) and campaign set (Fast Builders). The feed is the mutator
// picker's check feed. Each size's frame is written as
// mutator-button-skirmish-<w>x<h>.png and mutator-button-newgame-<w>x<h>.png,
// with four chosen as mutator-button-skirmish-four-<w>x<h>.png and
// mutator-button-newgame-four-<w>x<h>.png, and with the card open as
// mutator-button-<screen>-card-<w>x<h>.png. The last step starts a
// skirmish with nothing chosen and logs that V02 was handed an empty set.
// Each frame's lamp is read back: red with none chosen, green with four.

#include "brain_seats_room.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/mutator_picker/text.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace picker = oa::ui::mutator_picker;
namespace place = oa::ui::frontend_state;
namespace pad = oa::ui::pad_controls;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace mutators = oa::data::mutators;
namespace first_run = oa::ui::first_run;
namespace gui = oa::ui::gui_layout;

/// One window the check opens the setups in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr CheckWindow kWindows[] = {{640, 480}, {1280, 720}};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mutator button check: " + std::string(what));
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

/// Writes a frame as a PNG file, as the picker check does.
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

/// Writes one made-up mutator the Library lists: a manifest with a tag and
/// a works-with line for any mod, and a script whose mutate changes
/// nothing. Fast Builders also offers the build_speed choice the picker's
/// feed names.
///
/// @param folder the mutator's folder
/// @param id its id
/// @param name the name players see
/// @param build_speed true when it offers the build-speed choice
void write_package(
    const fs::path& folder, std::string_view id, std::string_view name, bool build_speed
) {
    oa::formats::oamut::Manifest manifest;
    manifest.format = oa::formats::oamut::format_version;
    manifest.id = std::string(id);
    manifest.name = std::string(name);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge";
    manifest.description = "A made-up mutator.";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(oa::formats::oamut::Condition{"*", {}, {}});
    manifest.layers = {oa::formats::oamut::Layer::files};
    if (build_speed) {
        oa::formats::oamut::Option speed;
        speed.id = "build_speed";
        speed.type = oa::formats::oamut::OptionType::choice;
        speed.label = "Build speed";
        speed.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "1"});
        speed.values.push_back(oa::formats::oamut::ChoiceValue{false, {}, "4"});
        speed.value_labels = {"1×", "4×"};
        speed.default_index = 0;
        manifest.options.push_back(std::move(speed));
    }
    write_text(folder / "oamut.yaml", oa::formats::oamut::write_manifest(manifest));
    write_text(folder / "mutator.lua", "function mutate(ctx)\n  return {}\nend\n");
}

/// The canonical text of one installed mutator, options at their defaults.
///
/// @param folder the player's Mutators folder
/// @param id the mutator's id
/// @return the loadout entry's loadout, of that one mutator
mutators::Loadout loadout_of_id(const fs::path& folder, std::string_view id) {
    mutators::MutatorChoice choice;
    choice.target = std::string(id);
    std::string error;
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(choice, folder, &error);
    require(resolved.has_value(), error.empty() ? std::string(id) + " did not resolve" : error);
    const mutators::ResolvedMutator held = *resolved;
    return mutators::loadout_of(std::span<const mutators::ResolvedMutator>(&held, 1));
}

/// Tells whether two window rectangles share more than a pixel.
///
/// A shared edge, and one pixel of rounding, do not count.
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

/// The distance between two edges.
///
/// @param left one edge
/// @param right the other
/// @return how far apart they are
int32_t apart(int32_t left, int32_t right) noexcept {
    return left > right ? left - right : right - left;
}

/// The picker's check feed, which the check reads through counting_inputs.
MutatorPickerFeed counted_feed{};

/// How many times the button or the picker read the feed's list.
std::size_t feed_reads = 0;

/// Reads the check feed's list and counts the read.
///
/// @param context the check feed's context
/// @param setup which setup asks
/// @return the check feed's list
picker::Inputs counting_inputs(void* context, picker::Setup setup) {
    ++feed_reads;
    return counted_feed.inputs(context, setup);
}

} // namespace

void Runtime::check_mutator_button() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");

    const fs::path mutators_folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    write_package(mutators_folder / "gatehouse", "gatehouse", "Gatehouse", false);
    write_package(mutators_folder / "gatehouse-keep", "gatehouse-keep", "Gatehouse Keep", false);
    write_package(mutators_folder / "fast-builders", "fast-builders", "Fast Builders", true);

    mutators::Loadout skirmish_set = loadout_of_id(mutators_folder, "gatehouse");
    {
        mutators::Loadout keep = loadout_of_id(mutators_folder, "gatehouse-keep");
        require(keep.entries.size() == 1, "Gatehouse Keep did not resolve to one entry");
        skirmish_set.entries.push_back(std::move(keep.entries.front()));
    }
    const mutators::Loadout campaign_set = loadout_of_id(mutators_folder, "fast-builders");
    preference_values_[std::string(
        mutators_memory::preference_key(mutators_memory::Setup::skirmish)
    )] = mutators::canonical_loadout(skirmish_set);
    preference_values_[std::string(
        mutators_memory::preference_key(mutators_memory::Setup::campaign)
    )] = mutators::canonical_loadout(campaign_set);
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();
    std::cout << "mutator button check: remembered Gatehouse, Gatehouse Keep and Fast Builders\n";

    install_mutator_picker_check_feed();
    counted_feed = mutator_picker_feed_;
    {
        MutatorPickerFeed counting = counted_feed;
        counting.inputs = counting_inputs;
        set_mutator_picker_feed(counting);
    }
    mark_mutator_picker_check_installed("gatehouse-keep");

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
        for (const AutomationControl& control : controls_of())
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto named_on = [&](std::string_view name,
                              std::string_view dialog) -> std::optional<AutomationControl> {
        for (const AutomationControl& control : controls_of())
            if (control.name == name && control.dialog == dialog && control.visible &&
                control.window_width > 0 && control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), why);
        return *control;
    };
    // The button's own control: its text counts the set, "Mutators · off" or
    // "Mutators · 2 on".
    const auto summary_of = [&](std::string_view why) {
        const std::optional<AutomationControl> control =
            named_on("oa.mutators", "oa.mutators-button");
        require(control.has_value(), why);
        return *control;
    };
    // The button's lamp as a frame of the window shows it: red with none
    // chosen and green with mutators on, as 3.1c's on and off buttons light
    // theirs.
    const auto require_lamp = [&](const renderer::Surface& picture,
                                  renderer::RoomLamp expected,
                                  std::string_view what,
                                  const std::string& on) {
        const AutomationControl button = summary_of("the button left before its lamp" + on);
        const renderer::RoomLamp lamp = mutators_lamp_seen(
            *this,
            oa_layer().view(),
            {button.window_x, button.window_y, button.window_width, button.window_height},
            false,
            picture
        );
        require(
            lamp == expected,
            "the lamp is not " + std::string(what) + " (it reads " +
                std::to_string(static_cast<int32_t>(lamp)) + ")" + on
        );
        std::cout << "mutator button check: the lamp is " << what << on << "\n";
    };
    // The button is one of the room's own parts: the screen draws no button
    // or chip of the kit (its card is the kit's), and its one button control
    // is the mutators button.
    const auto require_badge = [&](const std::string& on) {
        LayerScreen* shown = oa_layer().find("mutators-button");
        require(shown != nullptr, "the mutators-button screen is not up" + on);
        const kit::DisplayList list = shown->display_list();
        for (const kit::Item& item : list.items)
            require(
                item.role != kit::Role::button && item.role != kit::Role::chip,
                "the button draws a button or a chip of the kit" + on
            );
        bool button = false;
        for (const kit::Control& control : list.controls)
            if (control.name == "mutators" && control.kind == kit::ControlKind::button)
                button = true;
        require(button, "the button has no control" + on);
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    // The pointer resting on the button shows its card: one line a mutator
    // on, by its place in the set, and what a press does. Its frame is
    // written as mutator-button-<screen>-card-<w>x<h>.png.
    const auto check_card = [&](std::string_view screen,
                                std::size_t count,
                                const std::string& size,
                                const std::string& on) {
        const AutomationControl button = summary_of("the button left before its card" + on);
        const layout::Point rest{
            button.window_x + button.window_width / 2, button.window_y + button.window_height / 2
        };
        pointer_at(rest, SDL_EVENT_MOUSE_MOTION, 0);
        SDL_Delay(450);
        frames(4);
        std::size_t rows = 0;
        bool change = false;
        for (const AutomationControl& control : controls_of()) {
            constexpr std::string_view prefix = "oa.battleroom.tip.line.";
            if (control.name.compare(0, prefix.size(), prefix) != 0 || !control.visible)
                continue;
            if (control.text.find(": ") != std::string::npos &&
                std::isdigit(static_cast<unsigned char>(control.text.front())) != 0)
                ++rows;
            change = change || control.text == "Click to change.";
        }
        require(
            rows == count && change,
            "the card does not name the " + std::to_string(count) +
                " mutators on and what a press does (" + std::to_string(rows) + " rows)" + on
        );
        oa_layer().read_whole_window(true);
        renderer::Surface card_frame;
        capture_frame_ = &card_frame;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        const std::string picture =
            "mutator-button-" + std::string(screen) + "-card-" + size + ".png";
        write_png(fs::path(picture), card_frame);
        std::cout << "mutator button check: the card names the " << count << " mutators on; wrote "
                  << picture << "\n";
        pointer_at({0, 0}, SDL_EVENT_MOUSE_MOTION, 0);
        frames(2);
    };
    // Four mutators chosen for a moment: the button counts "Mutators · 4 on",
    // and the frame is written as mutator-button-<screen>-four-<w>x<h>.png.
    // The setup's set is put back.
    const auto capture_four = [&](mutators_memory::Setup setup,
                                  std::string_view screen,
                                  const std::string& size,
                                  const CheckWindow& window,
                                  const std::string& on) {
        std::vector<picker::Chosen>& held =
            setup == mutators_memory::Setup::campaign ? campaign_mutators_ : skirmish_mutators_;
        const std::vector<picker::Chosen> kept = held;
        std::vector<picker::Chosen> four;
        for (const char* id : {"gatehouse", "gatehouse-keep", "fast-builders", "steady-hands"}) {
            picker::Chosen one;
            one.id = id;
            four.push_back(std::move(one));
        }
        replace_setup_mutators(setup, std::move(four));
        frames(1);
        const AutomationControl counted = summary_of("the button left with four chosen" + on);
        require(
            counted.text == "Mutators · 4 on",
            "four chosen do not count Mutators · 4 on (" + counted.text + ")" + on
        );
        require_badge(on);
        oa_layer().read_whole_window(true);
        renderer::Surface four_frame;
        capture_frame_ = &four_frame;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            four_frame.width == static_cast<uint32_t>(window.width) &&
                four_frame.height == static_cast<uint32_t>(window.height),
            "the frame with four chosen is not the window's size" + on
        );
        const std::string picture =
            "mutator-button-" + std::string(screen) + "-four-" + size + ".png";
        write_png(fs::path(picture), four_frame);
        std::cout << "mutator button check: four chosen count Mutators · 4 on; wrote " << picture
                  << "\n";
        require_lamp(four_frame, renderer::RoomLamp::green, "green with four chosen", on);
        check_card(screen, 4, size, on);
        replace_setup_mutators(setup, kept);
        frames(1);
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
        if (LayerScreen* picker_open = oa_layer().find(picker::screen_name))
            oa_layer().close(picker_open);
        frames(1);
    };
    const auto gadget_index = [this](std::string_view name) {
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (tdf_names_equal(gadgets[index].common.name, name))
                return static_cast<int32_t>(index);
        return int32_t{-1};
    };
    const auto focused_name = [this] {
        const int32_t focus = frontend_focus();
        if (focus < 0 || static_cast<std::size_t>(focus) >= resources_.layout.gadgets.size())
            return std::string();
        return resources_.layout.gadgets[static_cast<std::size_t>(focus)].common.name;
    };
    const auto button_focused = [&] {
        const std::optional<AutomationControl> button =
            named_on("oa.mutators", "oa.mutators-button");
        return button.has_value() && button->focused && frontend_focus() < 0;
    };
    const auto list_has = [&](std::string_view needle) {
        LayerScreen* screen = oa_layer().find(picker::screen_name);
        require(screen != nullptr, "the mutator picker is not open");
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
    const auto switch_on = [&](std::string_view id) {
        const std::string name = "oa.mutators.row." + std::string(id) + ".switch.on";
        const AutomationControl control = require_named(name, name + " is not shown");
        require(control.checked, std::string(id) + " is not on");
    };
    const auto handed_ids = [this] {
        std::string ids;
        for (const mutators::ResolvedMutator& one : chosen_match_mutators()) {
            if (!ids.empty())
                ids += ", ";
            ids += one.choice.target;
        }
        return ids;
    };
    const auto id_handed = [&](std::string_view id) {
        for (const mutators::ResolvedMutator& one : chosen_match_mutators())
            if (one.choice.target == id)
                return true;
        return false;
    };
    // A list other than Fast Builders, as a between-missions load leaves one.
    const auto hold_other_list = [&] {
        mutators::MutatorChoice choice;
        choice.target = "gatehouse";
        std::string error;
        const std::optional<mutators::ResolvedMutator> resolved =
            mutators::resolve_choice(choice, mutators_folder, &error);
        require(resolved.has_value(), error.empty() ? "gatehouse did not resolve" : error);
        std::vector<mutators::ResolvedMutator> playback;
        playback.push_back(*resolved);
        play_loadout_once(std::move(playback));
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
        load(Screen::main_menu);
        frames(2);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(entry::resource_name(entry::Button::skirmish));
        frames(2);
        close_covering();
        require(screen_ == Screen::skirmish, "SKIRMISH did not open" + on);

        const AutomationControl button =
            require_named("oa.mutators", "oa.mutators is not shown" + on);
        require(
            button.text == "Mutators · off",
            "the button does not count Mutators · off (" + button.text + ")" + on
        );
        require(button.kind == AutomationControlKind::button, "oa.mutators is not a button" + on);
        require_badge(on);

        const LayerView seen = oa_layer().view();
        const auto origin = panel_origin();
        const int32_t percent = std::max(seen.scale_percent, int32_t{100});
        const auto gadget_window_of = [&](std::string_view name) {
            const int32_t index = gadget_index(name);
            require(index > 0, std::string(name) + " is not on the screen" + on);
            const gui::CommonFields& common =
                resources_.layout.gadgets[static_cast<std::size_t>(index)].common;
            const kit::Rect frame{
                static_cast<int32_t>(origin.x) + common.x,
                static_cast<int32_t>(origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            return kit::to_canvas(frame_to_points(seen, frame), percent);
        };
        const int32_t difficulty = gadget_index("Difficulty");
        const kit::Rect anchor_window = gadget_window_of("Difficulty");
        const kit::Rect previous_window = gadget_window_of("PrevMenu");
        const kit::Rect button_window = window_rect(button);
        const kit::Rect picture{
            seen.picture.x, seen.picture.y, seen.picture.width, seen.picture.height
        };
        // As large as Previous Menu, its left and right edges, centred
        // between Difficulty and Previous Menu.
        const int32_t slack = percent / 50 + 1;
        require(
            apart(button_window.x, previous_window.x) <= slack &&
                apart(
                    button_window.x + button_window.width, previous_window.x + previous_window.width
                ) <= slack,
            "the button's edges are not Previous Menu's (" + std::to_string(button_window.x) +
                " to " + std::to_string(button_window.x + button_window.width) + ", not " +
                std::to_string(previous_window.x) + " to " +
                std::to_string(previous_window.x + previous_window.width) + ")" + on
        );
        require(
            apart(button_window.width, previous_window.width) <= slack &&
                apart(button_window.height, previous_window.height) <= slack,
            "the button is not as large as Previous Menu" + on
        );
        const int32_t above = button_window.y - (anchor_window.y + anchor_window.height);
        const int32_t below = previous_window.y - (button_window.y + button_window.height);
        require(
            above > 0 && below > 0 && apart(above, below) <= slack,
            "the button is not centred between Difficulty and Previous Menu (" +
                std::to_string(above) + " above, " + std::to_string(below) + " below)" + on
        );
        require(inside_window(button_window, picture), "the button leaves the frame" + on);
        const auto& gadgets = resources_.layout.gadgets;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const gui::CommonFields& common = gadgets[index].common;
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
                !overlaps(button_window, gadget_window), "the button meets " + common.name + on
            );
        }
        std::cout << "mutator button check: SKIRMISH shows Mutators as large as Previous Menu, "
                     "centred above it in Difficulty's column"
                  << on << "\n";

        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the skirmish frame is not the window's size" + on
        );
        write_png(fs::path("mutator-button-skirmish-" + size + ".png"), presented);
        std::cout << "mutator button check: wrote mutator-button-skirmish-" << size << ".png\n";
        require_lamp(presented, renderer::RoomLamp::red, "red with none chosen", on);
        capture_four(mutators_memory::Setup::skirmish, "skirmish", size, window, on);

        // A finger on the button opens the picker. A finger on Difficulty
        // steps Difficulty and leaves the picker closed. A finger a few
        // pixels left of the button, nearer the button than any gadget,
        // opens the picker.
        const bool unregistered = touch_state().dispatch.accept_unregistered_touch;
        touch_state().dispatch.accept_unregistered_touch = true;
        constexpr SDL_TouchID kFingerDevice = 0x7518;
        const auto finger_at = [&](layout::Point pixel) {
            for (const SDL_EventType type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
                SDL_Event event =
                    window_finger_event(sdl_.window, sdl_.renderer, type, pixel, kFingerDevice, 1);
                dispatch_event(event, running);
                require(running, "a finger ended the run" + on);
            }
            frames(1);
        };
        const auto picker_open = [&] { return oa_layer().find(picker::screen_name) != nullptr; };
        const auto close_picker = [&] {
            if (LayerScreen* open = oa_layer().find(picker::screen_name))
                oa_layer().close(open);
            frames(1);
            require(!picker_open(), "the mutator picker stayed open" + on);
        };
        const auto rect_of = [](const kit::Rect& rect) {
            return place::PlaceRect{rect.x, rect.y, rect.width, rect.height};
        };
        const uint32_t difficulty_before = preferences_.difficulty;
        finger_at(
            {button_window.x + button_window.width / 2, button_window.y + button_window.height / 2}
        );
        require(picker_open(), "a finger on Mutators did not open the picker" + on);
        require(
            preferences_.difficulty == difficulty_before,
            "a finger on Mutators stepped Difficulty" + on
        );
        close_picker();
        const auto step_difficulty = [&](layout::Point pixel, std::string_view where) {
            const uint32_t before_step = preferences_.difficulty;
            const uint32_t expect = before_step <= 2U ? (before_step + 1U) % 3U : before_step;
            finger_at(pixel);
            require(!picker_open(), std::string(where) + " opened the picker" + on);
            require(
                preferences_.difficulty == expect && preferences_.skirmish_difficulty == expect,
                std::string(where) + " did not step Difficulty" + on
            );
        };
        step_difficulty(
            {anchor_window.x + anchor_window.width / 2, anchor_window.y + anchor_window.height / 2},
            "a finger in the middle of Difficulty"
        );
        step_difficulty(
            {anchor_window.x + anchor_window.width * 3 / 4,
             anchor_window.y + anchor_window.height / 2},
            "a finger on Difficulty's right quarter"
        );
        std::vector<place::PlaceRect> gadget_rects;
        gadget_rects.reserve(gadgets.size());
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const gui::CommonFields& common = gadgets[index].common;
            if (common.width <= 0 || common.height <= 0)
                continue;
            const kit::Rect frame{
                static_cast<int32_t>(origin.x) + common.x,
                static_cast<int32_t>(origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            gadget_rects.push_back(rect_of(kit::to_canvas(frame_to_points(seen, frame), percent)));
        }
        const place::PlaceRect button_rect = rect_of(button_window);
        layout::Point beside{};
        bool found_beside = false;
        const int32_t rows[] = {
            button_window.y + button_window.height / 2,
            button_window.y + button_window.height / 4,
            button_window.y + (button_window.height * 3) / 4,
        };
        for (const int32_t row : rows) {
            for (int32_t gap = 2; gap <= 12; ++gap) {
                const int32_t column = button_window.x - gap;
                const int64_t to_button = place::point_distance_squared(column, row, button_rect);
                if (to_button == 0)
                    continue;
                bool nearer_button = true;
                for (const place::PlaceRect& gadget : gadget_rects)
                    if (place::point_distance_squared(column, row, gadget) < to_button)
                        nearer_button = false;
                if (!nearer_button)
                    continue;
                beside = {column, row};
                found_beside = true;
                break;
            }
            if (found_beside)
                break;
        }
        require(
            found_beside,
            "no point a few pixels left of Mutators is nearer it than the gadgets" + on
        );
        const uint32_t beside_before = preferences_.difficulty;
        finger_at(beside);
        require(picker_open(), "a finger just left of Mutators did not open the picker" + on);
        require(
            preferences_.difficulty == beside_before,
            "a finger just left of Mutators stepped Difficulty" + on
        );
        close_picker();
        // A finger on each row between Difficulty and the button presses the
        // nearer of the two, and never neither. Each tap stands alone, not
        // the second of a double tap.
        const int32_t anchor_last = anchor_window.y + anchor_window.height - 1;
        const int32_t gap_column =
            std::min(button_window.x + button_window.width, anchor_window.x + anchor_window.width) -
            3;
        require(
            button_window.y > anchor_last + 1 &&
                gap_column >= std::max(button_window.x, anchor_window.x),
            "no gap lies between Difficulty and Mutators" + on
        );
        for (int32_t row = anchor_last + 1; row < button_window.y; ++row) {
            const int32_t to_anchor = row - anchor_last;
            const int32_t to_button = button_window.y - row;
            const std::string where =
                "a finger on the gap's row " + std::to_string(to_anchor) + " under Difficulty";
            touch_state().dispatch.last_tap_ns = 0;
            const uint32_t row_before = preferences_.difficulty;
            finger_at({gap_column, row});
            const bool stepped = preferences_.difficulty != row_before;
            const bool opened = picker_open();
            require(stepped || opened, where + " pressed nothing" + on);
            require(!stepped || !opened, where + " pressed both" + on);
            require(to_anchor >= to_button || stepped, where + " did not step Difficulty" + on);
            require(to_button >= to_anchor || opened, where + " did not open the picker" + on);
            if (opened)
                close_picker();
        }
        touch_state().dispatch.accept_unregistered_touch = unregistered;

        // With the pad's badges showing, the button stays one of the
        // room's own parts, where it was: no chip of the kit is drawn.
        const bool badges_were = touch_state().hud.pad.badges;
        touch_state().hud.pad.badges = true;
        frames(1);
        {
            require_badge(" with the pad's badges" + on);
            const AutomationControl with_badges =
                require_named("oa.mutators", "oa.mutators left with the pad's badges" + on);
            require(
                window_rect(with_badges).x == button_window.x &&
                    window_rect(with_badges).y == button_window.y,
                "the pad's badges moved the button" + on
            );
        }
        std::cout << "mutator button check: with the pad's badges the button stays the room's" << on
                  << "\n";
        touch_state().hud.pad.badges = badges_were;
        frames(1);

        set_frontend_focus(difficulty);
        frames(1);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(button_focused(), "Tab from Difficulty did not focus oa.mutators" + on);
        require(frontend_focus() < 0, "the GUI kept a focus ring on the button" + on);
        std::cout << "mutator button check: Tab from Difficulty focuses oa.mutators" << on << "\n";
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        const std::string after = focused_name();
        require(
            tdf_names_equal(after, "SelectMap") || tdf_names_equal(after, "PrevMenu"),
            "Tab from the button did not focus SelectMap or PrevMenu (" + after + ")" + on
        );
        std::cout << "mutator button check: Tab from the button focuses " << after << on << "\n";
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_SHIFT);
        frames(1);
        require(button_focused(), "Shift+Tab did not return to oa.mutators" + on);
        set_frontend_focus(difficulty);
        frames(1);
        press_key(SDLK_DOWN, SDL_SCANCODE_DOWN, SDL_KMOD_NONE);
        frames(1);
        require(button_focused(), "Down from Difficulty did not focus oa.mutators" + on);
        std::cout << "mutator button check: Down from Difficulty focuses oa.mutators" << on << "\n";

        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        frames(1);
        require(
            oa_layer().find(picker::screen_name) != nullptr, "Return did not open the picker" + on
        );
        switch_on("gatehouse");
        switch_on("gatehouse-keep");
        require(
            list_has("Your last skirmish set"),
            "the picker did not open on the remembered skirmish set" + on
        );
        std::cout << "mutator button check: Return opens the remembered skirmish set" << on << "\n";
        click_named("oa.mutators.use");
        require(
            oa_layer().find(picker::screen_name) == nullptr, "USE THESE left the picker open" + on
        );
        const AutomationControl chosen = summary_of("the button left after USE THESE" + on);
        require(
            chosen.text == "Mutators · 2 on",
            "USE THESE did not count the set on the button (" + chosen.text + ")" + on
        );
        require(
            skirmish_mutators_.size() == 2 && skirmish_mutators_[0].id == "gatehouse" &&
                skirmish_mutators_[1].id == "gatehouse-keep",
            "USE THESE did not choose Gatehouse and Gatehouse Keep" + on
        );
        require_badge(on);
        std::cout << "mutator button check: USE THESE sets Gatehouse, Gatehouse Keep and the "
                     "button reads "
                  << chosen.text << on << "\n";
        const std::size_t reads_before = feed_reads;
        frames(5);
        require(
            feed_reads == reads_before,
            "the button read the feed's list on " + std::to_string(feed_reads - reads_before) +
                " of 5 frames" + on
        );

        require(
            oa_layer().take_pad_action(pad::Action::library_search),
            "Y did not open the picker" + on
        );
        frames(1);
        require(oa_layer().find(picker::screen_name) != nullptr, "Y left the picker closed" + on);
        // The picker passes the D-pad and A on, so they become keys and reach it as keys.
        require(
            !oa_layer().take_pad_action(pad::Action::focus_down) &&
                !oa_layer().take_pad_action(pad::Action::press),
            "the picker kept the D-pad and A from becoming keys" + on
        );
        switch_on("gatehouse");
        switch_on("gatehouse-keep");
        require(
            !list_has("Your last skirmish set"),
            "Y opened the remembered set rather than the chosen one" + on
        );
        click_named("oa.mutators.cancel");
        require(
            oa_layer().find(picker::screen_name) == nullptr, "CANCEL left the picker open" + on
        );
        const AutomationControl kept = summary_of("the button left after CANCEL" + on);
        require(kept.text == chosen.text, "CANCEL changed the count" + on);
        std::cout << "mutator button check: Y opens the chosen set and CANCEL keeps it" << on
                  << "\n";

        exercise_click(skirmish::resource_name(skirmish::Button::select_map));
        frames(1);
        require(screen_ == Screen::map_selection, "SELMAP did not open" + on);
        close_map_modal();
        frames(1);
        require(screen_ == Screen::skirmish, "SELMAP did not return to SKIRMISH" + on);
        const AutomationControl after_map = summary_of("the button left after SELMAP" + on);
        require(after_map.text == chosen.text, "SELMAP cleared the count" + on);
        std::cout << "mutator button check: SELMAP keeps the chosen set" << on << "\n";

        // A chosen mutator that is no longer installed refuses the start
        // with its name, as a failed start does, and the setup keeps the set.
        const fs::path keep_folder = mutators_folder / "gatehouse-keep";
        const fs::path keep_aside = user_folder_ / "gatehouse-keep-aside";
        fs::rename(keep_folder, keep_aside);
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        fs::rename(keep_aside, keep_folder);
        require(
            screen_ == Screen::skirmish && match_ == nullptr,
            "a set that cannot be read left the skirmish setup" + on
        );
        require(
            status_.find("gatehouse-keep") != std::string::npos,
            "the refused start did not name Gatehouse Keep (" + status_ + ")" + on
        );
        close_covering();
        const AutomationControl after_refusal = summary_of("the button left after a refusal" + on);
        require(after_refusal.text == chosen.text, "a refused start cleared the count" + on);
        std::cout << "mutator button check: a refused start keeps the chosen set (" << status_
                  << ")" << on << "\n";

        exercise_click(skirmish::resource_name(skirmish::Button::start));
        require(
            screen_ == Screen::match && match_ != nullptr, "START did not start the skirmish" + on
        );
        require(
            id_handed("gatehouse") && id_handed("gatehouse-keep"), "START did not hand the set" + on
        );
        std::cout << "mutator button check: START handed " << handed_ids() << on << "\n";
        leave_match();
        load(Screen::main_menu);
        frames(1);
        close_covering();
        exercise_click(menu::resource_name(menu::Button::single_player));
        frames(1);
        exercise_click(entry::resource_name(entry::Button::skirmish));
        frames(2);
        close_covering();
        const AutomationControl cleared = summary_of("the button left after returning" + on);
        require(
            cleared.text == "Mutators · off",
            "returning to SKIRMISH kept the set (" + cleared.text + ")" + on
        );
        std::cout << "mutator button check: SKIRMISH from the main menu counts Mutators · off" << on
                  << "\n";

        load(Screen::single_player);
        frames(1);
        hold_other_list();
        load(Screen::new_campaign);
        frames(2);
        close_covering();
        require(screen_ == Screen::new_campaign, "NEWGAME did not open" + on);
        const AutomationControl campaign_button =
            require_named("oa.mutators", "oa.mutators is not shown on NEWGAME" + on);
        require(
            campaign_button.text == "Mutators · off",
            "NEWGAME's button does not count Mutators · off (" + campaign_button.text + ")" + on
        );
        require_badge(on);
        const LayerView campaign_seen = oa_layer().view();
        const auto campaign_origin = panel_origin();
        const int32_t campaign_percent = std::max(campaign_seen.scale_percent, int32_t{100});
        const auto campaign_points_of = [&](const gui::CommonFields& common) {
            const kit::Rect frame{
                static_cast<int32_t>(campaign_origin.x) + common.x,
                static_cast<int32_t>(campaign_origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            return frame_to_points(campaign_seen, frame);
        };
        const auto campaign_window_of = [&](const gui::CommonFields& common) {
            return kit::to_canvas(campaign_points_of(common), campaign_percent);
        };
        const int32_t arm_index = gadget_index("Arm");
        const int32_t core_index = gadget_index("Core");
        const int32_t level_index = gadget_index("Difficulty");
        const int32_t back_index = gadget_index("PrevMenu");
        require(
            arm_index > 0 && core_index > 0 && level_index > 0 && back_index > 0,
            "NEWGAME has no Arm, Core, Difficulty and PrevMenu" + on
        );
        const auto campaign_gadget_window = [&](int32_t index) {
            return campaign_window_of(
                resources_.layout.gadgets[static_cast<std::size_t>(index)].common
            );
        };
        const kit::Rect level_window = campaign_gadget_window(level_index);
        const kit::Rect back_window = campaign_gadget_window(back_index);
        const kit::Rect campaign_button_window = window_rect(campaign_button);
        const int32_t campaign_slack = campaign_percent / 50 + 1;
        // At the head of Difficulty's column, in line with the buttons
        // there: as large as Previous Menu, its left and right edges.
        require(
            apart(campaign_button_window.x, back_window.x) <= campaign_slack &&
                apart(
                    campaign_button_window.x + campaign_button_window.width,
                    back_window.x + back_window.width
                ) <= campaign_slack &&
                apart(campaign_button_window.height, back_window.height) <= campaign_slack,
            "NEWGAME's button is not as large as Previous Menu, its edges (" +
                std::to_string(campaign_button_window.x) + " to " +
                std::to_string(campaign_button_window.x + campaign_button_window.width) + ", not " +
                std::to_string(back_window.x) + " to " +
                std::to_string(back_window.x + back_window.width) + ")" + on
        );
        // Over Set Difficulty, the label just over Difficulty, and under
        // the side bar, centred down the space between them.
        int32_t label_top = level_window.y;
        for (std::size_t index = 1; index < resources_.layout.gadgets.size(); ++index) {
            const kit::Rect other = campaign_window_of(resources_.layout.gadgets[index].common);
            const int32_t bottom = other.y + other.height;
            if (other.width > 0 && other.height > 0 &&
                other.x < level_window.x + level_window.width &&
                level_window.x < other.x + other.width && bottom <= level_window.y &&
                level_window.y - bottom < back_window.height)
                label_top = std::min(label_top, other.y);
        }
        require(label_top < level_window.y, "NEWGAME has no label over Difficulty" + on);
        const kit::Rect core_window = campaign_gadget_window(core_index);
        const kit::Rect arm_window = campaign_gadget_window(arm_index);
        const int32_t bar_bottom =
            std::max(arm_window.y + arm_window.height, core_window.y + core_window.height);
        const int32_t under = campaign_button_window.y - bar_bottom;
        const int32_t over = label_top - (campaign_button_window.y + campaign_button_window.height);
        require(
            under > 0 && over > 0 && apart(under, over) <= campaign_slack + 1,
            "NEWGAME's button is not centred between the side bar and Set Difficulty (" +
                std::to_string(under) + " under the bar, " + std::to_string(over) +
                " over the label)" + on
        );
        for (std::size_t index = 1; index < resources_.layout.gadgets.size(); ++index) {
            const gui::CommonFields& common = resources_.layout.gadgets[index].common;
            if (common.width <= 0 || common.height <= 0)
                continue;
            const kit::Rect gadget_window = campaign_window_of(common);
            require(
                !overlaps(campaign_button_window, gadget_window),
                "NEWGAME's button meets " + common.name + on
            );
        }
        std::cout << "mutator button check: NEWGAME shows Mutators as large as Previous Menu, at "
                     "the head of Difficulty's column"
                  << on << "\n";

        oa_layer().read_whole_window(true);
        renderer::Surface campaign_frame;
        capture_frame_ = &campaign_frame;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            campaign_frame.width == static_cast<uint32_t>(window.width) &&
                campaign_frame.height == static_cast<uint32_t>(window.height),
            "the NEWGAME frame is not the window's size" + on
        );
        write_png(fs::path("mutator-button-newgame-" + size + ".png"), campaign_frame);
        std::cout << "mutator button check: wrote mutator-button-newgame-" << size << ".png\n";
        require_lamp(campaign_frame, renderer::RoomLamp::red, "red with none chosen", on);
        capture_four(mutators_memory::Setup::campaign, "newgame", size, window, on);

        const int64_t button_key =
            static_cast<int64_t>(campaign_button_window.y) * 5000 + campaign_button_window.x;
        int32_t predecessor = -1;
        int64_t best_key = 0;
        std::string predecessor_name;
        for (std::size_t index = 1; index < resources_.layout.gadgets.size(); ++index) {
            const gui::Gadget& gadget = resources_.layout.gadgets[index];
            const gui::CommonFields& common = gadget.common;
            if (common.width <= 0 || common.height <= 0 || !gui::keyboard_focusable(gadget))
                continue;
            const kit::Rect frame{
                static_cast<int32_t>(campaign_origin.x) + common.x,
                static_cast<int32_t>(campaign_origin.y) + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            const kit::Rect gadget_window =
                kit::to_canvas(frame_to_points(campaign_seen, frame), campaign_percent);
            const int64_t key = static_cast<int64_t>(gadget_window.y) * 5000 + gadget_window.x;
            if (key <= button_key && (predecessor < 0 || key > best_key)) {
                best_key = key;
                predecessor = static_cast<int32_t>(index);
                predecessor_name = common.name;
            }
        }
        require(predecessor > 0, "NEWGAME has no record just before oa.mutators" + on);
        set_frontend_focus(predecessor);
        frames(1);
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        frames(1);
        require(
            button_focused(), "Tab from " + predecessor_name + " did not focus oa.mutators" + on
        );
        std::cout << "mutator button check: Tab from " << predecessor_name
                  << " focuses oa.mutators on NEWGAME" << on << "\n";

        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        frames(1);
        require(
            oa_layer().find(picker::screen_name) != nullptr, "NEWGAME did not open the picker" + on
        );
        switch_on("fast-builders");
        require(
            list_has("Your last campaign set"),
            "the picker did not open on the remembered campaign set" + on
        );
        std::cout << "mutator button check: the remembered campaign set is offered" << on << "\n";
        click_named("oa.mutators.use");
        const AutomationControl campaign_chosen =
            summary_of("the campaign button left after USE THESE" + on);
        require(
            campaign_chosen.text == "Mutators · 1 on" && campaign_mutators_.size() == 1 &&
                campaign_mutators_[0].id == "fast-builders",
            "USE THESE did not choose Fast Builders (" + campaign_chosen.text + ")" + on
        );
        start_campaign_mission();
        require(match_ != nullptr, "the first mission did not start (" + status_ + ")" + on);
        require(id_handed("fast-builders"), "the first mission did not hand Fast Builders" + on);
        std::cout << "mutator button check: the first mission handed " << handed_ids() << on
                  << "\n";
        leave_match();
        hold_other_list();
        show_mission_briefing();
        frames(1);
        require(screen_ == Screen::briefing, "the briefing did not open for Back" + on);
        exercise_click("PrevMenu");
        frames(1);
        require(!mutators_held_for_next_match(), "Back left the save's held list" + on);
        std::cout << "mutator button check: Back from the briefing drops a held list" << on << "\n";
    }

    load(Screen::main_menu);
    frames(1);
    close_covering();
    require(
        campaign_mutators_.empty() && skirmish_mutators_.empty(),
        "the main menu kept a setup's chosen set"
    );
    std::cout << "mutator button check: the main menu empties both chosen sets\n";
    exercise_click(menu::resource_name(menu::Button::single_player));
    frames(1);
    exercise_click(entry::resource_name(entry::Button::skirmish));
    frames(2);
    close_covering();
    const AutomationControl empty_summary = summary_of("the empty start has no button");
    require(
        empty_summary.text == "Mutators · off",
        "the empty start does not count Mutators · off (" + empty_summary.text + ")"
    );
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_ != nullptr, "the empty skirmish did not start");
    require(chosen_match_mutators().empty(), "an empty skirmish handed " + handed_ids());
    std::cout << "mutator button check: an empty skirmish start handed V02 an empty set\n";
    restore_mutator_picker_feed();
}

} // namespace oa::app
