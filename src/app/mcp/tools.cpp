// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// game.info, game.status, game.speed, game.pause, the camera tools, the
// clock tools, orders, selection and game.quit (tools.hpp). The match
// status is the one the automation host reports. game.info reads the facts
// the frame hook copied and the match's map and rules. game.quit asks that
// host to leave. game.speed, game.pause, the camera, the clock, orders and
// selection go through the play host. The map tools read the match view.
#include "tools.hpp"

#include "reports.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/play_host.hpp"
#include "oa/core/world.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

constexpr std::string_view kStatusSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"awareness\":{\"type\":\"string\",\"enum\":[\"player\",\"full\"]}}}";

constexpr std::string_view kInfoSchema = "{\"type\":\"object\",\"additionalProperties\":false}";

/// The match the tools read: a test's stand-in, or the automation host's world.
///
/// @param serve the running game
/// @return the world; null while no match runs
const oa::World* match_world(const Serve& serve) {
    if (serve.reading.world != nullptr)
        return serve.reading.world;
    if (serve.runtime == nullptr)
        return nullptr;
    const AutomationHost host = automation_host(*serve.runtime);
    if (host.match_world == nullptr)
        return nullptr;
    return host.match_world(host.context);
}

/// Reads the call's awareness.
///
/// @param args the arguments
/// @param allowed whether the match allows full awareness
/// @param[out] full set when the call asks for full awareness
/// @return the failure, or empty
std::string awareness_failure(const json::Json& args, bool allowed, bool& full) {
    full = false;
    const json::Json* value = args.find("awareness");
    if (value == nullptr)
        return {};
    const std::string* text = value->string();
    if (text == nullptr || (*text != "player" && *text != "full"))
        return "awareness must be player or full";
    if (*text == "full") {
        if (!allowed)
            return "full awareness is only available in a single-player match";
        full = true;
    }
    return {};
}

/// Whose economy a status call may read.
///
/// Full awareness is the match view's rule. A missing callback refuses it,
/// which is a shared match and a test that set none.
///
/// @param serve the running game
/// @param call the arguments
/// @param[out] why the failure, or empty
/// @return the economy to write; player when the call fails
oa::app::automation::EconomyView
economy_shown(const Serve& serve, const ToolCall& call, std::string& why) {
    bool full = false;
    bool allowed = false;
    if (serve.reading.full_awareness_allowed != nullptr)
        allowed = serve.reading.full_awareness_allowed(serve.reading.awareness_context);
    why = awareness_failure(*call.arguments, allowed, full);
    if (!why.empty())
        return oa::app::automation::EconomyView::player;
    return full ? oa::app::automation::EconomyView::all : oa::app::automation::EconomyView::player;
}

/// The running match's status, as one JSON object.
///
/// tick is the match's tick, or 0 when no match runs. The other members are
/// the ones the automation endpoint reports of a match, plus the clock and
/// the unit limit while a match runs.
///
/// @param serve the running game
/// @param shown whose economy is written
/// @return the object
std::string status_text(const Serve& serve, oa::app::automation::EconomyView shown) {
    const oa::World* world = match_world(serve);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    if (world != nullptr)
        writer.integer(static_cast<int64_t>(world->game.tick));
    else
        writer.integer(0);
    oa::app::automation::write_match(writer, world, std::nullopt, shown);
    writer.end_object();
    return writer.text();
}

/// A failed tool answer.
///
/// @param why the failure, as the caller reads it
/// @return the answer
ToolReply fail(std::string why) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(why);
    return reply;
}

/// Answers game.status.
///
/// An enemy's economy is null unless the call asks for full awareness and
/// the match allows it. The resource reads with the player's awareness.
///
/// @param context the Serve
/// @param call the arguments
/// @return the status text, or the failure
ToolReply read_status(void* context, const ToolCall& call) {
    const Serve empty;
    const Serve& serve = context != nullptr ? *static_cast<const Serve*>(context) : empty;
    std::string why;
    const oa::app::automation::EconomyView shown = economy_shown(serve, call, why);
    if (!why.empty())
        return fail(std::move(why));
    ToolReply reply;
    reply.text = status_text(serve, shown);
    return reply;
}

/// Answers game.quit. The process leaves after the answer is written.
///
/// @param context the Serve
/// @return {"quitting":true}
ToolReply leave_game(void* context, const ToolCall& /*call*/) {
    if (auto* serve = static_cast<Serve*>(context))
        serve->quit = true;
    ToolReply reply;
    reply.text = "{\"quitting\":true}";
    return reply;
}

/// Reads oa://game/status. A resource has no arguments, so the economy is
/// the local player's and its allies'.
///
/// @param context the Serve
/// @return the status text
std::string read_status_resource(void* context) {
    const auto* serve = static_cast<const Serve*>(context);
    const Serve empty;
    return status_text(serve != nullptr ? *serve : empty, oa::app::automation::EconomyView::player);
}

/// Writes the mod, or null for the base game.
///
/// @param[in,out] writer the object being written
/// @param reading the facts
void write_mod(json::JsonWriter& writer, const GameReading& reading) {
    writer.key("mod");
    if (!reading.profile.has_mod) {
        writer.null();
        return;
    }
    writer.begin_object();
    writer.key("id");
    writer.string(reading.profile.mod_id);
    writer.key("name");
    writer.string(reading.profile.mod_name);
    writer.key("version");
    writer.string(reading.profile.mod_version);
    writer.end_object();
}

/// Writes the standard hacks that are on, each parameter as its canonical JSON.
///
/// @param[in,out] writer the object being written
/// @param reading the facts
void write_hacks(json::JsonWriter& writer, const GameReading& reading) {
    writer.key("hacks");
    writer.begin_array();
    for (const ActiveHack& hack : reading.profile.hacks) {
        writer.begin_object();
        writer.key("id");
        writer.string(hack.id);
        writer.key("values");
        writer.begin_object();
        for (const ActiveHack::Value& hack_value : hack.values) {
            writer.key(hack_value.name);
            writer.raw(hack_value.json);
        }
        writer.end_object();
        writer.end_object();
    }
    writer.end_array();
}

/// Writes the mutators the running match applied.
///
/// @param[in,out] writer the rules object, open
/// @param reading the facts
void write_mutators(json::JsonWriter& writer, const GameReading& reading) {
    writer.key("mutators");
    writer.begin_array();
    for (const MatchProfileReading::Mutator& mutator : reading.profile.mutators) {
        writer.begin_object();
        writer.key("id");
        writer.string(mutator.id);
        writer.key("version");
        writer.string(mutator.version);
        writer.key("name");
        writer.string(mutator.name);
        writer.end_object();
    }
    writer.end_array();
}

/// The match's fixed facts, as one JSON object.
///
/// Outside a match, in_match is false and the mod, the hacks and the engine
/// version stay filled. The map, the rules and the players are of the
/// running match.
///
/// @param serve the running game
/// @return the object
std::string info_text(const Serve& serve) {
    const oa::World* world = match_world(serve);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("in_match");
    writer.boolean(world != nullptr);
    writer.key("engine");
    writer.string(serve.reading.engine_version);
    write_mod(writer, serve.reading);
    write_hacks(writer, serve.reading);
    writer.key("map");
    if (world != nullptr)
        oa::app::automation::write_map(writer, *world);
    else
        writer.null();
    writer.key("rules");
    if (world != nullptr) {
        writer.begin_object();
        oa::app::automation::write_rule_fields(writer, *world);
        write_mutators(writer, serve.reading);
        writer.key("game_type");
        if (serve.reading.has_game_type)
            writer.string(serve.reading.game_type);
        else
            writer.null();
        writer.end_object();
    } else {
        writer.null();
    }
    writer.key("players");
    if (world != nullptr)
        oa::app::automation::write_seated_sides(writer, *world);
    else {
        writer.begin_array();
        writer.end_array();
    }
    writer.end_object();
    return writer.text();
}

/// Answers game.info.
///
/// @param context the Serve
/// @return the facts
ToolReply read_info(void* context, const ToolCall& /*call*/) {
    const auto* serve = static_cast<const Serve*>(context);
    const Serve empty;
    ToolReply reply;
    reply.text = info_text(serve != nullptr ? *serve : empty);
    return reply;
}

/// Reads oa://game/info.
///
/// @param context the Serve
/// @return the facts
std::string read_info_resource(void* context) {
    const auto* serve = static_cast<const Serve*>(context);
    const Serve empty;
    return info_text(serve != nullptr ? *serve : empty);
}

/// The play host of the frame being served, or an empty host outside one.
///
/// @param context the Serve
/// @return the host
PlayHost host_from_runtime(void* context) {
    const auto* serve = static_cast<const Serve*>(context);
    if (serve == nullptr || serve->runtime == nullptr)
        return {};
    return play_host(*serve->runtime);
}

} // namespace

void add_game_tools(Session& session, Serve& serve, PlayBridge& play) {
    play.host_of = host_from_runtime;
    play.context = &serve;

    Tool info;
    info.name = "game.info";
    info.description = "Reads the engine version, the mod, the hacks in force, the map, "
                       "the rules and each seated player's side and team.";
    info.input_schema = std::string(kInfoSchema);
    info.reads = true;
    info.context = &serve;
    info.call = read_info;
    session.add_tool(std::move(info));

    Tool status;
    status.name = "game.status";
    status.description = "Reads the running match: its tick, clock, unit limit, whether it is "
                         "paused, its speed, its players, their economy and its outcome.";
    status.input_schema = std::string(kStatusSchema);
    status.reads = true;
    status.context = &serve;
    status.call = read_status;
    session.add_tool(std::move(status));

    Tool quit;
    quit.name = "game.quit";
    quit.description = "Leaves the game.";
    quit.input_schema = std::string(kStatusSchema);
    quit.reads = false;
    quit.destructive = true;
    quit.idempotent = true;
    quit.context = &serve;
    quit.call = leave_game;
    session.add_tool(std::move(quit));

    Resource facts;
    facts.uri = "oa://game/info";
    facts.name = "game info";
    facts.description = "The engine, the mod, the map and the rules, as game.info reports them.";
    facts.mime_type = "application/json";
    facts.context = &serve;
    facts.read = read_info_resource;
    session.add_resource(std::move(facts));

    Resource resource;
    resource.uri = "oa://game/status";
    resource.name = "game status";
    resource.description = "The running match, as game.status reports it.";
    resource.mime_type = "application/json";
    resource.context = &serve;
    resource.read = read_status_resource;
    session.add_resource(std::move(resource));

    add_play_tools(session, play);
}

} // namespace oa::app::mcp
