// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// orders.issue, orders.state and units.select (actions.hpp). A call is
// checked when it arrives. One that would order a replay, a watcher, a
// finished match or no running match is refused then. One that can be made
// waits, and is applied after the pump and before the match clock. Which
// units this machine plays is decided then, by the play host. The answer
// names the tick and, for an order, the queues.
#include "oa/app/mcp/actions.hpp"

#include "oa/formats/json.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

// The most units one call names. A longer list is refused whole.
constexpr std::size_t kMaxOrderedUnits = 64;

// A unit name is at most 31 characters, the type field's size less its NUL.
constexpr std::size_t kMaxTypeName = 31;

constexpr std::string_view kIssueSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"units\",\"kind\"],"
    "\"properties\":{\"units\":{\"type\":\"array\"},"
    "\"kind\":{\"type\":\"string\",\"enum\":[\"move\",\"patrol\",\"attack\",\"guard\","
    "\"repair\",\"reclaim\",\"build\",\"factory\",\"stop\"]},"
    "\"append\":{\"type\":\"boolean\"},"
    "\"target\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535},"
    "\"point\":{\"type\":\"array\"},"
    "\"type\":{\"type\":\"string\"},"
    "\"count\":{\"type\":\"integer\"},"
    "\"facing\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":3}}}";

constexpr std::string_view kStateSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"units\",\"state\"],"
    "\"properties\":{\"units\":{\"type\":\"array\"},"
    "\"state\":{\"type\":\"string\",\"enum\":[\"fire\",\"move\",\"on\",\"off\",\"cloak\"]},"
    "\"value\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":2},"
    "\"on\":{\"type\":\"boolean\"}}}";

constexpr std::string_view kSelectSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"units\"],"
    "\"properties\":{\"units\":{\"type\":\"array\"},\"add\":{\"type\":\"boolean\"}}}";

/// A failure the tool ran into. The sentence is what the caller is told.
///
/// @param text why it was refused
/// @return the answer
ToolReply failed_reply(std::string text) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(text);
    return reply;
}

/// The host the bridge names, or an empty one when it names none.
///
/// @param play the bridge
/// @return the host
PlayHost host_now(PlayBridge& play) {
    if (play.host_of == nullptr)
        return {};
    return play.host_of(play.context);
}

/// Answers an order tool from the poll the session is on.
///
/// The token is the units member. The session moves the arguments object at
/// the end of every pump, and a member of its tree keeps its address.
///
/// @param play the bridge
/// @param request what the call asked for
/// @param call the poll and the arguments
/// @return the answer
ToolReply answer_command(PlayBridge& play, const PlayRequest& request, const ToolCall& call) {
    const PlayHost host = host_now(play);
    if (call.poll == 0)
        return play.queue.consider(host, request);
    const json::Json* token = call.arguments != nullptr ? call.arguments->find("units") : nullptr;
    return play.queue.follow(host, request, token, call.poll);
}

/// A whole number that fits in 32 bits.
///
/// @param value the JSON value
/// @return the number, or nothing when it is not one
std::optional<int32_t> int32_of(const json::Json& value) {
    const std::optional<int64_t> number = value.integer();
    if (!number)
        return std::nullopt;
    const int64_t lowest = std::numeric_limits<int32_t>::min();
    const int64_t highest = std::numeric_limits<int32_t>::max();
    if (*number < lowest || *number > highest)
        return std::nullopt;
    return static_cast<int32_t>(*number);
}

/// Reads the units, dropping a repeated id and keeping the first.
///
/// @param args the arguments
/// @param allow_empty true when an empty list is a clear
/// @param[out] ids the units
/// @return the sentence, or empty
std::string read_units(const json::Json& args, bool allow_empty, std::vector<uint16_t>& ids) {
    const json::Json* units = args.find("units");
    if (units == nullptr || units->type() != json::JsonType::array)
        return "units must be whole numbers";
    if (units->elements().size() > kMaxOrderedUnits)
        return "too many units";
    ids.clear();
    for (const json::Json& element : units->elements()) {
        const std::optional<int64_t> number = element.integer();
        if (!number || *number < 1 || *number > 65535)
            return "units must be whole numbers";
        const auto id = static_cast<uint16_t>(*number);
        bool repeated = false;
        for (uint16_t earlier : ids) {
            if (earlier == id)
                repeated = true;
        }
        if (!repeated)
            ids.push_back(id);
    }
    if (!allow_empty && ids.empty())
        return "name at least one unit";
    return {};
}

/// Reads a map point of two (x, z) or three (x, y, z) whole pixels.
///
/// @param point the point member, or null when the call gave none
/// @param[in,out] request the request; has_point and point are set when the call gave one
/// @return the sentence, or empty
std::string read_point(const json::Json* point, PlayRequest& request) {
    if (point == nullptr)
        return {};
    if (point->type() != json::JsonType::array)
        return "a point is two or three whole numbers";
    const auto elements = point->elements();
    if (elements.size() != 2 && elements.size() != 3)
        return "a point is two or three whole numbers";
    int32_t numbers[3] = {};
    for (std::size_t place = 0; place < elements.size(); ++place) {
        const std::optional<int32_t> number = int32_of(elements[place]);
        if (!number)
            return "a point is two or three whole numbers";
        numbers[place] = *number;
    }
    request.has_point = true;
    request.point.x = numbers[0];
    if (elements.size() == 2) {
        request.point.y = 0;
        request.point.z = numbers[1];
    } else {
        request.point.y = numbers[1];
        request.point.z = numbers[2];
    }
    return {};
}

/// Tells whether the units include the target.
///
/// @param ids the units
/// @param target the other unit
/// @return true when one of the units is the target
bool names_target(const std::vector<uint16_t>& ids, uint16_t target) {
    for (uint16_t id : ids) {
        if (id == target)
            return true;
    }
    return false;
}

/// Chooses the order from the kind and the fields the call gave.
///
/// @param name the kind
/// @param count_set true when the call gave a count
/// @param[in,out] request the request; its order is set
/// @return the sentence, or empty
std::string bind_kind(std::string_view name, bool count_set, PlayRequest& request) {
    if (name == "move" || name == "patrol") {
        if (!request.has_point)
            return "the order needs a point";
        request.order =
            name == "move" ? oa::app::PlayOrderKind::move : oa::app::PlayOrderKind::patrol;
        return {};
    }
    if (name == "attack") {
        if (request.target != 0 && names_target(request.units, request.target))
            return "a unit cannot take that order on itself";
        if (request.target != 0) {
            request.order = oa::app::PlayOrderKind::attack_unit;
            return {};
        }
        if (request.has_point) {
            request.order = oa::app::PlayOrderKind::attack_ground;
            return {};
        }
        return "an attack needs a unit or a point";
    }
    if (name == "guard" || name == "repair") {
        if (request.target == 0)
            return name == "guard" ? "a guard needs a unit" : "a repair needs a unit";
        if (names_target(request.units, request.target))
            return "a unit cannot take that order on itself";
        request.order =
            name == "guard" ? oa::app::PlayOrderKind::guard : oa::app::PlayOrderKind::repair;
        return {};
    }
    if (name == "reclaim") {
        if (request.target != 0 && names_target(request.units, request.target))
            return "a unit cannot take that order on itself";
        if (request.target != 0) {
            request.order = oa::app::PlayOrderKind::reclaim_unit;
            return {};
        }
        if (request.has_point) {
            request.order = oa::app::PlayOrderKind::reclaim_ground;
            return {};
        }
        return "a reclaim needs a unit or a point";
    }
    if (name == "build") {
        if (request.type_name.empty())
            return "the order needs a unit type";
        if (request.type_name.size() > kMaxTypeName)
            return "no such unit type";
        if (request.has_point) {
            request.order = oa::app::PlayOrderKind::mobile_build;
            return {};
        }
        if (count_set && request.count < 1)
            return "the count is not a number of units";
        request.order = oa::app::PlayOrderKind::building_build;
        return {};
    }
    if (name == "factory") {
        if (request.type_name.empty())
            return "the order needs a unit type";
        if (request.type_name.size() > kMaxTypeName)
            return "no such unit type";
        if (!count_set || request.count == 0)
            return "a factory order needs a count";
        request.order = oa::app::PlayOrderKind::factory;
        return {};
    }
    if (name == "stop") {
        request.order = oa::app::PlayOrderKind::stop;
        return {};
    }
    return "the order was refused";
}

/// Reads orders.issue. An empty error means request is ready to queue.
///
/// @param args the arguments
/// @param[out] request the request
/// @return the sentence, or empty
std::string read_issue(const json::Json& args, PlayRequest& request) {
    request.kind = PlayKind::order;
    if (const std::string why = read_units(args, false, request.units); !why.empty())
        return why;
    const json::Json* kind = args.find("kind");
    const std::string* name = kind != nullptr ? kind->string() : nullptr;
    if (name == nullptr)
        return "the order was refused";
    if (const json::Json* append = args.find("append")) {
        const std::optional<bool> value = append->boolean();
        if (!value)
            return "the order was refused";
        request.append = *value;
    }
    if (const json::Json* target = args.find("target")) {
        const std::optional<int64_t> value = target->integer();
        if (!value || *value < 1 || *value > 65535)
            return "no such target";
        request.target = static_cast<uint16_t>(*value);
    }
    if (const std::string why = read_point(args.find("point"), request); !why.empty())
        return why;
    if (const json::Json* type_name = args.find("type")) {
        const std::string* text = type_name->string();
        if (text == nullptr)
            return "the order needs a unit type";
        request.type_name = *text;
    }
    bool count_set = false;
    if (const json::Json* count = args.find("count")) {
        const std::optional<int32_t> value = int32_of(*count);
        if (!value)
            return "the count is not a number of units";
        request.count = *value;
        count_set = true;
    }
    if (const json::Json* facing = args.find("facing")) {
        const std::optional<int32_t> value = int32_of(*facing);
        if (!value || *value < 0 || *value > 3)
            return "the order was refused";
        request.facing = static_cast<uint8_t>(*value);
    }
    return bind_kind(*name, count_set, request);
}

/// Reads orders.state.
///
/// @param args the arguments
/// @param[out] request the request
/// @return the sentence, or empty
std::string read_state(const json::Json& args, PlayRequest& request) {
    request.kind = PlayKind::state;
    if (const std::string why = read_units(args, false, request.units); !why.empty())
        return why;
    const json::Json* state = args.find("state");
    const std::string* name = state != nullptr ? state->string() : nullptr;
    if (name == nullptr)
        return "the order was refused";
    if (*name == "fire" || *name == "move") {
        const json::Json* value = args.find("value");
        const std::optional<int32_t> number = value != nullptr ? int32_of(*value) : std::nullopt;
        if (!number || *number < 0 || *number > 2)
            return "the standing order needs a value";
        request.state =
            *name == "fire" ? oa::app::PlayStateKind::fire : oa::app::PlayStateKind::move;
        request.state_value = *number;
        return {};
    }
    if (*name == "on" || *name == "off") {
        request.state = *name == "on" ? oa::app::PlayStateKind::on : oa::app::PlayStateKind::off;
        return {};
    }
    if (*name == "cloak") {
        const json::Json* on = args.find("on");
        const std::optional<bool> value = on != nullptr ? on->boolean() : std::nullopt;
        if (!value)
            return "cloak needs on";
        request.state = oa::app::PlayStateKind::cloak;
        request.state_on = *value;
        return {};
    }
    return "the order was refused";
}

/// Reads units.select. An empty list with add false clears the selection.
///
/// @param args the arguments
/// @param[out] request the request
/// @return the sentence, or empty
std::string read_select(const json::Json& args, PlayRequest& request) {
    request.kind = PlayKind::select;
    if (const std::string why = read_units(args, true, request.units); !why.empty())
        return why;
    if (const json::Json* add = args.find("add")) {
        const std::optional<bool> value = add->boolean();
        if (!value)
            return "the order was refused";
        request.select_add = *value;
    }
    return {};
}

/// Answers orders.issue.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply orders_issue(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr || call.arguments == nullptr)
        return failed_reply("no match is running");
    PlayRequest request;
    if (const std::string why = read_issue(*call.arguments, request); !why.empty())
        return failed_reply(why);
    return answer_command(*play, request, call);
}

/// Answers orders.state.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply orders_state(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr || call.arguments == nullptr)
        return failed_reply("no match is running");
    PlayRequest request;
    if (const std::string why = read_state(*call.arguments, request); !why.empty())
        return failed_reply(why);
    return answer_command(*play, request, call);
}

/// Answers units.select.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply units_select(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr || call.arguments == nullptr)
        return failed_reply("no match is running");
    PlayRequest request;
    if (const std::string why = read_select(*call.arguments, request); !why.empty())
        return failed_reply(why);
    return answer_command(*play, request, call);
}

} // namespace

void add_order_tools(Session& session, PlayBridge& play) {
    Tool issue;
    issue.name = "orders.issue";
    issue.description =
        "Gives an order to this machine's units: move, patrol, attack, guard, repair, "
        "reclaim, build, factory or stop. append adds it behind the orders the unit "
        "already has. A replay, a watcher and a finished match are refused. The answer "
        "names the tick the order landed on and the queues afterwards.";
    issue.input_schema = std::string(kIssueSchema);
    issue.reads = false;
    issue.idempotent = false;
    issue.context = &play;
    issue.call = orders_issue;
    session.add_tool(std::move(issue));

    Tool standing;
    standing.name = "orders.state";
    standing.description =
        "Sets a standing order on this machine's units: fire, move, on, off or cloak. "
        "A unit the order does not reach is left as it is. A replay, a watcher and a "
        "finished match are refused. The answer names the tick and the queues afterwards.";
    standing.input_schema = std::string(kStateSchema);
    standing.reads = false;
    standing.idempotent = true;
    standing.context = &play;
    standing.call = orders_state;
    session.add_tool(std::move(standing));

    Tool selection;
    selection.name = "units.select";
    selection.description =
        "Selects this machine's units, as a click does, or clears the selection. "
        "add keeps the units already selected. A unit that cannot be selected refuses "
        "the call and leaves the selection as it was. A replay, a watcher and a finished "
        "match are refused. The answer names the tick and the units selected afterwards.";
    selection.input_schema = std::string(kSelectSchema);
    selection.reads = false;
    selection.idempotent = true;
    selection.context = &play;
    selection.call = units_select;
    session.add_tool(std::move(selection));
}

} // namespace oa::app::mcp
