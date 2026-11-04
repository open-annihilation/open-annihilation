// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --boneyards-chat: the Chat console of a picked planet driven through SDL
// input on a replayed session and checked against what the replay records,
// and the input and passes every Boneyards check drives the running game
// with, through the engine's check host. Test scaffolding; the Boneyards
// behaviour it drives is implemented elsewhere.
#include "boneyards_check.hpp"
#include "boneyards_options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/rover_replay.hpp"
#include "oa/boneyards/screens/chat.hpp"
#include "oa/boneyards/screens/lobby_widgets.hpp"
#include <SDL3/SDL.h>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::app {

oa::boneyards::console::StatusWidget* boneyards_console_status_widget();

namespace {

namespace by = oa::boneyards;
namespace by_console = oa::boneyards::console;
namespace room_type = oa::boneyards::screens::room_type;
using namespace boneyards_check;

// Passes the replies to one request may take once it is out: the replay's
// delays are a pump or two per reply.
constexpr std::size_t kReplyPasses = 40;

// The rank of a frequency row's type in the list: planetary defaults, then
// user-created frequencies, then battle debriefings (the room list's order).
int32_t type_rank(int32_t type) {
    switch (type) {
    case room_type::planetary_default:
        return 0;
    case room_type::user_created:
        return 1;
    default:
        return 2;
    }
}

// The log rows from row `from` on that hold `text`.
int32_t log_count(const by_console::StatusWidget* widget, int32_t from, const std::string& text) {
    char line[0x100]{};
    int32_t found = 0;
    for (int32_t i = from;
         by_console::boneyards_status_widget_chat_log_line(widget, i, line, sizeof line);
         ++i)
        found += std::string(line).find(text) != std::string::npos ? 1 : 0;
    return found;
}

bool row_of(
    const by_console::StatusWidget* widget,
    by_console::ChatList list,
    uint32_t id,
    by_console::ChatRowProbe* out
) {
    for (int32_t i = 0; by_console::boneyards_status_widget_chat_row(widget, list, i, out); ++i)
        if (out->id == id)
            return true;
    return false;
}

// The name a frequency row keeps of a room the replay lists: the room list
// copies at most 23 characters.
std::string row_name(const char* name) {
    return std::string(name).substr(0, oa::boneyards::screens::kRoomNameCharacters);
}

// The frequency rows must be the replay's rooms of the area that get one
// (every type but unlisted), each under its name in the replay, in type order
// with the entered one circled; the player rows the replay's members of the
// entered room.
bool rows_match(const by_console::StatusWidget* widget, uint32_t area, uint32_t room, bool quiet) {
    const auto say = [quiet](const std::string& what) {
        if (!quiet)
            std::cout << "boneyards login check: " << what << '\n';
    };
    by_console::ChatRowProbe row{};
    int32_t expected_rooms = 0;
    by::ReplayRoom expected{};
    for (std::size_t i = 0; by::rover_replay_room(area, i, &expected); ++i) {
        if (expected.type == room_type::unlisted)
            continue;
        ++expected_rooms;
        if (!row_of(widget, by_console::ChatList::frequencies, expected.room_id, &row) ||
            row.name != row_name(expected.name) || row.type != expected.type) {
            say("no frequency row for " + std::to_string(expected.room_id) + " '" + expected.name +
                "'");
            return false;
        }
        if (row.current != (expected.room_id == room)) {
            say(std::string("frequency row '") + expected.name + "' is " +
                (row.current ? "" : "not ") + "circled");
            return false;
        }
    }
    int32_t rooms = 0;
    int32_t last_rank = 0;
    for (; by_console::boneyards_status_widget_chat_row(
             widget, by_console::ChatList::frequencies, rooms, &row
         );
         ++rooms) {
        if (type_rank(row.type) < last_rank) {
            say(std::string("frequency row '") + row.name + "' is out of type order");
            return false;
        }
        last_rank = type_rank(row.type);
    }
    if (rooms != expected_rooms) {
        say(std::to_string(rooms) + " frequency rows for " + std::to_string(expected_rooms) +
            " in the replay");
        return false;
    }
    int32_t expected_players = 0;
    by::ReplayMember member{};
    for (std::size_t i = 0; by::rover_replay_member(area, room, i, &member);
         ++i, ++expected_players)
        if (!row_of(widget, by_console::ChatList::players, member.user_id, &row) ||
            std::string(row.name) != member.name) {
            say(std::string("no player row for ") + member.name);
            return false;
        }
    int32_t players = 0;
    while (by_console::boneyards_status_widget_chat_row(
        widget, by_console::ChatList::players, players, &row
    ))
        ++players;
    if (players != expected_players) {
        say(std::to_string(players) + " player rows for " + std::to_string(expected_players) +
            " members in the replay");
        return false;
    }
    return true;
}

// Every line the replay has a member say in the room, as the log prints it
// from row `from` on: the member's name, then the text. A line from the
// server (user 0) names no member, and the lobby drops a sender without a
// player row.
bool said_lines_shown(
    const by_console::StatusWidget* widget, uint32_t area, uint32_t room, int32_t from, bool quiet
) {
    by::ReplaySaid said{};
    for (std::size_t i = 0; by::rover_replay_said(area, room, i, &said); ++i) {
        if (said.user_id == 0)
            continue;
        by::ReplayMember member{};
        std::string line;
        for (std::size_t m = 0; by::rover_replay_member(area, room, m, &member); ++m)
            if (member.user_id == said.user_id)
                line = std::string(member.name) + ": " + said.text;
        if (!line.empty() && log_count(widget, from, line) == 0) {
            if (!quiet)
                std::cout << "boneyards login check: the log lacks \"" << line << "\"\n";
            return false;
        }
    }
    return true;
}

// The ticker text the system alert keeps for the replay's last ticker line the
// login brings: the line and the four-space spacer; "" without one.
std::string replay_ticker() {
    std::string ticker;
    by::ReplayAlert alert{};
    for (std::size_t i = 0; by::rover_replay_alert(i, &alert); ++i)
        if (alert.kind == 0 && alert.area_id == 0)
            ticker = std::string(alert.text) + "    ";
    return ticker;
}

// Whether `shown` is text the news ticker gives the label: characters read
// around the ticker but for its last one.
bool ticker_window(const std::string& ticker, const std::string& shown) {
    const std::string ring = ticker.substr(0, ticker.size() - 1);
    std::string around = ring;
    while (!ring.empty() && around.size() < shown.size() + ring.size())
        around += ring;
    return !shown.empty() && around.find(shown) != std::string::npos;
}

// The replay's pages the entry of `room` brings.
std::vector<std::string> replay_pages(uint32_t area, uint32_t room) {
    std::vector<std::string> pages;
    by::ReplayAlert alert{};
    for (std::size_t i = 0; by::rover_replay_alert(i, &alert); ++i)
        if (alert.kind == 2 && alert.area_id == area && alert.room_id == room)
            pages.emplace_back(alert.text);
    return pages;
}

// The entry line the frequency join prints for the room the replay lists as `room`.
std::string entered_line(uint32_t area, uint32_t room) {
    by::ReplayRoom expected{};
    for (std::size_t i = 0; by::rover_replay_room(area, i, &expected); ++i)
        if (expected.room_id == room)
            return "[Entered '" + row_name(expected.name) + "']";
    return "[Entered room " + std::to_string(room) + "]";
}

void print_rows(const by_console::StatusWidget* widget, const char* when) {
    by_console::ChatRowProbe row{};
    std::cout << "boneyards login check: " << when << ": frequencies";
    for (int32_t i = 0; by_console::boneyards_status_widget_chat_row(
             widget, by_console::ChatList::frequencies, i, &row
         );
         ++i)
        std::cout << (i == 0 ? " " : ", ") << row.id << " '" << row.name << "' " << row.count
                  << (row.current ? " (circled)" : "");
    std::cout << "; players";
    for (int32_t i = 0; by_console::boneyards_status_widget_chat_row(
             widget, by_console::ChatList::players, i, &row
         );
         ++i)
        std::cout << (i == 0 ? " " : ", ") << row.name;
    std::cout << '\n';
}

// The request the replay received at or after `from` that `match` accepts.
template <typename Match>
bool sent_since(std::size_t from, by::ReplaySent* out, const Match& match) {
    for (std::size_t i = from; by::rover_replay_sent(i, out); ++i)
        if (match(*out))
            return true;
    return false;
}

} // namespace

// Headless runs have no renderer: the check host takes the canvas
// coordinates as window coordinates there.
void boneyards_pointer(const CheckHost& host, uint32_t type, int32_t x, int32_t y, uint8_t clicks) {
    (void)host.pointer(host.context, type, x, y, clicks);
}

void boneyards_key(const CheckHost& host, uint32_t code) {
    for (const auto type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
        SDL_Event event{};
        event.type = type;
        event.key.key = code;
        (void)host.dispatch(host.context, &event);
    }
}

// Pick (done by the caller): the planet is the session's area and its census
// has arrived. Ticker: the Main Frame's news ticker must hold the replay's
// ticker line, which its label scrolls. Chat: a click on CHAT must bring the
// console up and enter a frequency, whose rows, entry line, the replay's lines
// and the pages the replay brings with it must then show as the replay has
// them. Say: the line typed and sent with Enter must reach Rover
// as a say to the whole frequency and come back to the log. Switch: a double
// click on the other planetary default's row must switch to it, leaving the
// first once it is entered, and announce the entry. Pane: a click on the
// second player row, a member whose profile the replay records, must show
// the player pane with that profile's name and clan, the activity light
// running from the click until the profile arrives. Create: CREATE must
// open the create-frequency pane, whose GO comes on for the typed name and
// asks Rover for the frequency, and the one the replay makes must be
// switched to and entered, closing the pane. Release: pressing it must leave
// the frequency and hand the map back.
int check_boneyards_chat(
    const CheckHost& host,
    const Options& options,
    const std::vector<std::string>& sounds,
    std::size_t pass,
    std::size_t passes
) {
    const auto area = static_cast<uint32_t>(*boneyards_options().boneyards_chat);
    by_console::StatusWidget* widget = boneyards_console_status_widget();
    by_console::MainFrameProbe probe{};
    const auto refresh = [&] { by_console::boneyards_status_widget_probe(widget, &probe); };
    const auto run = [&] {
        boneyards_pass(host);
        ++pass;
        refresh();
    };
    const auto pump = [&](const auto& done, std::size_t limit) {
        std::size_t used = 0;
        while (!done() && pass < passes && used < limit) {
            run();
            ++used;
        }
        return used;
    };
    const auto click = [&](int32_t x, int32_t y, uint8_t clicks) {
        boneyards_pointer(host, SDL_EVENT_MOUSE_MOTION, x, y, 0);
        boneyards_pointer(host, SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, clicks);
        boneyards_pointer(host, SDL_EVENT_MOUSE_BUTTON_UP, x, y, clicks);
        refresh();
    };
    const auto press_button = [&](by_console::ConsoleButton which) {
        const by_console::ScreenRect button = probe.console_buttons[static_cast<int32_t>(which)];
        click(button.x + button.width / 2, button.y + button.height / 2, 1);
    };
    const auto snapshot = [&](const char* step) {
        if (!options.snapshot.empty())
            write_ppm(
                options.snapshot.parent_path() /
                    (options.snapshot.stem().string() + '-' + step + ".ppm"),
                *host.surface(host.context)
            );
    };
    const auto in_room = [&] {
        return probe.chat_room_id != 0 && probe.chat_pending_room_id == 0 &&
               !probe.activity_light_running;
    };
    refresh();

    const std::string ticker = replay_ticker();
    if (!ticker.empty()) {
        std::cout << "boneyards login check: ticker \"" << probe.ticker_text << "\", label \""
                  << probe.ticker_shown << "\"\n";
        if (ticker != probe.ticker_text || !ticker_window(ticker, probe.ticker_shown)) {
            std::cout << "boneyards login check: the ticker does not scroll the replay's \""
                      << ticker << "\"\n";
            return kEntryChatTickerWrong;
        }
        snapshot("ticker");
    }

    const std::size_t open_from = sounds.size();
    press_button(by_console::ConsoleButton::chat);
    const auto shown = [&] {
        return probe.console_screen == by_console::console_screen::chat && probe.screen_shown &&
               !probe.layer_sliding;
    };
    std::size_t open_passes = pump([&] { return shown() && in_room(); }, passes);
    print_probe("CHAT pressed", probe);
    if (!shown() || !heard_since(sounds, open_from, kPlanetButtonSound) ||
        !heard_since(sounds, open_from, kGreenSlideSound) ||
        !heard_since(sounds, open_from, kBigButtonSound)) {
        std::cout << "boneyards login check: CHAT did not bring its console up\n";
        return kEntryOpenTimeout;
    }
    if (!in_room()) {
        std::cout << "boneyards login check: the chat console entered no frequency on " << area
                  << " (frequency " << probe.chat_room_id << ", pending "
                  << probe.chat_pending_room_id << ")\n";
        return kEntryChatNotEntered;
    }
    const uint32_t first_room = probe.chat_room_id;
    const std::string first_entry = entered_line(area, first_room);
    const std::vector<std::string> pages = replay_pages(area, first_room);
    const auto pages_shown = [&] {
        for (const std::string& page : pages)
            if (log_count(widget, 0, page) == 0)
                return false;
        return true;
    };
    const auto first_shown = [&] {
        return rows_match(widget, area, first_room, true) &&
               log_count(widget, 0, first_entry) > 0 &&
               said_lines_shown(widget, area, first_room, 0, true) && pages_shown();
    };
    open_passes += pump(first_shown, kReplyPasses);
    print_rows(widget, "entered");
    std::cout << "boneyards login check: frequency " << first_room << " entered after "
              << open_passes << " passes, " << probe.chat_lines
              << " lines; newest: " << probe.chat_last_line << '\n';
    if (!rows_match(widget, area, first_room, false) ||
        !said_lines_shown(widget, area, first_room, 0, false))
        return kEntryChatRowsWrong;
    if (log_count(widget, 0, first_entry) == 0) {
        std::cout << "boneyards login check: the log lacks " << first_entry << '\n';
        return kEntryChatRowsWrong;
    }
    for (const std::string& page : pages) {
        if (log_count(widget, 0, page) == 0) {
            std::cout << "boneyards login check: the log lacks the page \"" << page << "\"\n";
            return kEntryChatPageMissing;
        }
        std::cout << "boneyards login check: the page \"" << page << "\" is in the log\n";
    }
    snapshot("chat");

    if (!boneyards_options().boneyards_say.empty()) {
        const std::string say = boneyards_options().boneyards_say;
        const std::size_t sent_from = by::rover_replay_sent_count();
        const int32_t from = probe.chat_lines;
        SDL_Event text{};
        text.type = SDL_EVENT_TEXT_INPUT;
        text.text.text = say.c_str();
        (void)host.dispatch(host.context, &text);
        boneyards_key(host, SDLK_RETURN);
        refresh();
        by::ReplaySent sent{};
        const bool said = sent_since(sent_from, &sent, [&](const by::ReplaySent& s) {
            return s.request == by::ReplayRequest::say;
        });
        const std::size_t say_passes =
            pump([&] { return log_count(widget, from, ": " + say) > 0; }, kReplyPasses);
        if (!said || sent.room_id != first_room || sent.area_id != area ||
            sent.target_user_id != 0 || say != sent.text) {
            std::cout << "boneyards login check: \"" << say << "\" did not reach Rover as a say to "
                      << "frequency " << first_room << " on " << area << '\n';
            return kEntryChatSayNotSent;
        }
        if (log_count(widget, from, ": " + say) == 0) {
            std::cout << "boneyards login check: \"" << say << "\" was not echoed to the log\n";
            return kEntryChatSayNotSent;
        }
        std::cout << "boneyards login check: \"" << say << "\" said to frequency " << sent.room_id
                  << " and echoed after " << say_passes << " passes: " << probe.chat_last_line
                  << '\n';
    }

    by_console::ChatRowProbe member{};
    by::ReplayProfile expected{};
    constexpr int32_t kPaneRow = 1;
    if (!by_console::boneyards_status_widget_chat_row(
            widget, by_console::ChatList::players, kPaneRow, &member
        ) ||
        !member.shown || !by::rover_replay_profile(member.id, &expected)) {
        std::cout << "boneyards login check: no second player row with a profile in the replay\n";
        return kEntryChatPaneEmpty;
    }
    {
        click(member.x, member.y, 1);
        const bool light_ran = probe.activity_light_running;
        by_console::ChatPaneProbe pane{};
        const auto read_pane = [&] {
            return by_console::boneyards_status_widget_chat_pane(
                widget, by_console::ChatPane::player, &pane
            );
        };
        const auto pane_filled = [&] {
            return probe.chat_pane_user_id == member.id &&
                   std::string(probe.chat_pane_name) == expected.name && read_pane() &&
                   pane.shown && std::string(pane.name) == expected.name;
        };
        const std::size_t pane_passes = pump(pane_filled, kReplyPasses);
        if (!pane_filled() || std::string(probe.chat_pane_clan) != expected.clan ||
            std::string(pane.clan) != expected.clan) {
            std::cout << "boneyards login check: a click on " << member.name
                      << "'s row did not show the player pane with the replay's profile (pane "
                      << (pane.shown ? "shown" : "hidden") << ", " << probe.chat_pane_user_id
                      << " '" << pane.name << "' clan '" << pane.clan << "')\n";
            return kEntryChatPaneEmpty;
        }
        if (!light_ran || probe.activity_light_running) {
            std::cout << "boneyards login check: the activity light did not run from the click on "
                      << member.name << "'s row to the pane's profile (light "
                      << (light_ran ? "ran" : "did not run") << ", now "
                      << (probe.activity_light_running ? "running" : "dark") << ")\n";
            return kEntryActivityLightWrong;
        }
        std::cout << "boneyards login check: " << member.name
                  << "'s profile filled the player pane after " << pane_passes << " passes ("
                  << pane.rank << ", clan " << pane.clan
                  << "), the activity light running until it arrived\n";
        snapshot("chat-player");
    }

    by_console::ChatRowProbe target{};
    for (int32_t i = 0; by_console::boneyards_status_widget_chat_row(
             widget, by_console::ChatList::frequencies, i, &target
         );
         ++i)
        if (target.type == room_type::planetary_default && target.id != first_room)
            break;
    if (target.type != room_type::planetary_default || target.id == first_room || !target.shown) {
        std::cout << "boneyards login check: no other planetary default in view on " << area
                  << '\n';
        return kEntryChatNotSwitched;
    }
    const std::size_t switch_from = by::rover_replay_sent_count();
    const std::string target_entry = entered_line(area, target.id);
    const int32_t switch_lines = probe.chat_lines;
    click(target.x, target.y, 1);
    click(target.x, target.y, 2);
    by::ReplaySent sent{};
    const bool switched = sent_since(switch_from, &sent, [](const by::ReplaySent& s) {
        return s.request == by::ReplayRequest::switch_room;
    });
    if (!switched || sent.room_id != target.id || sent.area_id != area ||
        sent.password[0] != '\0' || sent.old_room_id != first_room || sent.old_area_id != area) {
        std::cout << "boneyards login check: a double click on '" << target.name << "' at "
                  << target.x << ',' << target.y << " did not switch to frequency " << target.id
                  << '\n';
        return kEntryChatNotSwitched;
    }
    const auto switch_shown = [&] {
        return in_room() && probe.chat_room_id == target.id &&
               log_count(widget, switch_lines, target_entry) > 0 &&
               rows_match(widget, area, target.id, true) &&
               said_lines_shown(widget, area, target.id, switch_lines, true);
    };
    const std::size_t switch_passes = pump(switch_shown, kReplyPasses);
    print_rows(widget, "switched");
    if (!switch_shown()) {
        std::cout << "boneyards login check: frequency " << target.id << " '" << target.name
                  << "' was not entered after the switch (frequency " << probe.chat_room_id
                  << ")\n";
        (void)rows_match(widget, area, target.id, false);
        (void)said_lines_shown(widget, area, target.id, switch_lines, false);
        return kEntryChatNotSwitched;
    }
    std::cout << "boneyards login check: switched from " << first_room << " to " << target.id
              << " '" << target.name << "' after " << switch_passes << " passes\n";
    snapshot("chat-switched");
    uint32_t release_room = target.id;

    if (!boneyards_options().boneyards_create.empty()) {
        const std::string name = boneyards_options().boneyards_create;
        by_console::ChatPaneProbe pane{};
        const auto read_pane = [&] {
            return by_console::boneyards_status_widget_chat_pane(
                widget, by_console::ChatPane::create_frequency, &pane
            );
        };
        const by_console::ScreenRect create = probe.chat_create_button;
        click(create.x + create.width / 2, create.y + create.height / 2, 1);
        if (!read_pane() || !pane.shown) {
            std::cout << "boneyards login check: CREATE did not open the create-frequency pane\n";
            return kEntryChatNotCreated;
        }
        const by_console::ScreenRect entry = pane.entries[0];
        click(entry.x + entry.width / 2, entry.y + entry.height / 2, 1);
        SDL_Event typed{};
        typed.type = SDL_EVENT_TEXT_INPUT;
        typed.text.text = name.c_str();
        (void)host.dispatch(host.context, &typed);
        const auto go_on = [&] {
            return read_pane() && pane.go_state == by::button_state::unpressed;
        };
        pump(go_on, kReplyPasses);
        snapshot("chat-create");
        if (!go_on()) {
            std::cout << "boneyards login check: GO stayed off with '" << name << "' typed\n";
            return kEntryChatNotCreated;
        }
        const std::size_t create_from = by::rover_replay_sent_count();
        const int32_t create_lines = probe.chat_lines;
        click(pane.go.x + pane.go.width / 2, pane.go.y + pane.go.height / 2, 1);
        const bool asked = sent_since(create_from, &sent, [](const by::ReplaySent& s) {
            return s.request == by::ReplayRequest::create_room;
        });
        if (!asked || sent.area_id != area || name != sent.name || sent.text[0] != '\0' ||
            sent.password[0] != '\0') {
            std::cout << "boneyards login check: GO did not ask Rover to create '" << name
                      << "' on " << area << '\n';
            return kEntryChatNotCreated;
        }
        const std::string created_entry = "[Entered '" + row_name(name.c_str()) + "']";
        const auto created_shown = [&] {
            return in_room() && probe.chat_room_id != target.id &&
                   log_count(widget, create_lines, created_entry) > 0 && read_pane() && !pane.shown;
        };
        const std::size_t create_passes = pump(created_shown, kReplyPasses);
        const bool switched_in = sent_since(create_from, &sent, [](const by::ReplaySent& s) {
            return s.request == by::ReplayRequest::switch_room;
        });
        if (!created_shown() || !switched_in || sent.room_id != probe.chat_room_id ||
            sent.area_id != area || sent.old_room_id != target.id) {
            std::cout << "boneyards login check: the frequency made for '" << name
                      << "' was not entered (frequency " << probe.chat_room_id << ", pending "
                      << probe.chat_pending_room_id << ")\n";
            return kEntryChatNotCreated;
        }
        print_rows(widget, "created");
        std::cout << "boneyards login check: '" << name << "' created as frequency "
                  << probe.chat_room_id << " and entered after " << create_passes << " passes\n";
        release_room = probe.chat_room_id;
        snapshot("chat-created");
    }

    const std::size_t leave_from = by::rover_replay_sent_count();
    const std::size_t release_from = sounds.size();
    press_button(by_console::ConsoleButton::release);
    const auto handed_back = [&] {
        return probe.console_screen == by_console::console_screen::none && !probe.layer_sliding &&
               !probe.layer_covering && !probe.map_locked && !probe.unlock_pending &&
               probe.release_button == by::button_state::disabled;
    };
    const std::size_t release_passes = pump(handed_back, passes);
    print_probe("Release pressed", probe);
    if (!handed_back() || !heard_since(sounds, release_from, kGreenSlideSound) ||
        !heard_since(sounds, release_from, kBigButtonSound)) {
        std::cout << "boneyards login check: Release did not hand the map back from the chat\n";
        return kEntryReleaseFailed;
    }
    const bool left = sent_since(leave_from, &sent, [&](const by::ReplaySent& s) {
        return s.request == by::ReplayRequest::leave_room && s.area_id == area &&
               s.room_id == release_room;
    });
    if (!left) {
        std::cout << "boneyards login check: Release did not leave frequency " << release_room
                  << '\n';
        return kEntryChatNotLeft;
    }
    std::cout << "boneyards login check: Release left frequency " << release_room
              << " and handed the map back after " << release_passes << " passes\n";
    return kEntryPassed;
}

} // namespace oa::app
