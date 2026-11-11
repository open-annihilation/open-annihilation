// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "detail.hpp"

#include "oa/formats/json.hpp"

#include <cstdint>
#include <initializer_list>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oademo {
namespace {

namespace json = oa::formats::json;

struct Member {
    std::string name{};
    const json::Json* value{nullptr};
};

class Members {
  public:

    explicit Members(const json::Json& object) {
        const auto names = object.names();
        const auto values = object.values();
        items_.reserve(names.size());
        for (std::size_t index = 0; index < names.size(); ++index)
            items_.push_back(Member{names[index], &values[index]});
    }

    /// Returns the last member of `name`, or null.
    ///
    /// @param name the member's name
    /// @return the member, or null
    [[nodiscard]] const json::Json* last(std::string_view name) const noexcept {
        const json::Json* found = nullptr;
        for (const Member& item : items_)
            if (item.name == name)
                found = item.value;
        return found;
    }

    /// Returns the members in the order read.
    ///
    /// @return the members
    [[nodiscard]] const std::vector<Member>& items() const noexcept { return items_; }

  private:

    std::vector<Member> items_{};
};

void append_json(json::JsonWriter& writer, const json::Json& value) {
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
            append_json(writer, element);
        writer.end_array();
        return;
    case json::JsonType::object:
        writer.begin_object();
        {
            const auto names = value.names();
            const auto values = value.values();
            for (std::size_t index = 0; index < names.size(); ++index) {
                writer.key(names[index]);
                append_json(writer, values[index]);
            }
        }
        writer.end_object();
        return;
    }
}

std::string compact_json(const json::Json& value) {
    json::JsonWriter writer;
    append_json(writer, value);
    return writer.text();
}

bool known_name(std::string_view name, std::initializer_list<std::string_view> names) noexcept {
    for (const std::string_view item : names)
        if (item == name)
            return true;
    return false;
}

void add_extra(std::vector<JsonExtra>& extras, std::string name, const json::Json& value) {
    extras.push_back(JsonExtra{std::move(name), compact_json(value)});
}

bool collect_extras(
    const Members& members,
    std::initializer_list<std::string_view> names,
    std::vector<JsonExtra>& extras,
    detail::Issue& issue
) {
    for (const Member& item : members.items()) {
        if (known_name(item.name, names))
            continue;
        add_extra(extras, item.name, *item.value);
    }
    (void)issue;
    return true;
}

bool fail_details(detail::Issue& issue) {
    issue = detail::Issue::details;
    return false;
}

bool take_string(const json::Json* value, std::string& out, bool required, detail::Issue& issue) {
    if (value == nullptr)
        return required ? fail_details(issue) : true;
    const std::string* text = value->string();
    if (text == nullptr)
        return fail_details(issue);
    out = *text;
    return true;
}

bool take_optional_string(
    const json::Json* value, std::optional<std::string>& out, detail::Issue& issue
) {
    if (value == nullptr)
        return true;
    const std::string* text = value->string();
    if (text == nullptr)
        return fail_details(issue);
    out = *text;
    return true;
}

bool hex64(std::string_view text) noexcept {
    if (text.size() != 64)
        return false;
    for (const char character : text) {
        const bool digit = character >= '0' && character <= '9';
        const bool lower = character >= 'a' && character <= 'f';
        const bool upper = character >= 'A' && character <= 'F';
        if (!digit && !lower && !upper)
            return false;
    }
    return true;
}

bool take_hex(const json::Json* value, std::string& out, bool required, detail::Issue& issue) {
    if (!take_string(value, out, required, issue))
        return false;
    if (value != nullptr && !hex64(out))
        return fail_details(issue);
    return true;
}

bool take_optional_hex(
    const json::Json* value, std::optional<std::string>& out, detail::Issue& issue
) {
    if (!take_optional_string(value, out, issue))
        return false;
    if (out.has_value() && !hex64(*out))
        return fail_details(issue);
    return true;
}

bool take_bool(const json::Json* value, bool& out, bool required, detail::Issue& issue) {
    if (value == nullptr)
        return required ? fail_details(issue) : true;
    const std::optional<bool> flag = value->boolean();
    if (!flag.has_value())
        return fail_details(issue);
    out = *flag;
    return true;
}

bool take_optional_bool(const json::Json* value, std::optional<bool>& out, detail::Issue& issue) {
    if (value == nullptr)
        return true;
    const std::optional<bool> flag = value->boolean();
    if (!flag.has_value())
        return fail_details(issue);
    out = *flag;
    return true;
}

bool take_i64(const json::Json* value, int64_t& out, bool required, detail::Issue& issue) {
    if (value == nullptr)
        return required ? fail_details(issue) : true;
    const std::optional<int64_t> number = value->integer();
    if (!number.has_value())
        return fail_details(issue);
    out = *number;
    return true;
}

bool take_optional_i64(const json::Json* value, std::optional<int64_t>& out, detail::Issue& issue) {
    if (value == nullptr)
        return true;
    const std::optional<int64_t> number = value->integer();
    if (!number.has_value())
        return fail_details(issue);
    out = *number;
    return true;
}

bool take_u64(const json::Json* value, uint64_t& out, bool required, detail::Issue& issue) {
    int64_t number = 0;
    if (!take_i64(value, number, required, issue))
        return false;
    if (value != nullptr && number < 0)
        return fail_details(issue);
    if (value != nullptr)
        out = static_cast<uint64_t>(number);
    return true;
}

bool take_optional_u64(
    const json::Json* value, std::optional<uint64_t>& out, detail::Issue& issue
) {
    if (value == nullptr)
        return true;
    uint64_t number = 0;
    if (!take_u64(value, number, true, issue))
        return false;
    out = number;
    return true;
}

bool fits_int(int64_t number) noexcept {
    return number >= std::numeric_limits<int>::min() && number <= std::numeric_limits<int>::max();
}

bool take_int(const json::Json* value, int& out, bool required, detail::Issue& issue) {
    int64_t number = 0;
    if (!take_i64(value, number, required, issue))
        return false;
    if (value != nullptr && !fits_int(number))
        return fail_details(issue);
    if (value != nullptr)
        out = static_cast<int>(number);
    return true;
}

bool take_optional_int(const json::Json* value, std::optional<int>& out, detail::Issue& issue) {
    if (value == nullptr)
        return true;
    int number = 0;
    if (!take_int(value, number, true, issue))
        return false;
    out = number;
    return true;
}

bool take_u32(const json::Json* value, uint32_t& out, detail::Issue& issue) {
    int64_t number = 0;
    if (!take_i64(value, number, true, issue))
        return false;
    if (number < 0 || number > static_cast<int64_t>(UINT32_MAX))
        return fail_details(issue);
    out = static_cast<uint32_t>(number);
    return true;
}

bool take_unsigned(const json::Json* value, unsigned& out, detail::Issue& issue) {
    int64_t number = 0;
    if (!take_i64(value, number, true, issue))
        return false;
    if (number < 0 ||
        static_cast<uint64_t>(number) > static_cast<uint64_t>(std::numeric_limits<unsigned>::max()))
        return fail_details(issue);
    out = static_cast<unsigned>(number);
    return true;
}

bool take_strings(
    const json::Json* value, std::vector<std::string>& out, bool required, detail::Issue& issue
) {
    if (value == nullptr) {
        if (required)
            return fail_details(issue);
        out.clear();
        return true;
    }
    if (value->type() != json::JsonType::array)
        return fail_details(issue);
    out.clear();
    for (const json::Json& element : value->elements()) {
        const std::string* text = element.string();
        if (text == nullptr)
            return fail_details(issue);
        out.push_back(*text);
    }
    return true;
}

bool read_version(const Members& members, int& version, detail::Issue& issue) {
    const json::Json* value = members.last("oademo");
    int64_t number = 0;
    if (!take_i64(value, number, true, issue))
        return false;
    if (number != format_version) {
        issue = detail::Issue::version;
        return false;
    }
    version = format_version;
    return true;
}

bool parse_package(const json::Json& value, PackageRef& package, detail::Issue& issue) {
    if (value.type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(value);
    if (!take_string(members.last("id"), package.id, true, issue) ||
        !take_string(members.last("version"), package.version, true, issue))
        return false;
    return collect_extras(members, {"id", "version"}, package.extra, issue);
}

bool parse_mod(const json::Json* value, std::optional<ModRef>& mod, detail::Issue& issue) {
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::object)
        return fail_details(issue);
    ModRef read;
    const Members members(*value);
    if (!take_string(members.last("id"), read.id, true, issue) ||
        !take_string(members.last("name"), read.name, true, issue) ||
        !take_string(members.last("version"), read.version, true, issue) ||
        !take_optional_i64(members.last("revision"), read.revision, issue) ||
        !take_optional_hex(members.last("sim_hash"), read.sim_hash, issue))
        return false;
    if (!collect_extras(
            members, {"id", "name", "version", "revision", "sim_hash"}, read.extra, issue
        ))
        return false;
    mod = std::move(read);
    return true;
}

bool parse_limits(const json::Json* value, LimitsInfo& limits, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_i64(members.last("weapon_types"), limits.weapon_types, true, issue) ||
        !take_i64(members.last("sides"), limits.sides, true, issue))
        return false;
    return collect_extras(members, {"weapon_types", "sides"}, limits.extra, issue);
}

bool parse_player(const json::Json& value, PlayerInfo& player, detail::Issue& issue) {
    if (value.type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(value);
    if (!take_int(members.last("number"), player.number, true, issue) ||
        !take_string(members.last("name"), player.name, true, issue) ||
        !take_string(members.last("side"), player.side, true, issue) ||
        !take_int(members.last("colour"), player.colour, true, issue) ||
        !take_optional_int(members.last("team"), player.team, issue) ||
        !take_string(members.last("kind"), player.kind, true, issue) ||
        !take_optional_bool(members.last("recorded_here"), player.recorded_here, issue) ||
        !take_optional_string(members.last("result"), player.result, issue) ||
        !take_optional_i64(members.last("kills"), player.kills, issue))
        return false;
    return collect_extras(
        members,
        {"number", "name", "side", "colour", "team", "kind", "recorded_here", "result", "kills"},
        player.extra,
        issue
    );
}

bool parse_recording(const json::Json* value, RecordingInfo& recording, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_hex(members.last("sha256"), recording.sha256, true, issue) ||
        !take_u64(members.last("bytes"), recording.bytes, true, issue) ||
        !take_int(members.last("tad_version"), recording.tad_version, true, issue) ||
        !take_u64(members.last("packets"), recording.packets, true, issue) ||
        !take_bool(members.last("addresses_removed"), recording.addresses_removed, true, issue))
        return false;
    return collect_extras(
        members,
        {"sha256", "bytes", "tad_version", "packets", "addresses_removed"},
        recording.extra,
        issue
    );
}

bool parse_recorded(const json::Json* value, RecordedInfo& recorded, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    recorded.after_crash = false;
    if (!take_optional_string(members.last("at"), recorded.at, issue) ||
        !take_optional_string(members.last("local"), recorded.local, issue) ||
        !take_optional_string(members.last("by"), recorded.by, issue) ||
        !take_optional_string(members.last("platform"), recorded.platform, issue) ||
        !take_string(members.last("how"), recorded.how, true, issue) ||
        !take_bool(members.last("after_crash"), recorded.after_crash, false, issue))
        return false;
    return collect_extras(
        members, {"at", "local", "by", "platform", "how", "after_crash"}, recorded.extra, issue
    );
}

bool parse_game(const json::Json* value, GameInfo& game, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_string(members.last("kind"), game.kind, true, issue) ||
        !take_optional_u64(members.last("length_ticks"), game.length_ticks, issue) ||
        !take_optional_u64(members.last("length_ms"), game.length_ms, issue) ||
        !take_optional_string(members.last("ended"), game.ended, issue))
        return false;
    bool took_kind_object = false;
    for (const Member& item : members.items()) {
        if (known_name(item.name, {"kind", "length_ticks", "length_ms", "ended"}))
            continue;
        if (!took_kind_object && item.name == game.kind &&
            item.value->type() == json::JsonType::object) {
            game.kind_object = compact_json(*item.value);
            took_kind_object = true;
            continue;
        }
        add_extra(game.extra, item.name, *item.value);
    }
    return true;
}

bool parse_map(const json::Json* value, MapInfo& map, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_string(members.last("name"), map.name, true, issue) ||
        !take_optional_hex(members.last("sha256"), map.sha256, issue))
        return false;
    return collect_extras(members, {"name", "sha256"}, map.extra, issue);
}

bool parse_content(const json::Json* value, ContentInfo& content, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    const json::Json* base = members.last("base");
    if (base == nullptr)
        return fail_details(issue);
    if (!parse_package(*base, content.base, issue) ||
        !take_strings(members.last("expansions"), content.expansions, false, issue) ||
        !parse_mod(members.last("mod"), content.mod, issue) ||
        !take_strings(members.last("mutators"), content.mutators, false, issue) ||
        !take_strings(members.last("brains"), content.brains, false, issue) ||
        !parse_limits(members.last("limits"), content.limits, issue))
        return false;
    return collect_extras(
        members, {"base", "expansions", "mod", "mutators", "brains", "limits"}, content.extra, issue
    );
}

bool parse_units(const json::Json* value, UnitsInfo& units, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_u32(members.last("count"), units.count, issue) ||
        !take_hex(members.last("table_sha256"), units.table_sha256, true, issue) ||
        !take_unsigned(members.last("index_bits"), units.index_bits, issue))
        return false;
    return collect_extras(members, {"count", "table_sha256", "index_bits"}, units.extra, issue);
}

bool parse_players(
    const json::Json* value, std::vector<PlayerInfo>& players, detail::Issue& issue
) {
    if (value == nullptr || value->type() != json::JsonType::array)
        return fail_details(issue);
    players.clear();
    for (const json::Json& element : value->elements()) {
        PlayerInfo player;
        if (!parse_player(element, player, issue))
            return false;
        players.push_back(std::move(player));
    }
    return true;
}

bool parse_map_need(const json::Json& value, MapNeed& map, detail::Issue& issue) {
    if (value.type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(value);
    if (!take_string(members.last("name"), map.name, true, issue) ||
        !take_hex(members.last("sha256"), map.sha256, true, issue))
        return false;
    return collect_extras(members, {"name", "sha256"}, map.extra, issue);
}

bool parse_map_needs(const json::Json* value, std::vector<MapNeed>& maps, detail::Issue& issue) {
    maps.clear();
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array)
        return fail_details(issue);
    for (const json::Json& element : value->elements()) {
        MapNeed map;
        if (!parse_map_need(element, map, issue))
            return false;
        maps.push_back(std::move(map));
    }
    return true;
}

bool parse_need(const json::Json& value, Need& need, detail::Issue& issue) {
    if (value.type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(value);
    if (!take_string(members.last("kind"), need.kind, true, issue) ||
        !take_string(members.last("id"), need.id, true, issue) ||
        !take_optional_string(members.last("version"), need.version, issue) ||
        !take_optional_i64(members.last("revision"), need.revision, issue) ||
        !take_optional_hex(members.last("sha256"), need.sha256, issue) ||
        !take_optional_hex(members.last("sim_hash"), need.sim_hash, issue) ||
        !take_optional_hex(members.last("units_table_sha256"), need.units_table_sha256, issue) ||
        !parse_map_needs(members.last("maps"), need.maps, issue))
        return false;
    return collect_extras(
        members,
        {"kind", "id", "version", "revision", "sha256", "sim_hash", "units_table_sha256", "maps"},
        need.extra,
        issue
    );
}

bool parse_engine(const json::Json* value, EngineNeed& engine, detail::Issue& issue) {
    if (value == nullptr || value->type() != json::JsonType::object)
        return fail_details(issue);
    const Members members(*value);
    if (!take_string(members.last("recorded_with"), engine.recorded_with, true, issue) ||
        !take_string(members.last("needs"), engine.needs, true, issue))
        return false;
    return collect_extras(members, {"recorded_with", "needs"}, engine.extra, issue);
}

bool object_root(std::string_view text, json::Json& root, detail::Issue& issue) {
    json::JsonError problem;
    const std::optional<json::Json> parsed = json::parse_json(text, problem);
    if (!parsed.has_value() || parsed->type() != json::JsonType::object)
        return fail_details(issue);
    root = *parsed;
    return true;
}

void write_extras(json::JsonWriter& writer, const std::vector<JsonExtra>& extras) {
    for (const JsonExtra& extra : extras) {
        writer.key(extra.name);
        writer.raw(extra.json);
    }
}

void write_optional_string(
    json::JsonWriter& writer, std::string_view name, const std::optional<std::string>& value
) {
    if (!value.has_value())
        return;
    writer.key(name);
    writer.string(*value);
}

void write_optional_u64(
    json::JsonWriter& writer, std::string_view name, const std::optional<uint64_t>& value
) {
    if (!value.has_value())
        return;
    writer.key(name);
    writer.integer(static_cast<int64_t>(*value));
}

void write_optional_i64(
    json::JsonWriter& writer, std::string_view name, const std::optional<int64_t>& value
) {
    if (!value.has_value())
        return;
    writer.key(name);
    writer.integer(*value);
}

void write_optional_int(
    json::JsonWriter& writer, std::string_view name, const std::optional<int>& value
) {
    if (!value.has_value())
        return;
    writer.key(name);
    writer.integer(*value);
}

void write_optional_bool(
    json::JsonWriter& writer, std::string_view name, const std::optional<bool>& value
) {
    if (!value.has_value())
        return;
    writer.key(name);
    writer.boolean(*value);
}

void write_strings(
    json::JsonWriter& writer, std::string_view name, const std::vector<std::string>& values
) {
    writer.key(name);
    writer.begin_array();
    for (const std::string& value : values)
        writer.string(value);
    writer.end_array();
}

void write_package(json::JsonWriter& writer, const PackageRef& package) {
    writer.begin_object();
    writer.key("id");
    writer.string(package.id);
    writer.key("version");
    writer.string(package.version);
    write_extras(writer, package.extra);
    writer.end_object();
}

void write_mod(json::JsonWriter& writer, const ModRef& mod) {
    writer.begin_object();
    writer.key("id");
    writer.string(mod.id);
    writer.key("name");
    writer.string(mod.name);
    writer.key("version");
    writer.string(mod.version);
    write_optional_i64(writer, "revision", mod.revision);
    write_optional_string(writer, "sim_hash", mod.sim_hash);
    write_extras(writer, mod.extra);
    writer.end_object();
}

void write_player(json::JsonWriter& writer, const PlayerInfo& player) {
    writer.begin_object();
    writer.key("number");
    writer.integer(player.number);
    writer.key("name");
    writer.string(player.name);
    writer.key("side");
    writer.string(player.side);
    writer.key("colour");
    writer.integer(player.colour);
    write_optional_int(writer, "team", player.team);
    writer.key("kind");
    writer.string(player.kind);
    write_optional_bool(writer, "recorded_here", player.recorded_here);
    write_optional_string(writer, "result", player.result);
    write_optional_i64(writer, "kills", player.kills);
    write_extras(writer, player.extra);
    writer.end_object();
}

} // namespace

namespace detail {

bool parse_meta(std::string_view text, Meta& meta, Issue& issue) {
    issue = Issue::none;
    json::Json root;
    if (!object_root(text, root, issue))
        return false;
    const Members members(root);
    Meta read;
    if (!read_version(members, read.oademo, issue) ||
        !parse_recording(members.last("recording"), read.recording, issue) ||
        !parse_recorded(members.last("recorded"), read.recorded, issue) ||
        !parse_game(members.last("game"), read.game, issue) ||
        !parse_map(members.last("map"), read.map, issue) ||
        !parse_content(members.last("content"), read.content, issue) ||
        !parse_units(members.last("units"), read.units, issue) ||
        !parse_players(members.last("players"), read.players, issue))
        return false;
    if (!collect_extras(
            members,
            {"oademo", "recording", "recorded", "game", "map", "content", "units", "players"},
            read.extra,
            issue
        ))
        return false;
    meta = std::move(read);
    return true;
}

bool parse_manifest(std::string_view text, Manifest& manifest, Issue& issue) {
    issue = Issue::none;
    json::Json root;
    if (!object_root(text, root, issue))
        return false;
    const Members members(root);
    Manifest read;
    if (!read_version(members, read.oademo, issue) ||
        !parse_engine(members.last("engine"), read.engine, issue))
        return false;
    const json::Json* needs = members.last("needs");
    if (needs == nullptr || needs->type() != json::JsonType::array)
        return fail_details(issue);
    for (const json::Json& element : needs->elements()) {
        Need need;
        if (!parse_need(element, need, issue))
            return false;
        read.needs.push_back(std::move(need));
    }
    if (!collect_extras(members, {"oademo", "engine", "needs"}, read.extra, issue))
        return false;
    manifest = std::move(read);
    return true;
}

std::optional<std::string> canonical_json(std::string_view text) {
    json::JsonError problem;
    const std::optional<json::Json> parsed = json::parse_json(text, problem);
    if (!parsed.has_value())
        return std::nullopt;
    return compact_json(*parsed);
}

} // namespace detail

std::string meta_text(const Meta& meta) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("oademo");
    writer.integer(meta.oademo);
    writer.key("recording");
    writer.begin_object();
    writer.key("sha256");
    writer.string(meta.recording.sha256);
    writer.key("bytes");
    writer.integer(static_cast<int64_t>(meta.recording.bytes));
    writer.key("tad_version");
    writer.integer(meta.recording.tad_version);
    writer.key("packets");
    writer.integer(static_cast<int64_t>(meta.recording.packets));
    writer.key("addresses_removed");
    writer.boolean(meta.recording.addresses_removed);
    write_extras(writer, meta.recording.extra);
    writer.end_object();
    writer.key("recorded");
    writer.begin_object();
    write_optional_string(writer, "at", meta.recorded.at);
    write_optional_string(writer, "local", meta.recorded.local);
    write_optional_string(writer, "by", meta.recorded.by);
    write_optional_string(writer, "platform", meta.recorded.platform);
    writer.key("how");
    writer.string(meta.recorded.how);
    writer.key("after_crash");
    writer.boolean(meta.recorded.after_crash);
    write_extras(writer, meta.recorded.extra);
    writer.end_object();
    writer.key("game");
    writer.begin_object();
    writer.key("kind");
    writer.string(meta.game.kind);
    write_optional_u64(writer, "length_ticks", meta.game.length_ticks);
    write_optional_u64(writer, "length_ms", meta.game.length_ms);
    write_optional_string(writer, "ended", meta.game.ended);
    if (meta.game.kind_object.has_value() &&
        !known_name(meta.game.kind, {"kind", "length_ticks", "length_ms", "ended"})) {
        writer.key(meta.game.kind);
        writer.raw(*meta.game.kind_object);
    }
    write_extras(writer, meta.game.extra);
    writer.end_object();
    writer.key("map");
    writer.begin_object();
    writer.key("name");
    writer.string(meta.map.name);
    write_optional_string(writer, "sha256", meta.map.sha256);
    write_extras(writer, meta.map.extra);
    writer.end_object();
    writer.key("content");
    writer.begin_object();
    writer.key("base");
    write_package(writer, meta.content.base);
    write_strings(writer, "expansions", meta.content.expansions);
    if (meta.content.mod.has_value()) {
        writer.key("mod");
        write_mod(writer, *meta.content.mod);
    }
    write_strings(writer, "mutators", meta.content.mutators);
    write_strings(writer, "brains", meta.content.brains);
    writer.key("limits");
    writer.begin_object();
    writer.key("weapon_types");
    writer.integer(meta.content.limits.weapon_types);
    writer.key("sides");
    writer.integer(meta.content.limits.sides);
    write_extras(writer, meta.content.limits.extra);
    writer.end_object();
    write_extras(writer, meta.content.extra);
    writer.end_object();
    writer.key("units");
    writer.begin_object();
    writer.key("count");
    writer.integer(static_cast<int64_t>(meta.units.count));
    writer.key("table_sha256");
    writer.string(meta.units.table_sha256);
    writer.key("index_bits");
    writer.integer(static_cast<int64_t>(meta.units.index_bits));
    write_extras(writer, meta.units.extra);
    writer.end_object();
    writer.key("players");
    writer.begin_array();
    for (const PlayerInfo& player : meta.players)
        write_player(writer, player);
    writer.end_array();
    write_extras(writer, meta.extra);
    writer.end_object();
    return writer.text();
}

std::string manifest_text(const Manifest& manifest) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("oademo");
    writer.integer(manifest.oademo);
    writer.key("engine");
    writer.begin_object();
    writer.key("recorded_with");
    writer.string(manifest.engine.recorded_with);
    writer.key("needs");
    writer.string(manifest.engine.needs);
    write_extras(writer, manifest.engine.extra);
    writer.end_object();
    writer.key("needs");
    writer.begin_array();
    for (const Need& need : manifest.needs) {
        writer.begin_object();
        writer.key("kind");
        writer.string(need.kind);
        writer.key("id");
        writer.string(need.id);
        write_optional_string(writer, "version", need.version);
        write_optional_i64(writer, "revision", need.revision);
        write_optional_string(writer, "sha256", need.sha256);
        write_optional_string(writer, "sim_hash", need.sim_hash);
        write_optional_string(writer, "units_table_sha256", need.units_table_sha256);
        if (!need.maps.empty()) {
            writer.key("maps");
            writer.begin_array();
            for (const MapNeed& map : need.maps) {
                writer.begin_object();
                writer.key("name");
                writer.string(map.name);
                writer.key("sha256");
                writer.string(map.sha256);
                write_extras(writer, map.extra);
                writer.end_object();
            }
            writer.end_array();
        }
        write_extras(writer, need.extra);
        writer.end_object();
    }
    writer.end_array();
    write_extras(writer, manifest.extra);
    writer.end_object();
    return writer.text();
}

} // namespace oa::formats::oademo
