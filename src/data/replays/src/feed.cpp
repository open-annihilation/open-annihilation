// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading Core Prime's replay feed. The shapes are the ones svc-content
// publishes: a page, one replay, and an error object.

#include "oa/data/replays/feed.hpp"

#include "oa/formats/json.hpp"
#include "oa/formats/oademo/oademo.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::replays {
namespace {

namespace json = oa::formats::json;
namespace oademo = oa::formats::oademo;

/// Reads `count` decimal digits.
bool digits(std::string_view text, std::size_t at, std::size_t count, int& out) noexcept {
    if (at + count > text.size())
        return false;
    int value = 0;
    for (std::size_t index = 0; index < count; ++index) {
        const char byte = text[at + index];
        if (byte < '0' || byte > '9')
            return false;
        value = value * 10 + (byte - '0');
    }
    out = value;
    return true;
}

/// Reads `YYYY-MM-DDTHH:MM:SSZ`, with an optional fraction before `Z`.
std::optional<int64_t> utc_seconds(std::string_view text) noexcept {
    if (text.size() < 20)
        return std::nullopt;
    int year = 0;
    int month = 0;
    int day = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    if (!digits(text, 0, 4, year) || text[4] != '-' || !digits(text, 5, 2, month) ||
        text[7] != '-' || !digits(text, 8, 2, day) || text[10] != 'T' ||
        !digits(text, 11, 2, hour) || text[13] != ':' || !digits(text, 14, 2, minute) ||
        text[16] != ':' || !digits(text, 17, 2, second))
        return std::nullopt;
    std::size_t index = 19;
    if (index < text.size() && text[index] == '.') {
        ++index;
        if (index >= text.size() || text[index] < '0' || text[index] > '9')
            return std::nullopt;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9')
            ++index;
    }
    if (index + 1 != text.size() || text[index] != 'Z')
        return std::nullopt;
    if (hour > 23 || minute > 59 || second > 59)
        return std::nullopt;
    const std::chrono::year_month_day date{
        std::chrono::year{year},
        std::chrono::month{static_cast<unsigned>(month)},
        std::chrono::day{static_cast<unsigned>(day)},
    };
    if (!date.ok())
        return std::nullopt;
    const auto stamp = std::chrono::sys_days{date} + std::chrono::hours{hour} +
                       std::chrono::minutes{minute} + std::chrono::seconds{second};
    return stamp.time_since_epoch().count();
}

const std::string* text_of(const json::Json& object, std::string_view name) {
    const json::Json* member = object.find(name);
    if (member == nullptr)
        return nullptr;
    return member->string();
}

std::optional<int64_t> whole_of(const json::Json& object, std::string_view name) {
    const json::Json* member = object.find(name);
    if (member == nullptr)
        return std::nullopt;
    return member->integer();
}

std::string text_or_empty(const json::Json& object, std::string_view name) {
    const std::string* text = text_of(object, name);
    return text == nullptr ? std::string{} : *text;
}

/// Percent-encodes one query value.
std::string encode_query(std::string_view text) {
    std::string encoded;
    encoded.reserve(text.size());
    for (const unsigned char byte : text) {
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                           byte == '.' || byte == '~';
        if (plain) {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        constexpr char hex[] = "0123456789ABCDEF";
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4]);
        encoded.push_back(hex[byte & 0x0F]);
    }
    return encoded;
}

void add_query(std::string& query, std::string_view key, std::string_view value) {
    if (value.empty())
        return;
    if (!query.empty())
        query.push_back('&');
    query += key;
    query.push_back('=');
    query += encode_query(value);
}

std::string_view trimmed_origin(std::string_view origin) {
    while (!origin.empty() && origin.back() == '/')
        origin.remove_suffix(1);
    return origin;
}

bool blank(std::string_view text) noexcept {
    for (const char byte : text)
        if (byte != ' ' && byte != '\t' && byte != '\n' && byte != '\r')
            return false;
    return true;
}

FeedPlayer read_player(const json::Json& object) {
    FeedPlayer player;
    player.name = text_or_empty(object, "name");
    player.side = text_or_empty(object, "side");
    if (const std::optional<int64_t> colour = whole_of(object, "colour")) {
        if (*colour >= 0 && *colour <= 2147483647)
            player.colour = static_cast<int32_t>(*colour);
    }
    if (const std::optional<int64_t> team = whole_of(object, "team")) {
        if (*team >= 0 && *team <= 2147483647)
            player.team = static_cast<int32_t>(*team);
    }
    player.result = text_or_empty(object, "result");
    if (const std::optional<int64_t> kills = whole_of(object, "kills"))
        player.kills = *kills;
    return player;
}

FeedFile read_file(const json::Json* object) {
    FeedFile file;
    if (object == nullptr || object->type() != json::JsonType::object)
        return file;
    file.sha256 = text_or_empty(*object, "sha256");
    if (const std::optional<int64_t> size = whole_of(*object, "size")) {
        if (*size > 0)
            file.size = static_cast<uint64_t>(*size);
    }
    return file;
}

std::optional<FeedMod> read_mod(const json::Json* object) {
    if (object == nullptr || object->type() == json::JsonType::null)
        return std::nullopt;
    if (object->type() != json::JsonType::object)
        return std::nullopt;
    FeedMod mod;
    mod.id = text_or_empty(*object, "id");
    mod.name = text_or_empty(*object, "name");
    mod.version = text_or_empty(*object, "version");
    if (const std::optional<int64_t> revision = whole_of(*object, "revision"))
        mod.revision = *revision;
    return mod;
}

void read_needs(const json::Json& object, oademo::Manifest& manifest) {
    const json::Json* needs = object.find("needs");
    if (needs == nullptr || needs->type() != json::JsonType::array)
        return;
    for (const json::Json& element : needs->elements()) {
        if (element.type() != json::JsonType::object)
            continue;
        oademo::Need need;
        need.kind = text_or_empty(element, "kind");
        need.id = text_or_empty(element, "id");
        if (const std::string* version = text_of(element, "version"))
            need.version = *version;
        if (const std::optional<int64_t> revision = whole_of(element, "revision"))
            need.revision = *revision;
        if (const std::string* sha = text_of(element, "sha256"))
            need.sha256 = *sha;
        if (const json::Json* maps = element.find("maps");
            maps != nullptr && maps->type() == json::JsonType::array) {
            for (const json::Json& map : maps->elements()) {
                if (map.type() != json::JsonType::object)
                    continue;
                oademo::MapNeed one;
                one.name = text_or_empty(map, "name");
                one.sha256 = text_or_empty(map, "sha256");
                need.maps.push_back(std::move(one));
            }
        }
        manifest.needs.push_back(std::move(need));
    }
}

/// Fills one entry from an object. A bad id leaves the id empty.
bool read_entry(const json::Json& object, FeedEntry& entry) {
    entry.id = text_or_empty(object, "id");
    if (!replay_id_ok(entry.id)) {
        entry.id.clear();
        return false;
    }
    if (const std::optional<int64_t> revision = whole_of(object, "revision"))
        entry.revision = *revision;
    entry.title = text_or_empty(object, "title");
    entry.source = text_or_empty(object, "source");
    entry.visibility = text_or_empty(object, "visibility");
    entry.recorded_at = text_or_empty(object, "recorded_at");
    if (const std::optional<int64_t> recorded = utc_seconds(entry.recorded_at))
        entry.recorded = *recorded;
    if (const std::optional<int64_t> length = whole_of(object, "length_ms")) {
        if (*length > 0)
            entry.length_ms = static_cast<uint64_t>(*length);
    }
    if (const json::Json* game = object.find("game");
        game != nullptr && game->type() == json::JsonType::object) {
        entry.game_kind = text_or_empty(*game, "kind");
        entry.game_id = text_or_empty(*game, "game_id");
        entry.planet = text_or_empty(*game, "planet");
        entry.sector = text_or_empty(*game, "sector");
    }
    if (const json::Json* map = object.find("map");
        map != nullptr && map->type() == json::JsonType::object)
        entry.map_name = text_or_empty(*map, "name");
    if (const json::Json* players = object.find("players");
        players != nullptr && players->type() == json::JsonType::array) {
        for (const json::Json& element : players->elements()) {
            if (element.type() == json::JsonType::object)
                entry.players.push_back(read_player(element));
        }
    }
    entry.mod = read_mod(object.find("mod"));
    entry.engine_needs = text_or_empty(object, "engine_needs");
    entry.uploader = text_or_empty(object, "uploader");
    if (const std::optional<int64_t> downloads = whole_of(object, "downloads")) {
        if (*downloads > 0)
            entry.downloads = *downloads;
    }
    if (const json::Json* files = object.find("files");
        files != nullptr && files->type() == json::JsonType::object) {
        entry.oademo = read_file(files->find("oademo"));
        entry.tad = read_file(files->find("tad"));
    }

    Replay& replay = entry.replay;
    replay.map = entry.map_name;
    replay.kind = entry.game_kind;
    replay.planet = entry.planet;
    replay.sector = entry.sector;
    replay.length_ms = entry.length_ms;
    replay.recorded = entry.recorded;
    replay.engine_needs = entry.engine_needs;
    replay.sha256 = entry.oademo.sha256;
    replay.size = entry.oademo.size;
    if (entry.mod) {
        replay.mod_id = entry.mod->id;
        replay.mod_name = entry.mod->name;
        replay.mod_version = entry.mod->version;
        replay.mod_revision = entry.mod->revision;
    }
    replay.players.reserve(entry.players.size());
    for (const FeedPlayer& player : entry.players) {
        Player one;
        one.name = player.name;
        one.side = player.side;
        one.colour = player.colour;
        one.team = player.team;
        replay.players.push_back(std::move(one));
    }

    entry.needs_present = object.find("needs") != nullptr;
    oademo::Manifest manifest;
    manifest.engine.needs = entry.engine_needs;
    read_needs(object, manifest);
    oademo::Meta meta;
    if (entry.mod) {
        oademo::ModRef mod;
        mod.id = entry.mod->id;
        mod.name = entry.mod->name;
        mod.version = entry.mod->version;
        meta.content.mod = std::move(mod);
    }
    if (entry.needs_present || !entry.engine_needs.empty())
        apply_manifest(manifest, meta, replay);
    replay.needs_read = entry.needs_present;
    return true;
}

FeedFailure read_failure(const json::Json& object) {
    FeedFailure failure;
    failure.error = text_or_empty(object, "error");
    failure.message = text_or_empty(object, "message");
    return failure;
}

bool is_error(const json::Json& object) {
    return object.find("replays") == nullptr && text_of(object, "error") != nullptr;
}

} // namespace

bool replay_id_ok(std::string_view id) noexcept {
    if (id.size() != 2 + replay_id_body || id[0] != 'r' || id[1] != '_')
        return false;
    for (std::size_t index = 2; index < id.size(); ++index) {
        const char byte = id[index];
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_';
        if (!plain)
            return false;
    }
    return true;
}

FeedPageResult parse_replay_page(std::string_view text) {
    FeedPageResult result;
    if (blank(text))
        return result;
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(text, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return result;
    if (is_error(*root)) {
        result.status = FeedStatus::error;
        result.failure = read_failure(*root);
        return result;
    }
    const std::optional<int64_t> marker = whole_of(*root, "replays");
    if (!marker || *marker != 1)
        return result;
    result.status = FeedStatus::page;
    if (const std::optional<int64_t> total = whole_of(*root, "total"))
        result.page.total = *total;
    result.page.next = text_or_empty(*root, "next");
    const json::Json* items = root->find("items");
    if (items == nullptr || items->type() != json::JsonType::array)
        return result;
    for (const json::Json& element : items->elements()) {
        if (element.type() != json::JsonType::object)
            continue;
        FeedEntry entry;
        if (!read_entry(element, entry))
            continue;
        result.page.items.push_back(std::move(entry));
    }
    return result;
}

FeedEntryResult parse_replay_entry(std::string_view text) {
    FeedEntryResult result;
    if (blank(text))
        return result;
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(text, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return result;
    if (is_error(*root)) {
        result.status = FeedStatus::error;
        result.failure = read_failure(*root);
        return result;
    }
    const std::string* id = text_of(*root, "id");
    if (id == nullptr)
        return result;
    if (!replay_id_ok(*id)) {
        result.status = FeedStatus::refused;
        return result;
    }
    if (!read_entry(*root, result.entry)) {
        result.status = FeedStatus::refused;
        return result;
    }
    result.status = FeedStatus::entry;
    return result;
}

std::string feed_query_text(const FeedQuery& query) {
    std::string text;
    const std::string_view sort = query.sort.empty() ? std::string_view{"newest"} : query.sort;
    add_query(text, "sort", sort);
    add_query(text, "kind", query.kind);
    add_query(text, "source", query.source);
    add_query(text, "planet", query.planet);
    add_query(text, "map", query.map);
    add_query(text, "player", query.player);
    add_query(text, "mod", query.mod);
    if (query.players)
        add_query(text, "players", std::to_string(*query.players));
    add_query(text, "after", query.after);
    int32_t first = query.first;
    if (first < 1)
        first = replay_page_size;
    if (first > replay_page_most)
        first = replay_page_most;
    add_query(text, "first", std::to_string(first));
    return text;
}

std::string feed_list_url(std::string_view origin, const FeedQuery& query) {
    std::string url(trimmed_origin(origin));
    url += replay_feed_path;
    url.push_back('?');
    url += feed_query_text(query);
    return url;
}

std::string feed_entry_url(std::string_view origin, std::string_view id) {
    if (!replay_id_ok(id))
        return {};
    std::string url(trimmed_origin(origin));
    url += replay_feed_path;
    url.push_back('/');
    url += id;
    return url;
}

} // namespace oa::data::replays
