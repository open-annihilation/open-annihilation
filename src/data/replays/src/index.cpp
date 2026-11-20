// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The index of a Recordings folder: one compact JSON object under
// `.replays`, written whole by a rename.

#include "oa/data/replays/replays.hpp"

#include "oa/formats/json.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

namespace oa::data::replays {
namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;

/// The member that names the index's version.
constexpr std::string_view version_member = "replays_index";

/// The member that holds the replays.
constexpr std::string_view replays_member = "replays";

/// The word the index writes for a `.oademo`.
constexpr std::string_view oademo_word = "oademo";

/// The word the index writes for a `.tad`.
constexpr std::string_view tad_word = "tad";

/// Writes a number of 64 bits without a sign.
///
/// @param[in,out] writer the JSON being written
/// @param value the number
void write_unsigned(json::JsonWriter& writer, uint64_t value) {
    writer.raw(std::to_string(value));
}

/// Writes an array of strings.
///
/// @param[in,out] writer the JSON being written
/// @param texts the strings
void write_texts(json::JsonWriter& writer, const std::vector<std::string>& texts) {
    writer.begin_array();
    for (const std::string& text : texts)
        writer.string(text);
    writer.end_array();
}

/// Writes one player as an object.
///
/// @param[in,out] writer the JSON being written
/// @param player the player
void write_player(json::JsonWriter& writer, const Player& player) {
    writer.begin_object();
    writer.key("name");
    writer.string(player.name);
    writer.key("colour");
    writer.integer(player.colour);
    writer.key("side");
    writer.string(player.side);
    if (player.team.has_value()) {
        writer.key("team");
        writer.integer(*player.team);
    }
    writer.key("kind");
    writer.string(player.kind);
    writer.key("recorded_here");
    writer.boolean(player.recorded_here);
    writer.key("number");
    writer.integer(player.number);
    writer.end_object();
}

/// Writes one package a replay needs.
///
/// @param[in,out] writer the JSON being written
/// @param need the package
void write_need(json::JsonWriter& writer, const ContentNeed& need);

/// Writes one replay as an object.
///
/// @param[in,out] writer the JSON being written
/// @param replay the replay
void write_replay(json::JsonWriter& writer, const Replay& replay) {
    writer.begin_object();
    writer.key("path");
    writer.string(replay.path);
    writer.key("size");
    write_unsigned(writer, replay.size);
    writer.key("modified");
    writer.integer(replay.modified);
    writer.key("file");
    writer.string(replay.file == FileKind::tad ? tad_word : oademo_word);
    writer.key("damage");
    writer.string(replay.damage);
    writer.key("sha256");
    writer.string(replay.sha256);
    writer.key("map");
    writer.string(replay.map);
    writer.key("kind");
    writer.string(replay.kind);
    writer.key("how");
    writer.string(replay.how);
    writer.key("recorded");
    writer.integer(replay.recorded);
    writer.key("recorded_by");
    writer.string(replay.recorded_by);
    writer.key("length_ms");
    write_unsigned(writer, replay.length_ms);
    writer.key("ended");
    writer.string(replay.ended);
    writer.key("after_crash");
    writer.boolean(replay.after_crash);
    writer.key("mod_id");
    writer.string(replay.mod_id);
    writer.key("mod_name");
    writer.string(replay.mod_name);
    writer.key("mod_version");
    writer.string(replay.mod_version);
    if (replay.mod_revision.has_value()) {
        writer.key("mod_revision");
        writer.integer(*replay.mod_revision);
    }
    writer.key("expansions");
    write_texts(writer, replay.expansions);
    writer.key("mutators");
    write_texts(writer, replay.mutators);
    writer.key("planet");
    writer.string(replay.planet);
    writer.key("sector");
    writer.string(replay.sector);
    if (!replay.game_id.empty()) {
        writer.key("game_id");
        writer.string(replay.game_id);
    }
    writer.key("players");
    writer.begin_array();
    for (const Player& player : replay.players)
        write_player(writer, player);
    writer.end_array();
    if (!replay.mission_name.empty()) {
        writer.key("mission_name");
        writer.string(replay.mission_name);
    }
    if (replay.timeline.has_value()) {
        writer.key("timeline");
        writer.string(*replay.timeline);
    }
    if (replay.watched_tick.has_value()) {
        writer.key("watched_tick");
        writer.integer(static_cast<int64_t>(*replay.watched_tick));
    }
    if (replay.read_stamp.has_value()) {
        writer.key("read_stamp");
        writer.string(*replay.read_stamp);
    }
    writer.key("needs_read");
    writer.boolean(replay.needs_read);
    if (!replay.engine_recorded.empty()) {
        writer.key("engine_recorded");
        writer.string(replay.engine_recorded);
    }
    if (!replay.engine_needs.empty()) {
        writer.key("engine_needs");
        writer.string(replay.engine_needs);
    }
    if (!replay.needs.empty()) {
        writer.key("needs");
        writer.begin_array();
        for (const ContentNeed& need : replay.needs)
            write_need(writer, need);
        writer.end_array();
    }
    writer.end_object();
}

/// Writes one package a replay needs.
///
/// @param[in,out] writer the JSON being written
/// @param need the package
void write_need(json::JsonWriter& writer, const ContentNeed& need) {
    writer.begin_object();
    writer.key("kind");
    writer.string(need.kind);
    writer.key("id");
    writer.string(need.id);
    if (!need.name.empty()) {
        writer.key("name");
        writer.string(need.name);
    }
    if (!need.version.empty()) {
        writer.key("version");
        writer.string(need.version);
    }
    if (need.revision.has_value()) {
        writer.key("revision");
        writer.integer(*need.revision);
    }
    if (!need.sha256.empty()) {
        writer.key("sha256");
        writer.string(need.sha256);
    }
    if (!need.map_name.empty()) {
        writer.key("map_name");
        writer.string(need.map_name);
    }
    if (!need.pack_name.empty()) {
        writer.key("pack_name");
        writer.string(need.pack_name);
    }
    writer.end_object();
}

/// Reads a string member, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the text; unchanged when the member is missing
/// @return false when the member is there and is not a string
bool take_text(const json::Json& object, std::string_view name, std::string& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::string* text = value->string();
    if (text == nullptr)
        return false;
    out = *text;
    return true;
}

/// Reads a whole-number member of 64 bits without a sign, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the number; unchanged when the member is missing
/// @return false when the member is there and is not such a number
bool take_unsigned(const json::Json& object, std::string_view name, uint64_t& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::string* text = value->number_text();
    if (text == nullptr || text->empty())
        return false;
    uint64_t number = 0;
    const char* first = text->data();
    const char* last = first + text->size();
    const std::from_chars_result parsed = std::from_chars(first, last, number);
    if (parsed.ec != std::errc() || parsed.ptr != last)
        return false;
    out = number;
    return true;
}

/// Reads a whole-number member of 64 bits, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the number; unchanged when the member is missing
/// @return false when the member is there and is not such a number
bool take_signed(const json::Json& object, std::string_view name, int64_t& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::optional<int64_t> number = value->integer();
    if (!number.has_value())
        return false;
    out = *number;
    return true;
}

/// Reads a whole-number member of 32 bits, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the number; unchanged when the member is missing
/// @return false when the member is there and is not such a number
bool take_int32(const json::Json& object, std::string_view name, int32_t& out) {
    int64_t number = out;
    if (!take_signed(object, name, number))
        return false;
    if (number < std::numeric_limits<int32_t>::min() ||
        number > std::numeric_limits<int32_t>::max())
        return false;
    out = static_cast<int32_t>(number);
    return true;
}

/// Reads an optional whole-number member, set only when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the number; unchanged when the member is missing
/// @return false when the member is there and is not such a number
template <typename Number>
bool take_optional(const json::Json& object, std::string_view name, std::optional<Number>& out) {
    if (object.find(name) == nullptr)
        return true;
    int64_t number = 0;
    if (!take_signed(object, name, number))
        return false;
    if constexpr (!std::is_same_v<Number, int64_t>) {
        if (number < std::numeric_limits<Number>::min() ||
            number > std::numeric_limits<Number>::max())
            return false;
    }
    out = static_cast<Number>(number);
    return true;
}

/// Reads an optional string member, set only when the object has it.
///
/// An empty string is kept: it is how a finished index pass that left nothing
/// is told from a file that has not been read.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the text; unchanged when the member is missing
/// @return false when the member is there and is not a string
bool take_optional_text(
    const json::Json& object, std::string_view name, std::optional<std::string>& out
) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::string* text = value->string();
    if (text == nullptr)
        return false;
    out = *text;
    return true;
}

/// Reads a boolean member, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the value; unchanged when the member is missing
/// @return false when the member is there and is not a boolean
bool take_flag(const json::Json& object, std::string_view name, bool& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    const std::optional<bool> flag = value->boolean();
    if (!flag.has_value())
        return false;
    out = *flag;
    return true;
}

/// Reads an array of strings, when the object has it.
///
/// @param object the object
/// @param name the member's name
/// @param[out] out the strings; unchanged when the member is missing
/// @return false when the member is there and is not an array of strings
bool take_texts(const json::Json& object, std::string_view name, std::vector<std::string>& out) {
    const json::Json* value = object.find(name);
    if (value == nullptr)
        return true;
    if (value->type() != json::JsonType::array)
        return false;
    std::vector<std::string> texts;
    for (const json::Json& element : value->elements()) {
        const std::string* text = element.string();
        if (text == nullptr)
            return false;
        texts.push_back(*text);
    }
    out = std::move(texts);
    return true;
}

/// Reads the packages an entry names. A bad member leaves them unread.
///
/// @param value the entry's object
/// @param[in,out] replay the replay
/// @return false when `needs` is present and is not an array of objects
bool read_needs(const json::Json& value, Replay& replay) {
    const json::Json* needs = value.find("needs");
    if (needs == nullptr)
        return true;
    if (needs->type() != json::JsonType::array)
        return false;
    std::vector<ContentNeed> read;
    for (const json::Json& element : needs->elements()) {
        if (element.type() != json::JsonType::object)
            return false;
        ContentNeed need;
        if (!take_text(element, "kind", need.kind) || !take_text(element, "id", need.id) ||
            !take_text(element, "name", need.name) ||
            !take_text(element, "version", need.version) ||
            !take_optional(element, "revision", need.revision) ||
            !take_text(element, "sha256", need.sha256) ||
            !take_text(element, "map_name", need.map_name) ||
            !take_text(element, "pack_name", need.pack_name))
            return false;
        if (read.size() < most_content_needs)
            read.push_back(std::move(need));
    }
    replay.needs = std::move(read);
    return true;
}

/// Reads one player of an entry.
///
/// @param value the player's object
/// @param[out] player the player
/// @return false when the value is not an object or a member has the wrong type
bool read_player(const json::Json& value, Player& player) {
    if (value.type() != json::JsonType::object)
        return false;
    return take_text(value, "name", player.name) && take_int32(value, "colour", player.colour) &&
           take_text(value, "side", player.side) && take_optional(value, "team", player.team) &&
           take_text(value, "kind", player.kind) &&
           take_flag(value, "recorded_here", player.recorded_here) &&
           take_int32(value, "number", player.number);
}

/// Reads one entry of an index.
///
/// @param value the entry's object
/// @param[out] replay the replay
/// @return false when the entry is not an object, has no path, or holds a
///         known member of the wrong type
bool read_entry(const json::Json& value, Replay& replay) {
    if (value.type() != json::JsonType::object)
        return false;
    const json::Json* path = value.find("path");
    if (path == nullptr || path->string() == nullptr || path->string()->empty())
        return false;
    replay.path = *path->string();
    std::string file_word(oademo_word);
    if (!take_unsigned(value, "size", replay.size) ||
        !take_signed(value, "modified", replay.modified) || !take_text(value, "file", file_word) ||
        !take_text(value, "damage", replay.damage) || !take_text(value, "sha256", replay.sha256) ||
        !take_text(value, "map", replay.map) || !take_text(value, "kind", replay.kind) ||
        !take_text(value, "how", replay.how) || !take_signed(value, "recorded", replay.recorded) ||
        !take_text(value, "recorded_by", replay.recorded_by) ||
        !take_unsigned(value, "length_ms", replay.length_ms) ||
        !take_text(value, "ended", replay.ended) ||
        !take_flag(value, "after_crash", replay.after_crash) ||
        !take_text(value, "mod_id", replay.mod_id) ||
        !take_text(value, "mod_name", replay.mod_name) ||
        !take_text(value, "mod_version", replay.mod_version) ||
        !take_optional(value, "mod_revision", replay.mod_revision) ||
        !take_texts(value, "expansions", replay.expansions) ||
        !take_texts(value, "mutators", replay.mutators) ||
        !take_text(value, "planet", replay.planet) || !take_text(value, "sector", replay.sector) ||
        !take_text(value, "game_id", replay.game_id) ||
        !take_flag(value, "needs_read", replay.needs_read) ||
        !take_text(value, "engine_recorded", replay.engine_recorded) ||
        !take_text(value, "engine_needs", replay.engine_needs) ||
        !take_text(value, "mission_name", replay.mission_name) ||
        !take_optional_text(value, "timeline", replay.timeline) ||
        !take_optional(value, "watched_tick", replay.watched_tick) ||
        !take_optional_text(value, "read_stamp", replay.read_stamp))
        return false;
    if (!read_needs(value, replay)) {
        replay.needs.clear();
        replay.engine_recorded.clear();
        replay.engine_needs.clear();
        replay.needs_read = false;
    }
    if (file_word == oademo_word)
        replay.file = FileKind::oademo;
    else if (file_word == tad_word)
        replay.file = FileKind::tad;
    else
        return false;
    // The stored timeline is kept whole. A pass that can no longer read the
    // replay then keeps the result and the marks. Whether it should be read
    // again (no `events` member, or a version other than 1) is decided from
    // the text's prefix, without parsing it, by the Library.
    const json::Json* players = value.find("players");
    if (players == nullptr)
        return true;
    if (players->type() != json::JsonType::array)
        return false;
    for (const json::Json& element : players->elements()) {
        Player player;
        if (!read_player(element, player))
            return false;
        replay.players.push_back(std::move(player));
    }
    return true;
}

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

} // namespace

std::string index_text(const Index& index) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key(version_member);
    writer.integer(index_version);
    writer.key(replays_member);
    writer.begin_array();
    for (const Replay& replay : index.replays)
        write_replay(writer, replay);
    writer.end_array();
    writer.end_object();
    return writer.text();
}

std::optional<Index> parse_index(std::string_view text) {
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(text, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return std::nullopt;
    const json::Json* version = root->find(version_member);
    if (version == nullptr || version->integer() != index_version)
        return std::nullopt;
    const json::Json* entries = root->find(replays_member);
    if (entries == nullptr || entries->type() != json::JsonType::array)
        return std::nullopt;
    Index parsed;
    parsed.replays.reserve(entries->elements().size());
    for (const json::Json& element : entries->elements()) {
        Replay replay;
        if (read_entry(element, replay))
            parsed.replays.push_back(std::move(replay));
    }
    std::stable_sort(
        parsed.replays.begin(), parsed.replays.end(), [](const Replay& left, const Replay& right) {
            return left.path < right.path;
        }
    );
    // A later entry of a path already read is dropped.
    const auto distinct_end = std::unique(
        parsed.replays.begin(), parsed.replays.end(), [](const Replay& left, const Replay& right) {
            return left.path == right.path;
        }
    );
    parsed.replays.erase(distinct_end, parsed.replays.end());
    return parsed;
}

fs::path index_path(const fs::path& recordings) {
    return recordings / oa::platform::path_from_utf8(replays_folder_name) /
           oa::platform::path_from_utf8(index_name);
}

IndexRead read_index(const fs::path& recordings) {
    IndexRead read;
    try {
        const fs::path file = index_path(recordings);
        std::error_code error;
        const fs::file_status status = fs::status(file, error);
        if (fs::status_known(status) && !fs::exists(status))
            return read;
        // From here on the file is there: whatever stops it being read is damage.
        read.damaged = true;
        const uint64_t size = fs::file_size(file, error);
        if (error || size > most_index_bytes)
            return read;
        std::ifstream in(file, std::ios::binary);
        if (!in)
            return read;
        std::string text(static_cast<std::size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        if (in.gcount() != static_cast<std::streamsize>(text.size()))
            return read;
        std::optional<Index> parsed = parse_index(text);
        if (!parsed.has_value())
            return read;
        read.index = std::move(*parsed);
        read.damaged = false;
        return read;
    } catch (const std::exception&) {
        read.index = {};
        read.damaged = true;
        return read;
    }
}

Index load_index(const fs::path& recordings) {
    return read_index(recordings).index;
}

std::optional<fs::path> set_aside(const fs::path& file) {
    try {
        std::error_code error;
        if (!fs::is_regular_file(file, error) || error)
            return std::nullopt;
        fs::path aside = file;
        aside += std::string(damaged_ending);
        // The first copy keeps the plain ending. Each later copy takes the
        // next free number, and an earlier copy is left where it is.
        if (fs::exists(aside, error) || error) {
            bool found = false;
            for (int number = 2; number < 10000; ++number) {
                fs::path numbered = file;
                numbered += std::string(damaged_ending) + "-" + std::to_string(number);
                error.clear();
                if (!fs::exists(numbered, error) && !error) {
                    aside = std::move(numbered);
                    found = true;
                    break;
                }
            }
            if (!found)
                return std::nullopt;
        }
        error.clear();
        fs::rename(file, aside, error);
        if (error)
            return std::nullopt;
        // A rename keeps the time the file was last written. The copy is
        // dated now, so its time says when it was set aside. A copy that
        // cannot be dated keeps the older time.
        std::error_code dated;
        fs::last_write_time(aside, fs::file_time_type::clock::now(), dated);
        return aside;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool save_index(const fs::path& recordings, const Index& index) {
    try {
        std::error_code error;
        fs::create_directories(
            recordings / oa::platform::path_from_utf8(replays_folder_name), error
        );
        if (error)
            return false;
        const std::string text = index_text(index);
        return oa::platform::replace_file(index_path(recordings), as_bytes(text), nullptr);
    } catch (const std::exception&) {
        return false;
    }
}

} // namespace oa::data::replays
