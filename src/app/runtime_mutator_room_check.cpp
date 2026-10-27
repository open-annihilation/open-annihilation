// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-room: Mutators for the host of a battle room hosted
// alone, at 640x480 and 1280x720. The button is one of the room's own on
// and off buttons, with no part of the kit, is as large as Select Map, or
// wider to the left where its words need it, stands over it to its right
// edge and meets no control of the room; it
// reads "Mutators: off", "Mutators: 2 on" with two on, its lamp lit, and
// "Mutators: 4 on" with four. Y opens the picker on a remembered multiplayer set
// (Gatehouse, Gatehouse Keep). USE THESE stores that set and the chat reads
// the turned-on lines. Turning them off posts the off line. In French the
// chat's lines are in the code page and fit the list. With Ridge Tough on,
// at 640x480 in each language 3.1c ships with, the two lines about a 3.1c
// player read whole and translated, and so does the line beside START. With
// the room's options locked, the button stays disabled and neither a press
// nor Y opens the picker. Each size's frame is written as
// mutator-room-host-<w>x<h>.png, mutator-room-host-on-<w>x<h>.png and
// mutator-room-host-four-<w>x<h>.png. The feed is the mutator picker's.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/screen.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
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
namespace picker = oa::ui::mutator_picker;
namespace pad = oa::ui::pad_controls;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace mutators = oa::data::mutators;
namespace first_run = oa::ui::first_run;
namespace mp = oa::ui::frontend_multiplayer;

/// One window the check opens the room in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr CheckWindow kWindows[] = {{640, 480}, {1280, 720}};

/// How many frames the room may take to open.
constexpr int32_t kOpeningFrames = 300;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mutator room check: " + std::string(what));
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

/// Writes one made-up mutator the Library lists.
///
/// @param folder the mutator's folder
/// @param id its id
/// @param name the name players see
void write_package(const fs::path& folder, std::string_view id, std::string_view name) {
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
/// @param first a rectangle
/// @param second another
/// @return true when they overlap
bool overlaps(const kit::Rect& first, const kit::Rect& second) noexcept {
    const int32_t near_edge = std::max(first.x, second.x);
    const int32_t far_edge = std::min(first.x + first.width, second.x + second.width);
    const int32_t top = std::max(first.y, second.y);
    const int32_t bottom = std::min(first.y + first.height, second.y + second.height);
    return far_edge - near_edge > 1 && bottom - top > 1;
}

kit::Rect window_rect(const AutomationControl& control) noexcept {
    return {control.window_x, control.window_y, control.window_width, control.window_height};
}

/// The distance between two edges.
///
/// @param one one edge
/// @param other the other
/// @return how far apart they are
int32_t apart(int32_t one, int32_t other) noexcept {
    return one > other ? one - other : other - one;
}

} // namespace

void Runtime::check_mutator_room() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");

    const fs::path mutators_folder =
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    write_package(mutators_folder / "gatehouse", "gatehouse", "Gatehouse");
    write_package(mutators_folder / "gatehouse-keep", "gatehouse-keep", "Gatehouse Keep");
    write_package(mutators_folder / "ridge-tough", "ridge-tough", "Ridge Tough");
    write_package(mutators_folder / "lantern-line", "lantern-line", "Lantern Line");

    mutators::Loadout remembered = loadout_of_id(mutators_folder, "gatehouse");
    {
        mutators::Loadout keep = loadout_of_id(mutators_folder, "gatehouse-keep");
        require(keep.entries.size() == 1, "Gatehouse Keep did not resolve to one entry");
        remembered.entries.push_back(std::move(keep.entries.front()));
    }
    preference_values_[std::string(
        mutators_memory::preference_key(mutators_memory::Setup::multiplayer)
    )] = mutators::canonical_loadout(remembered);
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();
    std::cout << "mutator room check: remembered set" << std::endl;

    install_mutator_picker_check_feed();
    mark_mutator_picker_check_installed("gatehouse-keep");
    touch_state().hud.pad.badges = false;

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
    const auto dismiss = [&] {
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
    };
    const auto room_open = [&] {
        mp::BattleRoomMap battle;
        return mp::multiplayer_battle_room_map(&battle) && !battle.joiner;
    };
    const auto chat_text = [&] {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the room has no game");
        std::string text;
        uint16_t index = mp::lobby_chat_tail(*lobby.game);
        const uint16_t head = mp::lobby_chat_head(*lobby.game);
        for (std::size_t step = 0; step < mp::kChatLines && index != head; ++step) {
            const char* line = mp::lobby_chat_line(*lobby.game, index);
            text.append(line, ::strnlen(line, mp::kChatLineBytes));
            text.push_back('\n');
            index = static_cast<uint16_t>((static_cast<unsigned>(index) + 1U) % mp::kChatLines);
        }
        return text;
    };
    const auto has_id = [&](std::string_view id) {
        const auto held = room_mutators();
        for (const mutators::ResolvedMutator& one : held)
            if (one.manifest.id == id)
                return true;
        return false;
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
        if (const std::optional<AutomationControl> hint = named("oa.mutators.hint"))
            return hint->text.find(needle) != std::string::npos;
        return false;
    };
    const auto guest_absent = [&](std::string_view where) {
        const std::string place(where);
        require(!named("oa.mutators-view").has_value(), "oa.mutators-view shows " + place);
        require(!named("oa.get-mutators").has_value(), "oa.get-mutators shows " + place);
        require(
            !named("oa.get-mutators-progress").has_value(),
            "oa.get-mutators-progress shows " + place
        );
        if (oa_layer().find("get-mutators") != nullptr) {
            const LayerPlacement placed = oa_layer().placement_of("get-mutators");
            require(
                placed.shown.width == 0 && placed.shown.height == 0,
                "the guest's block has a place " + place
            );
        }
    };

    bool opened = false;
    for (int32_t frame = 0; frame < kOpeningFrames && !opened; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        if (frame % 15 == 14)
            dismiss();
        opened = room_open();
    }
    require(opened, "the battle room did not open");
    dismiss();
    frames(1);
    require(room_open(), "closing a notice left the battle room");
    std::cout << "mutator room check: battle room opened" << std::endl;
    guest_absent("after the room opens");

    // Places the button on one window and checks it against the room: as
    // large as Select Map, over it from its left edge, centred between the
    // player rows and Select Map, meeting no control, one of the room's own
    // parts. `button_text` is what the button reads: "Mutators: off" or
    // "Mutators: 2 on"; its lamp is lit with mutators on.
    const auto check_window = [&](const CheckWindow& window,
                                  std::string_view button_text,
                                  std::string_view picture_kind) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        touch_state().hud.pad.badges = false;
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        frames(2);
        require(room_open(), "the battle room left" + on);

        const AutomationControl button =
            require_named("oa.mutators", "oa.mutators is not shown" + on);
        require(
            button.text == button_text,
            "the button does not read " + std::string(button_text) + " (" + button.text + ")" + on
        );
        {
            LayerScreen* shown = oa_layer().find("mutators-button");
            require(shown != nullptr, "the mutators-button screen is not up" + on);
            require(shown->display_list().items.empty(), "the button draws a part of the kit" + on);
            require(
                button.checked == (button_text != "Mutators: off"),
                "the button's lamp does not follow the mutators on" + on
            );
        }

        const std::vector<mp::BattleRoomControl> room = mp::multiplayer_battle_room_controls();
        const mp::BattleRoomControl* map_control = nullptr;
        for (const mp::BattleRoomControl& control : room) {
            if (tdf_names_equal(control.name, "MAP"))
                map_control = &control;
        }
        require(map_control != nullptr, "Select Map is not in the room" + on);
        const LayerView placed = oa_layer().view();
        const int32_t percent = std::max(placed.scale_percent, int32_t{100});
        const auto to_window = [&](int32_t x, int32_t y, int32_t width, int32_t height) {
            const kit::Rect frame{x, y, width, height};
            return kit::to_canvas(frame_to_points(placed, frame), percent);
        };
        const kit::Rect map_window =
            to_window(map_control->x, map_control->y, map_control->width, map_control->height);
        const kit::Rect button_window = window_rect(button);
        const int32_t slack = percent / 50 + 1;
        require(
            apart(button_window.x + button_window.width, map_window.x + map_window.width) <= slack,
            "the button's right edge is not Select Map's" + on
        );
        require(
            button_window.width + slack >= map_window.width &&
                apart(button_window.height, map_window.height) <= slack,
            "the button is not as large as Select Map" + on
        );
        require(
            button_window.y + button_window.height <= map_window.y + 1,
            "the button is not above Select Map" + on
        );
        // The last player row ends at 269: the button sits centred between it
        // and Select Map.
        const kit::Rect rows_end = to_window(9, 269, 486, 1);
        require(button_window.y >= rows_end.y, "the button left the space over Select Map" + on);
        const int32_t above = button_window.y - rows_end.y;
        const int32_t below = map_window.y - (button_window.y + button_window.height);
        require(
            above > 0 && below > 0 && apart(above, below) <= slack + 1,
            "the button is not centred over Select Map (" + std::to_string(above) + " above, " +
                std::to_string(below) + " below)" + on
        );
        for (const mp::BattleRoomControl& control : room) {
            if (control.width <= 0 || control.height <= 0)
                continue;
            const kit::Rect control_window =
                to_window(control.x, control.y, control.width, control.height);
            require(
                !overlaps(button_window, control_window), "the button meets " + control.name + on
            );
        }
        std::cout << "mutator room check: " << size << " shows oa.mutators reading " << button.text
                  << ", as large as Select Map and over it, meeting no room control" << std::endl;

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
        const std::string picture =
            "mutator-room-" + std::string(picture_kind) + "-" + size + ".png";
        write_png(fs::path(picture), presented);
        std::cout << "mutator room check: wrote " << picture << std::endl;
        guest_absent(on);
    };
    for (const CheckWindow& window : kWindows)
        check_window(window, "Mutators: off", "host");

    require(oa_layer().take_pad_action(pad::Action::library_search), "Y did not open the picker");
    frames(1);
    require(oa_layer().find(picker::screen_name) != nullptr, "Y left the picker closed");
    const auto switch_on = [&](std::string_view id) {
        const std::string name = "oa.mutators.row." + std::string(id) + ".switch.on";
        const AutomationControl control = require_named(name, name + " is not shown");
        require(control.checked, std::string(id) + " is not on");
    };
    switch_on("gatehouse");
    switch_on("gatehouse-keep");
    require(list_has("last multiplayer set"), "Y did not open the remembered multiplayer set");
    std::cout << "mutator room check: Y opens the picker on the remembered set" << std::endl;
    guest_absent("after Y");

    click_named("oa.mutators.use");
    require(oa_layer().find(picker::screen_name) == nullptr, "USE THESE left the picker open");
    frames(2);
    require(room_open(), "USE THESE left the battle room");
    require(
        has_id("gatehouse") && has_id("gatehouse-keep"), "USE THESE did not set the room's list"
    );
    {
        const std::string chat = chat_text();
        require(
            chat.find("turned on mutators") != std::string::npos &&
                chat.find("Open Annihilation only") != std::string::npos,
            "the chat did not read the turned-on lines (" + chat + ")"
        );
        std::string rest = chat;
        const std::size_t keep_at = rest.find("Gatehouse Keep");
        require(
            keep_at != std::string::npos, "the chat did not name Gatehouse Keep (" + chat + ")"
        );
        rest.erase(keep_at, std::string("Gatehouse Keep").size());
        require(
            rest.find("Gatehouse") != std::string::npos,
            "the chat did not name Gatehouse (" + chat + ")"
        );
    }
    std::cout
        << "mutator room check: USE THESE sets the room and the chat reads the turned-on lines"
        << std::endl;
    guest_absent("after USE THESE");
    // With two on the button counts them.
    for (const CheckWindow& window : kWindows)
        check_window(window, "Mutators: 2 on", "host-on");
    {
        // Four on for a moment: the button counts four. The two are put back.
        const auto held = room_mutators();
        std::vector<mutators::ResolvedMutator> two(held.begin(), held.end());
        std::vector<mutators::ResolvedMutator> four = two;
        for (const char* id : {"ridge-tough", "lantern-line"}) {
            mutators::MutatorChoice choice;
            choice.target = id;
            std::string error;
            std::optional<mutators::ResolvedMutator> one =
                mutators::resolve_choice(choice, mutators_folder, &error);
            require(one.has_value(), std::string(id) + " did not resolve (" + error + ")");
            four.push_back(std::move(*one));
        }
        set_room_mutators(std::move(four));
        frames(2);
        for (const CheckWindow& window : kWindows)
            check_window(window, "Mutators: 4 on", "host-four");
        set_room_mutators(std::move(two));
        frames(2);
        require(room_mutators().size() == 2, "the two did not come back after four");
    }

    require(
        oa_layer().take_pad_action(pad::Action::library_search), "Y did not open the picker again"
    );
    frames(1);
    const auto turn_off = [&](std::string_view id) {
        click_named("oa.mutators.row." + std::string(id) + ".switch.off");
        if (named("oa.prompt.turn-off"))
            click_named("oa.prompt.turn-off");
    };
    turn_off("gatehouse-keep");
    turn_off("gatehouse");
    click_named("oa.mutators.use");
    require(oa_layer().find(picker::screen_name) == nullptr, "USE THESE left the picker open");
    frames(2);
    require(room_mutators().empty(), "turning the set off left the room holding mutators");
    {
        const std::string chat = chat_text();
        require(
            chat.find("turned mutators off") != std::string::npos,
            "the chat did not read the off line (" + chat + ")"
        );
    }
    std::cout << "mutator room check: turning them off posts the off line" << std::endl;
    guest_absent("after turning them off");

    // French, Unicode chat off: the unranked line's é is one code-page byte,
    // and each stored line fits the chat list at 640x480.
    set_language_choice("fr");
    dismiss();
    require(
        SDL_SetWindowSize(sdl_.window, 640, 480) && SDL_SyncWindow(sdl_.window),
        "the window did not take 640x480 for the French line"
    );
    apply_output_mode();
    frames(2);
    require(room_open(), "the French room left the battle room");
    require(!unicode_chat_on(), "Unicode chat is on for the French room");
    // The host's name as a 3.1c player's would arrive: code-page bytes. The
    // line converts it with the words once, so ö stays one byte.
    char kept_name[sizeof(Player::name)]{};
    Player* named_host = nullptr;
    {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the French room has no game");
        const uint8_t host_slot = mp::lobby_host_slot(lobby);
        require(host_slot != mp::kNoSlot, "the French room has no host");
        named_host = &mp::slot_player(lobby, static_cast<int32_t>(host_slot));
        std::memcpy(kept_name, named_host->name, sizeof kept_name);
        std::memset(named_host->name, 0, sizeof named_host->name);
        std::memcpy(named_host->name, "J\xF6rg", 4);
    }
    require(
        oa_layer().take_pad_action(pad::Action::library_search),
        "Y did not open the picker in French"
    );
    frames(1);
    switch_on("gatehouse");
    click_named("oa.mutators.use");
    frames(2);
    require(room_open(), "USE THESE left the French battle room");
    {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the French room has no game");
        const mp::Control* output = mp::panel_control(mp::multiplayer_panel(), "OUTPUT");
        require(output != nullptr && output->width > 2, "the chat list has no width");
        const int32_t room = output->width - 2;
        // The list draws in hattfont12. A battle room loads no match font,
        // so the check loads it here, after the lines were posted, and
        // measures with it: a line posted without a font to measure it by
        // is still caught.
        ensure_gui_font();
        require(!gui_font_.sequences.empty(), "hattfont12.gaf did not load for the chat list");
        bool saw_accent = false;
        std::string joined;
        uint16_t index = mp::lobby_chat_tail(*lobby.game);
        const uint16_t head = mp::lobby_chat_head(*lobby.game);
        for (std::size_t step = 0; step < mp::kChatLines && index != head; ++step) {
            const char* raw = mp::lobby_chat_line(*lobby.game, index);
            const std::string bytes(raw, ::strnlen(raw, mp::kChatLineBytes));
            if (bytes.find('\xE9') != std::string::npos)
                saw_accent = true;
            require(
                bytes.find("\xC3\xA9") == std::string::npos, "a chat line still holds UTF-8 for é"
            );
            require(
                oa::ui::frontend_renderer::measure_gadget_glyphs(gui_font_, bytes) <= room,
                "a chat line is wider than the list (" + bytes + ")"
            );
            std::string_view words(bytes);
            if (words.starts_with("*** "))
                words.remove_prefix(4);
            if (!joined.empty())
                joined.push_back(' ');
            joined.append(words);
            index = static_cast<uint16_t>((static_cast<unsigned>(index) + 1U) % mp::kChatLines);
        }
        require(saw_accent, "the French line did not post é as the code page's E9");
        require(
            joined.find("J\xF6rg a activ\xE9") != std::string::npos,
            "the host's code-page name did not stay whole (" + joined + ")"
        );
        // The wrapped line keeps every word, the last included.
        require(
            joined.find(
                "Cette partie est seulement Open Annihilation, et non class\xE9"
                "e."
            ) != std::string::npos,
            "the French unranked line lost words (" + joined + ")"
        );
    }
    std::memcpy(named_host->name, kept_name, sizeof kept_name);
    std::cout << "mutator room check: the French line wraps whole, and é is one glyph" << std::endl;
    set_language_choice("en");
    dismiss();
    frames(1);

    // The two lines about a 3.1c player and the host's waiting line beside
    // START, at 640x480 in English and in each language 3.1c ships with.
    // Visitor sits in slot 1 as a remote player does: on 3.1c, then on Open
    // Annihilation without Ridge Tough. Every line is the interface
    // catalogue's, in the code page while Unicode chat is off, and reads
    // whole: each stored chat line fits the list in hattfont12, and the
    // waiting line is not cut.
    {
        // The picker's feed is the check's own and offers no Ridge Tough, so
        // the room is handed the installed copy as USE THESE hands a set.
        mutators::MutatorChoice choice;
        choice.target = "ridge-tough";
        std::string error;
        std::optional<mutators::ResolvedMutator> ridge =
            mutators::resolve_choice(choice, mutators_folder, &error);
        require(ridge.has_value(), "Ridge Tough did not resolve (" + error + ")");
        std::vector<mutators::ResolvedMutator> chosen;
        chosen.push_back(std::move(*ridge));
        set_room_mutators(std::move(chosen));
    }
    frames(2);
    require(room_open(), "Ridge Tough left the battle room");
    require(
        has_id("ridge-tough") && !has_id("gatehouse"), "the room does not play Ridge Tough alone"
    );
    constexpr int32_t visitor_slot = 1;
    constexpr uint32_t visitor_id = 0x7E57A001U;
    constexpr std::string_view seated_words =
        "*** {player} plays Total Annihilation 3.1c, which cannot play with mutators.";
    constexpr std::string_view refused_words =
        "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.";
    {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the room has no game for Visitor");
        require(mp::slot_info(lobby, visitor_slot) != nullptr, "slot 1 has no setup block");
    }
    const Player kept_visitor = mp::slot_player(mp::multiplayer_lobby(), visitor_slot);
    const mp::PlayerSetupInfo kept_info = *mp::slot_info(mp::multiplayer_lobby(), visitor_slot);
    const auto kept_console_flags = mp::multiplayer_lobby().game->console_flags;
    // Visitor never sends a word. Once the room has been open longer than
    // the players' time-out, as it has on a slow build, the room would take
    // a silent Visitor for a stalled player at once and put TIMEOUT over the
    // battle room, and the Mutators button leaves the layer while anything
    // covers the room. While Visitor sits, the room drops no one, as the
    // console's Drop 0 asks.
    const auto seat_visitor = [&](bool open_annihilation) {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        lobby.game->console_flags |= OA_CONSOLE_FLAG_NO_DROP;
        Player& visitor = mp::slot_player(lobby, visitor_slot);
        visitor.in_use = 1;
        visitor.player_id = visitor_id;
        visitor.index = visitor_slot;
        std::memset(visitor.name, 0, sizeof visitor.name);
        std::memcpy(visitor.name, "Visitor", 7);
        mp::slot_set_status(lobby, visitor, mp::kSlotRemote);
        mp::PlayerSetupInfo* info = mp::slot_info(lobby, visitor_slot);
        info->state = mp::kInfoStatePlaying;
        info->net_id = visitor_id;
        info->engine_signature[0] = open_annihilation ? oa::netgame::engine_signature_first : 0;
        info->engine_signature[1] = open_annihilation ? oa::netgame::engine_signature_second : 0;
        lobby.presence_records.block_player[visitor_slot] = visitor_id;
    };
    const auto unseat_visitor = [&] {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        lobby.game->console_flags = kept_console_flags;
        mp::slot_player(lobby, visitor_slot) = kept_visitor;
        *mp::slot_info(lobby, visitor_slot) = kept_info;
        mp::PresenceRecords& records = lobby.presence_records;
        records.block_player[visitor_slot] = 0;
        records.peer_size[visitor_slot] = 0;
        records.peer_id[visitor_slot] = 0;
        std::memset(records.peer[visitor_slot], 0, sizeof records.peer[visitor_slot]);
    };
    const auto capture = [&](const std::string& picture) {
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_png(fs::path(picture), presented);
        std::cout << "mutator room check: wrote " << picture << std::endl;
    };
    // A language pack in the check's user folder adds its language, such as
    // Simplified Chinese, whose text Unicode chat carries.
    std::vector<std::string_view> tags{"en", "de", "fr", "es", "it"};
    if (const oa::data::languages::Language* chinese = oa::data::languages::find_by_tag("zh-Hans");
        chinese != nullptr && oa::data::languages::playable(*chinese))
        tags.push_back("zh-Hans");
    for (const std::string_view tag : tags) {
        const std::string in = " in " + std::string(tag);
        set_language_choice(tag);
        dismiss();
        frames(2);
        require(room_open(), "the room left the battle room" + in);
        const bool unicode = unicode_chat_on();
        require(unicode || tag != "zh-Hans", "Unicode chat is off" + in);
        require(!unicode || tag == "zh-Hans", "Unicode chat is on" + in);

        // Visitor on 3.1c: the room posts the seated line, and the refusal
        // a 3.1c joiner gets.
        seat_visitor(false);
        {
            mp::Lobby& lobby = mp::multiplayer_lobby();
            // The chat starts empty, so the frame shows this language's lines.
            const uint16_t mark = mp::lobby_chat_head(*lobby.game);
            mp::lobby_chat_tail(*lobby.game) = mark;
            lobby.classic_lines_posted = false;
            mp::lobby_note_classic_room(lobby);
            mp::lobby_post_classic_line(lobby, refused_words.data(), visitor_slot);
            const mp::Control* output = mp::panel_control(mp::multiplayer_panel(), "OUTPUT");
            require(output != nullptr && output->width > 2, "the chat list has no width" + in);
            const int32_t room = output->width - 2;
            // The list draws in hattfont12, which the check loads itself:
            // a line posted with no font to measure it by is still caught.
            ensure_gui_font();
            require(!gui_font_.sequences.empty(), "hattfont12.gaf did not load" + in);
            std::string joined;
            uint16_t index = mark;
            const uint16_t head = mp::lobby_chat_head(*lobby.game);
            for (std::size_t step = 0; step < mp::kChatLines && index != head; ++step) {
                const char* raw = mp::lobby_chat_line(*lobby.game, index);
                const std::string bytes(raw, ::strnlen(raw, mp::kChatLineBytes));
                const int32_t width =
                    unicode ? gui_text_width(gui_font_, bytes)
                            : oa::ui::frontend_renderer::measure_gadget_glyphs(gui_font_, bytes);
                require(
                    width <= room, "a 3.1c line is wider than the list" + in + " (" + bytes + ")"
                );
                std::string_view words(bytes);
                if (words.starts_with("*** "))
                    words.remove_prefix(4);
                joined.append(words);
                index = static_cast<uint16_t>((static_cast<unsigned>(index) + 1U) % mp::kChatLines);
            }
            // A break at a space drops it, so the words are compared
            // without spaces.
            std::erase(joined, ' ');
            for (const std::string_view words : {seated_words, refused_words}) {
                std::string line(oa::data::languages::interface_text(words));
                const std::size_t place = line.find("{player}");
                require(place != std::string::npos, "a 3.1c line lost {player}" + in);
                line.replace(place, std::string_view("{player}").size(), "Visitor");
                require(
                    tag == "en" || line.find("plays Total Annihilation") == std::string::npos,
                    "a 3.1c line has no translation" + in + " (" + line + ")"
                );
                std::string expected = unicode ? line : oa::netgame::chat_code_page(line);
                expected.erase(0, 4);
                std::erase(expected, ' ');
                require(
                    joined.find(expected) != std::string::npos,
                    "a 3.1c line is not posted whole" + in + " (" + joined + ")"
                );
            }
        }
        frames(1);
        capture("mutator-room-3.1c-" + std::string(tag) + "-640x480.png");
        unseat_visitor();

        // Visitor on Open Annihilation, without Ridge Tough: START waits,
        // and the line beside it names both, whole.
        seat_visitor(true);
        {
            mp::Lobby& lobby = mp::multiplayer_lobby();
            mp::PresenceRecords& records = lobby.presence_records;
            const auto host_state =
                oa::netgame::presence_mutators_state(records.local, records.local_size);
            require(host_state.has_value(), "the host's record carries no mutators" + in);
            oa::netgame::PresenceMutators field;
            field.digest = host_state->digest;
            field.total = host_state->total;
            field.held = host_state->total;
            field.lacking = true;
            field.index = 0;
            oa::netgame::presence_mutator_set_state(field.states, 0, 1);
            oa::netgame::PresenceRecord record;
            record.mutators = field;
            std::size_t written = 0;
            require(
                oa::netgame::encode_presence(
                    record, records.peer[visitor_slot], sizeof records.peer[visitor_slot], &written
                ) == oa::netgame::WireError::ok,
                "Visitor's record did not encode" + in
            );
            records.peer_size[visitor_slot] = static_cast<uint16_t>(written);
            records.peer_id[visitor_slot] = visitor_id;
            require(
                mp::lobby_mutators_block(lobby).hold != mp::MutatorsHold::none,
                "START does not wait for Visitor" + in
            );
        }
        {
            // The button's line follows the room on a frame, and a slow
            // build may need several before it shows. The button leaves the
            // layer while the room is not in front, and comes back on the
            // next frame once it is, so a frame without it is waited out;
            // three in a row with the room in front and hosted are not.
            const std::string expected = mp::start_reason(mp::PlayerLack{"Visitor", "Ridge Tough"});
            bool whole = false;
            std::string seen;
            int32_t absent = 0;
            int32_t absent_in_front = 0;
            for (int32_t wait = 0; wait < 60 && !whole; ++wait) {
                frames(1);
                LayerScreen* button_screen = oa_layer().find("mutators-button");
                if (button_screen == nullptr) {
                    ++absent;
                    absent_in_front = room_open() ? absent_in_front + 1 : 0;
                    require(
                        absent_in_front < 3,
                        "the Mutators button left the layer while the room stayed in front" + in
                    );
                    continue;
                }
                absent_in_front = 0;
                seen.clear();
                for (const kit::Control& line : button_screen->display_list().controls) {
                    if (line.name != "mutators.waiting")
                        continue;
                    if (line.text == expected)
                        whole = true;
                    else if (line.text.find("Visitor") != std::string::npos)
                        seen += line.text;
                }
            }
            require(
                oa_layer().find("mutators-button") != nullptr,
                "the Mutators button is not on the layer" + in + " (off it on " +
                    std::to_string(absent) + " frames)"
            );
            if (absent > 0)
                std::cout << "mutator room check: the Mutators button was off the layer on "
                          << absent << " frames while the room was not in front" << in << std::endl;
            require(whole, "the waiting line is cut" + in + " (" + seen + ")");
        }
        frames(1);
        capture("mutator-room-waiting-" + std::string(tag) + "-640x480.png");
        unseat_visitor();
        frames(1);
        std::cout << "mutator room check: the 3.1c lines and the waiting line read whole" << in
                  << std::endl;
    }
    set_language_choice("en");
    dismiss();
    frames(1);

    mp::Lobby& lobby = mp::multiplayer_lobby();
    require(lobby.game != nullptr, "the room has no game");
    const uint16_t word = mp::lobby_option_word(*lobby.game);
    mp::lobby_option_word(*lobby.game) = static_cast<uint16_t>(word | 1U);
    frames(1);
    require(room_open(), "locking the options left the battle room");
    const AutomationControl locked = require_named("oa.mutators", "oa.mutators left when locked");
    require(!locked.enabled, "oa.mutators stays enabled while the options are locked");
    click_named("oa.mutators");
    require(
        oa_layer().find(picker::screen_name) == nullptr,
        "a press opened the picker while the options are locked"
    );
    oa_layer().take_pad_action(pad::Action::library_search);
    frames(1);
    require(
        oa_layer().find(picker::screen_name) == nullptr,
        "Y opened the picker while the options are locked"
    );
    guest_absent("while the options are locked");
    std::cout << "mutator room check: locked options leave oa.mutators disabled" << std::endl;
}

} // namespace oa::app
