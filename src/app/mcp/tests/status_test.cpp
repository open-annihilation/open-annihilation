// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// game.info and game.status against a stand-in match. The economy is the
// local player's and a player allied with it. Full awareness, refused in a
// shared match, reads an enemy's too. The clock's display is checked at
// 0, 59 s and 1 h.
#include "tools.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/play_host.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/core/world.h"
#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

AutomationHost automation_host(Runtime&) {
    return {};
}

PlayHost play_host(Runtime&) {
    return {};
}

} // namespace oa::app

namespace oa::app::mcp {

// Serve holds a UiState by value, and the tools construct one when no game is
// attached. The real state lives in ui.cpp with the screen tools. This test
// never calls those tools, so an empty state lets Serve be built without
// linking them or the game.
struct UiState::State {};

UiState::UiState() : state_(std::make_unique<State>()) {
}

UiState::~UiState() = default;

UiState::UiState(UiState&&) noexcept = default;

UiState& UiState::operator=(UiState&&) noexcept = default;

} // namespace oa::app::mcp

namespace {

namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;

/// Parses one JSON text. A failure is a failed check and an empty result.
///
/// @param text the text
/// @return the value
std::optional<json::Json> parsed(std::string_view text) {
    json::JsonError failure;
    std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    return value;
}

/// One request.
///
/// @param id the id, as JSON
/// @param method the method
/// @param params the params object, as JSON; empty omits params
/// @return the request
std::string request(std::string_view id, std::string_view method, std::string_view params = {}) {
    std::string text = "{\"jsonrpc\":\"2.0\",\"id\":";
    text.append(id);
    text.append(",\"method\":\"");
    text.append(method);
    text.push_back('"');
    if (!params.empty()) {
        text.append(",\"params\":");
        text.append(params);
    }
    text.push_back('}');
    return text;
}

/// A session that has finished initialize, with the game tools installed.
///
/// @param[in,out] serve the game the tools read
/// @param[in,out] bridge the play queue; it outlives the session
/// @param observe list only tools that read
/// @return the session
mcp::Session open_session(mcp::Serve& serve, mcp::PlayBridge& bridge, bool observe) {
    mcp::SessionConfig config;
    config.observe = observe;
    mcp::Session session(std::move(config));
    mcp::add_game_tools(session, serve, bridge);
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    return session;
}

/// The one answer a message produced, parsed.
///
/// @param[in,out] session the session
/// @param message the message
/// @return the answer, or nothing when it was not one JSON object
std::optional<json::Json> only(mcp::Session& session, std::string_view message) {
    const std::vector<std::string> lines = session.receive(message);
    OA_CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return std::nullopt;
    return parsed(lines[0]);
}

/// The text of a tool result.
///
/// @param answer the tools/call answer
/// @return the text, or nothing
std::optional<std::string> tool_text(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    OA_CHECK(result != nullptr);
    if (result == nullptr)
        return std::nullopt;
    const json::Json* content = result->find("content");
    OA_CHECK(content != nullptr && !content->elements().empty());
    if (content == nullptr || content->elements().empty())
        return std::nullopt;
    const json::Json* text = content->elements().front().find("text");
    OA_CHECK(text != nullptr && text->string() != nullptr);
    if (text == nullptr || text->string() == nullptr)
        return std::nullopt;
    return *text->string();
}

/// The text of a successful tool result, parsed.
///
/// @param answer the tools/call answer
/// @return the text's JSON, or nothing
std::optional<json::Json> tool_body(const json::Json& answer) {
    const std::optional<std::string> text = tool_text(answer);
    if (!text)
        return std::nullopt;
    return parsed(*text);
}

/// Tells whether a tool result is a failure.
///
/// @param answer the tools/call answer
/// @return true when isError is set
bool tool_failed(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    if (result == nullptr)
        return false;
    const json::Json* flag = result->find("isError");
    return flag != nullptr && flag->boolean().value_or(false);
}

/// The text of a resources/read result, parsed.
///
/// @param answer the resources/read answer
/// @return the text's JSON, or nothing
std::optional<json::Json> resource_body(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    OA_CHECK(result != nullptr);
    if (result == nullptr)
        return std::nullopt;
    const json::Json* contents = result->find("contents");
    OA_CHECK(contents != nullptr && !contents->elements().empty());
    if (contents == nullptr || contents->elements().empty())
        return std::nullopt;
    const json::Json* text = contents->elements().front().find("text");
    OA_CHECK(text != nullptr && text->string() != nullptr);
    if (text == nullptr || text->string() == nullptr)
        return std::nullopt;
    return parsed(*text->string());
}

/// Tells whether a tool list names a tool.
///
/// @param tools the tools array
/// @param name the tool
/// @return true when it is listed
bool listed(const json::Json& tools, std::string_view name) {
    for (const json::Json& tool : tools.elements()) {
        const json::Json* found = tool.find("name");
        if (found != nullptr && found->string() != nullptr && *found->string() == name)
            return true;
    }
    return false;
}

/// Copies a short text into a fixed field.
///
/// @param[out] field the field
/// @param size the field's size
/// @param value the text
void set_text(char* field, size_t size, std::string_view value) {
    std::memset(field, 0, size);
    const size_t count = value.size() < size ? value.size() : size - 1;
    if (count > 0)
        std::memcpy(field, value.data(), count);
}

/// Seats a player.
///
/// @param[in,out] world the match
/// @param slot the seat
/// @param name the name
/// @param status Player.status
/// @param side the side
/// @param colour the logo colour
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

/// A match of three seats: Pilot locally, Brute a computer enemy, Scout a
/// computer allied with Pilot through allied_by.
///
/// Pilot's alliance list names Brute and does not decide the economy.
///
/// @return the match
std::unique_ptr<oa::World> stand_in() {
    auto world = std::make_unique<oa::World>();
    for (oa::Player& player : world->game.players)
        player.index = OA_PLAYER_COUNT;
    world->game.side_count = 2;
    set_text(world->game.sides[0].name, sizeof world->game.sides[0].name, "ARM");
    set_text(world->game.sides[1].name, sizeof world->game.sides[1].name, "CORE");
    seat(*world, 0, "Pilot", OA_PLAYER_STATUS_LOCAL, 0, 2);
    seat(*world, 1, "Brute", OA_PLAYER_STATUS_COMPUTER, 1, 5);
    seat(*world, 2, "Scout", OA_PLAYER_STATUS_COMPUTER, 0, 3);
    world->game.players[1].team = 1;
    world->game.players[2].team = 2;
    world->game.players[0].alliance[1] = 1;
    world->game.players[0].allied_by[2] = 1;
    world->game.local_player_index = 0;
    world->game.requested_speed = 10;
    world->game.max_units_setting = 400;
    world->game.map_width = 128;
    world->game.map_height = 64;
    world->game.session_rules = 1;
    world->game.visibility_flags =
        static_cast<uint8_t>(OA_VISIBILITY_MAPPING | OA_VISIBILITY_LINE_OF_SIGHT);
    set_text(world->player_info[0].map_name, sizeof world->player_info[0].map_name, "Coast");
    world->player_info[0].metal_hundreds = 5;
    world->player_info[0].energy_hundreds = 10;

    oa::Player& pilot = world->game.players[0];
    pilot.unit_count = 3;
    pilot.kills = 2;
    pilot.units_created = 7;
    pilot.board_row = 0;
    pilot.resource_flags = OA_PLAYER_RESOURCE_SHARES_STORAGE;
    pilot.metal = 1000.f;
    pilot.metal_storage = 250.5f;
    pilot.shared_metal_storage = 40.f;
    pilot.metal_produced = 12.25f;
    pilot.metal_requested = 3.f;
    pilot.metal_produced_total = 40.0;
    pilot.metal_requested_total = 10.0;
    pilot.metal_wasted_total = 1.0;
    pilot.energy = 500.f;
    pilot.energy_storage = 200.f;
    pilot.shared_energy_storage = 15.f;
    pilot.energy_produced = 8.f;
    pilot.energy_requested = 4.f;
    pilot.energy_produced_total = 90.0;
    pilot.energy_requested_total = 20.0;
    pilot.energy_wasted_total = 2.0;

    oa::Player& brute = world->game.players[1];
    brute.unit_count = 4;
    brute.losses = 2;
    brute.units_created = 4;
    brute.board_row = 2;
    brute.metal = 50.f;
    brute.metal_storage = 80.f;

    oa::Player& scout = world->game.players[2];
    scout.unit_count = 1;
    scout.units_created = 3;
    scout.board_row = 1;
    scout.metal = 80.f;
    scout.metal_storage = 10.f;
    scout.metal_produced = 1.f;
    scout.metal_requested = 1.f;
    scout.metal_produced_total = 5.0;
    scout.metal_requested_total = 2.0;
    scout.metal_wasted_total = 0.0;
    return world;
}

/// Fills the facts a frame hook would have copied, for the made-up mod Ridge.
///
/// @param[in,out] serve the tools' game
void fill_ridge(mcp::Serve& serve) {
    serve.reading.engine_version = "0.9.0-test";
    serve.reading.profile.has_mod = true;
    serve.reading.profile.mod_id = "ridge";
    serve.reading.profile.mod_name = "Ridge";
    serve.reading.profile.mod_version = "28";
    oa::app::ActiveHack hack;
    hack.id = "ridge.dawn";
    hack.values.push_back(oa::app::ActiveHack::Value{"hours", "28"});
    serve.reading.profile.hacks.push_back(std::move(hack));
    oa::app::MatchProfileReading::Mutator mutator;
    mutator.id = "long-nights";
    mutator.version = "1";
    mutator.name = "Long Nights";
    serve.reading.profile.mutators.push_back(std::move(mutator));
}

/// The player at a slot of a players array.
///
/// @param players the array
/// @param slot the slot
/// @return the player, or null
const json::Json* player_at(const json::Json& players, int64_t slot) {
    for (const json::Json& player : players.elements()) {
        const json::Json* found = player.find("slot");
        if (found != nullptr && found->integer() && *found->integer() == slot)
            return &player;
    }
    return nullptr;
}

/// A number member's text.
///
/// @param object the object
/// @param name the member
/// @return the text, or null
const std::string* real_of(const json::Json& object, std::string_view name) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return nullptr;
    return value->number_text();
}

/// Checks one resource's amount, storage, rates and totals.
///
/// @param store the metal or energy object
/// @param amount the amount text
/// @param storage the storage text
/// @param income the income text
/// @param expense the expense text
void check_store(
    const json::Json& store,
    std::string_view amount,
    std::string_view storage,
    std::string_view income,
    std::string_view expense
) {
    const std::string* amount_text = real_of(store, "amount");
    const std::string* storage_text = real_of(store, "storage");
    const std::string* income_text = real_of(store, "income");
    const std::string* expense_text = real_of(store, "expense");
    OA_CHECK(amount_text != nullptr && *amount_text == amount);
    OA_CHECK(storage_text != nullptr && *storage_text == storage);
    OA_CHECK(income_text != nullptr && *income_text == income);
    OA_CHECK(expense_text != nullptr && *expense_text == expense);
}

/// game.info in a match and outside one, and observe mode's tool list.
void check_info() {
    const std::unique_ptr<oa::World> world = stand_in();
    mcp::Serve serve;
    fill_ridge(serve);
    serve.reading.world = world.get();
    mcp::PlayBridge bridge;
    mcp::Session session = open_session(serve, bridge, false);

    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"game.info\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* in_match = body->find("in_match");
            const json::Json* engine = body->find("engine");
            OA_CHECK(in_match != nullptr && in_match->boolean() && *in_match->boolean());
            OA_CHECK(engine != nullptr && engine->string() && *engine->string() == "0.9.0-test");
            const json::Json* mod = body->find("mod");
            OA_CHECK(mod != nullptr && mod->type() == json::JsonType::object);
            if (mod != nullptr) {
                const json::Json* id = mod->find("id");
                const json::Json* name = mod->find("name");
                const json::Json* version = mod->find("version");
                OA_CHECK(id != nullptr && id->string() && *id->string() == "ridge");
                OA_CHECK(name != nullptr && name->string() && *name->string() == "Ridge");
                OA_CHECK(version != nullptr && version->string() && *version->string() == "28");
            }
            const json::Json* hacks = body->find("hacks");
            OA_CHECK(hacks != nullptr && hacks->elements().size() == 1);
            if (hacks != nullptr && !hacks->elements().empty()) {
                const json::Json* id = hacks->elements().front().find("id");
                const json::Json* values = hacks->elements().front().find("values");
                OA_CHECK(id != nullptr && id->string() && *id->string() == "ridge.dawn");
                const json::Json* hours = values != nullptr ? values->find("hours") : nullptr;
                OA_CHECK(hours != nullptr && hours->integer() && *hours->integer() == 28);
            }
            const json::Json* map = body->find("map");
            OA_CHECK(map != nullptr);
            if (map != nullptr) {
                const json::Json* name = map->find("name");
                const json::Json* width = map->find("width");
                const json::Json* height = map->find("height");
                OA_CHECK(name != nullptr && name->string() && *name->string() == "Coast");
                OA_CHECK(width != nullptr && width->integer() && *width->integer() == 128);
                OA_CHECK(height != nullptr && height->integer() && *height->integer() == 64);
            }
            const json::Json* rules = body->find("rules");
            OA_CHECK(rules != nullptr && rules->type() == json::JsonType::object);
            if (rules != nullptr) {
                const json::Json* commander = rules->find("commander");
                const json::Json* sight = rules->find("line_of_sight");
                const json::Json* mapped = rules->find("mapped");
                const json::Json* metal = rules->find("metal");
                const json::Json* energy = rules->find("energy");
                const json::Json* limit = rules->find("unit_limit");
                const json::Json* kind = rules->find("game_type");
                OA_CHECK(
                    commander != nullptr && commander->string() && *commander->string() == "ends"
                );
                OA_CHECK(sight != nullptr && sight->string() && *sight->string() == "circular");
                OA_CHECK(mapped != nullptr && mapped->boolean() && !*mapped->boolean());
                OA_CHECK(metal != nullptr && metal->integer() && *metal->integer() == 500);
                OA_CHECK(energy != nullptr && energy->integer() && *energy->integer() == 1000);
                OA_CHECK(limit != nullptr && limit->integer() && *limit->integer() == 400);
                OA_CHECK(kind != nullptr && kind->type() == json::JsonType::null);
                const json::Json* mutators = rules->find("mutators");
                OA_CHECK(mutators != nullptr && mutators->elements().size() == 1);
                if (mutators != nullptr && !mutators->elements().empty()) {
                    const json::Json* id = mutators->elements().front().find("id");
                    OA_CHECK(id != nullptr && id->string() && *id->string() == "long-nights");
                }
            }
            const json::Json* players = body->find("players");
            OA_CHECK(players != nullptr && players->elements().size() == 3);
            if (players != nullptr) {
                const json::Json* scout = player_at(*players, 2);
                OA_CHECK(scout != nullptr);
                if (scout != nullptr) {
                    const json::Json* side_name = scout->find("side_name");
                    const json::Json* team = scout->find("team");
                    OA_CHECK(
                        side_name != nullptr && side_name->string() && *side_name->string() == "ARM"
                    );
                    OA_CHECK(team != nullptr && team->integer() && *team->integer() == 2);
                }
            }
        }
    }

    if (const std::optional<json::Json> answer =
            only(session, request("3", "resources/read", "{\"uri\":\"oa://game/info\"}"))) {
        if (const std::optional<json::Json> body = resource_body(*answer)) {
            const json::Json* engine = body->find("engine");
            OA_CHECK(engine != nullptr && engine->string() && *engine->string() == "0.9.0-test");
        }
    }

    serve.reading.world = nullptr;
    serve.reading.profile.has_mod = false;
    if (const std::optional<json::Json> answer =
            only(session, request("4", "tools/call", "{\"name\":\"game.info\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* in_match = body->find("in_match");
            const json::Json* engine = body->find("engine");
            const json::Json* mod = body->find("mod");
            const json::Json* map = body->find("map");
            const json::Json* rules = body->find("rules");
            const json::Json* players = body->find("players");
            const json::Json* hacks = body->find("hacks");
            OA_CHECK(in_match != nullptr && in_match->boolean() && !*in_match->boolean());
            OA_CHECK(engine != nullptr && engine->string() && *engine->string() == "0.9.0-test");
            OA_CHECK(mod != nullptr && mod->type() == json::JsonType::null);
            OA_CHECK(map != nullptr && map->type() == json::JsonType::null);
            OA_CHECK(rules != nullptr && rules->type() == json::JsonType::null);
            OA_CHECK(players != nullptr && players->elements().empty());
            OA_CHECK(hacks != nullptr && hacks->elements().size() == 1);
        }
    }

    mcp::PlayBridge watching;
    mcp::Session observed = open_session(serve, watching, true);
    if (const std::optional<json::Json> answer = only(observed, request("2", "tools/list"))) {
        const json::Json* result = answer->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        OA_CHECK(tools != nullptr);
        if (tools != nullptr) {
            OA_CHECK(listed(*tools, "game.info"));
            OA_CHECK(listed(*tools, "game.status"));
            OA_CHECK(!listed(*tools, "game.quit"));
        }
    }
}

/// The economy, the hidden enemy, and the refusal of full awareness.
void check_economy() {
    const std::unique_ptr<oa::World> world = stand_in();
    world->game.tick = 0;
    mcp::Serve serve;
    serve.reading.world = world.get();
    bool allowed = false;
    serve.reading.full_awareness_allowed = [](void* context) {
        return *static_cast<const bool*>(context);
    };
    serve.reading.awareness_context = &allowed;
    mcp::PlayBridge bridge;
    mcp::Session session = open_session(serve, bridge, false);

    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"game.status\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* limit = body->find("unit_limit");
            const json::Json* clock = body->find("clock");
            OA_CHECK(limit != nullptr && limit->integer() && *limit->integer() == 400);
            OA_CHECK(clock != nullptr);
            if (clock != nullptr) {
                const json::Json* display = clock->find("display");
                const json::Json* seconds = clock->find("seconds");
                OA_CHECK(
                    display != nullptr && display->string() && *display->string() == "00:00:00"
                );
                OA_CHECK(seconds != nullptr && seconds->integer() && *seconds->integer() == 0);
            }
            const json::Json* players = body->find("players");
            OA_CHECK(players != nullptr);
            if (players != nullptr) {
                const json::Json* pilot = player_at(*players, 0);
                const json::Json* brute = player_at(*players, 1);
                const json::Json* scout = player_at(*players, 2);
                OA_CHECK(pilot != nullptr && brute != nullptr && scout != nullptr);
                if (pilot != nullptr) {
                    const json::Json* created = pilot->find("units_created");
                    const json::Json* row = pilot->find("board_row");
                    const json::Json* kills = pilot->find("kills");
                    OA_CHECK(created != nullptr && created->integer() && *created->integer() == 7);
                    OA_CHECK(row != nullptr && row->integer() && *row->integer() == 0);
                    OA_CHECK(kills != nullptr && kills->integer() && *kills->integer() == 2);
                    const json::Json* economy = pilot->find("economy");
                    OA_CHECK(economy != nullptr && economy->type() == json::JsonType::object);
                    if (economy != nullptr) {
                        const json::Json* metal = economy->find("metal");
                        const json::Json* energy = economy->find("energy");
                        OA_CHECK(metal != nullptr && energy != nullptr);
                        if (metal != nullptr) {
                            check_store(*metal, "1000", "250.5", "12.25", "3");
                            const std::string* produced = real_of(*metal, "produced_total");
                            const std::string* shared = real_of(*metal, "shared_storage");
                            OA_CHECK(produced != nullptr && *produced == "40");
                            OA_CHECK(shared != nullptr && *shared == "40");
                        }
                        if (energy != nullptr) {
                            check_store(*energy, "500", "200", "8", "4");
                            const std::string* shared = real_of(*energy, "shared_storage");
                            OA_CHECK(shared != nullptr && *shared == "15");
                        }
                    }
                }
                if (brute != nullptr) {
                    const json::Json* economy = brute->find("economy");
                    OA_CHECK(economy != nullptr && economy->type() == json::JsonType::null);
                }
                if (scout != nullptr) {
                    const json::Json* economy = scout->find("economy");
                    const json::Json* metal = economy != nullptr ? economy->find("metal") : nullptr;
                    OA_CHECK(metal != nullptr);
                    if (metal != nullptr) {
                        check_store(*metal, "80", "10", "1", "1");
                        OA_CHECK(metal->find("shared_storage") == nullptr);
                    }
                }
            }
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "3",
                "tools/call",
                "{\"name\":\"game.status\",\"arguments\":{\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        const std::optional<std::string> text = tool_text(*answer);
        OA_CHECK(
            text && text->find("full awareness is only available in a single-player match") !=
                        std::string::npos
        );
    }

    serve.reading.full_awareness_allowed = nullptr;
    serve.reading.awareness_context = nullptr;
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "4",
                "tools/call",
                "{\"name\":\"game.status\",\"arguments\":{\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
    }

    allowed = true;
    serve.reading.full_awareness_allowed = [](void* context) {
        return *static_cast<const bool*>(context);
    };
    serve.reading.awareness_context = &allowed;
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "5",
                "tools/call",
                "{\"name\":\"game.status\",\"arguments\":{\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* players = body->find("players");
            const json::Json* brute = players != nullptr ? player_at(*players, 1) : nullptr;
            const json::Json* economy = brute != nullptr ? brute->find("economy") : nullptr;
            const json::Json* metal = economy != nullptr ? economy->find("metal") : nullptr;
            const std::string* amount = metal != nullptr ? real_of(*metal, "amount") : nullptr;
            OA_CHECK(amount != nullptr && *amount == "50");
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "6",
                "tools/call",
                "{\"name\":\"game.status\",\"arguments\":{\"awareness\":\"nope\"}}"
            )
        ))
        // The schema's enum refuses the value before the tool runs: a protocol error.
        OA_CHECK(answer->find("error") != nullptr);

    allowed = false;
    if (const std::optional<json::Json> answer =
            only(session, request("7", "resources/read", "{\"uri\":\"oa://game/status\"}"))) {
        if (const std::optional<json::Json> body = resource_body(*answer)) {
            const json::Json* players = body->find("players");
            const json::Json* brute = players != nullptr ? player_at(*players, 1) : nullptr;
            const json::Json* economy = brute != nullptr ? brute->find("economy") : nullptr;
            OA_CHECK(economy != nullptr && economy->type() == json::JsonType::null);
        }
    }
}

/// The clock's display at 59 s and at 1 h.
void check_clock() {
    const std::unique_ptr<oa::World> world = stand_in();
    mcp::Serve serve;
    serve.reading.world = world.get();
    mcp::PlayBridge bridge;
    mcp::Session session = open_session(serve, bridge, false);

    world->game.tick = 59u * 30u;
    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"game.status\"}"))) {
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* clock = body->find("clock");
            const json::Json* display = clock != nullptr ? clock->find("display") : nullptr;
            const json::Json* seconds = clock != nullptr ? clock->find("seconds") : nullptr;
            const json::Json* tick = clock != nullptr ? clock->find("tick") : nullptr;
            OA_CHECK(display != nullptr && display->string() && *display->string() == "00:00:59");
            OA_CHECK(seconds != nullptr && seconds->integer() && *seconds->integer() == 59);
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 59 * 30);
        }
    }

    world->game.tick = 3600u * 30u;
    if (const std::optional<json::Json> answer =
            only(session, request("3", "tools/call", "{\"name\":\"game.status\"}"))) {
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* clock = body->find("clock");
            const json::Json* display = clock != nullptr ? clock->find("display") : nullptr;
            const json::Json* seconds = clock != nullptr ? clock->find("seconds") : nullptr;
            OA_CHECK(display != nullptr && display->string() && *display->string() == "01:00:00");
            OA_CHECK(seconds != nullptr && seconds->integer() && *seconds->integer() == 3600);
        }
    }
}

} // namespace

int main() {
    check_info();
    check_economy();
    check_clock();
    return oa::test::check_exit_status();
}
