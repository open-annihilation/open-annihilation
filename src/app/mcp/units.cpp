// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// units.list, units.get, units.orders and units.types (units.hpp). The
// match view copies every live unit and says which ones the local player
// is shown. These tools are what a caller receives: a unit that is not
// shown is left out, and reading it fails, unless the call asks for full
// awareness and the match is not shared.
#include "units.hpp"

#include "oa/formats/json.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

constexpr std::string_view kListSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"owner\":{\"type\":\"string\"},"
    "\"type\":{\"type\":\"string\"},"
    "\"x\":{\"type\":\"integer\"},"
    "\"z\":{\"type\":\"integer\"},"
    "\"radius\":{\"type\":\"integer\",\"minimum\":0},"
    "\"awareness\":{\"type\":\"string\",\"enum\":[\"player\",\"full\"]}}}";

constexpr std::string_view kUnitSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"id\"],"
    "\"properties\":{\"id\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":65535},"
    "\"awareness\":{\"type\":\"string\",\"enum\":[\"player\",\"full\"]}}}";

constexpr std::string_view kTypesSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"type\":{\"type\":\"string\"}}}";

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

/// A finished tool answer.
///
/// @param text the JSON object
/// @return the answer
ToolReply ok(std::string text) {
    ToolReply reply;
    reply.text = std::move(text);
    return reply;
}

/// The running match, as the host published it for this call.
struct Bound {
    const oa::app::MatchView* table{}; ///< the published view
    uint32_t tick{};                   ///< the match's tick
    uint8_t viewer{};                  ///< the local player
    bool full_allowed{};               ///< full awareness is allowed
};

/// Binds a call to the published view.
///
/// @param host the view
/// @param[out] bound the binding
/// @return the failure, or empty when a match is running
std::string bind_match(const UnitHost* host, Bound& bound) {
    if (host == nullptr || host->table.active == nullptr ||
        !host->table.active(host->table.context))
        return "no match is running";
    bound.table = &host->table;
    if (host->table.tick != nullptr)
        bound.tick = host->table.tick(host->table.context);
    if (host->table.viewer != nullptr)
        bound.viewer = host->table.viewer(host->table.context);
    if (host->table.full_awareness_allowed != nullptr)
        bound.full_allowed = host->table.full_awareness_allowed(host->table.context);
    return {};
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

/// Folds one ASCII letter.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_ascii(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Compares two type names without regard to ASCII case.
///
/// @param first one name
/// @param second the other
/// @return true when they are the same name
bool same_type(std::string_view first, std::string_view second) {
    if (first.size() != second.size())
        return false;
    for (std::size_t cursor = 0; cursor < first.size(); ++cursor) {
        const unsigned char one = fold_ascii(static_cast<unsigned char>(first[cursor]));
        const unsigned char other = fold_ascii(static_cast<unsigned char>(second[cursor]));
        if (one != other)
            return false;
    }
    return true;
}

/// Reads a whole number that fits in 32 bits.
///
/// @param value the JSON number
/// @return the number, or nothing when it does not fit
std::optional<int32_t> fit_int32(const json::Json& value) {
    const std::optional<int64_t> number = value.integer();
    if (!number)
        return std::nullopt;
    if (*number < std::numeric_limits<int32_t>::min() ||
        *number > std::numeric_limits<int32_t>::max())
        return std::nullopt;
    return static_cast<int32_t>(*number);
}

/// A units.list filter.
struct ListFilter {
    bool any_owner = true;        ///< no owner filter
    bool owner_is_viewer = false; ///< owner "me"
    uint8_t owner{};              ///< a numbered owner
    bool type_set = false;        ///< a type name was given
    std::string type_name;        ///< the type name
    bool area = false;            ///< x, z and radius were given
    int32_t area_x{};             ///< the area's centre, map pixel
    int32_t area_z{};             ///< map pixel
    int64_t radius{};             ///< the area's radius, map pixels
    bool full = false;            ///< include units the player is not shown
};

/// The furthest a coordinate may be before its square no longer fits in 64 bits.
constexpr int64_t kSafeOffset = 3037000499LL;

/// Tells whether a unit lies in the area, the edge included.
///
/// @param unit the unit
/// @param filter the area
/// @return true when it lies inside
bool inside_area(const oa::app::UnitRecord& unit, const ListFilter& filter) {
    const int64_t dx = static_cast<int64_t>(unit.x) - static_cast<int64_t>(filter.area_x);
    const int64_t dz = static_cast<int64_t>(unit.z) - static_cast<int64_t>(filter.area_z);
    if (dx > kSafeOffset || dx < -kSafeOffset || dz > kSafeOffset || dz < -kSafeOffset)
        return false;
    return dx * dx + dz * dz <= filter.radius * filter.radius;
}

/// Reads units.list's filter.
///
/// @param args the arguments
/// @param allowed whether the match allows full awareness
/// @param[out] filter the filter
/// @return the failure, or empty
std::string read_filter(const json::Json& args, bool allowed, ListFilter& filter) {
    if (const std::string why = awareness_failure(args, allowed, filter.full); !why.empty())
        return why;
    if (const json::Json* owner = args.find("owner")) {
        const std::string* text = owner->string();
        if (text == nullptr)
            return "owner must be me, all or a player from 0 to 9";
        if (same_type(*text, "all") || text->empty()) {
            filter.any_owner = true;
        } else if (same_type(*text, "me")) {
            filter.any_owner = false;
            filter.owner_is_viewer = true;
        } else if (text->size() == 1 && (*text)[0] >= '0' && (*text)[0] <= '9') {
            filter.any_owner = false;
            filter.owner = static_cast<uint8_t>((*text)[0] - '0');
        } else {
            return "owner must be me, all or a player from 0 to 9";
        }
    }
    if (const json::Json* kind = args.find("type")) {
        const std::string* text = kind->string();
        if (text == nullptr)
            return "type must be a unit name";
        if (!text->empty()) {
            filter.type_set = true;
            filter.type_name = *text;
        }
    }
    const json::Json* at_x = args.find("x");
    const json::Json* at_z = args.find("z");
    const json::Json* reach = args.find("radius");
    const int present =
        (at_x != nullptr ? 1 : 0) + (at_z != nullptr ? 1 : 0) + (reach != nullptr ? 1 : 0);
    if (present == 0)
        return {};
    if (present != 3)
        return "area needs x, z and radius together";
    const std::optional<int32_t> x = fit_int32(*at_x);
    const std::optional<int32_t> z = fit_int32(*at_z);
    const std::optional<int32_t> radius = fit_int32(*reach);
    if (!x || !z || !radius || *radius < 0)
        return "area needs x, z and radius in map pixels";
    filter.area = true;
    filter.area_x = *x;
    filter.area_z = *z;
    filter.radius = *radius;
    return {};
}

/// Tells whether a unit passes the filter. Visibility is separate.
///
/// @param unit the unit
/// @param filter the filter
/// @param viewer the local player
/// @return true when it passes
bool passes(const oa::app::UnitRecord& unit, const ListFilter& filter, uint8_t viewer) {
    if (!filter.any_owner) {
        const uint8_t wanted = filter.owner_is_viewer ? viewer : filter.owner;
        if (unit.owner != wanted)
            return false;
    }
    if (filter.type_set && !same_type(unit.type, filter.type_name))
        return false;
    if (filter.area && !inside_area(unit, filter))
        return false;
    return true;
}

/// Writes one unit.
///
/// @param[in,out] writer the JSON being written
/// @param unit the unit
void write_unit(json::JsonWriter& writer, const oa::app::UnitRecord& unit) {
    writer.begin_object();
    writer.key("id");
    writer.integer(unit.id);
    writer.key("type");
    writer.string(unit.type);
    writer.key("name");
    writer.string(unit.name);
    writer.key("side");
    writer.string(unit.side);
    writer.key("owner");
    writer.integer(unit.owner);
    writer.key("position");
    writer.begin_array();
    writer.integer(unit.x);
    writer.integer(unit.y);
    writer.integer(unit.z);
    writer.end_array();
    writer.key("health");
    writer.integer(unit.health);
    writer.key("build");
    writer.integer(unit.build);
    writer.key("flags");
    writer.begin_object();
    writer.key("selected");
    writer.boolean(unit.selected);
    writer.key("radar");
    writer.boolean(unit.radar);
    writer.key("cloaked");
    writer.boolean(unit.cloaked);
    writer.end_object();
    writer.key("order");
    writer.string(unit.order);
    writer.key("shown");
    writer.boolean(unit.shown);
    writer.end_object();
}

/// Writes one order.
///
/// @param[in,out] writer the JSON being written
/// @param order the order
void write_order(json::JsonWriter& writer, const oa::app::OrderRecord& order) {
    writer.begin_object();
    writer.key("kind");
    writer.string(order.kind);
    writer.key("target");
    writer.integer(order.target);
    writer.key("point");
    writer.begin_array();
    writer.integer(order.x);
    writer.integer(order.y);
    writer.integer(order.z);
    writer.end_array();
    writer.key("parameter_1");
    writer.integer(order.parameter_1);
    writer.key("parameter_2");
    writer.integer(order.parameter_2);
    writer.key("phase");
    writer.integer(order.phase);
    writer.key("issue_tick");
    writer.integer(static_cast<int64_t>(order.issue_tick));
    writer.key("facing");
    writer.integer(order.facing);
    writer.key("build_started");
    writer.boolean(order.build_started);
    writer.key("seen_x");
    writer.integer(order.seen_x);
    writer.key("seen_z");
    writer.integer(order.seen_z);
    writer.key("build");
    writer.string(order.build);
    writer.end_object();
}

/// Writes a queue.
///
/// @param[in,out] writer the JSON being written
/// @param name the member's name
/// @param queue the orders
void write_queue(
    json::JsonWriter& writer, std::string_view name, const std::vector<oa::app::OrderRecord>& queue
) {
    writer.key(name);
    writer.begin_array();
    for (const oa::app::OrderRecord& order : queue)
        write_order(writer, order);
    writer.end_array();
}

/// Copies the live units.
///
/// @param bound the running match
/// @return the units; empty when the view has no units entry
std::vector<oa::app::UnitRecord> copy_units(const Bound& bound) {
    std::vector<oa::app::UnitRecord> listed;
    if (bound.table->units != nullptr)
        bound.table->units(bound.table->context, &listed);
    return listed;
}

/// Finds a unit by its slot.
///
/// @param listed the live units
/// @param id the slot
/// @return the unit, or null
const oa::app::UnitRecord* find_unit(const std::vector<oa::app::UnitRecord>& listed, uint16_t id) {
    for (const oa::app::UnitRecord& unit : listed)
        if (unit.id == id)
            return &unit;
    return nullptr;
}

/// The arguments, or a failure when the call has none.
///
/// @param call the call
/// @param[out] args the arguments
/// @return the failure, or empty
std::string arguments_of(const ToolCall& call, const json::Json*& args) {
    if (call.arguments == nullptr)
        return "arguments must be an object";
    args = call.arguments;
    return {};
}

/// Answers units.list.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the units the caller may see
ToolReply list_units(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->units == nullptr)
        return fail("no match is running");
    ListFilter filter;
    if (const std::string why = read_filter(*args, bound.full_allowed, filter); !why.empty())
        return fail(why);
    const std::vector<oa::app::UnitRecord> listed = copy_units(bound);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("units");
    writer.begin_array();
    for (const oa::app::UnitRecord& unit : listed) {
        // A unit the player is not shown stays out, unless this call's full
        // awareness was allowed above.
        if (!unit.shown && !filter.full)
            continue;
        if (!passes(unit, filter, bound.viewer))
            continue;
        write_unit(writer, unit);
    }
    writer.end_array();
    writer.end_object();
    return ok(writer.text());
}

/// Reads the call's unit id.
///
/// @param args the arguments
/// @param[out] id the slot
/// @return the failure, or empty
std::string read_id(const json::Json& args, uint16_t& id) {
    const json::Json* value = args.find("id");
    if (value == nullptr || !value->integer())
        return "missing id";
    const int64_t raw = *value->integer();
    if (raw < 0 || raw > 65535)
        return "no such unit";
    id = static_cast<uint16_t>(raw);
    return {};
}

/// Finds a unit the call may read.
///
/// @param listed the live units
/// @param id the slot
/// @param full whether the call may read a unit it is not shown
/// @param[out] unit the unit, when it may be read
/// @return the failure, or empty
std::string visible_unit(
    const std::vector<oa::app::UnitRecord>& listed,
    uint16_t id,
    bool full,
    const oa::app::UnitRecord*& unit
) {
    unit = find_unit(listed, id);
    if (unit == nullptr)
        return "no such unit";
    if (!unit->shown && !full)
        return "the unit is not in view";
    return {};
}

/// Answers units.get.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the unit
ToolReply get_unit(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->units == nullptr)
        return fail("no match is running");
    bool full = false;
    if (const std::string why = awareness_failure(*args, bound.full_allowed, full); !why.empty())
        return fail(why);
    uint16_t id = 0;
    if (const std::string why = read_id(*args, id); !why.empty())
        return fail(why);
    const std::vector<oa::app::UnitRecord> listed = copy_units(bound);
    const oa::app::UnitRecord* unit = nullptr;
    if (const std::string why = visible_unit(listed, id, full, unit); !why.empty())
        return fail(why);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("unit");
    write_unit(writer, *unit);
    writer.end_object();
    return ok(writer.text());
}

/// Answers units.orders.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the two queues
ToolReply read_orders(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->units == nullptr || bound.table->orders == nullptr)
        return fail("no match is running");
    bool full = false;
    if (const std::string why = awareness_failure(*args, bound.full_allowed, full); !why.empty())
        return fail(why);
    uint16_t id = 0;
    if (const std::string why = read_id(*args, id); !why.empty())
        return fail(why);
    const std::vector<oa::app::UnitRecord> listed = copy_units(bound);
    const oa::app::UnitRecord* unit = nullptr;
    if (const std::string why = visible_unit(listed, id, full, unit); !why.empty())
        return fail(why);
    oa::app::OrderQueues queues;
    if (!bound.table->orders(bound.table->context, id, &queues))
        return fail("no such unit");
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("id");
    writer.integer(id);
    write_queue(writer, "primary", queues.primary);
    write_queue(writer, "secondary", queues.secondary);
    writer.key("primary_truncated");
    writer.boolean(queues.primary_truncated);
    writer.key("secondary_truncated");
    writer.boolean(queues.secondary_truncated);
    writer.end_object();
    return ok(writer.text());
}

/// Writes one type.
///
/// @param[in,out] writer the JSON being written
/// @param record the type
void write_type(json::JsonWriter& writer, const oa::app::TypeRecord& record) {
    writer.begin_object();
    writer.key("index");
    writer.integer(record.index);
    writer.key("type");
    writer.string(record.type);
    writer.key("name");
    writer.string(record.name);
    writer.key("description");
    writer.string(record.description);
    writer.key("side");
    writer.string(record.side);
    writer.key("builds");
    writer.begin_array();
    for (const std::string& built : record.builds)
        writer.string(built);
    writer.end_array();
    writer.key("weapons");
    writer.begin_array();
    for (const std::string& weapon : record.weapons)
        writer.string(weapon);
    writer.end_array();
    writer.key("speed");
    writer.integer(record.speed);
    writer.key("sight");
    writer.integer(record.sight);
    writer.key("radar");
    writer.integer(record.radar);
    writer.key("sonar");
    writer.integer(record.sonar);
    writer.end_object();
}

/// Answers units.types.
///
/// @param context the UnitHost
/// @param call the arguments
/// @return the types
ToolReply list_types(void* context, const ToolCall& call) {
    const json::Json* args = nullptr;
    if (const std::string why = arguments_of(call, args); !why.empty())
        return fail(why);
    Bound bound;
    if (const std::string why = bind_match(static_cast<const UnitHost*>(context), bound);
        !why.empty())
        return fail(why);
    if (bound.table->types == nullptr)
        return fail("no match is running");
    std::string wanted;
    if (const json::Json* kind = args->find("type")) {
        const std::string* text = kind->string();
        if (text == nullptr)
            return fail("type must be a unit name");
        wanted = *text;
    }
    std::vector<oa::app::TypeRecord> catalog;
    bound.table->types(bound.table->context, &catalog);
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(bound.tick));
    writer.key("types");
    writer.begin_array();
    for (const oa::app::TypeRecord& record : catalog) {
        if (!wanted.empty() && !same_type(record.type, wanted))
            continue;
        write_type(writer, record);
    }
    writer.end_array();
    writer.end_object();
    return ok(writer.text());
}

} // namespace

void add_unit_tools(Session& session, UnitHost& host) {
    Tool list;
    list.name = "units.list";
    list.description =
        "Lists the units the player is shown: id, type, name, side, owner, position, "
        "health, build progress, flags and the current order. owner is me, all or a "
        "player from 0 to 9. type is a unit name. An area is x, z and radius in map "
        "pixels. awareness full, in a single-player match, includes units the player "
        "is not shown.";
    list.input_schema = std::string(kListSchema);
    list.reads = true;
    list.context = &host;
    list.call = list_units;
    session.add_tool(std::move(list));

    Tool one;
    one.name = "units.get";
    one.description = "Reads one unit the player is shown. A unit that is not shown is "
                      "refused, unless awareness is full and the match is single-player.";
    one.input_schema = std::string(kUnitSchema);
    one.reads = true;
    one.context = &host;
    one.call = get_unit;
    session.add_tool(std::move(one));

    Tool queues;
    queues.name = "units.orders";
    queues.description =
        "Reads the primary and secondary order queues of a unit the player is shown. "
        "A build order names its product and how many remain.";
    queues.input_schema = std::string(kUnitSchema);
    queues.reads = true;
    queues.context = &host;
    queues.call = read_orders;
    session.add_tool(std::move(queues));

    Tool types;
    types.name = "units.types";
    types.description =
        "Reads the unit types: build options, weapons, maximum speed as a 16.16 fixed "
        "value, and sight, radar and sonar distances. type limits the list to one unit "
        "name. Types are the catalogue, not the units on the field.";
    types.input_schema = std::string(kTypesSchema);
    types.reads = true;
    types.context = &host;
    types.call = list_types;
    session.add_tool(std::move(types));
}

} // namespace oa::app::mcp
