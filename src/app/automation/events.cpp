// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The events the automation endpoint sends (events.hpp).
#include "events.hpp"

#include "reports.hpp"

#include "oa/core/game_state.h"
#include "oa/core/world.h"
#include "oa/sim/messages.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <algorithm>
#include <optional>
#include <string>

namespace oa::app::automation {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

// Every kind of event, in the order subscribe's answer lists them.
constexpr EventKindName kEventKinds[] = {
    {event_kind::screen, "screen"},
    {event_kind::player, "player"},
    {event_kind::ready, "ready"},
    {event_kind::chat, "chat"},
    {event_kind::loaded, "loaded"},
    {event_kind::match, "match"},
    {event_kind::pause, "pause"},
    {event_kind::speed, "speed"},
    {event_kind::alliance, "alliance"},
    {event_kind::transfer, "transfer"},
    {event_kind::game_over, "game_over"},
    {event_kind::focus, "focus"},
};

// The name the endpoint gives the screen a match is played on.
constexpr std::string_view kMatchScreen = "match";
// Game.sim_run_flags: the match is paused.
constexpr uint16_t kRunFlagPaused = 0x0001;
// The most a loading screen's row reports, in percent.
constexpr uint32_t kFullProgress = 100;
// What starts and ends a chat line's sender ("<Name> text"), and what
// separates the sender from the one player a line goes to ("<Name->To> text").
constexpr std::string_view kSenderOpen = "<";
constexpr std::string_view kSenderClose = "> ";
constexpr std::string_view kSenderTo = "->";

/// Starts an event when the hooks want its kind.
///
/// @param hooks where events go
/// @param kind the kind's bit
/// @return the event, its object open, or nothing when it is not wanted
std::optional<JsonWriter> begin_event(const EventHooks& hooks, uint32_t kind) {
    if (hooks.wanted == nullptr || hooks.begin == nullptr || hooks.send == nullptr ||
        !hooks.wanted(hooks.context, kind))
        return std::nullopt;
    for (const EventKindName& named : kEventKinds)
        if (named.kind == kind)
            return hooks.begin(hooks.context, named.name);
    return std::nullopt;
}

/// Sends an event begun with begin_event.
///
/// @param hooks where events go
/// @param[in,out] event the event; its object is closed
void send_event(const EventHooks& hooks, JsonWriter& event) {
    hooks.send(hooks.context, event);
}

/// Sends a screen event.
///
/// @param hooks where events go
/// @param name the screen shown
/// @param previous the screen shown before; null when none is named
/// @param dialogs the dialogs over the screen, in their order
void send_screen(
    const EventHooks& hooks,
    std::string_view name,
    const std::string* previous,
    std::span<const std::string> dialogs
) {
    auto event = begin_event(hooks, event_kind::screen);
    if (!event)
        return;
    event->key("name");
    event->string(name);
    event->key("previous");
    if (previous != nullptr)
        event->string(*previous);
    else
        event->null();
    event->key("dialogs");
    event->begin_array();
    for (const std::string& dialog : dialogs)
        event->string(dialog);
    event->end_array();
    send_event(hooks, *event);
}

/// Sends a chat event for a line of a chat ring, when the line is a chat
/// line ("<Name> text" or "<Name->To> text").
///
/// @param hooks where events go
/// @param where "room" or "match"
/// @param line the line, in the game's code page (game_text)
void send_chat(const EventHooks& hooks, std::string_view where, std::string_view line) {
    if (!line.starts_with(kSenderOpen))
        return;
    const size_t close = line.find(kSenderClose);
    if (close == std::string_view::npos)
        return;
    std::string_view from = line.substr(kSenderOpen.size(), close - kSenderOpen.size());
    std::string_view to;
    if (const size_t arrow = from.find(kSenderTo); arrow != std::string_view::npos) {
        to = from.substr(arrow + kSenderTo.size());
        from = from.substr(0, arrow);
    }
    auto event = begin_event(hooks, event_kind::chat);
    if (!event)
        return;
    event->key("where");
    event->string(where);
    event->key("from");
    event->string(from);
    if (!to.empty()) {
        event->key("to");
        event->string(to);
    }
    event->key("text");
    event->string(line.substr(close + kSenderClose.size()));
    send_event(hooks, *event);
}

/// Sends a player event: a seat of the battle room taken or left.
///
/// @param hooks where events go
/// @param change "joined" or "left"
/// @param slot the seat
/// @param name the player's name
void send_seat(
    const EventHooks& hooks, std::string_view change, uint8_t slot, std::string_view name
) {
    auto event = begin_event(hooks, event_kind::player);
    if (!event)
        return;
    event->key("change");
    event->string(change);
    event->key("slot");
    event->integer(slot);
    event->key("name");
    event->string(name);
    send_event(hooks, *event);
}

/// Sends a match event.
///
/// @param hooks where events go
/// @param change "started" or "ended"
void send_match(const EventHooks& hooks, std::string_view change) {
    auto event = begin_event(hooks, event_kind::match);
    if (!event)
        return;
    event->key("change");
    event->string(change);
    send_event(hooks, *event);
}

/// Compares the battle room with what the watch saw of it last.
///
/// @param[in,out] watch what the watch saw last
/// @param lobby the lobby, while the battle room shows; null otherwise
/// @param hooks where events go
void watch_room(EventWatch& watch, mp::Lobby* lobby, const EventHooks& hooks) {
    if (lobby == nullptr || lobby->game == nullptr) {
        watch.in_room = false;
        watch.seats = {};
        return;
    }
    oa::Game& game = *lobby->game;
    if (!watch.in_room) {
        watch.in_room = true;
        watch.seats = {};
        watch.room_chat_next = static_cast<uint16_t>(mp::lobby_chat_tail(game) % mp::kChatLines);
    }
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        oa::Player& player = mp::slot_player(*lobby, slot);
        const bool taken = mp::slot_active(player);
        EventWatch::Seat& seat = watch.seats[slot];
        if (seat.taken && (!taken || seat.player_id != player.player_id)) {
            send_seat(hooks, "left", slot, seat.name);
            seat = {};
        }
        if (!taken)
            continue;
        const std::string name = game_text(player.name, sizeof player.name);
        if (!seat.taken) {
            seat.taken = true;
            seat.player_id = player.player_id;
            send_seat(hooks, "joined", slot, name);
        }
        seat.name = name;
        const mp::PlayerSetupInfo* info = mp::slot_info(*lobby, slot);
        const bool ready = info != nullptr && (info->options & mp::option::ready) != 0;
        if (ready != seat.ready) {
            seat.ready = ready;
            if (auto event = begin_event(hooks, event_kind::ready)) {
                event->key("slot");
                event->integer(slot);
                event->key("name");
                event->string(name);
                event->key("ready");
                event->boolean(ready);
                send_event(hooks, *event);
            }
        }
    }
    const auto head = static_cast<uint16_t>(mp::lobby_chat_head(game) % mp::kChatLines);
    for (; watch.room_chat_next != head;
         watch.room_chat_next = static_cast<uint16_t>((watch.room_chat_next + 1) % mp::kChatLines))
        send_chat(
            hooks,
            "room",
            game_text(mp::lobby_chat_line(game, watch.room_chat_next), mp::kChatLineBytes)
        );
}

/// Starts watching a match the watch has not seen: no seats loaded and no
/// chat lines yet, its pause, speed and alliances as they are.
///
/// @param[in,out] watch what the watch saw last
/// @param match the match
void begin_match(EventWatch& watch, const oa::World& match) {
    const oa::Game& game = match.game;
    watch.match = &match;
    watch.match_started = false;
    watch.paused = (game.sim_run_flags & kRunFlagPaused) != 0;
    watch.speed = game.requested_speed;
    watch.decided = false;
    watch.results.reset();
    watch.match_chat_next = static_cast<uint16_t>(game.chat_head % OA_CHAT_LINE_COUNT);
    watch.load_progress = {};
    for (uint8_t from = 0; from < OA_PLAYER_COUNT; ++from)
        for (uint8_t to = 0; to < OA_PLAYER_COUNT; ++to)
            watch.alliances[from][to] = game.players[from].alliance[to] != 0 ? 1 : 0;
    watch.local_load = -1;
}

/// Compares the running match with what the watch saw of it last.
///
/// @param[in,out] watch what the watch saw last, watching the match
/// @param screen the screen shown
/// @param hooks where events go
void watch_match(EventWatch& watch, std::string_view screen, const EventHooks& hooks) {
    const oa::Game& game = watch.match->game;
    // Kept while the world is here: the end is reported after it may be gone.
    watch.results = match_results(*watch.match);
    if (!watch.match_started && screen == kMatchScreen) {
        watch.match_started = true;
        send_match(hooks, "started");
    }
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const oa::Player& player = game.players[slot];
        if (!player_seated(player) || player.load_progress == watch.load_progress[slot])
            continue;
        watch.load_progress[slot] = player.load_progress;
        if (auto event = begin_event(hooks, event_kind::loaded)) {
            event->key("player");
            event->string(game_text(player.name, sizeof player.name));
            event->key("slot");
            event->integer(slot);
            event->key("progress");
            event->integer(player.load_progress);
            event->key("local");
            event->boolean(player.status == OA_PLAYER_STATUS_LOCAL);
            send_event(hooks, *event);
        }
    }
    const bool paused = (game.sim_run_flags & kRunFlagPaused) != 0;
    if (paused != watch.paused) {
        watch.paused = paused;
        if (auto event = begin_event(hooks, event_kind::pause)) {
            event->key("paused");
            event->boolean(paused);
            send_event(hooks, *event);
        }
    }
    if (game.requested_speed != watch.speed) {
        watch.speed = game.requested_speed;
        if (auto event = begin_event(hooks, event_kind::speed)) {
            event->key("speed");
            event->integer(game.requested_speed);
            send_event(hooks, *event);
        }
    }
    for (uint8_t from = 0; from < OA_PLAYER_COUNT; ++from) {
        const oa::Player& giver = game.players[from];
        for (uint8_t to = 0; to < OA_PLAYER_COUNT; ++to) {
            const uint8_t allied = giver.alliance[to] != 0 ? 1 : 0;
            if (allied == watch.alliances[from][to])
                continue;
            watch.alliances[from][to] = allied;
            const oa::Player& taker = game.players[to];
            if (from == to || !player_seated(giver) || !player_seated(taker))
                continue;
            if (auto event = begin_event(hooks, event_kind::alliance)) {
                event->key("from");
                event->string(game_text(giver.name, sizeof giver.name));
                event->key("from_slot");
                event->integer(from);
                event->key("to");
                event->string(game_text(taker.name, sizeof taker.name));
                event->key("to_slot");
                event->integer(to);
                event->key("allied");
                event->boolean(allied != 0);
                send_event(hooks, *event);
            }
        }
    }
    if (!watch.decided && match_decided(game)) {
        watch.decided = true;
        if (auto event = begin_event(hooks, event_kind::game_over)) {
            event->key("outcome");
            if (const auto outcome = match_outcome(game))
                event->string(*outcome);
            else
                event->null();
            event->key("winner");
            if (const auto winner = match_winner(game))
                event->string(
                    game_text(game.players[*winner].name, sizeof game.players[*winner].name)
                );
            else
                event->null();
            if (watch.results)
                write_result_fields(*event, *watch.results, false);
            send_event(hooks, *event);
        }
    }
    const auto head = static_cast<uint16_t>(game.chat_head % OA_CHAT_LINE_COUNT);
    for (; watch.match_chat_next != head;
         watch.match_chat_next =
             static_cast<uint16_t>((watch.match_chat_next + 1) % OA_CHAT_LINE_COUNT))
        send_chat(
            hooks,
            "match",
            game_text(game.chat_lines[watch.match_chat_next], oa::sim::messages::text_bytes)
        );
}

/// Stops watching the match, reporting its end when its start was reported.
/// Reads nothing of the match, which may be gone.
///
/// @param[in,out] watch what the watch saw last
/// @param hooks where events go
void end_match(EventWatch& watch, const EventHooks& hooks) {
    if (watch.match_started) {
        auto event = begin_event(hooks, event_kind::match);
        if (event) {
            event->key("change");
            event->string("ended");
            if (watch.results)
                write_result_fields(*event, *watch.results, true);
            send_event(hooks, *event);
        }
    }
    watch.match = nullptr;
    watch.match_started = false;
    watch.results.reset();
}

} // namespace

std::span<const EventKindName> event_kind_names() noexcept {
    return kEventKinds;
}

uint32_t event_kind_named(std::string_view name) noexcept {
    for (const EventKindName& named : kEventKinds)
        if (named.name == name)
            return named.kind;
    return 0;
}

/// Sends one focus event when the focused control's name changes.
///
/// The first sight stores the name and sends nothing. A later sight with the
/// same name sends nothing. The stored name moves on even when the event is
/// not wanted. An empty name is a real change and is written null. A moment
/// that did not read the focus clears the first sight, and the next one that
/// does stores the name and sends nothing. The name is already the first
/// focused control that is shown: a hidden one is left out before it arrives.
///
/// @param[in,out] watch what the watch saw last
/// @param now what the game holds now
/// @param hooks where events go
void watch_focus(EventWatch& watch, const Observed& now, const EventHooks& hooks) {
    if (!now.focus_read) {
        watch.focus_seen = false;
        return;
    }
    if (!watch.focus_seen) {
        watch.focus_seen = true;
        watch.focus_name = now.focus_name;
        return;
    }
    if (now.focus_name == watch.focus_name)
        return;
    watch.focus_name = now.focus_name;
    auto event = begin_event(hooks, event_kind::focus);
    if (!event)
        return;
    event->key("name");
    if (now.focus_name.empty())
        event->null();
    else
        event->string(now.focus_name);
    event->key("label");
    event->string(now.focus_label);
    event->key("role");
    event->string(now.focus_role);
    send_event(hooks, *event);
}

void watch_events(EventWatch& watch, const Observed& now, const EventHooks& hooks) {
    if (!watch.seen) {
        watch.seen = true;
        watch.screen = now.screen;
    }
    if (watch.announce_screen) {
        watch.announce_screen = false;
        watch.screen = now.screen;
        send_screen(hooks, now.screen, nullptr, now.dialogs);
    } else if (now.screen != watch.screen) {
        send_screen(hooks, now.screen, &watch.screen, now.dialogs);
        watch.screen = now.screen;
    }
    // The screen is heard before the focus on it, so a change of screen
    // announces the screen and then the control that opened focused.
    watch_focus(watch, now, hooks);
    watch_room(watch, now.room, hooks);
    if (now.match != watch.match) {
        if (watch.match != nullptr)
            end_match(watch, hooks);
        if (now.match != nullptr)
            begin_match(watch, *now.match);
    }
    if (watch.match != nullptr)
        watch_match(watch, now.screen, hooks);
}

void watch_match_gone(EventWatch& watch, const EventHooks& hooks) {
    if (watch.match != nullptr)
        end_match(watch, hooks);
}

void watch_loading(
    EventWatch& watch,
    const oa::World* match,
    std::span<const uint8_t> rows,
    const EventHooks& hooks
) {
    if (rows.empty())
        return;
    uint32_t sum = 0;
    for (const uint8_t row : rows)
        sum += std::min<uint32_t>(row, kFullProgress);
    const auto progress = static_cast<int32_t>(sum / rows.size());
    if (progress == watch.local_load)
        return;
    watch.local_load = progress;
    auto event = begin_event(hooks, event_kind::loaded);
    if (!event)
        return;
    const oa::Player* local = nullptr;
    if (match != nullptr && match->game.local_player_index < OA_PLAYER_COUNT &&
        player_seated(match->game.players[match->game.local_player_index]))
        local = &match->game.players[match->game.local_player_index];
    event->key("player");
    if (local != nullptr)
        event->string(game_text(local->name, sizeof local->name));
    else
        event->null();
    event->key("slot");
    if (local != nullptr)
        event->integer(match->game.local_player_index);
    else
        event->null();
    event->key("progress");
    event->integer(progress);
    event->key("local");
    event->boolean(true);
    send_event(hooks, *event);
}

void announce_screen(EventWatch& watch) noexcept {
    watch.announce_screen = true;
}

bool screen_event_due(const EventWatch& watch, std::string_view screen) noexcept {
    return watch.announce_screen || (watch.seen && watch.screen != screen);
}

} // namespace oa::app::automation
