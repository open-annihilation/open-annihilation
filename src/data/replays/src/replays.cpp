// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading one replay file, listing a Recordings folder, and scanning it
// against an index.

#include "oa/data/replays/replays.hpp"

#include "oa/data/replays/interface_words.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/platform/files.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::replays {
namespace {

namespace fs = std::filesystem;
namespace json = oa::formats::json;
namespace oademo = oa::formats::oademo;
namespace saves = oa::data::saves;
namespace tad = oa::formats::tad;

/// The ending of a `.oademo`'s name.
constexpr std::string_view oademo_ending = ".oademo";

/// The ending of a `.tad`'s name.
constexpr std::string_view tad_ending = ".tad";

/// The bytes read from a file at a time.
constexpr std::size_t read_chunk_bytes = 1U << 20U;

/// Seconds in a day.
constexpr int64_t seconds_per_day = 86400;

/// The characters Windows-1252 gives the bytes 0x80 to 0x9F. The five bytes
/// it leaves undefined, 0x81, 0x8D, 0x8F, 0x90 and 0x9D, stand for the
/// characters of the same value, as every byte from 0xA0 does.
constexpr std::array<uint16_t, 32> windows_1252_high = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/// Converts a time of a file clock to whole seconds since 1970.
///
/// A clock with to_sys converts directly, and one without it through UTC, as
/// file clocks only do.
///
/// @param stamp a time of the clock
/// @return seconds since 1970
template <typename Clock>
[[nodiscard]] int64_t seconds_of_clock(typename Clock::time_point stamp) {
    if constexpr (requires { Clock::to_sys(stamp); }) {
        return std::chrono::floor<std::chrono::seconds>(Clock::to_sys(stamp))
            .time_since_epoch()
            .count();
    } else {
        const auto utc = Clock::to_utc(stamp);
        using Utc = typename decltype(utc)::clock;
        return std::chrono::floor<std::chrono::seconds>(Utc::to_sys(utc))
            .time_since_epoch()
            .count();
    }
}

/// Converts a file's modification time to seconds since 1970.
///
/// @param stamp the time the file system gives
/// @return seconds since 1970 (UTC)
int64_t seconds_since_1970(fs::file_time_type stamp) {
    return seconds_of_clock<fs::file_time_type::clock>(stamp);
}

/// Tells whether a text ends with an ending, without ASCII case.
///
/// @param text the text
/// @param ending the ending, lower case
/// @return true when the text's last characters are the ending
bool ends_with_ascii(std::string_view text, std::string_view ending) noexcept {
    if (text.size() < ending.size())
        return false;
    const std::string_view tail = text.substr(text.size() - ending.size());
    for (std::size_t at = 0; at < tail.size(); ++at) {
        char letter = tail[at];
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
        if (letter != ending[at])
            return false;
    }
    return true;
}

/// Returns a text with its ASCII capitals made small.
///
/// @param text the text
/// @return the text in lower case
std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    for (char& letter : lowered)
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    return lowered;
}

/// Keeps one need, up to most_content_needs, with a lower-case digest.
void keep_need(Replay& replay, ContentNeed need) {
    if (replay.needs.size() >= most_content_needs)
        return;
    if (!need.sha256.empty())
        need.sha256 = ascii_lower(need.sha256);
    replay.needs.push_back(std::move(need));
}

/// Copies a `.tad` mutators sector onto the replay. A sector that is not a loadout is skipped.
void take_mutator_sector(const tad::Demo& demo, Replay& replay) {
    const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::mutators);
    if (sector == nullptr)
        return;
    const std::string_view text = tad::sector_text(*sector);
    json::JsonError problem;
    const std::optional<json::Json> root = json::parse_json(text, problem);
    if (!root.has_value() || root->type() != json::JsonType::object)
        return;
    const json::Json* list = root->find("mutators");
    if (list == nullptr || list->type() != json::JsonType::array)
        return;
    for (const json::Json& element : list->elements()) {
        if (element.type() != json::JsonType::object)
            continue;
        const json::Json* id = element.find("id");
        const json::Json* hash = element.find("sha256");
        const std::string* id_text = id == nullptr ? nullptr : id->string();
        const std::string* hash_text = hash == nullptr ? nullptr : hash->string();
        if (id_text == nullptr || id_text->empty() || hash_text == nullptr || hash_text->empty())
            continue;
        ContentNeed one;
        one.kind = "oamut";
        one.id = *id_text;
        one.sha256 = *hash_text;
        const json::Json* version = element.find("version");
        if (version != nullptr && version->string() != nullptr)
            one.version = *version->string();
        keep_need(replay, std::move(one));
    }
}

/// Fills gaps the mutators sector did not, from the note beside a `.tad`.
void take_content_note(const fs::path& file, Replay& replay) {
    fs::path note = file;
    note += saves::content_note_suffix;
    std::ifstream in(note, std::ios::binary);
    if (!in)
        return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>{});
    const saves::ContentRecord record = saves::content_record_from_text(text);
    if (!record.present)
        return;
    if (!record.mod.empty()) {
        bool named = false;
        for (const ContentNeed& need : replay.needs) {
            if (need.kind == "oamod")
                named = true;
        }
        if (!named) {
            ContentNeed one;
            one.kind = "oamod";
            one.name = record.mod;
            keep_need(replay, std::move(one));
        }
    }
    for (const std::string& id : record.expansions) {
        bool named = false;
        for (const ContentNeed& need : replay.needs) {
            if (need.kind == "expansion" && need.id == id)
                named = true;
        }
        if (!named) {
            ContentNeed one;
            one.kind = "expansion";
            one.id = id;
            one.name = saves::bundle_english_name(id);
            keep_need(replay, std::move(one));
        }
    }
    for (const saves::ContentMutator& item : record.mutators) {
        ContentNeed* found = nullptr;
        for (ContentNeed& need : replay.needs) {
            if (need.kind == "oamut" && need.id == item.id)
                found = &need;
        }
        if (found != nullptr) {
            if (found->name.empty())
                found->name = item.name;
            if (found->sha256.empty())
                found->sha256 = ascii_lower(item.sha256);
            if (found->version.empty())
                found->version = item.version;
            continue;
        }
        ContentNeed one;
        one.kind = "oamut";
        one.id = item.id;
        one.name = item.name;
        one.version = item.version;
        one.sha256 = item.sha256;
        keep_need(replay, std::move(one));
    }
}

/// Appends one character as UTF-8.
///
/// @param[in,out] out the text written so far
/// @param code the character, below U+10000
void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80U) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

/// Returns a recording's text as UTF-8.
///
/// The text ends at its first NUL. A text that is UTF-8 is kept as it is; any
/// other is read as Windows-1252.
///
/// @param bytes the text as recorded
/// @return the text, UTF-8
std::string recorded_text(std::string_view bytes) {
    const std::size_t end = bytes.find('\0');
    if (end != std::string_view::npos)
        bytes = bytes.substr(0, end);
    if (json::utf8_valid(bytes))
        return std::string(bytes);
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const char letter : bytes) {
        const auto byte = static_cast<uint8_t>(letter);
        uint32_t code = byte;
        if (byte >= 0x80U && byte <= 0x9FU)
            code = windows_1252_high[static_cast<std::size_t>(byte - 0x80U)];
        append_utf8(text, code);
    }
    return text;
}

/// Reads a run of decimal digits.
///
/// @param text the text
/// @param at the first digit
/// @param count how many digits
/// @return the number, or nothing when a character is not a digit
std::optional<uint32_t> digits_at(std::string_view text, std::size_t at, std::size_t count) {
    uint32_t value = 0;
    for (std::size_t offset = 0; offset < count; ++offset) {
        const char digit = text[at + offset];
        if (digit < '0' || digit > '9')
            return std::nullopt;
        value = value * 10U + static_cast<uint32_t>(digit - '0');
    }
    return value;
}

/// Tells how many days a month has.
///
/// @param year the year
/// @param month the month, 1 to 12
/// @return the days, 28 to 31
uint32_t days_in_month(uint32_t year, uint32_t month) noexcept {
    constexpr std::array<uint32_t, 12> days = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4U == 0U && year % 100U != 0U) || year % 400U == 0U;
    if (month == 2U && leap)
        return 29;
    return days[month - 1U];
}

/// Reads an instant written `YYYY-MM-DDTHH:MM:SSZ`.
///
/// @param text the text
/// @return seconds since 1970 (UTC), or nothing when the text is not such an
///         instant of a real date and time
std::optional<int64_t> utc_seconds(std::string_view text) {
    if (text.size() != 20 || text[4] != '-' || text[7] != '-' || text[10] != 'T' ||
        text[13] != ':' || text[16] != ':' || text[19] != 'Z')
        return std::nullopt;
    const std::optional<uint32_t> year = digits_at(text, 0, 4);
    const std::optional<uint32_t> month = digits_at(text, 5, 2);
    const std::optional<uint32_t> day = digits_at(text, 8, 2);
    const std::optional<uint32_t> hour = digits_at(text, 11, 2);
    const std::optional<uint32_t> minute = digits_at(text, 14, 2);
    const std::optional<uint32_t> second = digits_at(text, 17, 2);
    if (!year || !month || !day || !hour || !minute || !second)
        return std::nullopt;
    if (*month < 1U || *month > 12U || *day < 1U || *day > days_in_month(*year, *month) ||
        *hour > 23U || *minute > 59U || *second > 59U)
        return std::nullopt;
    const int64_t days = day_number(static_cast<int32_t>(*year), *month, *day);
    return days * seconds_per_day + static_cast<int64_t>(*hour) * 3600 +
           static_cast<int64_t>(*minute) * 60 + static_cast<int64_t>(*second);
}

/// Reads one string member of a JSON object.
///
/// @param object the object
/// @param name the member's name
/// @return the member's text, or empty when it is missing or not a string
std::string string_member(const json::Json& object, std::string_view name) {
    const json::Json* value = object.find(name);
    if (value == nullptr || value->string() == nullptr)
        return {};
    return *value->string();
}

/// Fills a replay from a `.oademo`'s description.
///
/// @param bytes the whole file
/// @param[in,out] replay the replay; `damage` is set when the file does not read
void read_oademo(std::span<const uint8_t> bytes, Replay& replay) {
    const oademo::DescriptionResult described = oademo::read_description(bytes);
    if (!described.ok()) {
        replay.damage = described.error.empty() ? std::string(file_unreadable) : described.error;
        return;
    }
    const oademo::Meta& meta = *described.meta;
    replay.sha256 = ascii_lower(meta.recording.sha256);
    replay.map = meta.map.name;
    replay.kind = meta.game.kind;
    replay.how = meta.recorded.how;
    if (meta.recorded.at.has_value()) {
        const std::optional<int64_t> at = utc_seconds(*meta.recorded.at);
        if (at.has_value())
            replay.recorded = *at;
    }
    replay.recorded_by = meta.recorded.by.value_or(std::string());
    replay.length_ms = meta.game.length_ms.value_or(0);
    replay.ended = meta.game.ended.value_or(std::string());
    replay.after_crash = meta.recorded.after_crash;
    if (meta.content.mod.has_value()) {
        replay.mod_id = meta.content.mod->id;
        replay.mod_name = meta.content.mod->name;
        replay.mod_version = meta.content.mod->version;
        replay.mod_revision = meta.content.mod->revision;
    }
    replay.expansions = meta.content.expansions;
    replay.mutators = meta.content.mutators;
    if (meta.game.kind_object.has_value()) {
        json::JsonError problem;
        const std::optional<json::Json> object = json::parse_json(*meta.game.kind_object, problem);
        if (object.has_value() && object->type() == json::JsonType::object) {
            replay.planet = string_member(*object, "planet");
            replay.sector = string_member(*object, "sector");
            // A campaign's title is the mission name, as the file writes it.
            // The Library trims trailing spaces when it shows the title. The
            // map stem stays in `map` for a search that still names the file.
            if (meta.game.kind == kind_campaign)
                replay.mission_name = string_member(*object, "mission_name");
        }
    }
    replay.players.reserve(meta.players.size());
    int32_t place = 1;
    for (const oademo::PlayerInfo& info : meta.players) {
        Player player;
        player.name = info.name;
        player.colour = static_cast<int32_t>(info.colour);
        player.side = info.side;
        if (info.team.has_value())
            player.team = static_cast<int32_t>(*info.team);
        player.kind = info.kind;
        player.recorded_here = info.recorded_here.value_or(false);
        player.number = info.number == 0 ? place : static_cast<int32_t>(info.number);
        replay.players.push_back(std::move(player));
        if (place < std::numeric_limits<int32_t>::max())
            ++place;
    }
    if (described.manifest.has_value())
        apply_manifest(*described.manifest, meta, replay);
    else if (!replay.map.empty()) {
        ContentNeed map_need;
        map_need.kind = "oamap";
        map_need.map_name = replay.map;
        keep_need(replay, std::move(map_need));
    }
    replay.needs_read = true;
}

/// Returns the side a `.tad` records, as text.
///
/// @param side the recorded side
/// @return arm for 0, core for 1, otherwise the number
std::string side_text(uint8_t side) {
    if (side == static_cast<uint8_t>(tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

/// Fills a replay from a `.tad`'s header, players, sectors and packet times.
///
/// @param bytes the whole file
/// @param[in,out] replay the replay; `damage` is set when the file does not parse
void read_tad(std::span<const uint8_t> bytes, Replay& replay) {
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok()) {
        replay.damage = std::string(tad_unreadable);
        return;
    }
    const tad::Demo& demo = *parsed.demo;
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    replay.sha256.assign(hex.begin(), hex.end());
    replay.map = recorded_text(demo.map_name);
    replay.kind = std::string(kind_multiplayer);
    if (!demo.packets.empty())
        replay.length_ms = demo.packets.back().time_ms;
    const tad::Sector* recorder = tad::find_sector(demo, tad::SectorType::recorder_version);
    if (recorder != nullptr)
        replay.recorded_by = recorded_text(tad::sector_text(*recorder));
    replay.players.reserve(demo.players.size());
    int32_t place = 1;
    for (const tad::Player& recorded_player : demo.players) {
        Player player;
        player.name = recorded_text(recorded_player.name);
        player.colour = recorded_player.color;
        player.side = side_text(recorded_player.side);
        player.kind = "human";
        player.number =
            recorded_player.number == 0 ? place : static_cast<int32_t>(recorded_player.number);
        replay.players.push_back(std::move(player));
        if (place < std::numeric_limits<int32_t>::max())
            ++place;
    }
    take_mutator_sector(demo, replay);
    if (!replay.map.empty()) {
        ContentNeed map_need;
        map_need.kind = "oamap";
        map_need.map_name = replay.map;
        keep_need(replay, std::move(map_need));
    }
    replay.needs_read = true;
}

/// Fills a replay from its file's bytes.
///
/// `recorded` is changed only when a `.oademo` names `recorded.at`.
///
/// @param bytes the whole file
/// @param kind which reader reads it
/// @param[in,out] replay the replay
void read_into(std::span<const uint8_t> bytes, FileKind kind, Replay& replay) {
    replay.file = kind;
    try {
        if (kind == FileKind::tad)
            read_tad(bytes, replay);
        else
            read_oademo(bytes, replay);
    } catch (const std::bad_alloc&) {
        replay.damage = std::string(file_unreadable);
    }
}

/// Clears what a reader filled and marks a replay damaged.
///
/// @param[in,out] replay the replay
/// @param why the sentence that says why the file was not read
void mark_damaged(Replay& replay, std::string_view why) {
    Replay plain;
    plain.path = std::move(replay.path);
    plain.size = replay.size;
    plain.modified = replay.modified;
    plain.recorded = replay.modified;
    plain.file = replay.file;
    plain.damage = std::string(why);
    plain.needs_read = true;
    replay = std::move(plain);
}

/// Returns the file a path below Recordings names.
///
/// @param recordings the Recordings folder
/// @param path the file, '/' between folders, UTF-8
/// @return the file, or nothing when a part is empty, `.` or `..`, or names a
///         drive, a root or more than one folder
std::optional<fs::path> file_below(const fs::path& recordings, std::string_view path) {
    if (path.empty())
        return std::nullopt;
    fs::path file = recordings;
    std::string_view rest = path;
    for (;;) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..")
            return std::nullopt;
        const fs::path piece = oa::platform::path_from_utf8(part);
        if (piece.has_root_path() || piece.filename() != piece)
            return std::nullopt;
        file /= piece;
        if (slash == std::string_view::npos)
            return file;
        rest = rest.substr(slash + 1);
    }
}

/// How reading a whole file went.
enum class Loaded : uint8_t {
    read,       ///< `bytes` holds the file
    unreadable, ///< the file did not open or read
    too_large,  ///< the file holds more than most_file_bytes
};

/// Reads a whole file, at most most_file_bytes.
///
/// @param file the file
/// @param[out] bytes the file's bytes
/// @return how the read went
Loaded load_file(const fs::path& file, std::vector<uint8_t>& bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Loaded::unreadable;
    bytes.clear();
    std::vector<char> chunk(read_chunk_bytes);
    for (;;) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            if (bytes.size() + static_cast<std::size_t>(got) > most_file_bytes)
                return Loaded::too_large;
            const auto* first = reinterpret_cast<const uint8_t*>(chunk.data());
            bytes.insert(bytes.end(), first, first + got);
        }
        if (in.eof())
            return Loaded::read;
        if (!in)
            return Loaded::unreadable;
    }
}

/// Compares a listed or indexed path with another, in byte order.
///
/// @param left a replay
/// @param right a path
/// @return true when the replay's path sorts first
bool path_before(const Replay* left, std::string_view right) noexcept {
    return std::string_view(left->path) < right;
}

} // namespace

void apply_manifest(const oademo::Manifest& manifest, const oademo::Meta& meta, Replay& replay) {
    replay.engine_recorded = manifest.engine.recorded_with;
    replay.engine_needs = manifest.engine.needs;
    bool named_map = false;
    for (const oademo::Need& need : manifest.needs) {
        if (need.kind == "oamap") {
            named_map = true;
            if (need.maps.empty()) {
                ContentNeed one;
                one.kind = "oamap";
                one.id = need.id;
                one.version = need.version.value_or(std::string{});
                one.revision = need.revision;
                one.sha256 = need.sha256.value_or(std::string{});
                keep_need(replay, std::move(one));
                continue;
            }
            for (const oademo::MapNeed& map : need.maps) {
                ContentNeed one;
                one.kind = "oamap";
                one.id = need.id;
                one.version = need.version.value_or(std::string{});
                one.revision = need.revision;
                one.sha256 = need.sha256.value_or(std::string{});
                one.map_name = map.name;
                keep_need(replay, std::move(one));
            }
            continue;
        }
        ContentNeed one;
        one.kind = need.kind;
        one.id = need.id;
        one.version = need.version.value_or(std::string{});
        one.revision = need.revision;
        one.sha256 = need.sha256.value_or(std::string{});
        if (need.kind == "oamod" && meta.content.mod.has_value() && meta.content.mod->id == need.id)
            one.name = meta.content.mod->name;
        if (need.kind == "expansion")
            one.name = saves::bundle_english_name(need.id);
        keep_need(replay, std::move(one));
    }
    if (!named_map && !replay.map.empty()) {
        ContentNeed map;
        map.kind = "oamap";
        map.map_name = replay.map;
        keep_need(replay, std::move(map));
    }
}

bool damaged(const Replay& replay) noexcept {
    return !replay.damage.empty();
}

bool imported(const Replay& replay) noexcept {
    const std::string_view path = replay.path;
    return path.size() > imported_folder_name.size() &&
           path.substr(0, imported_folder_name.size()) == imported_folder_name &&
           path[imported_folder_name.size()] == '/';
}

std::optional<FileKind> file_kind_of(std::string_view name) noexcept {
    if (ends_with_ascii(name, oademo_ending))
        return FileKind::oademo;
    if (ends_with_ascii(name, tad_ending))
        return FileKind::tad;
    return std::nullopt;
}

Replay read_replay(std::span<const uint8_t> bytes, FileKind kind) {
    Replay replay;
    read_into(bytes, kind, replay);
    if (!replay.damage.empty())
        mark_damaged(replay, std::string(replay.damage));
    return replay;
}

Replay read_file(const fs::path& recordings, std::string_view path) {
    Replay replay;
    replay.path = std::string(path);
    const std::optional<FileKind> named = file_kind_of(path);
    replay.file = named.value_or(FileKind::oademo);
    const std::optional<fs::path> file = file_below(recordings, path);
    if (!file.has_value()) {
        mark_damaged(replay, file_unreadable);
        return replay;
    }
    std::error_code error;
    const uint64_t size = fs::file_size(*file, error);
    if (error) {
        mark_damaged(replay, file_unreadable);
        return replay;
    }
    const fs::file_time_type written = fs::last_write_time(*file, error);
    if (error) {
        mark_damaged(replay, file_unreadable);
        return replay;
    }
    replay.size = size;
    replay.modified = seconds_since_1970(written);
    replay.recorded = replay.modified;
    if (size > most_file_bytes) {
        mark_damaged(replay, file_too_large);
        return replay;
    }
    std::vector<uint8_t> bytes;
    Loaded loaded = Loaded::unreadable;
    try {
        loaded = load_file(*file, bytes);
    } catch (const std::bad_alloc&) {
        loaded = Loaded::unreadable;
    }
    if (loaded != Loaded::read) {
        mark_damaged(replay, loaded == Loaded::too_large ? file_too_large : file_unreadable);
        return replay;
    }
    const FileKind kind =
        named.value_or(oademo::recognised(bytes) ? FileKind::oademo : FileKind::tad);
    read_into(bytes, kind, replay);
    if (!replay.damage.empty()) {
        mark_damaged(replay, std::string(replay.damage));
        return replay;
    }
    if (kind == FileKind::tad)
        take_content_note(*file, replay);
    return replay;
}

namespace {

/// Lists one folder's replay files, and the folders below it while they are
/// within most_folder_depth.
///
/// @param folder the folder
/// @param prefix the folder's path below Recordings, ending '/', or empty for Recordings
/// @param depth how many folders below Recordings `folder` is
/// @param[in,out] out the files found so far
void walk(
    const fs::path& folder, const std::string& prefix, std::size_t depth, std::vector<Listed>& out
) {
    std::error_code error;
    fs::directory_iterator entries(folder, fs::directory_options::none, error);
    const fs::directory_iterator end;
    while (!error && entries != end) {
        const fs::directory_entry& entry = *entries;
        const std::string name = oa::platform::path_to_utf8(entry.path().filename());
        std::error_code status_error;
        const fs::file_status status = entry.symlink_status(status_error);
        if (!name.empty() && name.front() != '.' && !status_error && !fs::is_symlink(status)) {
            if (fs::is_directory(status)) {
                if (depth < most_folder_depth)
                    walk(entry.path(), prefix + name + "/", depth + 1, out);
            } else if (fs::is_regular_file(status)) {
                const std::optional<FileKind> kind = file_kind_of(name);
                std::error_code size_error;
                std::error_code time_error;
                const uint64_t size = kind.has_value() ? entry.file_size(size_error) : 0;
                const fs::file_time_type written =
                    kind.has_value() ? entry.last_write_time(time_error) : fs::file_time_type{};
                if (kind.has_value() && !size_error && !time_error) {
                    Listed listed;
                    listed.path = prefix + name;
                    listed.size = size;
                    listed.modified = seconds_since_1970(written);
                    listed.file = *kind;
                    out.push_back(std::move(listed));
                }
            }
        }
        entries.increment(error);
    }
}

} // namespace

std::vector<Listed> list_folder(const fs::path& recordings) {
    std::vector<Listed> out;
    walk(recordings, std::string(), 0, out);
    std::sort(out.begin(), out.end(), [](const Listed& left, const Listed& right) {
        return left.path < right.path;
    });
    return out;
}

Index scan(const fs::path& recordings, const Index& previous, const ScanHooks& hooks) {
    const std::vector<Listed> files = list_folder(recordings);
    std::vector<const Replay*> known;
    known.reserve(previous.replays.size());
    for (const Replay& replay : previous.replays)
        known.push_back(&replay);
    std::stable_sort(known.begin(), known.end(), [](const Replay* left, const Replay* right) {
        return left->path < right->path;
    });
    Index result;
    result.replays.reserve(files.size());
    for (std::size_t at = 0; at < files.size(); ++at) {
        if (hooks.stop != nullptr && hooks.stop(hooks.context))
            break;
        const Listed& listed = files[at];
        const auto place = std::lower_bound(known.begin(), known.end(), listed.path, path_before);
        const Replay* before =
            place != known.end() && (*place)->path == listed.path ? *place : nullptr;
        if (before != nullptr && before->size == listed.size &&
            before->modified == listed.modified && before->file == listed.file &&
            before->needs_read) {
            result.replays.push_back(*before);
        } else {
            Replay replay = read_file(recordings, listed.path);
            if (before != nullptr) {
                replay.kept = before->kept;
                // The file is read again for its needs. A timeline, a watched
                // tick and a refusal stamp written onto the index stay when
                // the file itself has not changed.
                if (before->size == listed.size && before->modified == listed.modified &&
                    before->file == listed.file) {
                    replay.timeline = before->timeline;
                    replay.watched_tick = before->watched_tick;
                    replay.read_stamp = before->read_stamp;
                }
            }
            result.replays.push_back(std::move(replay));
        }
        if (hooks.replay != nullptr)
            hooks.replay(hooks.context, result.replays.back(), at + 1, files.size());
    }
    return result;
}

bool set_kept(Index& index, std::span<const std::string> paths, bool kept) {
    std::vector<std::string_view> marked(paths.begin(), paths.end());
    std::sort(marked.begin(), marked.end());
    bool changed = false;
    for (Replay& replay : index.replays) {
        if (replay.kept == kept ||
            !std::binary_search(marked.begin(), marked.end(), std::string_view(replay.path)))
            continue;
        replay.kept = kept;
        changed = true;
    }
    return changed;
}

const Replay* find(const Index& index, std::string_view path) noexcept {
    const auto place = std::lower_bound(
        index.replays.begin(),
        index.replays.end(),
        path,
        [](const Replay& replay, std::string_view wanted) {
            return std::string_view(replay.path) < wanted;
        }
    );
    if (place != index.replays.end() && place->path == path)
        return &*place;
    // An index that is not sorted is still searched, one entry at a time.
    for (const Replay& replay : index.replays)
        if (replay.path == path)
            return &replay;
    return nullptr;
}

std::optional<int64_t> kept_set_aside_at(const fs::path& recordings) {
    const std::string prefix = std::string(kept_name) + std::string(damaged_ending);
    std::optional<int64_t> newest;
    std::error_code error;
    fs::directory_iterator entries(kept_path(recordings).parent_path(), error);
    const fs::directory_iterator end;
    for (; !error && entries != end; entries.increment(error)) {
        const std::string name = oa::platform::path_to_utf8(entries->path().filename());
        std::error_code kind_error;
        if (!name.starts_with(prefix) || !entries->is_regular_file(kind_error) || kind_error)
            continue;
        // A copy whose time cannot be read could have been set aside at any
        // time, so it spares every replay.
        std::error_code time_error;
        const fs::file_time_type written = entries->last_write_time(time_error);
        const int64_t at =
            time_error ? std::numeric_limits<int64_t>::max() : seconds_since_1970(written);
        if (!newest || at > *newest)
            newest = at;
    }
    return newest;
}

namespace {

bool ends_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size())
        return false;
    for (std::size_t at = 0; at < suffix.size(); ++at) {
        unsigned char left = static_cast<unsigned char>(text[text.size() - suffix.size() + at]);
        unsigned char right = static_cast<unsigned char>(suffix[at]);
        if (left >= 'A' && left <= 'Z')
            left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right)
            return false;
    }
    return true;
}

std::string strip_replay_extension(std::string name) {
    if (ends_ci(name, ".oademo"))
        name.resize(name.size() - std::string_view(".oademo").size());
    else if (ends_ci(name, ".tad"))
        name.resize(name.size() - std::string_view(".tad").size());
    return name;
}

bool reserved_stem(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const unsigned char character : name) {
        const unsigned char folded = character >= 'A' && character <= 'Z'
                                         ? static_cast<unsigned char>(character - 'A' + 'a')
                                         : character;
        lower.push_back(static_cast<char>(folded));
    }
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
        return true;
    if (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt"))) {
        const char digit = lower[3];
        return digit >= '1' && digit <= '9';
    }
    return false;
}

} // namespace

std::string recording_stem(std::string_view title) {
    constexpr std::size_t cap = 120;
    std::string name;
    name.reserve(title.size());
    for (const unsigned char character : title) {
        const bool bad = character < 0x20 || character == 0x7F || character == '/' ||
                         character == '\\' || character == ':' || character == '*' ||
                         character == '?' || character == '"' || character == '<' ||
                         character == '>' || character == '|';
        name.push_back(bad ? '_' : static_cast<char>(character));
    }
    std::size_t begin = 0;
    while (begin < name.size() && (name[begin] == ' ' || name[begin] == '.'))
        ++begin;
    std::size_t end = name.size();
    while (end > begin && (name[end - 1] == ' ' || name[end - 1] == '.'))
        --end;
    name = name.substr(begin, end - begin);
    name = strip_replay_extension(std::move(name));
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.size() > cap) {
        end = cap;
        // A continuation byte is 10xxxxxx. Walking back drops the character
        // the cap cut through, and leaves the one before it whole.
        while (end > 0 && (static_cast<unsigned char>(name[end]) & 0xC0U) == 0x80U)
            --end;
        name.resize(end);
    }
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty())
        name = "replay";
    if (reserved_stem(name))
        name.push_back('_');
    return name;
}

} // namespace oa::data::replays
