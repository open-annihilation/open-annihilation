// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the automation endpoint reports of a match and of the battle room,
// and the events it learns, over a match world and a lobby the check builds:
// the members of the match and room answers, with and without a match or a
// room; the outcome and winner; the game's text, written as it is when it is
// UTF-8 and as the marker and its base64 when it is not; each kind of event
// as what it follows changes, only the kinds wanted, numbered by the
// endpoint; and that none of it changes the game.
#include "events.hpp"
#include "reports.hpp"

#include "oa/formats/json.hpp"
#include "oa/core/player_setup.h"
#include "oa/core/world.h"
#include "oa/sim/scenario/outcome.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace automation = oa::app::automation;
namespace event_kind = automation::event_kind;
namespace json = oa::formats::json;
namespace mp = oa::ui::frontend_multiplayer;
namespace outcome_flag = oa::sim::scenario::outcome_flag;

using json::Json;
using json::JsonWriter;

/// Returns the object a writer's members make.
///
/// @param members writes the members into the open object
/// @return the object as the JSON reader reads it
Json object_of(const std::function<void(JsonWriter&)>& members) {
    JsonWriter writer;
    writer.begin_object();
    members(writer);
    writer.end_object();
    json::JsonError error;
    const auto parsed = json::parse_json(writer.text(), error);
    OA_CHECK(parsed && parsed->type() == json::JsonType::object);
    return parsed ? *parsed : Json{};
}

/// Returns a member's text.
///
/// @param object the object
/// @param name the member
/// @return its text; "<none>" when it is missing or not a string
std::string text(const Json& object, std::string_view name) {
    const Json* member = object.find(name);
    return member != nullptr && member->string() != nullptr ? *member->string() : "<none>";
}

/// Returns a member's whole number.
///
/// @param object the object
/// @param name the member
/// @return the number; nothing when it is missing or not one
std::optional<int64_t> number(const Json& object, std::string_view name) {
    const Json* member = object.find(name);
    return member != nullptr ? member->integer() : std::nullopt;
}

/// Tells whether a member is the boolean given.
///
/// @param object the object
/// @param name the member
/// @param value the boolean
/// @return true when it is
bool is(const Json& object, std::string_view name, bool value) {
    const Json* member = object.find(name);
    return member != nullptr && member->boolean() == value;
}

/// Tells whether a member is null.
///
/// @param object the object
/// @param name the member
/// @return true when it is
bool is_null(const Json& object, std::string_view name) {
    const Json* member = object.find(name);
    return member != nullptr && member->type() == json::JsonType::null;
}

/// Copies text into a fixed field, zero-filled.
///
/// @param[out] field the field
/// @param size its size in bytes
/// @param value the text
void set_text(char* field, size_t size, std::string_view value) {
    std::memset(field, 0, size);
    std::memcpy(field, value.data(), value.size() < size ? value.size() : size - 1);
}

/// Seats a player in a match world.
///
/// @param[in,out] world the world
/// @param slot the player's slot
/// @param name the player's name
/// @param status its Player.status
/// @param side its side
/// @param colour its logo colour
void seat(
    oa::World& world,
    uint8_t slot,
    std::string_view name,
    uint8_t status,
    uint8_t side,
    uint8_t colour
) {
    oa::Player& player = world.game.players[slot];
    player.in_use = 1;
    player.index = slot;
    player.status = status;
    player.team = OA_PLAYER_NO_TEAM;
    player.info = slot + 1u;
    set_text(player.name, sizeof player.name, name);
    world.player_info[slot].side = side;
    world.player_info[slot].color = colour;
}

/// Makes a match world of two players: Pilot, this machine's, on side 0,
/// allied with Brute, a computer player on side 1 and team 1.
///
/// @return the world
std::unique_ptr<oa::World> two_player_world() {
    auto world = std::make_unique<oa::World>();
    for (auto& player : world->game.players)
        player.index = OA_PLAYER_COUNT;
    world->game.side_count = 2;
    set_text(world->game.sides[0].name, sizeof world->game.sides[0].name, "ARM");
    set_text(world->game.sides[1].name, sizeof world->game.sides[1].name, "CORE");
    seat(*world, 0, "Pilot", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*world, 1, "Brute", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    world->game.players[1].team = 1;
    world->game.players[0].alliance[1] = 1;
    world->game.players[0].unit_count = 3;
    world->game.players[1].unit_count = 4;
    world->game.players[0].kills = 2;
    world->game.players[1].losses = 2;
    world->game.local_player_index = 0;
    world->game.requested_speed = 10;
    world->game.tick = 300;
    return world;
}

/// Returns a copy of an object's bytes, to compare with after it was read.
///
/// @param object the object
/// @return its bytes
template <class T>
std::vector<uint8_t> bytes_of(const T& object) {
    const auto* first = reinterpret_cast<const uint8_t*>(&object);
    return {first, first + sizeof object};
}

/// The events a watch sent, and the kinds it is to send.
struct Sent {
    uint32_t wanted{event_kind::all};
    std::vector<Json> events;
    int64_t seq{};
};

/// Returns hooks that keep the events a watch sends.
///
/// @param[in,out] sent where they are kept
/// @return the hooks
automation::EventHooks hooks_into(Sent& sent) {
    automation::EventHooks hooks;
    hooks.context = &sent;
    hooks.wanted = [](void* context, uint32_t kind) {
        return (static_cast<Sent*>(context)->wanted & kind) != 0;
    };
    hooks.begin = [](void* context, std::string_view kind) {
        auto& kept = *static_cast<Sent*>(context);
        JsonWriter event;
        event.begin_object();
        event.key("event");
        event.string(kind);
        event.key("seq");
        event.integer(++kept.seq);
        return event;
    };
    hooks.send = [](void* context, JsonWriter& event) {
        event.end_object();
        json::JsonError error;
        if (auto parsed = json::parse_json(event.text(), error))
            static_cast<Sent*>(context)->events.push_back(std::move(*parsed));
    };
    return hooks;
}

/// Takes the events sent since the last call.
///
/// @param[in,out] sent the events
/// @return them, in order
std::vector<Json> take(Sent& sent) {
    std::vector<Json> taken = std::move(sent.events);
    sent.events.clear();
    return taken;
}

void check_text() {
    const char utf8[8] = "caf\xc3\xa9";
    OA_CHECK(automation::game_text(utf8, sizeof utf8) == "caf\xc3\xa9");
    const char code_page[8] = "caf\xe9";
    OA_CHECK(automation::game_text(code_page, sizeof code_page) == "caf\xe9");
    const char full[4] = {'a', 'b', 'c', 'd'};
    OA_CHECK(automation::game_text(full, sizeof full) == "abcd");
    // A player's name in UTF-8 is written as it is; one in the game's code
    // page, as another machine's game sends it, as the marker and the
    // base64 of its bytes, from which a client recovers them.
    auto world = two_player_world();
    set_text(world->game.players[0].name, sizeof world->game.players[0].name, "Andr\xc3\xa9");
    set_text(world->game.players[1].name, sizeof world->game.players[1].name, "J\xf6rg");
    const Json match = object_of([&](JsonWriter& json) {
        automation::write_match(json, world.get(), std::nullopt);
    });
    const auto players = match.find("players")->elements();
    OA_CHECK(players.size() == 2);
    if (players.size() == 2) {
        OA_CHECK(text(players[0], "name") == "Andr\xc3\xa9");
        OA_CHECK(text(players[1], "name") == "Non-Unicode Text Error::SvZyZw==");
    }
}

void check_match() {
    const Json none =
        object_of([](JsonWriter& json) { automation::write_match(json, nullptr, std::nullopt); });
    OA_CHECK(
        is(none, "in_match", false) && is_null(none, "speed") &&
        none.find("players")->elements().empty() && is(none, "game_over", false) &&
        is_null(none, "winner") && none.find("state_hash") == nullptr
    );

    auto world = two_player_world();
    world->game.sim_run_flags = 1;
    const auto before = bytes_of(*world);
    const Json match = object_of([&](JsonWriter& json) {
        automation::write_match(json, world.get(), 0x0123456789abcdefull);
    });
    OA_CHECK(bytes_of(*world) == before);
    OA_CHECK(is(match, "in_match", true) && is(match, "paused", true));
    OA_CHECK(number(match, "speed") == 10 && number(match, "local_player") == 0);
    OA_CHECK(text(match, "state_hash") == "0123456789abcdef");
    OA_CHECK(is(match, "game_over", false) && is_null(match, "outcome"));
    const auto players = match.find("players")->elements();
    OA_CHECK(players.size() == 2);
    if (players.size() == 2) {
        const Json& pilot = players[0];
        const Json& brute = players[1];
        OA_CHECK(text(pilot, "name") == "Pilot" && text(pilot, "status") == "local");
        OA_CHECK(text(pilot, "side_name") == "ARM" && number(pilot, "colour") == 2);
        OA_CHECK(is_null(pilot, "team") && is(pilot, "local", true) && is(pilot, "alive", true));
        OA_CHECK(
            pilot.find("allies")->elements().size() == 1 &&
            pilot.find("allies")->elements()[0].integer() == 1
        );
        OA_CHECK(number(pilot, "units") == 3 && number(pilot, "kills") == 2);
        OA_CHECK(text(brute, "side_name") == "CORE" && number(brute, "team") == 1);
        OA_CHECK(is(brute, "computer", true) && brute.find("allies")->elements().empty());
        OA_CHECK(number(brute, "losses") == 2);
    }

    // Won: the local player wins.
    world->game.outcome_flags =
        outcome_flag::finished | outcome_flag::won | outcome_flag::victory_transition;
    const Json won = object_of([&](JsonWriter& json) {
        automation::write_match(json, world.get(), std::nullopt);
    });
    OA_CHECK(is(won, "game_over", true) && text(won, "outcome") == "victory");
    OA_CHECK(text(won, "winner") == "Pilot");
    // Lost with no units left: the one player with units wins.
    world->game.outcome_flags = outcome_flag::finished | outcome_flag::defeat_transition;
    world->game.players[0].unit_count = 0;
    const Json lost = object_of([&](JsonWriter& json) {
        automation::write_match(json, world.get(), std::nullopt);
    });
    OA_CHECK(text(lost, "outcome") == "defeat" && text(lost, "winner") == "Brute");
    // Lost with two players standing: no one winner.
    world->game.players[0].unit_count = 1;
    const Json open = object_of([&](JsonWriter& json) {
        automation::write_match(json, world.get(), std::nullopt);
    });
    OA_CHECK(is_null(open, "winner"));
}

/// A battle room: Host hosts it in slot 0 on Coast to Coast, ready, with a
/// computer player in slot 3, and has said hello.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};

    Room() {
        mp::lobby_reset(lobby, *game);
        set_text(mp::lobby_game_name(*game), sizeof game->game_name, "Skirmish Night");
        mp::lobby_seat_local(lobby, 0, true, "Host");
        mp::PlayerSetupInfo& host = *mp::slot_info(lobby, 0);
        set_text(host.map_name, sizeof host.map_name, "Coast to Coast");
        host.options = mp::option::ready | mp::option::commander_step | mp::option::los_limited;
        host.energy_hundreds = 10;
        host.metal_hundreds = 5;
        host.max_units = 250;
        oa::Player& computer = mp::slot_player(lobby, 3);
        computer.in_use = 1;
        computer.index = 3;
        computer.player_id = 7;
        computer.status = mp::kSlotComputer;
        set_text(computer.name, sizeof computer.name, "Ai");
        mp::lobby_post_chat(lobby, "<Host> hello");
    }
};

void check_room() {
    const Json none =
        object_of([](JsonWriter& json) { automation::write_room(json, nullptr, true); });
    OA_CHECK(
        is(none, "in_room", false) && is_null(none, "session") &&
        none.find("players")->elements().empty() && is_null(none, "options")
    );

    Room room;
    const Json hidden =
        object_of([&](JsonWriter& json) { automation::write_room(json, &room.lobby, false); });
    OA_CHECK(is(hidden, "in_room", false) && hidden.find("chat")->elements().empty());

    const auto before = bytes_of(*room.game);
    const Json shown =
        object_of([&](JsonWriter& json) { automation::write_room(json, &room.lobby, true); });
    OA_CHECK(bytes_of(*room.game) == before);
    OA_CHECK(is(shown, "in_room", true) && text(shown, "session") == "Skirmish Night");
    OA_CHECK(text(shown, "host") == "Host" && number(shown, "host_slot") == 0);
    OA_CHECK(number(shown, "local_slot") == 0 && is(shown, "hosting", true));
    OA_CHECK(text(shown, "map") == "Coast to Coast");
    const auto players = shown.find("players")->elements();
    OA_CHECK(players.size() == 2);
    if (players.size() == 2) {
        OA_CHECK(text(players[0], "name") == "Host" && is(players[0], "ready", true));
        OA_CHECK(text(players[0], "status") == "local" && is_null(players[0], "team"));
        OA_CHECK(number(players[1], "slot") == 3 && text(players[1], "status") == "computer");
        OA_CHECK(is(players[1], "ready", false));
    }
    const Json* options = shown.find("options");
    OA_CHECK(options != nullptr && text(*options, "commander") == "ends");
    if (options != nullptr) {
        OA_CHECK(text(*options, "line_of_sight") == "circular" && is(*options, "mapped", true));
        OA_CHECK(number(*options, "energy") == 1000 && number(*options, "metal") == 500);
        OA_CHECK(number(*options, "max_units") == 250 && is(*options, "cheats", false));
    }
    const auto chat = shown.find("chat")->elements();
    OA_CHECK(
        chat.size() == 1 && chat[0].string() != nullptr && *chat[0].string() == "<Host> hello"
    );
}

void check_events() {
    Sent sent;
    const auto hooks = hooks_into(sent);
    automation::EventWatch watch;
    automation::Observed now;
    now.screen = "main_menu";

    // The first sight keeps what it sees; a subscription to screen is told it.
    automation::watch_events(watch, now, hooks);
    OA_CHECK(take(sent).empty());
    automation::announce_screen(watch);
    automation::watch_events(watch, now, hooks);
    auto events = take(sent);
    OA_CHECK(
        events.size() == 1 && text(events[0], "event") == "screen" &&
        text(events[0], "name") == "main_menu" && is_null(events[0], "previous")
    );

    // The battle room: its seats and their ready flags, and its chat.
    Room room;
    now.screen = "mp_battleroom";
    now.room = &room.lobby;
    const auto room_before = bytes_of(*room.game);
    automation::watch_events(watch, now, hooks);
    OA_CHECK(bytes_of(*room.game) == room_before);
    events = take(sent);
    OA_CHECK(events.size() == 5);
    if (events.size() == 5) {
        OA_CHECK(text(events[0], "previous") == "main_menu");
        OA_CHECK(
            text(events[1], "event") == "player" && text(events[1], "change") == "joined" &&
            text(events[1], "name") == "Host"
        );
        OA_CHECK(text(events[2], "event") == "ready" && is(events[2], "ready", true));
        OA_CHECK(text(events[3], "name") == "Ai" && number(events[3], "slot") == 3);
        OA_CHECK(
            text(events[4], "event") == "chat" && text(events[4], "where") == "room" &&
            text(events[4], "from") == "Host" && text(events[4], "text") == "hello"
        );
    }
    automation::watch_events(watch, now, hooks);
    OA_CHECK(take(sent).empty());
    mp::slot_player(room.lobby, 3).status = mp::kSlotOpen;
    mp::slot_info(room.lobby, 0)->options = 0;
    mp::lobby_post_chat(room.lobby, "<Host->Ai> psst");
    mp::lobby_post_chat(room.lobby, "Ai left the game");
    automation::watch_events(watch, now, hooks);
    events = take(sent);
    OA_CHECK(events.size() == 3);
    if (events.size() == 3) {
        OA_CHECK(text(events[0], "event") == "ready" && is(events[0], "ready", false));
        OA_CHECK(text(events[1], "change") == "left" && text(events[1], "name") == "Ai");
        OA_CHECK(
            text(events[2], "from") == "Host" && text(events[2], "to") == "Ai" &&
            text(events[2], "text") == "psst"
        );
    }

    // A match: started once its screen shows, then each change of its pause,
    // speed, alliances, load progress, outcome and message log.
    auto world = two_player_world();
    now.screen = "loading";
    now.room = nullptr;
    now.match = world.get();
    automation::watch_events(watch, now, hooks);
    events = take(sent);
    OA_CHECK(events.size() == 1 && text(events[0], "name") == "loading");
    now.screen = "match";
    world->game.sim_run_flags = 1;
    world->game.requested_speed = 15;
    world->game.players[1].alliance[0] = 1;
    world->game.players[0].load_progress = 100;
    std::memcpy(world->game.chat_lines[0], "<Brute> attack", sizeof "<Brute> attack");
    std::memcpy(world->game.chat_lines[1], "Game speed 15", sizeof "Game speed 15");
    world->game.chat_head = 2;
    const auto match_before = bytes_of(*world);
    automation::watch_events(watch, now, hooks);
    OA_CHECK(bytes_of(*world) == match_before);
    events = take(sent);
    std::vector<std::string> kinds;
    for (const Json& event : events)
        kinds.push_back(text(event, "event"));
    OA_CHECK((
        kinds ==
        std::vector<std::string>{"screen", "match", "loaded", "pause", "speed", "alliance", "chat"}
    ));
    if (events.size() == 7) {
        OA_CHECK(text(events[1], "change") == "started");
        OA_CHECK(
            text(events[2], "player") == "Pilot" && number(events[2], "progress") == 100 &&
            is(events[2], "local", true)
        );
        OA_CHECK(is(events[3], "paused", true) && number(events[4], "speed") == 15);
        OA_CHECK(
            text(events[5], "from") == "Brute" && text(events[5], "to") == "Pilot" &&
            is(events[5], "allied", true)
        );
        OA_CHECK(text(events[6], "where") == "match" && text(events[6], "text") == "attack");
    }
    world->game.outcome_flags = outcome_flag::finished | outcome_flag::defeat_transition;
    world->game.players[0].unit_count = 0;
    automation::watch_events(watch, now, hooks);
    events = take(sent);
    OA_CHECK(
        events.size() == 1 && text(events[0], "event") == "game_over" &&
        text(events[0], "outcome") == "defeat" && text(events[0], "winner") == "Brute"
    );
    if (events.size() == 1) {
        // The two players are mutual allies, so both won, and the shared
        // results name no second winner. This machine's own winner stays Brute.
        OA_CHECK(events[0].find("state_hash") == nullptr);
        OA_CHECK(number(events[0], "tick") == 300);
        const Json* results = events[0].find("results");
        OA_CHECK(results != nullptr && results->elements().size() == 2);
        if (results != nullptr && results->elements().size() == 2) {
            OA_CHECK(text(results->elements()[0], "name") == "Pilot");
            OA_CHECK(text(results->elements()[0], "outcome") == "won");
            OA_CHECK(text(results->elements()[1], "name") == "Brute");
            OA_CHECK(text(results->elements()[1], "outcome") == "won");
        }
    }

    // Only the kinds wanted are sent, and the watch goes on following the rest.
    sent.wanted = event_kind::match;
    world->game.sim_run_flags = 0;
    automation::watch_events(watch, now, hooks);
    OA_CHECK(take(sent).empty());
    automation::watch_match_gone(watch, hooks);
    events = take(sent);
    OA_CHECK(events.size() == 1 && text(events[0], "change") == "ended");
    if (events.size() == 1) {
        OA_CHECK(text(events[0], "winner") == "Brute");
        OA_CHECK(number(events[0], "tick") == 300);
        OA_CHECK(events[0].find("results") != nullptr);
        OA_CHECK(events[0].find("state_hash") == nullptr);
    }
    OA_CHECK(watch.match == nullptr);

    // A new match's loading, before its frames: this machine's progress.
    sent.wanted = event_kind::all;
    const uint8_t rows[] = {100, 50, 0, 0, 0, 0};
    automation::watch_loading(watch, nullptr, rows, hooks);
    automation::watch_loading(watch, nullptr, rows, hooks);
    events = take(sent);
    OA_CHECK(
        events.size() == 1 && is_null(events[0], "player") && number(events[0], "progress") == 25 &&
        is(events[0], "local", true)
    );
    for (const Json& event : events)
        OA_CHECK(number(event, "seq").has_value());
}

/// A world with no one seated. Seats set their own index.
///
/// @return the world
std::unique_ptr<oa::World> fresh_world() {
    auto world = std::make_unique<oa::World>();
    for (auto& player : world->game.players)
        player.index = OA_PLAYER_COUNT;
    world->game.tick = 300;
    return world;
}

/// Finds a seat of a results array by name.
///
/// @param results the results array
/// @param name the player's name
/// @return the seat; null when it is not there
const Json* seat_named(const Json& results, std::string_view name) {
    for (const Json& seat_row : results.elements())
        if (text(seat_row, "name") == name)
            return &seat_row;
    return nullptr;
}

/// How a match ended for each seat: one player against another, a team, a
/// player who left, and a draw. A watcher changes none of that.
void check_results() {
    auto free_for_all = fresh_world();
    seat(*free_for_all, 0, "Pilot", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*free_for_all, 1, "Brute", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    seat(*free_for_all, 2, "Eye", OA_PLAYER_STATUS_COMPUTER, 0, 1);
    free_for_all->game.players[0].unit_count = 0;
    free_for_all->game.players[0].units_created = 4;
    free_for_all->game.players[0].losses = 4;
    free_for_all->game.players[1].unit_count = 3;
    free_for_all->game.players[1].units_created = 3;
    free_for_all->game.players[1].kills = 1;
    free_for_all->game.players[1].losses = 2;
    free_for_all->player_info[2].options = OA_SETUP_OPTION_WATCHER;
    const automation::MatchResults free = automation::match_results(*free_for_all);
    const Json free_json =
        object_of([&](JsonWriter& json) { automation::write_result_fields(json, free, true); });
    OA_CHECK(text(free_json, "winner") == "Brute");
    OA_CHECK(number(free_json, "tick") == 300);
    OA_CHECK(free_json.find("state_hash") == nullptr);
    const Json* free_results = free_json.find("results");
    OA_CHECK(free_results != nullptr && free_results->elements().size() == 3);
    if (free_results != nullptr) {
        const Json* pilot = seat_named(*free_results, "Pilot");
        const Json* brute = seat_named(*free_results, "Brute");
        const Json* eye = seat_named(*free_results, "Eye");
        OA_CHECK(pilot != nullptr && text(*pilot, "outcome") == "lost");
        OA_CHECK(brute != nullptr && text(*brute, "outcome") == "won");
        OA_CHECK(eye != nullptr && text(*eye, "outcome") == "watcher");
        if (pilot != nullptr && brute != nullptr && eye != nullptr) {
            OA_CHECK(is_null(*pilot, "team") && number(*pilot, "side") == 0);
            OA_CHECK(number(*pilot, "units") == 0 && number(*pilot, "losses") == 4);
            OA_CHECK(number(*brute, "units") == 3 && number(*brute, "kills") == 1);
            OA_CHECK(number(*brute, "losses") == 2 && number(*brute, "side") == 1);
            OA_CHECK(number(*eye, "slot") == 2);
        }
    }

    auto team = fresh_world();
    seat(*team, 0, "Alpha", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*team, 1, "Beta", OA_PLAYER_STATUS_COMPUTER, 0, 3);
    seat(*team, 2, "Gone", OA_PLAYER_STATUS_COMPUTER, 0, 4);
    seat(*team, 3, "Foe", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    team->game.players[0].team = 1;
    team->game.players[1].team = 1;
    team->game.players[2].team = 1;
    team->game.players[3].team = 2;
    team->game.players[0].unit_count = 2;
    team->game.players[1].unit_count = 1;
    team->game.players[2].unit_count = 0;
    team->game.players[2].units_created = 3;
    team->game.players[3].unit_count = 0;
    team->game.players[3].units_created = 4;
    const automation::MatchResults allied = automation::match_results(*team);
    OA_CHECK(!allied.winner);
    OA_CHECK(allied.seats.size() == 4);
    if (allied.seats.size() == 4) {
        OA_CHECK(allied.seats[0].outcome == "won" && allied.seats[0].team == 1);
        OA_CHECK(allied.seats[1].outcome == "won");
        OA_CHECK(allied.seats[2].name == "Gone" && allied.seats[2].outcome == "won");
        OA_CHECK(allied.seats[3].name == "Foe" && allied.seats[3].outcome == "lost");
    }

    auto departed = fresh_world();
    seat(*departed, 0, "Pilot", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*departed, 1, "Brute", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    departed->game.players[0].in_use = 0;
    departed->game.players[0].reject_reason = 1;
    departed->game.players[0].units_created = 5;
    departed->game.players[0].unit_count = 2;
    departed->game.players[1].unit_count = 3;
    const automation::MatchResults left = automation::match_results(*departed);
    OA_CHECK(left.winner && *left.winner == "Brute");
    OA_CHECK(left.seats.size() == 2);
    if (left.seats.size() == 2) {
        OA_CHECK(left.seats[0].outcome == "left" && left.seats[0].units == 2);
        OA_CHECK(left.seats[1].outcome == "won");
    }

    auto drawn = fresh_world();
    seat(*drawn, 0, "Pilot", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*drawn, 1, "Brute", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    drawn->game.players[0].unit_count = 0;
    drawn->game.players[0].units_created = 2;
    drawn->game.players[1].unit_count = 0;
    drawn->game.players[1].units_created = 2;
    const automation::MatchResults draw = automation::match_results(*drawn);
    OA_CHECK(!draw.winner);
    OA_CHECK(draw.seats.size() == 2);
    if (draw.seats.size() == 2) {
        OA_CHECK(draw.seats[0].outcome == "drawn" && draw.seats[1].outcome == "drawn");
        OA_CHECK(!draw.seats[0].team && !draw.seats[1].team);
    }
}

void check_kinds() {
    OA_CHECK(automation::event_kind_named("game_over") == event_kind::game_over);
    OA_CHECK(automation::event_kind_named("everything") == 0);
    uint32_t every = 0;
    for (const auto& kind : automation::event_kind_names())
        every |= kind.kind;
    OA_CHECK(every == event_kind::all && automation::event_kind_names().size() == 12);
}

} // namespace

int main() {
    check_text();
    check_match();
    check_room();
    check_events();
    check_results();
    check_kinds();
    return oa::test::check_exit_status();
}
