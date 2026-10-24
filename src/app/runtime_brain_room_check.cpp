// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-brain-room: a battle room hosted alone, at 640x480 and 1280x720.
// With no brain installed, a seated computer shows no seat button and the
// frame matches one taken with that screen closed, and the record this
// machine sends carries no brain field. Steady Commander and Lantern Scout
// are then written, and the remembered multiplayer seats name Steady
// Commander at firm. A new computer reads "Steady Commander 1.2 · Firm",
// its difficulty reads Hard and is greyed, and the record carries that
// brain. The button meets no control of the room but its row's empty
// screen-size column. The picker offers the built-in player's three levels;
// Medium makes the label "Built-in · Medium" and drops the brain field.
// Lantern Scout with every computer seat sets both rows, and its card names
// full awareness. Each size's frame is written as
// brain-room-host-<w>x<h>.png while Steady Commander is showing.

#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/brain_library/seats.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/brain_picker/screen.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
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
namespace brain = oa::data::brain_library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace brains = oa::ui::brain_picker;
namespace kit = oa::ui::kit;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace mp = oa::ui::frontend_multiplayer;
namespace net = oa::netgame;
namespace settings = oa::ui::engine_settings;
namespace first_run = oa::ui::first_run;

/// One window the check opens the room in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr CheckWindow kWindows[] = {{640, 480}, {1280, 720}};

/// How many frames the room may take to open, or a computer to be seated.
constexpr int32_t kOpeningFrames = 300;

/// The script both made-up brains share. Its hooks do nothing.
constexpr std::string_view kScript = "local brain = {}\nreturn brain\n";

/// The leaving reason a computer row is cleared with. The room's own name
/// for it is not visible here.
constexpr uint8_t kLeaving = 2;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("brain room check: " + std::string(what));
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

/// A seat label: the brain, its version when it has one, and its level.
///
/// @param brain_name the brain, or "Built-in"
/// @param version the version; empty leaves it out
/// @param level the level
/// @return the label
std::string
labelled(std::string_view brain_name, std::string_view version, std::string_view level) {
    std::string text(brain_name);
    if (!version.empty()) {
        text.push_back(' ');
        text.append(version);
    }
    text.append(" \u00B7 ");
    text.append(level);
    return text;
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

/// A control's window rectangle.
///
/// @param control the control
/// @return the rectangle
kit::Rect window_rect(const AutomationControl& control) noexcept {
    return {control.window_x, control.window_y, control.window_width, control.window_height};
}

/// The text a panel control shows.
///
/// @param control the control
/// @return the text, up to the first missing byte
std::string shown_text(const mp::Control& control) {
    const std::size_t length = ::strnlen(control.text.data(), control.text.size());
    return std::string(control.text.data(), length);
}

/// A row control's name, such as RES3. The digit is the slot.
///
/// @param stem RES or MEM
/// @param slot the slot, 0 to 9
/// @return the name
std::string row_name(std::string_view stem, int32_t slot) {
    std::string name(stem);
    name.push_back(static_cast<char>('0' + slot));
    return name;
}

/// How many pixels of two frames differ. A different size counts as every pixel.
///
/// @param left one frame
/// @param right the other
/// @return the count
std::size_t pixels_differ(const renderer::Surface& left, const renderer::Surface& right) {
    if (left.width != right.width || left.height != right.height ||
        left.rgb.size() != right.rgb.size())
        return std::max(left.rgb.size(), right.rgb.size()) / 3U + 1U;
    std::size_t differing = 0;
    for (std::size_t at = 0; at + 2 < left.rgb.size(); at += 3) {
        if (std::memcmp(left.rgb.data() + at, right.rgb.data() + at, 3) != 0)
            ++differing;
    }
    return differing;
}

} // namespace

void Runtime::check_brain_room() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    require(static_cast<bool>(options_.preferences_file), "needs --preferences-file");

    preference_values_[std::string(settings::key::language)] = "en";
    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preferences_dirty_ = true;
    save_preferences();
    set_language_choice("en");

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
        for (const AutomationControl& control : controls_of()) {
            if (control.visible && control.text.find(needle) != std::string::npos)
                return true;
        }
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
    const auto show_row = [&](std::string_view row_name_text) {
        if (named(row_name_text).has_value())
            return;
        if (named("oa.brains.back").has_value())
            click_named("oa.brains.back");
        for (int32_t turn = 0; turn < 12 && !named(row_name_text).has_value(); ++turn) {
            if (!wheel_list(1.0F))
                break;
        }
        for (int32_t turn = 0; turn < 24 && !named(row_name_text).has_value(); ++turn) {
            if (!wheel_list(-1.0F))
                break;
        }
        require_named(row_name_text, std::string(row_name_text) + " is not shown");
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
        if (LayerScreen* picker_open = oa_layer().find(brains::screen_name))
            oa_layer().close(picker_open);
    };
    const auto room_open = [&] {
        mp::BattleRoomMap battle;
        return mp::multiplayer_battle_room_map(&battle) && !battle.joiner;
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
    // Sizing is separate from the room check: the first size is applied
    // before the room opens, and a later size still has to leave the room up.
    const auto size_window = [&](const CheckWindow& window) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        frames(2);
    };
    const auto resize = [&](const CheckWindow& window) {
        size_window(window);
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        require(room_open(), "the battle room left on the " + size + " window");
    };

    size_window(kWindows[0]);
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
    std::cout << "brain room check: battle room opened\n";

    const auto open_slot = [&] {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the room has no game");
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            if (mp::slot_player(lobby, slot).status == mp::kSlotOpen)
                return slot;
        }
        return int32_t{-1};
    };
    const auto seat_computer = [&] {
        const int32_t slot = open_slot();
        require(slot >= 0, "the room has no open slot");
        mp::Lobby& lobby = mp::multiplayer_lobby();
        mp::lobby_add_computer(lobby, slot);
        const oa::Player& added = mp::slot_player(lobby, slot);
        require(
            added.status == mp::kSlotComputer && added.player_id != 0,
            "the computer player was not added"
        );
        const uint32_t player_id = added.player_id;
        bool seated = false;
        for (int32_t frame = 0; frame < kOpeningFrames && !seated; ++frame) {
            frames(1);
            const int32_t found = mp::slot_for_player_id(mp::multiplayer_lobby(), player_id);
            if (found < 0)
                continue;
            const oa::Player& player = mp::slot_player(mp::multiplayer_lobby(), found);
            seated = player.in_use != 0 && player.status == mp::kSlotComputer;
        }
        require(seated, "the computer player was not seated");
        note_room_computers(mp::multiplayer_lobby());
        frames(2);
        const int32_t found = mp::slot_for_player_id(mp::multiplayer_lobby(), player_id);
        require(found >= 0, "the computer player left after it was seated");
        return found;
    };
    const auto mem_of = [&](int32_t slot) -> const mp::Control* {
        return mp::panel_control(mp::multiplayer_panel(), row_name("MEM", slot));
    };
    const auto brain_field = [&]() -> std::optional<std::vector<net::PresenceBrain>> {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        mp::presence_refresh(lobby);
        require(lobby.presence_records.local_size > 0, "the room composed no presence record");
        std::span<const uint8_t> value;
        if (!net::find_presence_field(
                lobby.presence_records.local,
                lobby.presence_records.local_size,
                net::PresenceTag::computer_brains,
                &value
            ))
            return std::nullopt;
        return net::decode_presence_brains(value);
    };

    const int32_t plain = seat_computer();
    require(!any_seat(), "a seat button shows with no brain installed");
    if (LayerScreen* seats = oa_layer().find("brain-room"))
        oa_layer().close(seats);
    const renderer::Surface closed = capture();
    keep_brain_room_screen();
    const renderer::Surface allowed = capture();
    const std::size_t differing = pixels_differ(closed, allowed);
    require(
        differing == 0,
        "the no-brain frame differs from the closed screen (" + std::to_string(differing) +
            " pixels)"
    );
    require(oa_layer().find("brain-room") == nullptr, "the seat screen is up with nothing to draw");
    require(!brain_field().has_value(), "the record carries a brain field with no brain seated");
    std::cout << "brain room check: no brain: no seat button, the frame equals the closed "
                 "screen, and the record has no brain field\n";

    const oa::Player& plain_player = mp::slot_player(mp::multiplayer_lobby(), plain);
    mp::lobby_reject(mp::multiplayer_lobby(), plain_player.player_id, kLeaving);
    frames(2);
    note_room_computers(mp::multiplayer_lobby());
    std::cout << "brain room check: the computer left and the remembered place starts again\n";

    const fs::path brains_root = user_folder_ / std::string(brain::brains_folder_name);
    write_brain(
        brains_root / "steady-commander",
        "steady-commander",
        "Steady Commander",
        1,
        2,
        2,
        "firm",
        "Firm",
        oabrain::Difficulty::hard,
        false
    );
    write_brain(
        brains_root / "lantern-scout",
        "lantern-scout",
        "Lantern Scout",
        1,
        0,
        2,
        "wide",
        "Wide",
        oabrain::Difficulty::hard,
        true
    );
    brain::Seat remembered_seat;
    remembered_seat.place = 0;
    remembered_seat.brain = "steady-commander";
    remembered_seat.level = "firm";
    brain::Seats remembered;
    remembered.seats.push_back(std::move(remembered_seat));
    preference_values_[std::string(settings::key::brains_multiplayer)] =
        brain::seats_text(remembered);
    preferences_dirty_ = true;
    save_preferences();
    reload_installed_brains();
    std::cout
        << "brain room check: Steady Commander and Lantern Scout installed, firm remembered\n";

    const int32_t steady_slot = seat_computer();
    const std::string steady_label = labelled("Steady Commander", "1.2", "Firm");
    const std::string steady_name = "oa.brain-seat." + std::to_string(steady_slot);
    const AutomationControl steady = require_named(steady_name, steady_name + " is not shown");
    require(
        steady.text == steady_label,
        "the row does not read " + steady_label + " (" + steady.text + ")"
    );
    const mp::Control* steady_mem = mem_of(steady_slot);
    require(steady_mem != nullptr, "the row has no MEM");
    require(
        steady_mem->type == mp::ControlType::label && steady_mem->grayed &&
            shown_text(*steady_mem) == "Hard",
        "MEM is not a greyed Hard (" + shown_text(*steady_mem) + ")"
    );
    const std::optional<std::vector<net::PresenceBrain>> steady_brains = brain_field();
    require(
        steady_brains.has_value() && steady_brains->size() == 1 &&
            (*steady_brains)[0].id == "steady-commander" && (*steady_brains)[0].version == "1.2" &&
            (*steady_brains)[0].level_id == "firm" && (*steady_brains)[0].level == "Firm" &&
            !(*steady_brains)[0].full_awareness,
        "the record's brain field is not Steady Commander 1.2 at firm"
    );
    std::cout << "brain room check: the row reads " << steady.text
              << ", MEM is a greyed Hard, and the record carries that brain\n";

    const auto check_overlap = [&](const CheckWindow& window) {
        resize(window);
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        const AutomationControl button =
            require_named(steady_name, steady_name + " is not shown" + on);
        require(button.text == steady_label, "the row changed" + on + " (" + button.text + ")");
        const std::vector<mp::BattleRoomControl> room = mp::multiplayer_battle_room_controls();
        const LayerView placed = oa_layer().view();
        const int32_t percent = std::max(placed.scale_percent, int32_t{100});
        const auto to_window = [&](int32_t x, int32_t y, int32_t width, int32_t height) {
            const kit::Rect frame{x, y, width, height};
            return kit::to_canvas(frame_to_points(placed, frame), percent);
        };
        const std::string res_name = row_name("RES", steady_slot);
        const mp::BattleRoomControl* res = nullptr;
        for (const mp::BattleRoomControl& control : room) {
            if (control.name == res_name && control.width > 0 && control.height > 0)
                res = &control;
        }
        require(res != nullptr, "the row's screen-size column is not in the room" + on);
        const kit::Rect res_window = to_window(res->x, res->y, res->width, res->height);
        const kit::Rect button_window = window_rect(button);
        require(
            overlaps(button_window, res_window), "the button is not over its row's column" + on
        );
        const mp::Control* res_control = mp::panel_control(mp::multiplayer_panel(), res_name);
        require(
            res_control != nullptr && shown_text(*res_control).empty(),
            "the row's screen-size column is not empty" + on
        );
        for (const mp::BattleRoomControl& control : room) {
            if (control.name == res_name || control.width <= 0 || control.height <= 0)
                continue;
            const kit::Rect control_window =
                to_window(control.x, control.y, control.width, control.height);
            require(
                !overlaps(button_window, control_window), "the button meets " + control.name + on
            );
        }
        pointer_at({0, 0}, SDL_EVENT_MOUSE_MOTION, 0);
        frames(1);
        const renderer::Surface presented = capture();
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the frame is not the window's size" + on
        );
        const std::string picture = "brain-room-host-" + size + ".png";
        write_png(fs::path(picture), presented);
        std::cout << "brain room check: " << size
                  << " the button meets no control but its row's empty column, wrote " << picture
                  << "\n";
    };
    for (const CheckWindow& window : kWindows)
        check_overlap(window);

    resize(kWindows[0]);
    click_named(steady_name);
    require(oa_layer().find(brains::screen_name) != nullptr, "the button did not open the picker");
    show_row("oa.brains.row.built-in");
    click_named("oa.brains.row.built-in");
    if (!named("oa.brains.level.easy").has_value() && named("oa.brains.details").has_value())
        click_named("oa.brains.details");
    require_named("oa.brains.level.easy", "the built-in player's Easy is not shown");
    require_named("oa.brains.level.medium", "the built-in player's Medium is not shown");
    require_named("oa.brains.level.hard", "the built-in player's Hard is not shown");
    std::cout << "brain room check: the picker offers the built-in player's three levels\n";
    click_named("oa.brains.level.medium");
    click_named("oa.brains.use");
    require(oa_layer().find(brains::screen_name) == nullptr, "USE THIS left the picker open");
    frames(2);
    const std::string built_label = labelled("Built-in", {}, "Medium");
    const AutomationControl built =
        require_named(steady_name, steady_name + " left after USE THIS");
    require(
        built.text == built_label, "USE THIS did not put " + built_label + " (" + built.text + ")"
    );
    const mp::Control* built_mem = mem_of(steady_slot);
    require(
        built_mem != nullptr && shown_text(*built_mem) == "Medium",
        "MEM does not read Medium (" +
            (built_mem != nullptr ? shown_text(*built_mem) : std::string{}) + ")"
    );
    require(
        !brain_field().has_value(), "the record still carries a brain field for the built-in player"
    );
    std::cout << "brain room check: the built-in player at Medium reads " << built.text
              << ", MEM reads Medium, and the record has no brain field\n";

    const int32_t second_slot = seat_computer();
    const std::string second_name = "oa.brain-seat." + std::to_string(second_slot);
    click_named(steady_name);
    require(
        oa_layer().find(brains::screen_name) != nullptr,
        "the seat button did not open the picker again"
    );
    show_row("oa.brains.row.lantern-scout");
    click_named("oa.brains.row.lantern-scout");
    if (!named("oa.brains.every-seat.on").has_value() && named("oa.brains.details").has_value())
        click_named("oa.brains.details");
    click_named("oa.brains.every-seat.on");
    const AutomationControl every =
        require_named("oa.brains.every-seat", "every computer seat is not shown");
    require(every.checked, "every computer seat is not on");
    click_named("oa.brains.use");
    require(
        oa_layer().find(brains::screen_name) == nullptr,
        "USE THIS left the picker open after Lantern Scout"
    );
    frames(2);
    const std::string lantern_label = labelled("Lantern Scout", "1.0", "Wide");
    const AutomationControl first_lantern =
        require_named(steady_name, steady_name + " left after Lantern Scout");
    const AutomationControl second_lantern =
        require_named(second_name, second_name + " is not shown after Lantern Scout");
    require(
        first_lantern.text == lantern_label && second_lantern.text == lantern_label,
        "every computer seat did not set both rows (" + first_lantern.text + ", " +
            second_lantern.text + ")"
    );
    std::cout << "brain room check: Lantern Scout with every computer seat sets both rows\n";

    const layout::Point rest{
        first_lantern.window_x + first_lantern.window_width / 2,
        first_lantern.window_y + first_lantern.window_height / 2
    };
    pointer_at(rest, SDL_EVENT_MOUSE_MOTION, 0);
    SDL_Delay(450);
    frames(4);
    require(
        text_has("Full awareness") && text_has("Sees every unit"),
        "the card does not show the full-awareness row"
    );
    std::cout << "brain room check: the card shows full awareness\n";
}

} // namespace oa::app
