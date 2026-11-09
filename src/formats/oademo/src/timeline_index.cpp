// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oademo/timeline_index.hpp"

#include "oa/formats/json.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::formats::oademo {
namespace {

namespace json = oa::formats::json;

/// A marker kind and the word it is written as.
struct KindName {
    MarkerKind kind{};
    std::string_view name{};
};

/// A battle event and the word it is written as.
struct BattleName {
    BattleKind kind{};
    std::string_view name{};
};

/// Every battle event, in the order of BattleKind.
constexpr std::array<BattleName, 5> battle_kind_names{{
    {BattleKind::unit_lost, "unit_lost"},
    {BattleKind::building_lost, "building_lost"},
    {BattleKind::engagement, "engagement"},
    {BattleKind::commander, "commander"},
    {BattleKind::commander_lost, "commander_lost"},
}};

/// Every kind, in the order of MarkerKind.
constexpr std::array<KindName, 9> marker_kind_names{{
    {MarkerKind::start, "start"},
    {MarkerKind::end, "end"},
    {MarkerKind::commander_death, "commander_death"},
    {MarkerKind::player_left, "player_left"},
    {MarkerKind::player_dropped, "player_dropped"},
    {MarkerKind::player_defeated, "player_defeated"},
    {MarkerKind::alliance, "alliance"},
    {MarkerKind::pause, "pause"},
    {MarkerKind::chat, "chat"},
}};

/// Returns the last member of an object with a name.
///
/// @param object a JSON value
/// @param name the member's name
/// @return the member, or null when the value is not an object or lacks it
const json::Json* member(const json::Json& object, std::string_view name) noexcept {
    const json::Json* found = nullptr;
    if (object.type() != json::JsonType::object)
        return nullptr;
    const auto names = object.names();
    const auto values = object.values();
    for (std::size_t index = 0; index < names.size(); ++index)
        if (names[index] == name)
            found = &values[index];
    return found;
}

/// Reads a whole number of at least zero that fits 32 bits.
///
/// @param value the member, or null
/// @param[out] out the number
/// @return false when the member is missing or is not such a number
bool take_u32(const json::Json* value, uint32_t& out) noexcept {
    if (value == nullptr)
        return false;
    const std::optional<int64_t> number = value->integer();
    if (!number.has_value() || *number < 0 || *number > std::numeric_limits<uint32_t>::max())
        return false;
    out = static_cast<uint32_t>(*number);
    return true;
}

/// Reads a whole number that fits 32 bits, signed.
///
/// @param value the member, or null
/// @param[out] out the number
/// @return false when the member is missing or is not such a number
bool take_i32(const json::Json* value, int32_t& out) noexcept {
    if (value == nullptr)
        return false;
    const std::optional<int64_t> number = value->integer();
    if (!number.has_value() || *number < std::numeric_limits<int32_t>::min() ||
        *number > std::numeric_limits<int32_t>::max())
        return false;
    out = static_cast<int32_t>(*number);
    return true;
}

/// Reads a player number.
///
/// @param value the member, or null
/// @param[out] out the number, 0 to 255
/// @return false when the member is missing or is not such a number
bool take_player(const json::Json* value, int& out) noexcept {
    uint32_t number = 0;
    if (!take_u32(value, number) || number > 255)
        return false;
    out = static_cast<int>(number);
    return true;
}

/// Reads a boolean.
///
/// @param value the member, or null
/// @param[out] out the value
/// @return false when the member is missing or is not a boolean
bool take_bool(const json::Json* value, bool& out) noexcept {
    if (value == nullptr)
        return false;
    const std::optional<bool> flag = value->boolean();
    if (!flag.has_value())
        return false;
    out = *flag;
    return true;
}

/// Reads a string.
///
/// @param value the member, or null
/// @param[out] out the text
/// @return false when the member is missing or is not a string
bool take_string(const json::Json* value, std::string& out) {
    if (value == nullptr || value->string() == nullptr)
        return false;
    out = *value->string();
    return true;
}

/// Reads an array of whole numbers.
///
/// @tparam Number the element type, uint32_t or int32_t
/// @param value the member, or null
/// @param[out] out the numbers
/// @return false when the member is missing or holds anything else
template <typename Number>
bool take_numbers(const json::Json* value, std::vector<Number>& out) {
    if (value == nullptr || value->type() != json::JsonType::array)
        return false;
    out.clear();
    out.reserve(value->elements().size());
    for (const json::Json& element : value->elements()) {
        Number number{};
        if constexpr (std::is_signed_v<Number>) {
            if (!take_i32(&element, number))
                return false;
        } else {
            if (!take_u32(&element, number))
                return false;
        }
        out.push_back(number);
    }
    return true;
}

/// Reads the array of whole numbers `damage` holds, which may exceed 32 bits.
///
/// @param value the member, or null
/// @param[out] out the numbers
/// @return false when the member is missing or holds anything else
bool take_damage(const json::Json* value, std::vector<uint64_t>& out) {
    if (value == nullptr || value->type() != json::JsonType::array)
        return false;
    out.clear();
    out.reserve(value->elements().size());
    for (const json::Json& element : value->elements()) {
        const std::optional<int64_t> number = element.integer();
        if (!number.has_value() || *number < 0)
            return false;
        out.push_back(static_cast<uint64_t>(*number));
    }
    return true;
}

/// Writes a member holding an array of whole numbers.
///
/// @tparam Number the element type
/// @param[in,out] writer the JSON being written
/// @param name the member's name
/// @param numbers the numbers
template <typename Number>
void write_numbers(
    json::JsonWriter& writer, std::string_view name, const std::vector<Number>& numbers
) {
    writer.key(name);
    writer.begin_array();
    for (const Number number : numbers)
        writer.integer(static_cast<int64_t>(number));
    writer.end_array();
}

/// Writes the members of a marker beyond its tick and kind.
///
/// @param[in,out] writer the JSON being written
/// @param marker the marker
void write_marker_details(json::JsonWriter& writer, const IndexMarker& marker) {
    switch (marker.kind) {
    case MarkerKind::start:
    case MarkerKind::end:
        return;
    case MarkerKind::commander_death:
        writer.key("player");
        writer.integer(marker.player);
        if (marker.other != 0) {
            writer.key("by");
            writer.integer(marker.other);
        }
        return;
    case MarkerKind::player_left:
    case MarkerKind::player_dropped:
    case MarkerKind::player_defeated:
        writer.key("player");
        writer.integer(marker.player);
        return;
    case MarkerKind::alliance:
        writer.key("player");
        writer.integer(marker.player);
        writer.key("other");
        writer.integer(marker.other);
        writer.key("allied");
        writer.boolean(marker.flag);
        return;
    case MarkerKind::pause:
        writer.key("player");
        writer.integer(marker.player);
        writer.key("paused");
        writer.boolean(marker.flag);
        return;
    case MarkerKind::chat:
        writer.key("player");
        writer.integer(marker.player);
        writer.key("text");
        writer.string(marker.text);
        return;
    }
}

/// Writes the totals of one player at the end of the replay.
///
/// @param[in,out] writer the JSON being written
/// @param totals the totals
void write_final(json::JsonWriter& writer, const IndexTotals& totals) {
    writer.key("kills");
    writer.integer(totals.kills);
    writer.key("losses");
    writer.integer(totals.losses);
    writer.key("commanders_lost");
    writer.integer(totals.commanders_lost);
    writer.key("units");
    writer.integer(totals.units);
    writer.key("metal");
    writer.integer(totals.metal);
    writer.key("energy");
    writer.integer(totals.energy);
}

/// Reads the totals of one player at the end of the replay.
///
/// @param value the object
/// @param[out] totals the totals
/// @return false when a member is missing or wrong
bool parse_final(const json::Json* value, IndexTotals& totals) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return false;
    return take_u32(member(*value, "kills"), totals.kills) &&
           take_u32(member(*value, "losses"), totals.losses) &&
           take_u32(member(*value, "commanders_lost"), totals.commanders_lost) &&
           take_u32(member(*value, "units"), totals.units) &&
           take_i32(member(*value, "metal"), totals.metal) &&
           take_i32(member(*value, "energy"), totals.energy);
}

/// Reads a marker.
///
/// @param value the object
/// @param[out] marker the marker
/// @param[out] known false for a kind this version does not know
/// @return false when a member is missing or wrong
bool parse_marker(const json::Json& value, IndexMarker& marker, bool& known) {
    known = true;
    if (value.type() != json::JsonType::object)
        return false;
    std::string kind;
    if (!take_u32(member(value, "tick"), marker.tick) || !take_string(member(value, "kind"), kind))
        return false;
    known = false;
    for (const KindName& entry : marker_kind_names)
        if (entry.name == kind) {
            marker.kind = entry.kind;
            known = true;
        }
    if (!known)
        return true;
    switch (marker.kind) {
    case MarkerKind::start:
    case MarkerKind::end:
        return true;
    case MarkerKind::commander_death:
        if (!take_player(member(value, "player"), marker.player))
            return false;
        if (const json::Json* killer = member(value, "by"); killer != nullptr)
            return take_player(killer, marker.other);
        return true;
    case MarkerKind::player_left:
    case MarkerKind::player_dropped:
    case MarkerKind::player_defeated:
        return take_player(member(value, "player"), marker.player);
    case MarkerKind::alliance:
        return take_player(member(value, "player"), marker.player) &&
               take_player(member(value, "other"), marker.other) &&
               take_bool(member(value, "allied"), marker.flag);
    case MarkerKind::pause:
        return take_player(member(value, "player"), marker.player) &&
               take_bool(member(value, "paused"), marker.flag);
    case MarkerKind::chat:
        return take_player(member(value, "player"), marker.player) &&
               take_string(member(value, "text"), marker.text);
    }
    return false;
}

/// Reads an object of series.
///
/// @param value the `totals` object's `players` member
/// @param[out] series the series
/// @return false when a member is missing or wrong
bool parse_series(const json::Json& value, IndexSeries& series) {
    if (value.type() != json::JsonType::object)
        return false;
    return take_player(member(value, "player"), series.player) &&
           take_numbers(member(value, "kills"), series.kills) &&
           take_numbers(member(value, "losses"), series.losses) &&
           take_numbers(member(value, "commanders_lost"), series.commanders_lost) &&
           take_numbers(member(value, "units"), series.units) &&
           take_numbers(member(value, "metal"), series.metal) &&
           take_numbers(member(value, "energy"), series.energy);
}

/// Reads the word `ended.how` holds.
///
/// @param text the word
/// @param[out] end the end
/// @return false for a word that is none of the three
bool parse_end(std::string_view text, IndexEnd& end) noexcept {
    for (const IndexEnd candidate : {IndexEnd::finished, IndexEnd::left, IndexEnd::cut})
        if (index_end_name(candidate) == text) {
            end = candidate;
            return true;
        }
    return false;
}

/// Reads the word `result.by` holds.
///
/// @param text the word
/// @param[out] by how the result was found
/// @return false for a word that is none of the three
bool parse_result_by(std::string_view text, ResultBy& by) noexcept {
    for (const ResultBy candidate : {ResultBy::none, ResultBy::commanders, ResultBy::mission})
        if (result_by_name(candidate) == text) {
            by = candidate;
            return true;
        }
    return false;
}

/// Reads the verdict object.
///
/// @param value the object
/// @param[out] verdict the verdict
/// @return false when a member is missing or wrong
bool parse_verdict(const json::Json* value, IndexVerdict& verdict) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return false;
    return take_bool(member(*value, "finished"), verdict.finished) &&
           take_bool(member(*value, "clean"), verdict.clean) &&
           take_bool(member(*value, "paced"), verdict.paced) &&
           take_bool(member(*value, "content_differs"), verdict.content_differs) &&
           take_u32(member(*value, "errors"), verdict.errors) &&
           take_u32(member(*value, "first_error_tick"), verdict.first_error_tick) &&
           take_string(member(*value, "last_error"), verdict.last_error);
}

/// Copies a value as compact JSON.
///
/// @param[in,out] writer the JSON being written
/// @param value the value
void write_value(json::JsonWriter& writer, const json::Json& value) {
    switch (value.type()) {
    case json::JsonType::null:
        writer.null();
        return;
    case json::JsonType::boolean:
        writer.boolean(value.boolean().value_or(false));
        return;
    case json::JsonType::number:
        writer.raw(*value.number_text());
        return;
    case json::JsonType::string:
        writer.string(*value.string());
        return;
    case json::JsonType::array:
        writer.begin_array();
        for (const json::Json& element : value.elements())
            write_value(writer, element);
        writer.end_array();
        return;
    case json::JsonType::object: {
        writer.begin_object();
        const auto names = value.names();
        const auto values = value.values();
        for (std::size_t index = 0; index < names.size(); ++index) {
            writer.key(names[index]);
            write_value(writer, values[index]);
        }
        writer.end_object();
        return;
    }
    }
}

/// Tells whether a member name is one this version interprets.
///
/// @param name the name
/// @return true for a known member
bool known_member(std::string_view name) noexcept {
    for (const std::string_view known :
         {"oademo_timeline",
          "length",
          "ended",
          "verdict",
          "result",
          "players",
          "markers",
          "markers_omitted",
          "totals",
          "intensity",
          "events"})
        if (known == name)
            return true;
    return false;
}

/// Reads a battle event's kind.
///
/// @param text the word
/// @param[out] kind the kind
/// @return false when the word is not one this version knows
bool parse_battle_kind(std::string_view text, BattleKind& kind) noexcept {
    for (const BattleName& entry : battle_kind_names)
        if (entry.name == text) {
            kind = entry.kind;
            return true;
        }
    return false;
}

/// Writes one battle event as a compact array.
///
/// The tick, the kind and the place are always written. `player`, `metal`,
/// `health` and `amount` follow, and stop at the last one that is not 0.
///
/// @param writer the writer
/// @param event the event
void write_battle_event(json::JsonWriter& writer, const BattleEvent& event) {
    writer.begin_array();
    writer.integer(event.tick);
    writer.string(battle_kind_name(event.kind));
    writer.integer(event.x);
    writer.integer(event.z);
    const bool amount = event.amount != 0;
    const bool health = amount || event.health != 0;
    const bool metal = health || event.metal != 0;
    const bool player = metal || event.player != 0;
    if (player)
        writer.integer(event.player);
    if (metal)
        writer.integer(static_cast<int64_t>(event.metal));
    if (health)
        writer.integer(static_cast<int64_t>(event.health));
    if (amount)
        writer.integer(event.amount);
    writer.end_array();
}

/// Reads the optional tail of a battle-event array.
///
/// @param items the array, already known to hold the tick, kind and place
/// @param[in,out] event the event
/// @return false when a present tail value has the wrong type
bool take_battle_tail(std::span<const json::Json> items, BattleEvent& event) {
    if (items.size() > 4 && !take_player(&items[4], event.player))
        return false;
    if (items.size() > 5 && !take_u32(&items[5], event.metal))
        return false;
    if (items.size() > 6 && !take_u32(&items[6], event.health))
        return false;
    if (items.size() > 7 && !take_i32(&items[7], event.amount))
        return false;
    return true;
}

/// Reads one battle event from the compact array the writer emits.
///
/// @param element the event's array
/// @param[out] event the event
/// @param[out] known false when the kind is one this version does not know
/// @return false when the array is not one, or a known event holds a wrong value
bool parse_battle_array(const json::Json& element, BattleEvent& event, bool& known) {
    known = false;
    if (element.type() != json::JsonType::array)
        return false;
    const std::span<const json::Json> items = element.elements();
    if (items.size() < 4 || items.size() > 8)
        return false;
    std::string kind;
    if (!take_u32(&items[0], event.tick) || !take_string(&items[1], kind) ||
        !take_i32(&items[2], event.x) || !take_i32(&items[3], event.z))
        return false;
    if (!parse_battle_kind(kind, event.kind))
        return true;
    known = true;
    return take_battle_tail(items, event);
}

/// Reads one battle event from the object form.
///
/// @param element the event's object
/// @param[out] event the event
/// @param[out] known false when the kind is one this version does not know
/// @return false when the object is missing a member or holds a wrong one
bool parse_battle_object(const json::Json& element, BattleEvent& event, bool& known) {
    known = false;
    if (element.type() != json::JsonType::object)
        return false;
    std::string kind;
    if (!take_string(member(element, "kind"), kind))
        return false;
    if (!parse_battle_kind(kind, event.kind))
        return true;
    known = true;
    if (!take_u32(member(element, "tick"), event.tick) ||
        !take_i32(member(element, "x"), event.x) || !take_i32(member(element, "z"), event.z))
        return false;
    if (const json::Json* player = member(element, "player");
        player != nullptr && !take_player(player, event.player))
        return false;
    if (const json::Json* metal = member(element, "metal");
        metal != nullptr && !take_u32(metal, event.metal))
        return false;
    if (const json::Json* health = member(element, "health");
        health != nullptr && !take_u32(health, event.health))
        return false;
    if (const json::Json* amount = member(element, "amount");
        amount != nullptr && !take_i32(amount, event.amount))
        return false;
    return true;
}

/// Reads one battle event, as an array or as an object.
///
/// @param element the event
/// @param[out] event the event
/// @param[out] known false when the kind is one this version does not know
/// @return false when the value is neither, or a known event holds a wrong one
bool parse_battle_event(const json::Json& element, BattleEvent& event, bool& known) {
    if (element.type() == json::JsonType::array)
        return parse_battle_array(element, event, known);
    if (element.type() == json::JsonType::object)
        return parse_battle_object(element, event, known);
    known = false;
    return false;
}

} // namespace

std::string_view index_end_name(IndexEnd end) noexcept {
    switch (end) {
    case IndexEnd::finished:
        return "finished";
    case IndexEnd::left:
        return "left";
    case IndexEnd::cut:
        return "cut";
    }
    return "cut";
}

std::string_view marker_kind_name(MarkerKind kind) noexcept {
    for (const KindName& entry : marker_kind_names)
        if (entry.kind == kind)
            return entry.name;
    return "start";
}

std::string_view battle_kind_name(BattleKind kind) noexcept {
    for (const BattleName& entry : battle_kind_names)
        if (entry.kind == kind)
            return entry.name;
    return "unit_lost";
}

std::string_view result_by_name(ResultBy by) noexcept {
    switch (by) {
    case ResultBy::none:
        return "none";
    case ResultBy::commanders:
        return "commanders";
    case ResultBy::mission:
        return "mission";
    }
    return "none";
}

std::string timeline_index_text(const TimelineIndex& index) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("oademo_timeline");
    writer.integer(timeline_version);
    writer.key("length");
    writer.begin_object();
    writer.key("ticks");
    writer.integer(index.length_ticks);
    if (index.length_ms.has_value()) {
        writer.key("ms");
        writer.integer(static_cast<int64_t>(*index.length_ms));
    }
    writer.end_object();
    writer.key("ended");
    writer.begin_object();
    writer.key("how");
    writer.string(index_end_name(index.ended));
    writer.key("tick");
    writer.integer(index.ended_tick);
    writer.end_object();
    writer.key("verdict");
    writer.begin_object();
    writer.key("finished");
    writer.boolean(index.verdict.finished);
    writer.key("clean");
    writer.boolean(index.verdict.clean);
    writer.key("paced");
    writer.boolean(index.verdict.paced);
    writer.key("content_differs");
    writer.boolean(index.verdict.content_differs);
    writer.key("errors");
    writer.integer(index.verdict.errors);
    writer.key("first_error_tick");
    writer.integer(index.verdict.first_error_tick);
    writer.key("last_error");
    writer.string(index.verdict.last_error);
    writer.end_object();
    writer.key("result");
    writer.begin_object();
    writer.key("winners");
    writer.begin_array();
    for (const int winner : index.result.winners)
        writer.integer(winner);
    writer.end_array();
    writer.key("by");
    writer.string(result_by_name(index.result.by));
    writer.end_object();
    writer.key("players");
    writer.begin_array();
    for (const IndexPlayer& player : index.players) {
        writer.begin_object();
        writer.key("number");
        writer.integer(player.number);
        writer.key("name");
        writer.string(player.name);
        writer.key("watcher");
        writer.boolean(player.watcher);
        writer.key("final");
        writer.begin_object();
        write_final(writer, player.final);
        writer.end_object();
        writer.end_object();
    }
    writer.end_array();
    writer.key("markers");
    writer.begin_array();
    for (const IndexMarker& marker : index.markers) {
        writer.begin_object();
        writer.key("tick");
        writer.integer(marker.tick);
        writer.key("kind");
        writer.string(marker_kind_name(marker.kind));
        write_marker_details(writer, marker);
        writer.end_object();
    }
    writer.end_array();
    if (index.markers_omitted != 0) {
        writer.key("markers_omitted");
        writer.integer(index.markers_omitted);
    }
    writer.key("totals");
    writer.begin_object();
    writer.key("from_tick");
    writer.integer(index.first_tick);
    writer.key("period_ticks");
    writer.integer(index.period_ticks);
    writer.key("players");
    writer.begin_array();
    for (const IndexSeries& series : index.totals) {
        writer.begin_object();
        writer.key("player");
        writer.integer(series.player);
        write_numbers(writer, "kills", series.kills);
        write_numbers(writer, "losses", series.losses);
        write_numbers(writer, "commanders_lost", series.commanders_lost);
        write_numbers(writer, "units", series.units);
        write_numbers(writer, "metal", series.metal);
        write_numbers(writer, "energy", series.energy);
        writer.end_object();
    }
    writer.end_array();
    writer.end_object();
    writer.key("intensity");
    writer.begin_object();
    writer.key("from_tick");
    writer.integer(index.first_tick);
    writer.key("period_ticks");
    writer.integer(index.period_ticks);
    write_numbers(writer, "damage", index.damage);
    writer.end_object();
    writer.key("events");
    writer.begin_array();
    for (const BattleEvent& event : index.events)
        write_battle_event(writer, event);
    writer.end_array();
    for (const JsonExtra& extra : index.extra) {
        writer.key(extra.name);
        writer.raw(extra.json);
    }
    writer.end_object();
    return writer.text();
}

std::optional<TimelineIndex> parse_timeline_index(std::string_view text) {
    json::JsonError problem;
    const std::optional<json::Json> parsed = json::parse_json(text, problem);
    if (!parsed.has_value() || parsed->type() != json::JsonType::object)
        return std::nullopt;
    const json::Json& root = *parsed;
    TimelineIndex index;
    const json::Json* version = member(root, "oademo_timeline");
    const std::optional<int64_t> version_number =
        version != nullptr ? version->integer() : std::nullopt;
    // Version 2 and later are refused. Version 1 is this writer and every
    // timeline from before battle events were a member.
    if (!version_number.has_value() || *version_number != timeline_version)
        return std::nullopt;
    index.version = static_cast<int>(*version_number);

    const json::Json* length = member(root, "length");
    if (length == nullptr || !take_u32(member(*length, "ticks"), index.length_ticks))
        return std::nullopt;
    if (const json::Json* milliseconds = member(*length, "ms"); milliseconds != nullptr) {
        const std::optional<int64_t> number = milliseconds->integer();
        if (!number.has_value() || *number < 0)
            return std::nullopt;
        index.length_ms = static_cast<uint64_t>(*number);
    }

    const json::Json* ended = member(root, "ended");
    std::string how;
    if (ended == nullptr || !take_string(member(*ended, "how"), how) ||
        !parse_end(how, index.ended) || !take_u32(member(*ended, "tick"), index.ended_tick))
        return std::nullopt;

    if (!parse_verdict(member(root, "verdict"), index.verdict))
        return std::nullopt;

    const json::Json* result = member(root, "result");
    std::string by;
    if (result == nullptr || !take_string(member(*result, "by"), by) ||
        !parse_result_by(by, index.result.by))
        return std::nullopt;
    const json::Json* winners = member(*result, "winners");
    if (winners == nullptr || winners->type() != json::JsonType::array)
        return std::nullopt;
    for (const json::Json& winner : winners->elements()) {
        int number = 0;
        if (!take_player(&winner, number))
            return std::nullopt;
        index.result.winners.push_back(number);
    }

    const json::Json* players = member(root, "players");
    if (players == nullptr || players->type() != json::JsonType::array)
        return std::nullopt;
    for (const json::Json& element : players->elements()) {
        IndexPlayer player;
        if (element.type() != json::JsonType::object ||
            !take_player(member(element, "number"), player.number) ||
            !take_string(member(element, "name"), player.name) ||
            !take_bool(member(element, "watcher"), player.watcher) ||
            !parse_final(member(element, "final"), player.final))
            return std::nullopt;
        index.players.push_back(std::move(player));
    }

    const json::Json* markers = member(root, "markers");
    if (markers == nullptr || markers->type() != json::JsonType::array)
        return std::nullopt;
    for (const json::Json& element : markers->elements()) {
        IndexMarker marker;
        bool known = false;
        if (!parse_marker(element, marker, known))
            return std::nullopt;
        if (known)
            index.markers.push_back(std::move(marker));
    }
    if (const json::Json* omitted = member(root, "markers_omitted"); omitted != nullptr)
        if (!take_u32(omitted, index.markers_omitted))
            return std::nullopt;

    const json::Json* totals = member(root, "totals");
    const json::Json* intensity = member(root, "intensity");
    if (totals == nullptr || intensity == nullptr)
        return std::nullopt;
    if (!take_u32(member(*totals, "from_tick"), index.first_tick) ||
        !take_u32(member(*totals, "period_ticks"), index.period_ticks) || index.period_ticks == 0)
        return std::nullopt;
    const json::Json* series = member(*totals, "players");
    if (series == nullptr || series->type() != json::JsonType::array)
        return std::nullopt;
    for (const json::Json& element : series->elements()) {
        IndexSeries one;
        if (!parse_series(element, one))
            return std::nullopt;
        index.totals.push_back(std::move(one));
    }
    uint32_t intensity_from = 0;
    uint32_t intensity_period = 0;
    if (!take_u32(member(*intensity, "from_tick"), intensity_from) ||
        !take_u32(member(*intensity, "period_ticks"), intensity_period) ||
        intensity_from != index.first_tick || intensity_period != index.period_ticks ||
        !take_damage(member(*intensity, "damage"), index.damage))
        return std::nullopt;

    if (const json::Json* events = member(root, "events"); events != nullptr) {
        if (events->type() != json::JsonType::array)
            return std::nullopt;
        index.events_recorded = true;
        for (const json::Json& element : events->elements()) {
            BattleEvent event;
            bool known = false;
            if (!parse_battle_event(element, event, known))
                return std::nullopt;
            if (known)
                index.events.push_back(event);
        }
    }

    const auto names = root.names();
    const auto values = root.values();
    for (std::size_t at = 0; at < names.size(); ++at) {
        if (known_member(names[at]))
            continue;
        json::JsonWriter extra;
        write_value(extra, values[at]);
        index.extra.push_back(JsonExtra{names[at], extra.text()});
    }
    return index;
}

std::optional<int> timeline_prefix_version(std::string_view text) noexcept {
    constexpr std::string_view prefix = "{\"oademo_timeline\":";
    if (!text.starts_with(prefix))
        return std::nullopt;
    std::size_t at = prefix.size();
    if (at >= text.size() || text[at] < '0' || text[at] > '9')
        return std::nullopt;
    int version = 0;
    int digits = 0;
    while (at < text.size() && text[at] >= '0' && text[at] <= '9' && digits < 9) {
        version = version * 10 + (text[at] - '0');
        ++at;
        ++digits;
    }
    if (digits == 0 || at >= text.size() || text[at] != ',')
        return std::nullopt;
    return version;
}

bool timeline_wants_events(std::string_view text) noexcept {
    if (text.empty())
        return false;
    const std::optional<int> version = timeline_prefix_version(text);
    if (!version.has_value() || *version != timeline_version)
        return true;
    return text.find("\"events\":") == std::string_view::npos;
}

} // namespace oa::formats::oademo
