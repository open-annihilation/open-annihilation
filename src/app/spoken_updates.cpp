// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Spoken updates for one frame (Runtime::speak_updates). Lines from the
// lobby notes, the battle room and the match are queued, collapsed and
// posted back as announce notes. Nothing is recorded unless a screen reader
// is running or the screen-reader preference is on. With that gate closed
// the frame only keeps mute notes.

#include "oa/app/runtime.hpp"

#include "oa/app/spoken_rules.hpp"
#include "reports.hpp"
#include "oa/app/check_host.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/core/world.h"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/screen_reader.hpp"
#include "oa/platform/screen_reader/announce.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/gui_layout.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace bridge = oa::platform::screen_reader;
namespace mp = oa::ui::frontend_multiplayer;
namespace spoken = oa::app::spoken;

constexpr std::size_t kInboxCap = 256;
constexpr std::string_view kNoSaves = "There are no saved games to choose from";

struct InboxNote {
    std::string kind;
    std::string text;
};

std::vector<InboxNote> inbox;

struct SpokenCursor {
    const Runtime* owner{};
    bool live{};
    bridge::LineQueue queue;
    spoken::Memory memory{};
    std::vector<std::string> muted;
    uint16_t room_chat{};
    bool match_open{};
    const void* match_identity{};
    bool match_over{};
    uint16_t match_chat{};
    bool no_saves{};
};

SpokenCursor cursor;

/// Replaces every `{token}` in a pattern.
///
/// @param pattern the looked-up pattern
/// @param token the place name, without braces
/// @param value the text that replaces it
/// @return the filled pattern
std::string fill_one(std::string pattern, std::string_view token, std::string_view value) {
    const std::string place = std::string("{") + std::string(token) + "}";
    std::string::size_type at = 0;
    while ((at = pattern.find(place, at)) != std::string::npos) {
        pattern.replace(at, place.size(), value);
        at += value.size();
    }
    return pattern;
}

/// The interface's words for an English sentence.
///
/// @param english the sentence as the source writes it
/// @return the sentence in the player's language, or the English
std::string say(const char* english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Looks up a pattern and fills its places.
///
/// @param english the pattern
/// @param places each place name and the text that replaces it
/// @return the filled sentence
std::string say_filled(
    const char* english, std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    std::string pattern = say(english);
    for (const auto& place : places)
        pattern = fill_one(std::move(pattern), place.first, place.second);
    return pattern;
}

/// The room word a burst may translate. A map name and a chat room stay as
/// they are. "match" and "battle room" are catalogue words.
///
/// @param room the room the queue named
/// @return the words
std::string room_word(const std::string& room) {
    if (room == "match" || room == "battle room")
        return say(room.c_str());
    return room;
}

/// Remembers or forgets a muted player.
///
/// @param[in,out] names the muted names
/// @param name the player
/// @param mute_on true to drop that player's lines
void remember_mute(std::vector<std::string>& names, std::string_view name, bool mute_on) {
    if (name.empty())
        return;
    for (std::string& kept : names) {
        if (kept == name) {
            if (!mute_on)
                kept.clear();
            return;
        }
    }
    if (mute_on)
        names.emplace_back(name);
}

/// Queues the offers.
///
/// @param[in,out] queue the queue kept across frames
/// @param offers the lines
void push_offers(bridge::LineQueue& queue, std::vector<bridge::SpokenOffer> offers) {
    for (bridge::SpokenOffer& offer : offers)
        queue.push(std::move(offer));
}

/// The cursor for this runtime. A different runtime starts again.
///
/// @param runtime the running app
/// @return the cursor
SpokenCursor& cursor_for(const Runtime& runtime) {
    if (cursor.owner != &runtime) {
        cursor = {};
        cursor.owner = &runtime;
    }
    return cursor;
}

/// Forgets what was seen, and keeps the mute list, so the next open frame
/// is a first sight.
///
/// @param[in,out] state the cursor
void close_cursor(SpokenCursor& state) {
    const Runtime* owner = state.owner;
    std::vector<std::string> muted = std::move(state.muted);
    state = {};
    state.owner = owner;
    state.muted = std::move(muted);
    state.live = false;
}

/// One seat as the room shows it now.
///
/// @param lobby the battle room
/// @param slot the seat
/// @param self_player the local player
/// @return the seat
spoken::SeatNow seat_now(mp::Lobby& lobby, uint8_t slot, const oa::Player& self_player) {
    spoken::SeatNow seat;
    oa::Player& player = mp::slot_player(lobby, slot);
    if (!mp::slot_active(player))
        return seat;
    seat.taken = true;
    seat.player_id = player.player_id;
    seat.name = automation::game_text(player.name, sizeof player.name);
    seat.self = &player == &self_player;
    const mp::PlayerSetupInfo* info = mp::slot_info(lobby, slot);
    seat.has_details = info != nullptr;
    if (info == nullptr)
        return seat;
    seat.ready = (info->options & mp::option::ready) != 0;
    seat.side = info->side;
    seat.color = info->color;
    seat.team = mp::lobby_player_team(player);
    if (seat.side < lobby.side_names.size())
        seat.side_name = lobby.side_names[seat.side];
    return seat;
}

/// The battle room as this frame sees it.
///
/// @param shown the screen id
/// @param creator_left true when the creator's leave closed the room
/// @return the room
spoken::RoomNow room_now(oa::app::ScreenId shown, bool creator_left) {
    spoken::RoomNow room;
    room.creator_left = creator_left;
    room.open = shown == mp::kScreenBattleroom && mp::multiplayer_lobby().game != nullptr;
    if (!room.open)
        return room;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    const oa::Player& self_player = mp::local_player(lobby);
    room.seats.resize(OA_PLAYER_COUNT);
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        room.seats[slot] = seat_now(lobby, slot, self_player);
    const uint8_t host = mp::lobby_host_slot(lobby);
    room.host_present = host < OA_PLAYER_COUNT;
    room.host_slot = host;
    if (room.host_present) {
        if (const mp::PlayerSetupInfo* info = mp::slot_info(lobby, static_cast<int32_t>(host))) {
            room.host_has_details = true;
            room.map = automation::game_text(info->map_name, sizeof info->map_name);
            room.option_bits = static_cast<uint16_t>(info->options & ~mp::option::ready);
            room.metal = info->metal_hundreds;
            room.energy = info->energy_hundreds;
            room.units = info->max_units;
        }
    }
    room.start_frame = lobby.start_frame;
    return room;
}

/// The local player's name in the battle room, or empty outside it.
///
/// @param open true when the room snapshot is open
/// @return the name
std::string battle_self_name(bool open) {
    if (!open || mp::multiplayer_lobby().game == nullptr)
        return {};
    const oa::Player& self_player = mp::local_player(mp::multiplayer_lobby());
    return automation::game_text(self_player.name, sizeof self_player.name);
}

/// New battle-room chat since the cursor, then the cursor moves to the head.
///
/// @param[in,out] state the cursor
/// @param open true when the room is showing
/// @param was_open true when it was already showing
/// @param hear which lines may be spoken
void take_room_chat(SpokenCursor& state, bool open, bool was_open, const spoken::Hear& hear) {
    if (!open || mp::multiplayer_lobby().game == nullptr)
        return;
    oa::Game& game = *mp::multiplayer_lobby().game;
    const auto head = static_cast<uint16_t>(mp::lobby_chat_head(game) % mp::kChatLines);
    if (!was_open) {
        state.room_chat = head;
        return;
    }
    const std::vector<uint16_t> indices =
        spoken::ring_indices(state.room_chat, head, static_cast<uint16_t>(mp::kChatLines));
    std::vector<std::string> lines;
    lines.reserve(indices.size());
    for (const uint16_t index : indices) {
        lines.push_back(
            automation::game_text(mp::lobby_chat_line(game, index), mp::kChatLineBytes)
        );
    }
    state.room_chat = head;
    const std::string room =
        state.memory.map.empty() ? std::string("battle room") : state.memory.map;
    push_offers(state.queue, spoken::take_chat_lines(hear, room, lines, battle_self_name(true)));
}

/// Whether the load-game box is showing its empty sentence.
///
/// @return true when that label is up
bool no_saves_showing() {
    const auto* resources = oa::ui::frontend_dialogs::dialog_resources();
    if (resources == nullptr)
        return false;
    for (const oa::ui::gui_layout::Gadget& gadget : resources->layout.gadgets) {
        const auto* label = std::get_if<oa::ui::gui_layout::LabelFields>(&gadget.fields);
        if (label == nullptr)
            continue;
        if (label->text.find(kNoSaves) != std::string::npos ||
            label->source_text.find(kNoSaves) != std::string::npos)
            return true;
    }
    return false;
}

/// One match message, copied so the packed line can be read.
///
/// @param game the match
/// @param index the ring slot
/// @return the line
oa::sim::messages::MessageLine message_at(const oa::Game& game, uint16_t index) {
    oa::sim::messages::MessageLine line{};
    if (index < OA_CHAT_LINE_COUNT)
        std::memcpy(&line, game.chat_lines[index], sizeof line);
    return line;
}

/// New match chat and unit reports since the cursor.
///
/// The first sight stores the head and does not speak the lines already
/// there. A result that is already decided is not announced.
///
/// @param[in,out] state the cursor
/// @param hear which lines may be spoken
/// @param world the match, or null
/// @param commander_of true when that unit's definition is a commander
/// @param attack_ending the translated ": Under Attack"
template <typename CommanderOf>
void take_match_lines(
    SpokenCursor& state,
    const spoken::Hear& hear,
    const oa::World* world,
    CommanderOf&& commander_of,
    const std::string& attack_ending
) {
    if (world == nullptr) {
        state.match_open = false;
        state.match_identity = nullptr;
        state.match_over = false;
        return;
    }
    const oa::Game& game = world->game;
    const auto head = static_cast<uint16_t>(game.chat_head % OA_CHAT_LINE_COUNT);
    if (!state.match_open || state.match_identity != world) {
        state.match_open = true;
        state.match_identity = world;
        state.match_over = automation::match_decided(game);
        state.match_chat = head;
        return;
    }
    if (!state.match_over && automation::match_decided(game)) {
        state.match_over = true;
        if (hear.speaking) {
            const auto outcome = automation::match_outcome(game);
            const char* word = outcome && *outcome == "victory" ? "Victory" : "Defeat";
            bridge::SpokenOffer offer;
            offer.text = say(word);
            offer.politeness = bridge::Politeness::polite;
            state.queue.push(std::move(offer));
        }
    }
    const std::vector<uint16_t> indices =
        spoken::ring_indices(state.match_chat, head, OA_CHAT_LINE_COUNT);
    std::vector<std::string> chat;
    std::vector<spoken::ReportRow> reports;
    std::string self_name;
    if (game.local_player_index < OA_PLAYER_COUNT) {
        self_name = automation::game_text(
            game.players[game.local_player_index].name,
            sizeof game.players[game.local_player_index].name
        );
    }
    for (const uint16_t index : indices) {
        const oa::sim::messages::MessageLine line = message_at(game, index);
        chat.emplace_back(line.text, strnlen(line.text, sizeof line.text));
        spoken::ReportRow row;
        row.text.assign(line.text, strnlen(line.text, sizeof line.text));
        row.kind = static_cast<uint8_t>(line.kind & spoken::kKindMask);
        row.commander = commander_of(line.value);
        if (world->units != nullptr && line.value != 0 && line.value < world->unit_slot_count) {
            row.local_unit = world->units[line.value].owner_index == game.local_player_index;
        }
        reports.push_back(std::move(row));
    }
    state.match_chat = head;
    push_offers(state.queue, spoken::take_chat_lines(hear, "match", chat, self_name));
    push_offers(state.queue, spoken::take_reports(hear, reports, attack_ending));
}

} // namespace

void remember_spoken_note(const char* kind, const char* text) {
    if (kind == nullptr || kind[0] == '\0' || std::strcmp(kind, "announce") == 0)
        return;
    // With no one listening, speak_updates keeps only the mutes. The rest is
    // not copied at all.
    if (!bridge::announcements_open() && std::strcmp(kind, "muzzle") != 0 &&
        std::strcmp(kind, "hear") != 0)
        return;
    if (inbox.size() >= kInboxCap)
        inbox.erase(inbox.begin());
    InboxNote note;
    note.kind = kind;
    note.text = text != nullptr ? text : "";
    inbox.push_back(std::move(note));
}

bool spoken_updates_open() {
    return bridge::announcements_open();
}

void Runtime::speak_updates() {
    SpokenCursor& state = cursor_for(*this);
    const std::vector<InboxNote> notes = std::move(inbox);
    inbox.clear();
    const bool speaking = bridge::announcements_open();
    if (!speaking) {
        for (const InboxNote& note : notes) {
            if (note.kind != "muzzle" && note.kind != "hear")
                continue;
            const bool mute_on = note.kind == "muzzle";
            remember_mute(state.muted, note.text, mute_on);
        }
        (void)mp::lobby_take_creator_left();
        close_cursor(state);
        return;
    }
    const bool silent = !state.live;
    state.live = true;
    const uint64_t now_ms = SDL_GetTicks();
    state.queue.set_now(now_ms);
    for (const std::string& name : state.muted) {
        if (!name.empty())
            state.queue.set_muted(name, true);
    }
    const auto stored_preference = [this](const char* key) -> std::string {
        const auto found = preference_values_.find(std::string(key));
        if (found == preference_values_.end())
            return {};
        return found->second;
    };
    spoken::Hear hear;
    hear.speaking = true;
    hear.chat = bridge::speak_chat_of(stored_preference(bridge::speak_chat_key));
    hear.lobby = bridge::speak_lobby_of(stored_preference(bridge::speak_lobby_key));
    hear.ticker = bridge::speak_ticker_of(stored_preference(bridge::speak_ticker_key));
    std::vector<spoken::Note> batch;
    batch.reserve(notes.size());
    for (const InboxNote& note : notes) {
        if (note.kind == "muzzle" || note.kind == "hear") {
            const bool mute_on = note.kind == "muzzle";
            remember_mute(state.muted, note.text, mute_on);
            state.queue.set_muted(note.text, mute_on);
            continue;
        }
        spoken::Note item;
        item.kind = note.kind;
        item.text = note.text;
        batch.push_back(item);
    }
    push_offers(state.queue, spoken::take_notes(hear, state.memory, batch, now_ms));

    oa::app::ScreenId shown = 0;
    const CheckHost check = check_host(*this);
    if (check.screen != nullptr)
        shown = check.screen(check.context);
    const bool creator_left = mp::lobby_take_creator_left();
    const spoken::RoomNow room = room_now(shown, creator_left);
    const bool was_open = state.memory.room_open;
    push_offers(state.queue, spoken::take_room(hear, state.memory, room));
    take_room_chat(state, room.open, was_open, hear);

    const AutomationHost host = automation_host(*this);
    const oa::World* world = nullptr;
    if (host.match_world != nullptr)
        world = host.match_world(host.context);
    const std::string attack_ending = std::string(": ") + translate_ui("Under Attack");
    take_match_lines(
        state,
        hear,
        world,
        [this](uint16_t unit) {
            const auto* definition = definition_for(unit);
            return definition != nullptr && definition->commander;
        },
        attack_ending
    );

    const bool found = no_saves_showing();
    if (silent) {
        state.no_saves = found;
    } else if (found && !state.no_saves) {
        state.no_saves = true;
        bridge::SpokenOffer offer;
        offer.text = say("There are no saved games to choose from");
        offer.politeness = bridge::Politeness::assertive;
        state.queue.push(std::move(offer));
    } else if (!found) {
        state.no_saves = false;
    }

    const std::vector<bridge::Announcement> flushed = state.queue.flush();
    for (const bridge::Announcement& item : flushed) {
        if (item.burst) {
            const std::string count = std::to_string(item.count);
            const std::string room_name = room_word(item.room);
            bridge::announce(
                say_filled("{count} new lines in {room}", {{"count", count}, {"room", room_name}}),
                item.politeness
            );
        } else {
            bridge::announce(item.text, item.politeness);
        }
    }
    const std::vector<bridge::Announcement> spoken_lines = bridge::take_announcements();
    for (const bridge::Announcement& item : spoken_lines)
        post_extension_note(*this, "announce", item.text.c_str());
}

} // namespace oa::app
