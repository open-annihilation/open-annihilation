// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The unit tools with a stand-in match view: the list, one unit, the order
// queues and the types. A unit the player is not shown is left out unless
// the call asks for full awareness and the match allows it.
#include "units.hpp"

#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;

/// The stand-in match the tools read.
struct Picture {
    bool running = true;
    bool allowed = true;
    uint32_t tick = 12;
    uint8_t viewer = 0;
    std::vector<oa::app::UnitRecord> units;
    oa::app::OrderQueues queues{};
    uint16_t order_id = 1;
    std::vector<oa::app::TypeRecord> types;
};

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

bool picture_active(void* context) {
    return static_cast<Picture*>(context)->running;
}

uint32_t picture_tick(void* context) {
    return static_cast<Picture*>(context)->tick;
}

uint8_t picture_viewer(void* context) {
    return static_cast<Picture*>(context)->viewer;
}

bool picture_allowed(void* context) {
    return static_cast<Picture*>(context)->allowed;
}

void picture_units(void* context, std::vector<oa::app::UnitRecord>* listed) {
    *listed = static_cast<Picture*>(context)->units;
}

bool picture_orders(void* context, uint16_t id, oa::app::OrderQueues* queues) {
    const auto* picture = static_cast<const Picture*>(context);
    if (id != picture->order_id)
        return false;
    *queues = picture->queues;
    return true;
}

void picture_types(void* context, std::vector<oa::app::TypeRecord>* catalog) {
    *catalog = static_cast<Picture*>(context)->types;
}

/// Binds the stand-in to a host.
///
/// @param picture the stand-in
/// @param[out] host the host the tools read
void bind_picture(Picture& picture, mcp::UnitHost& host) {
    host.table.context = &picture;
    host.table.active = picture_active;
    host.table.tick = picture_tick;
    host.table.viewer = picture_viewer;
    host.table.full_awareness_allowed = picture_allowed;
    host.table.units = picture_units;
    host.table.orders = picture_orders;
    host.table.types = picture_types;
}

/// A session that has finished initialize, with the unit tools installed.
///
/// @param host the stand-in host
/// @param observe list only tools that read
/// @return the session
mcp::Session open_session(mcp::UnitHost& host, bool observe = false) {
    mcp::SessionConfig config;
    config.observe = observe;
    mcp::Session session(std::move(config));
    mcp::add_unit_tools(session, host);
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    return session;
}

/// The one answer a message produced, parsed.
///
/// @param session the session
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
/// A failure's text is the message, not JSON. A success's text is one JSON object.
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

/// The ids in a units array, in order.
///
/// @param units the array
/// @return the ids
std::vector<int64_t> unit_ids(const json::Json& units) {
    std::vector<int64_t> ids;
    for (const json::Json& unit : units.elements()) {
        const json::Json* id = unit.find("id");
        if (id != nullptr && id->integer())
            ids.push_back(*id->integer());
    }
    return ids;
}

/// One unit of the stand-in.
///
/// @param id its slot
/// @param type its unit name
/// @param owner its owner
/// @param shown whether the player is shown it
/// @param x its map pixel
/// @param z its map pixel
/// @return the record
oa::app::UnitRecord
make_unit(uint16_t id, std::string type, uint8_t owner, bool shown, int32_t x, int32_t z) {
    oa::app::UnitRecord unit;
    unit.id = id;
    unit.type = std::move(type);
    unit.name = unit.type;
    unit.side = "ARM";
    unit.owner = owner;
    unit.x = x;
    unit.z = z;
    unit.health = 100;
    unit.build = 100;
    unit.shown = shown;
    unit.order = "Standby";
    return unit;
}

/// The list, one unit and the refusal of a unit the player is not shown.
void check_visibility() {
    Picture picture;
    picture.units.push_back(make_unit(1, "ARMCOM", 0, true, 100, 80));
    picture.units.push_back(make_unit(2, "CORCOM", 1, false, 900, 900));
    picture.units.push_back(make_unit(3, "ARMLAB", 0, true, 120, 80));
    picture.units.push_back(make_unit(4, "CORAK", 1, true, 400, 400));
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer = only(session, request("2", "tools/list"))) {
        const json::Json* result = answer->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        OA_CHECK(tools != nullptr && tools->elements().size() == 4);
    }

    if (const std::optional<json::Json> answer =
            only(session, request("3", "tools/call", "{\"name\":\"units.list\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* units = body->find("units");
            OA_CHECK(units != nullptr);
            if (units != nullptr) {
                const std::vector<int64_t> ids = unit_ids(*units);
                OA_CHECK(ids.size() == 3);
                OA_CHECK(ids.size() >= 1 && ids[0] == 1);
                bool hidden = false;
                for (int64_t id : ids)
                    hidden = hidden || id == 2;
                OA_CHECK(!hidden);
            }
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "4",
                "tools/call",
                "{\"name\":\"units.list\",\"arguments\":{\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* units = body->find("units");
            OA_CHECK(units != nullptr && units->elements().size() == 4);
            if (units != nullptr) {
                for (const json::Json& unit : units->elements()) {
                    const json::Json* id = unit.find("id");
                    const json::Json* shown = unit.find("shown");
                    if (id != nullptr && id->integer() && *id->integer() == 2)
                        OA_CHECK(shown != nullptr && shown->boolean() && !*shown->boolean());
                }
            }
        }
    }

    picture.allowed = false;
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "5",
                "tools/call",
                "{\"name\":\"units.list\",\"arguments\":{\"awareness\":\"full\"}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(text->find("full awareness") != std::string::npos);
    }
    picture.allowed = true;

    if (const std::optional<json::Json> answer = only(
            session, request("6", "tools/call", "{\"name\":\"units.get\",\"arguments\":{\"id\":2}}")
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "the unit is not in view");
    }
    if (const std::optional<json::Json> answer = only(
            session, request("7", "tools/call", "{\"name\":\"units.get\",\"arguments\":{\"id\":9}}")
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "no such unit");
    }
    if (const std::optional<json::Json> answer = only(
            session, request("8", "tools/call", "{\"name\":\"units.get\",\"arguments\":{\"id\":1}}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* unit = body->find("unit");
            const json::Json* type = unit != nullptr ? unit->find("type") : nullptr;
            OA_CHECK(type != nullptr && type->string() && *type->string() == "ARMCOM");
        }
    }
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "9",
                "tools/call",
                "{\"name\":\"units.get\",\"arguments\":{\"id\":1,\"extra\":true}}"
            )
        ))
        OA_CHECK(answer->find("error") != nullptr);
}

/// Owner, type and area filters, and a call with no match.
void check_filters() {
    Picture picture;
    picture.units.push_back(make_unit(1, "ARMCOM", 0, true, 100, 80));
    picture.units.push_back(make_unit(2, "CORCOM", 1, false, 900, 900));
    picture.units.push_back(make_unit(3, "ARMLAB", 0, true, 120, 80));
    picture.units.push_back(make_unit(4, "CORAK", 1, true, 400, 400));
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer = only(
            session,
            request("2", "tools/call", "{\"name\":\"units.list\",\"arguments\":{\"owner\":\"me\"}}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* units = body->find("units");
            OA_CHECK(units != nullptr);
            if (units != nullptr) {
                const std::vector<int64_t> ids = unit_ids(*units);
                OA_CHECK(ids.size() == 2 && ids[0] == 1 && ids[1] == 3);
            }
        }
    }
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "3", "tools/call", "{\"name\":\"units.list\",\"arguments\":{\"type\":\"armlab\"}}"
            )
        )) {
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* units = body->find("units");
            OA_CHECK(units != nullptr && unit_ids(*units).size() == 1);
            if (units != nullptr && !units->elements().empty()) {
                const json::Json* id = units->elements().front().find("id");
                OA_CHECK(id != nullptr && id->integer() && *id->integer() == 3);
            }
        }
    }
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "4",
                "tools/call",
                "{\"name\":\"units.list\",\"arguments\":{\"x\":100,\"z\":80,\"radius\":10}}"
            )
        )) {
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* units = body->find("units");
            OA_CHECK(units != nullptr && unit_ids(*units).size() == 1);
        }
    }
    if (const std::optional<json::Json> answer = only(
            session, request("5", "tools/call", "{\"name\":\"units.list\",\"arguments\":{\"x\":1}}")
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(text->find("area") != std::string::npos);
    }

    picture.running = false;
    if (const std::optional<json::Json> answer =
            only(session, request("6", "tools/call", "{\"name\":\"units.list\"}"))) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "no match is running");
    }
}

/// A factory queue, and the refusal of a hidden unit's orders.
void check_orders() {
    Picture picture;
    picture.units.push_back(make_unit(1, "ARMLAB", 0, true, 100, 80));
    picture.units.push_back(make_unit(2, "CORCOM", 1, false, 900, 900));
    oa::app::OrderRecord order;
    order.kind = "BuildingBuild";
    order.parameter_1 = 4;
    order.parameter_2 = 3;
    order.build = "ARMPW";
    picture.queues.primary.push_back(order);
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer = only(
            session,
            request("2", "tools/call", "{\"name\":\"units.orders\",\"arguments\":{\"id\":1}}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* primary = body->find("primary");
            OA_CHECK(primary != nullptr && primary->elements().size() == 1);
            if (primary != nullptr && !primary->elements().empty()) {
                const json::Json& queued = primary->elements().front();
                const json::Json* kind = queued.find("kind");
                const json::Json* count = queued.find("parameter_2");
                const json::Json* build = queued.find("build");
                OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "BuildingBuild");
                OA_CHECK(count != nullptr && count->integer() && *count->integer() == 3);
                OA_CHECK(build != nullptr && build->string() && *build->string() == "ARMPW");
            }
            const json::Json* secondary = body->find("secondary");
            OA_CHECK(secondary != nullptr && secondary->elements().empty());
        }
    }
    if (const std::optional<json::Json> answer = only(
            session,
            request("3", "tools/call", "{\"name\":\"units.orders\",\"arguments\":{\"id\":2}}")
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(*text == "the unit is not in view");
    }
}

/// A type's build options, and observe mode listing the unit tools.
void check_types() {
    Picture picture;
    oa::app::TypeRecord lab;
    lab.index = 3;
    lab.type = "ARMLAB";
    lab.name = "Kbot Lab";
    lab.side = "ARM";
    lab.builds = {"ARMPW", "ARMCK"};
    lab.speed = 0;
    lab.sight = 400;
    lab.radar = 0;
    picture.types.push_back(lab);
    oa::app::TypeRecord commander;
    commander.index = 1;
    commander.type = "ARMCOM";
    commander.name = "Commander";
    picture.types.push_back(commander);
    mcp::UnitHost host;
    bind_picture(picture, host);
    mcp::Session session = open_session(host);

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "2", "tools/call", "{\"name\":\"units.types\",\"arguments\":{\"type\":\"armlab\"}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* types = body->find("types");
            OA_CHECK(types != nullptr && types->elements().size() == 1);
            if (types != nullptr && !types->elements().empty()) {
                const json::Json& record = types->elements().front();
                const json::Json* builds = record.find("builds");
                OA_CHECK(builds != nullptr && builds->elements().size() == 2);
                if (builds != nullptr && !builds->elements().empty()) {
                    const json::Json& first = builds->elements().front();
                    OA_CHECK(first.string() && *first.string() == "ARMPW");
                }
                const json::Json* sight = record.find("sight");
                OA_CHECK(sight != nullptr && sight->integer() && *sight->integer() == 400);
            }
        }
    }

    mcp::UnitHost observed_host;
    bind_picture(picture, observed_host);
    mcp::Session observed = open_session(observed_host, true);
    if (const std::optional<json::Json> answer = only(observed, request("2", "tools/list"))) {
        const json::Json* result = answer->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        bool listed = false;
        if (tools != nullptr) {
            for (const json::Json& tool : tools->elements()) {
                const json::Json* name = tool.find("name");
                if (name != nullptr && name->string() && *name->string() == "units.list")
                    listed = true;
            }
        }
        OA_CHECK(listed);
    }
}

} // namespace

int main() {
    check_visibility();
    check_filters();
    check_orders();
    check_types();
    return oa::test::check_exit_status();
}
