// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-enhanced-room: the battle room's Enhanced Mode switch for the host
// of a room hosted alone, at 640x480 and 1280x720. The label "Enhanced Mode"
// stands over the switch in the strip right of BATTLE ROOM, the switch's
// right edge the player list's, and neither meets a control of the room.
// The switch starts from Settings (On here) and plays Revision 1, posting
// "*** Enhanced Mode: Revision 1." once; a press turns it off with
// "*** Enhanced Mode: Off." and the room's rules follow. Tab reaches it from
// the host's own row and leaves it for Dead Commander; Return turns it. A
// seated 3.1c Visitor, and an Open Annihilation Visitor whose record has no
// revision, each turn the room Off naming Visitor, the lamp blinking, and
// their leaving brings Revision 1 back with one line. An Open Annihilation
// Visitor whose record has not arrived changes nothing and puts "Waiting
// for Visitor: Revision 1" beside START. A Visitor with a later revision
// names the host as the one with no later revision. A launch that locks the
// room's options keeps the switch grayed, its lamps dark, and Off, and a Boneyards room
// starts Off and may be turned on. In each language 3.1c ships with, the
// lines read whole, fitted to the chat list, and the waiting line is not
// cut. Each state's frame is written as enhanced-room-<state>-<w>x<h>.png.

#include "brain_seats_room.hpp"
#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"

#include "oa/app/enhanced_mode.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/png.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace dialogs = oa::ui::frontend_dialogs;
namespace layout = oa::ui::display_layout;
namespace first_run = oa::ui::first_run;
namespace mp = oa::ui::frontend_multiplayer;
namespace mode = oa::data::enhanced_mode;

/// One window the check opens the room in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr CheckWindow kWindows[] = {{640, 480}, {1280, 720}};

/// How many frames the room may take to open.
constexpr int32_t kOpeningFrames = 300;

/// The switch's automation name, as the endpoint lists it.
constexpr std::string_view kSwitch = "oa.enhanced-mode";

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("enhanced room check: " + std::string(what));
}

/// Writes a frame as a PNG file in the working folder.
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
        "could not encode " + oa::platform::path_to_utf8(file)
    );
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    require(static_cast<bool>(out), "could not write " + oa::platform::path_to_utf8(file));
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

/// Fills a catalogue sentence's places.
///
/// @param english the sentence, in English
/// @param name the place's name, without braces
/// @param value what it becomes
/// @return the sentence, in the language shown
std::string filled(std::string_view english, std::string_view name, std::string_view value) {
    std::string text(oa::data::languages::interface_text(english));
    const std::string place = "{" + std::string(name) + "}";
    if (const std::size_t at = text.find(place); at != std::string::npos)
        text.replace(at, place.size(), value);
    return text;
}

/// Counts where one text occurs in another, without overlapping.
///
/// @param text where to look
/// @param needle what to find; empty finds nothing
/// @return how many times
int32_t occurrences(std::string_view text, std::string_view needle) {
    if (needle.empty())
        return 0;
    int32_t count = 0;
    for (std::size_t at = text.find(needle); at != std::string_view::npos;
         at = text.find(needle, at + needle.size()))
        ++count;
    return count;
}

/// A launch that never started a game, for a Boneyards room.
///
/// @return true
bool launch_held(void* /*context*/) {
    return true;
}

} // namespace

void Runtime::check_enhanced_room() {
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
    touch_state().hud.pad.badges = false;
    // The host's Settings say On: the switch starts from them.
    engine_settings_state().current.enhanced_mode = true;

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
        frames(2);
    };
    // The pointer leaves the switch, so its card does not open over the
    // pictures by resting there.
    const auto move_away = [&] {
        pointer_at(layout::Point{2, 2}, SDL_EVENT_MOUSE_MOTION, 0);
        frames(1);
    };
    const auto press_switch = [&] {
        click_named(kSwitch);
        move_away();
    };
    const auto card_lines = [&] {
        std::vector<std::string> lines;
        for (const AutomationControl& control : controls_of())
            if (control.name.starts_with("oa.battleroom.tip.line.") && control.visible)
                lines.push_back(control.text);
        return lines;
    };
    const auto press_key = [&](SDL_Keycode key, SDL_Scancode scancode, SDL_Keymod held) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = key;
        event.key.scancode = scancode;
        event.key.mod = held;
        event.key.down = true;
        dispatch_event(event, running);
        require(running, "a key ended the run");
        frames(2);
    };
    const auto dismiss = [&] {
        dialogs::reset_dialogs();
        if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
            oa_layer().close(notice);
        if (LayerScreen* question = oa_layer().find(QuestionScreen::screen_name))
            oa_layer().close(question);
        if (LayerScreen* welcome = oa_layer().find("first-run"))
            oa_layer().close(welcome);
    };
    const auto room_open = [&] {
        mp::BattleRoomMap battle;
        return mp::multiplayer_battle_room_map(&battle) && !battle.joiner;
    };
    const auto lobby_of = [&]() -> mp::Lobby& {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        require(lobby.game != nullptr, "the room has no game");
        return lobby;
    };
    // The chat is read from a mark: posting a line may move the list's own
    // first line back to keep it full.
    uint16_t chat_mark = 0;
    const auto clear_chat = [&] {
        mp::Lobby& lobby = lobby_of();
        chat_mark = mp::lobby_chat_head(*lobby.game);
        mp::lobby_chat_tail(*lobby.game) = chat_mark;
    };
    // The chat's stored lines since the mark, each as the list draws it.
    const auto chat_lines = [&] {
        mp::Lobby& lobby = lobby_of();
        std::vector<std::string> lines;
        uint16_t index = chat_mark;
        const uint16_t head = mp::lobby_chat_head(*lobby.game);
        for (std::size_t step = 0; step < mp::kChatLines && index != head; ++step) {
            const char* raw = mp::lobby_chat_line(*lobby.game, index);
            lines.emplace_back(raw, ::strnlen(raw, mp::kChatLineBytes));
            index = static_cast<uint16_t>((static_cast<unsigned>(index) + 1U) % mp::kChatLines);
        }
        return lines;
    };
    // How many times a room line was posted whole since the chat was
    // cleared: the stored lines joined without their "*** " and spaces,
    // as a break at a space drops it. Every stored line fits the list.
    const auto posted = [&](const std::string& line, std::string_view where) {
        const mp::Control* output = mp::panel_control(mp::multiplayer_panel(), "OUTPUT");
        require(
            output != nullptr && output->width > 2,
            "the chat list has no width" + std::string(where)
        );
        const int32_t room_width = output->width - 2;
        ensure_gui_font();
        require(!gui_font_.sequences.empty(), "hattfont12.gaf did not load" + std::string(where));
        const bool unicode = unicode_chat_on();
        std::string joined;
        for (const std::string& bytes : chat_lines()) {
            const int32_t width =
                unicode ? gui_text_width(gui_font_, bytes)
                        : oa::ui::frontend_renderer::measure_gadget_glyphs(gui_font_, bytes);
            require(
                width <= room_width,
                "a chat line is wider than the list" + std::string(where) + " (" + bytes + ")"
            );
            std::string_view words(bytes);
            if (words.starts_with("*** "))
                words.remove_prefix(4);
            joined.append(words);
        }
        std::erase(joined, ' ');
        std::string expected = unicode ? line : oa::netgame::chat_code_page(line);
        if (expected.starts_with("*** "))
            expected.erase(0, 4);
        std::erase(expected, ' ');
        return occurrences(joined, expected);
    };
    const auto require_once = [&](const std::string& line, std::string_view where) {
        const int32_t count = posted(line, where);
        std::string seen;
        for (const std::string& stored : chat_lines())
            seen += stored + " | ";
        require(
            count == 1,
            "\"" + line + "\" was posted " + std::to_string(count) + " times" + std::string(where) +
                " (" + seen + ")"
        );
    };
    const auto local_record = [&] {
        const mp::PresenceRecords& records = lobby_of().presence_records;
        return std::vector<uint8_t>(records.local, records.local + records.local_size);
    };
    const auto played_revision = [&] { return enhanced_mode_choice(*this).revision; };
    const auto revision_line = [&] { return filled("*** Enhanced Mode: Revision {n}.", "n", "1"); };
    const auto switch_text = [&](uint32_t revision) {
        if (revision == 0)
            return std::string(oa::data::languages::interface_text("Enhanced Mode · off"));
        return filled("Enhanced Mode · Revision {n}", "n", std::to_string(revision));
    };
    // The room as the switch shows it: its words, whether it is checked and
    // enabled, and the rules this machine holds.
    const auto require_room =
        [&](uint32_t revision, bool checked, bool enabled, std::string_view where) {
            const std::string place(where);
            const AutomationControl control =
                require_named(kSwitch, std::string(kSwitch) + " is not shown" + place);
            require(
                control.text == switch_text(revision),
                "the switch reads " + control.text + ", not " + switch_text(revision) + place
            );
            require(control.checked == checked, "the switch's checked state is wrong" + place);
            require(control.enabled == enabled, "the switch's enabled state is wrong" + place);
            const std::optional<mode::Choice> held = room_enhanced_choice();
            require(held.has_value(), "the room holds no choice" + place);
            require(held->revision == revision, "the room holds another revision" + place);
            if (revision != 0)
                require(
                    held->features == mode::revision_features(revision),
                    "the room holds features that are not the revision's" + place
                );
            require(played_revision() == revision, "this machine plays another revision" + place);
            const std::vector<uint8_t> record = local_record();
            require(
                oa::netgame::presence_enhanced_highest_revision(record.data(), record.size()) ==
                    mode::highest_revision,
                "the host's record does not announce its highest revision" + place
            );
            const std::optional<uint32_t> announced =
                oa::netgame::presence_enhanced_room_revision(record.data(), record.size());
            require(
                revision == 0 ? !announced.has_value() : announced == revision,
                "the host's record does not announce the room's revision" + place
            );
        };
    const auto capture = [&](const std::string& picture) {
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_png(fs::path(picture), presented);
        std::cout << "enhanced room check: wrote " << picture << std::endl;
        return presented;
    };
    const auto lamp_in = [&](const renderer::Surface& presented) {
        const AutomationControl control = require_named(kSwitch, "the switch is not shown");
        return mutators_lamp_seen(*this, oa_layer().view(), window_rect(control), true, presented);
    };
    const auto set_window = [&](const CheckWindow& window) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        frames(2);
        require(room_open(), "the battle room left on the " + size + " window");
        return size;
    };

    bool opened = false;
    for (int32_t frame = 0; frame < kOpeningFrames && !opened; ++frame) {
        run_frame(running);
        require(running, "a frame ended the run");
        if (frame % 15 == 14)
            dismiss();
        opened = room_open() && named(kSwitch).has_value();
    }
    require(opened, "the battle room did not open with the switch");
    dismiss();
    frames(2);
    std::cout << "enhanced room check: battle room opened" << std::endl;

    // The switch starts from the host's Settings, On, and plays Revision 1.
    require(room_enhanced_switch(), "the switch did not start from the host's Settings");
    require_room(1, true, true, " as the room opens");
    require_once(revision_line(), " as the room opens");
    std::cout << "enhanced room check: the switch starts On from Settings, Revision 1, one line"
              << std::endl;

    // Places the switch on one window and checks it against the room: the
    // label over the switch in the strip right of BATTLE ROOM, the switch's
    // right edge the player list's, meeting no control of the room. Writes
    // the state's picture and reads its lamp back.
    const auto check_window = [&](const CheckWindow& window,
                                  std::string_view state,
                                  renderer::RoomLamp lamp) {
        const std::string size = set_window(window);
        const std::string on = " on the " + size + " window";
        const AutomationControl button = require_named(kSwitch, "the switch is not shown" + on);
        const AutomationControl label =
            require_named("oa.enhanced-mode.label", "the switch's label is not shown" + on);
        require(
            label.text == oa::data::languages::interface_text("Enhanced Mode"),
            "the label reads " + label.text + on
        );
        const LayerView placed = oa_layer().view();
        const int32_t percent = std::max(placed.scale_percent, int32_t{100});
        const auto to_window = [&](int32_t x, int32_t y, int32_t width, int32_t height) {
            return kit::to_canvas(frame_to_points(placed, {x, y, width, height}), percent);
        };
        const int32_t slack = percent / 50 + 1;
        const kit::Rect button_window = window_rect(button);
        const kit::Rect label_window = window_rect(label);
        // The player rows span frame columns 9 to 495; the strip right of
        // BATTLE ROOM runs from row 6 to the column headings at row 45.
        const kit::Rect rows = to_window(9, 269, 486, 1);
        const kit::Rect strip_top = to_window(214, 6, 1, 1);
        const kit::Rect headings = to_window(214, 45, 1, 1);
        require(
            apart(button_window.x + button_window.width, rows.x + rows.width) <= slack,
            "the switch's right edge is not the player list's (" +
                std::to_string(button_window.x + button_window.width) + ", not " +
                std::to_string(rows.x + rows.width) + ")" + on
        );
        require(
            button_window.y >= strip_top.y && label_window.y >= strip_top.y &&
                button_window.y + button_window.height <= headings.y + 1,
            "the switch leaves the strip right of BATTLE ROOM" + on
        );
        require(
            label_window.y + label_window.height <= button_window.y + slack &&
                apart(label_window.x, button_window.x) <= slack,
            "the label is not over the switch" + on
        );
        const kit::Rect expected = to_window(374, 21, 120, 20);
        require(
            apart(button_window.width, expected.width) <= slack &&
                apart(button_window.height, expected.height) <= slack,
            "the switch is not as large as the settings column's" + on
        );
        for (const mp::BattleRoomControl& control : mp::multiplayer_battle_room_controls()) {
            if (control.width <= 0 || control.height <= 0)
                continue;
            const kit::Rect control_window =
                to_window(control.x, control.y, control.width, control.height);
            require(
                !overlaps(button_window, control_window) && !overlaps(label_window, control_window),
                "the switch meets " + control.name + on
            );
        }
        const renderer::Surface presented =
            capture("enhanced-room-" + std::string(state) + "-" + size + ".png");
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the frame is not the window's size" + on
        );
        const renderer::RoomLamp seen = lamp_in(presented);
        if (lamp == renderer::RoomLamp::red_blinking)
            require(seen != renderer::RoomLamp::green, "the blinking lamp shows green" + on);
        else
            require(
                seen == lamp,
                "the lamp reads " + std::to_string(static_cast<int32_t>(seen)) + ", not " +
                    std::to_string(static_cast<int32_t>(lamp)) + on
            );
        std::cout << "enhanced room check: " << size << " " << state << ": " << button.text
                  << ", right of BATTLE ROOM, its right edge the player list's, meeting no "
                     "room control"
                  << std::endl;
    };
    for (const CheckWindow& window : kWindows)
        check_window(window, "on", renderer::RoomLamp::green);
    set_window(kWindows[0]);

    // A press turns it off; the room's rules follow, and one line says so.
    clear_chat();
    press_switch();
    require_room(0, false, true, " after a press");
    require_once(filled("*** Enhanced Mode: Off.", "", ""), " after a press");
    require(posted(revision_line(), "") == 0, "turning it off posted the revision line");
    for (const CheckWindow& window : kWindows)
        check_window(window, "off", renderer::RoomLamp::red);
    set_window(kWindows[0]);
    std::cout << "enhanced room check: a press turns it Off with one line" << std::endl;

    // Tab reaches it from the host's own row and leaves it for Dead
    // Commander; Return turns it on.
    {
        mp::Lobby& lobby = lobby_of();
        const std::string team = "TEAMICONS" + std::to_string(lobby.game->local_player_index);
        int32_t team_index = -1;
        int32_t team_place = -1;
        for (const mp::BattleRoomControl& control : mp::multiplayer_battle_room_controls())
            if (tdf_names_equal(control.name, team) && control.focus_place >= 0) {
                team_index = control.index;
                team_place = control.focus_place;
            }
        require(team_index > 0, team + " takes no Tab");
        int32_t after_index = -1;
        std::string after_name;
        for (const mp::BattleRoomControl& control : mp::multiplayer_battle_room_controls())
            if (control.focus_place == team_place + 1) {
                after_index = control.index;
                after_name = control.name;
            }
        mp::multiplayer_set_battle_room_focus(team_index);
        frames(1);
        const auto switch_focused = [&] {
            LayerScreen* screen = oa_layer().find("enhanced-room");
            return screen != nullptr && screen->interaction().focused != kit::no_control;
        };
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        require(switch_focused(), "Tab from " + team + " did not reach the switch");
        clear_chat();
        press_key(SDLK_RETURN, SDL_SCANCODE_RETURN, SDL_KMOD_NONE);
        require_room(1, true, true, " after Return");
        require_once(revision_line(), " after Return");
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_NONE);
        require(!switch_focused(), "Tab did not leave the switch");
        require(
            after_index < 0 || mp::multiplayer_battle_room_focus() == after_index,
            "Tab from the switch did not reach " + after_name
        );
        press_key(SDLK_TAB, SDL_SCANCODE_TAB, SDL_KMOD_SHIFT);
        require(switch_focused(), "Shift+Tab did not return to the switch");
        // The room's focus goes back to no control, so the pictures show none.
        mp::multiplayer_set_battle_room_focus(team_index);
        frames(1);
        mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        frames(1);
        std::cout << "enhanced room check: Tab reaches the switch after " << team
                  << " and leaves it for " << after_name << "; Return turns it" << std::endl;
    }

    constexpr int32_t visitor_slot = 1;
    constexpr uint32_t visitor_id = 0x7E57A001U;
    const Player kept_visitor = mp::slot_player(lobby_of(), visitor_slot);
    const mp::PlayerSetupInfo kept_info = *mp::slot_info(lobby_of(), visitor_slot);
    const auto kept_console_flags = lobby_of().game->console_flags;
    // Visitor never sends a word; while Visitor sits, the room drops no one,
    // as the console's Drop 0 asks.
    const auto seat_visitor = [&](bool open_annihilation) {
        mp::Lobby& lobby = lobby_of();
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
    const auto give_record = [&](std::optional<uint32_t> highest, bool hosts_hash) {
        mp::Lobby& lobby = lobby_of();
        mp::PresenceRecords& records = lobby.presence_records;
        oa::netgame::PresenceRecord record;
        record.enhanced_highest_revision = highest;
        const std::vector<uint8_t> host = local_record();
        if (hosts_hash)
            record.sim_hash = oa::netgame::presence_sim_hash(host.data(), host.size());
        else
            record.sim_hash = oa::netgame::PresenceDigest{};
        std::size_t written = 0;
        require(
            oa::netgame::encode_presence(
                record, records.peer[visitor_slot], sizeof records.peer[visitor_slot], &written
            ) == oa::netgame::WireError::ok,
            "Visitor's record did not encode"
        );
        records.peer_size[visitor_slot] = static_cast<uint16_t>(written);
        records.peer_id[visitor_slot] = visitor_id;
    };
    const auto unseat_visitor = [&] {
        mp::Lobby& lobby = lobby_of();
        lobby.game->console_flags = kept_console_flags;
        mp::slot_player(lobby, visitor_slot) = kept_visitor;
        *mp::slot_info(lobby, visitor_slot) = kept_info;
        mp::PresenceRecords& records = lobby.presence_records;
        records.block_player[visitor_slot] = 0;
        records.peer_size[visitor_slot] = 0;
        records.peer_id[visitor_slot] = 0;
        std::memset(records.peer[visitor_slot], 0, sizeof records.peer[visitor_slot]);
        frames(2);
    };
    const auto lacks_line = [&] {
        return filled(
            "*** Enhanced Mode is off: {player}'s game doesn't have it.", "player", "Visitor"
        );
    };
    const auto lacks_reason = [&] {
        return filled("{player}'s game doesn't have it.", "player", "Visitor");
    };

    // A 3.1c Visitor, then an Open Annihilation Visitor whose record has no
    // revision: each turns the room Off naming Visitor, and leaving brings
    // Revision 1 back with one line.
    for (const bool open_annihilation : {false, true}) {
        const std::string who =
            open_annihilation ? " (Open Annihilation without a revision)" : " (3.1c)";
        clear_chat();
        seat_visitor(open_annihilation);
        if (open_annihilation)
            give_record(std::nullopt, true);
        frames(2);
        require_room(0, true, true, " with Visitor seated" + who);
        require_once(lacks_line(), " with Visitor seated" + who);
        const AutomationControl reason =
            require_named("oa.enhanced-mode.reason", "the reason is not shown" + who);
        require(reason.text == lacks_reason(), "the reason reads " + reason.text + who);
        frames(3);
        require(posted(lacks_line(), who) == 1, "the line was posted again" + who);
        if (!open_annihilation) {
            for (const CheckWindow& window : kWindows)
                check_window(window, "31c", renderer::RoomLamp::red_blinking);
            set_window(kWindows[0]);
            // The lamp blinks: red on one frame and unlit on another.
            bool seen_red = false;
            bool seen_unlit = false;
            const uint64_t started = SDL_GetTicks();
            while (!(seen_red && seen_unlit) && SDL_GetTicks() - started < 4000) {
                frames(1);
                oa_layer().read_whole_window(true);
                renderer::Surface presented;
                capture_frame_ = &presented;
                render();
                capture_frame_ = nullptr;
                oa_layer().read_whole_window(false);
                const renderer::RoomLamp lamp = lamp_in(presented);
                require(lamp != renderer::RoomLamp::green, "the lamp is green while Off");
                (lamp == renderer::RoomLamp::red ? seen_red : seen_unlit) = true;
                SDL_Delay(20);
            }
            require(seen_red && seen_unlit, "the lamp does not blink while Visitor keeps it Off");
        } else {
            for (const CheckWindow& window : kWindows)
                check_window(window, "no-revision", renderer::RoomLamp::red_blinking);
            set_window(kWindows[0]);
        }
        clear_chat();
        unseat_visitor();
        require_room(1, true, true, " after Visitor left" + who);
        require_once(revision_line(), " after Visitor left" + who);
        require(!named("oa.enhanced-mode.reason").has_value(), "the reason stays" + who);
        std::cout << "enhanced room check: Visitor" << who
                  << " turns the room Off with its line, and leaving brings Revision 1 back"
                  << std::endl;
    }

    // An Open Annihilation Visitor whose record has not arrived changes
    // nothing, posts nothing, and START waits for it.
    {
        clear_chat();
        seat_visitor(true);
        frames(3);
        require_room(1, true, true, " while Visitor's record is on its way");
        require(chat_lines().empty(), "a record on its way posted a line");
        require(!mp::lobby_enhanced_allow_start(lobby_of()), "START does not wait for Visitor");
        const AutomationControl waiting =
            require_named("oa.enhanced-mode.waiting", "the waiting line is not shown");
        require(
            waiting.text == mp::enhanced_waiting_line("Visitor", 1),
            "the waiting line reads " + waiting.text
        );
        for (const CheckWindow& window : kWindows)
            check_window(window, "waiting", renderer::RoomLamp::green);
        set_window(kWindows[0]);
        // Visitor's record arrives with the host's rules: START no longer waits.
        give_record(1u, true);
        frames(3);
        require(mp::lobby_enhanced_allow_start(lobby_of()), "START still waits for Visitor");
        require(!named("oa.enhanced-mode.waiting").has_value(), "the waiting line stays");
        require(chat_lines().empty(), "an agreeing record posted a line");
        // A Visitor with a later revision: the room still plays the host's
        // highest, so nothing is posted, and the reason names the host as
        // the game with no later revision.
        give_record(2u, true);
        frames(3);
        require_room(1, true, true, " with a later Visitor");
        require(chat_lines().empty(), "a later Visitor at the same revision posted a line");
        const AutomationControl lower =
            require_named("oa.enhanced-mode.reason", "the lower reason is not shown");
        require(
            lower.text == filled("{player}'s game has no later revision.", "player", "Steve"),
            "the lower reason reads " + lower.text
        );
        for (const CheckWindow& window : kWindows)
            check_window(window, "lower", renderer::RoomLamp::green);
        set_window(kWindows[0]);
        clear_chat();
        unseat_visitor();
        require_room(1, true, true, " after the later Visitor left");
        require(chat_lines().empty(), "a lower room that rises to the same revision posted a line");
        std::cout << "enhanced room check: a record on its way waits beside START; a later "
                     "revision names the host"
                  << std::endl;
    }

    // In each language 3.1c ships with: the lines read whole, fitted to the
    // chat list, and the waiting line beside START is not cut.
    for (const std::string_view tag : {"en", "de", "fr", "es", "it"}) {
        const std::string in = " in " + std::string(tag);
        set_language_choice(tag);
        dismiss();
        frames(2);
        require(room_open(), "the room left the battle room" + in);
        require_room(1, true, true, in);
        require(
            tag == "en" || switch_text(1) != "Enhanced Mode · Revision 1",
            "the switch's words have no translation" + in
        );
        clear_chat();
        seat_visitor(false);
        frames(2);
        require_once(lacks_line(), in);
        require(
            tag == "en" || lacks_line().find("doesn't have it") == std::string::npos,
            "the line has no translation" + in
        );
        frames(1);
        capture("enhanced-room-31c-" + std::string(tag) + "-640x480.png");
        clear_chat();
        unseat_visitor();
        require_once(revision_line(), in);
        seat_visitor(true);
        frames(3);
        const AutomationControl waiting =
            require_named("oa.enhanced-mode.waiting", "the waiting line is not shown" + in);
        require(
            waiting.text == mp::enhanced_waiting_line("Visitor", 1),
            "the waiting line is cut" + in + " (" + waiting.text + ")"
        );
        capture("enhanced-room-waiting-" + std::string(tag) + "-640x480.png");
        unseat_visitor();
        std::cout << "enhanced room check: the lines read whole and fit the chat" << in
                  << std::endl;
    }
    set_language_choice("en");
    dismiss();
    frames(2);

    // A launch that locks the room's options keeps the switch grayed and Off.
    {
        mp::Lobby& lobby = lobby_of();
        const uint16_t word = mp::lobby_option_word(*lobby.game);
        clear_chat();
        mp::lobby_option_word(*lobby.game) = static_cast<uint16_t>(word | 1U);
        frames(3);
        require(room_open(), "locking the options left the battle room");
        require_room(0, false, false, " while the options are locked");
        require_once(filled("*** Enhanced Mode: Off.", "", ""), " as the options lock");
        // A press, Return and Space change nothing; the press shows the card,
        // which says why.
        click_named(kSwitch);
        require_room(0, false, false, " after a press on the locked switch");
        const std::vector<std::string> card = card_lines();
        require(
            std::find(card.begin(), card.end(), mp::enhanced_room_launch_lock_text()) != card.end(),
            "the locked switch's card does not say why"
        );
        press_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE, SDL_KMOD_NONE);
        require(card_lines().empty(), "Escape left the card open");
        move_away();
        require(room_open(), "Escape on the card left the battle room");
        const AutomationControl locked = require_named(kSwitch, "the locked switch is not shown");
        require(
            locked.description.find(mp::enhanced_room_launch_lock_text()) != std::string::npos,
            "the locked switch does not say why"
        );
        for (const CheckWindow& window : kWindows)
            check_window(window, "locked", renderer::RoomLamp::unlit);
        set_window(kWindows[0]);
        mp::lobby_option_word(*lobby_of().game) = word;
        frames(2);
        require_room(0, false, true, " once the options unlock");
        press_switch();
        require_room(1, true, true, " turned on after the lock");
        std::cout << "enhanced room check: a launch that locks the options keeps it grayed and Off"
                  << std::endl;
    }

    // A Boneyards room: the launch counts for a ladder, so the switch starts
    // Off, and the host may turn it on.
    {
        mp::Lobby& lobby = lobby_of();
        const mp::LaunchLink kept_link = lobby.launch_link;
        mp::launch::LaunchBlock block{};
        block.stock_files = 1;
        block.bar_mutators = 1;
        lobby.launch_link.block = &block;
        lobby.launch_link.context = nullptr;
        lobby.launch_link.launch_active = launch_held;
        // The room is entered afresh, as a Boneyards launch opens it.
        if (room_enhanced_.held)
            release_enhanced_rules_once(true);
        room_enhanced_ = {};
        clear_chat();
        frames(3);
        require(!room_enhanced_switch(), "the Boneyards room's switch did not start Off");
        require_room(0, false, true, " in a Boneyards room");
        require(chat_lines().empty(), "a Boneyards room that opens Off posted a line");
        for (const CheckWindow& window : kWindows)
            check_window(window, "boneyards", renderer::RoomLamp::red);
        set_window(kWindows[0]);
        press_switch();
        require_room(1, true, true, " once the Boneyards host turns it on");
        require_once(revision_line(), " once the Boneyards host turns it on");
        lobby_of().launch_link = kept_link;
        if (room_enhanced_.held)
            release_enhanced_rules_once(true);
        room_enhanced_ = {};
        frames(3);
        require_room(1, true, true, " back in a room without a launch");
        std::cout << "enhanced room check: a Boneyards room starts Off and its host may turn it on"
                  << std::endl;
    }
    std::cout << "enhanced room check: passed" << std::endl;
}

} // namespace oa::app
