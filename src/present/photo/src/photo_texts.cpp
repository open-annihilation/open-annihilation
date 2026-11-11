// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/photo/photo_texts.hpp"

#include "oa/formats/json.hpp"
#include "oa/present/game_text.hpp"

#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace oa::present::photo {
namespace {

/// Reads game text as UTF-8. A NUL is dropped: a text chunk cannot hold one.
///
/// @param bytes the game text
/// @return UTF-8, with no NUL
std::string game_utf8(std::string_view bytes) {
    std::string cleaned;
    cleaned.reserve(bytes.size());
    for (const char byte : bytes) {
        if (byte != '\0')
            cleaned.push_back(byte);
    }
    return oa::present::decode_game_text(cleaned, true);
}

/// The file's own name, after the last slash or backslash.
///
/// @param path a path or a file name
/// @return the file name, as UTF-8
std::string file_name_only(std::string_view path) {
    const std::size_t cut = path.find_last_of("/\\");
    const std::string_view name = cut == std::string_view::npos ? path : path.substr(cut + 1);
    return game_utf8(name);
}

/// A number as JSON text, with a dot when it has a fraction. Not finite, and
/// a magnitude past a million million, are written as 0. Trailing zeros of a
/// fraction are dropped, and a value that rounds to zero is 0, never -0.
///
/// @param value the number
/// @return the JSON number
std::string decimal_text(double value) {
    if (!std::isfinite(value) || value >= 1.0e12 || value <= -1.0e12)
        return "0";
    const long long scaled = std::llround(value * 1000000.0);
    if (scaled == 0)
        return "0";
    unsigned long long magnitude = 0;
    bool negative = false;
    if (scaled < 0) {
        if (scaled == std::numeric_limits<long long>::min())
            return "0";
        negative = true;
        magnitude = static_cast<unsigned long long>(-scaled);
    } else {
        magnitude = static_cast<unsigned long long>(scaled);
    }
    const unsigned long long whole = magnitude / 1000000ULL;
    unsigned long long fraction = magnitude % 1000000ULL;
    std::string text;
    if (negative)
        text.push_back('-');
    text += std::to_string(whole);
    if (fraction == 0)
        return text;
    text.push_back('.');
    char digits[6];
    for (int place = 5; place >= 0; --place) {
        digits[place] = static_cast<char>('0' + static_cast<int>(fraction % 10ULL));
        fraction /= 10ULL;
    }
    int last = 5;
    while (last >= 0 && digits[last] == '0')
        --last;
    text.append(digits, static_cast<std::size_t>(last + 1));
    return text;
}

/// The battle's clock. Thirty ticks are one second. Under an hour the text
/// is minutes and seconds, such as 3:41. From an hour it is hours, minutes
/// and seconds, such as 1:03:41.
///
/// @param tick the match's tick
/// @return the clock
std::string clock_text(uint32_t tick) {
    const uint32_t seconds = tick / 30U;
    const uint32_t second = seconds % 60U;
    const uint32_t minute = (seconds / 60U) % 60U;
    const uint32_t hour = seconds / 3600U;
    const auto pair = [](uint32_t value) {
        const std::string text = std::to_string(value);
        return text.size() >= 2 ? text : std::string("0") + text;
    };
    if (hour == 0U)
        return std::to_string(minute) + ":" + pair(second);
    return std::to_string(hour) + ":" + pair(minute) + ":" + pair(second);
}

/// Two digits, zero-padded. A negative value is written as 00.
///
/// @param value the number
/// @return the digits
std::string two_digits(int value) {
    if (value < 0)
        value = 0;
    const std::string text = std::to_string(value);
    if (text.size() >= 2)
        return text;
    return std::string("0") + text;
}

/// At least four digits for a year. A negative year is written as 0000.
///
/// @param year the year
/// @return the digits
std::string year_text(int year) {
    if (year < 0)
        year = 0;
    std::string text = std::to_string(year);
    while (text.size() < 4)
        text.insert(text.begin(), '0');
    return text;
}

/// Days from 1970-01-01, Howard Hinnant's civil calendar. The month shift
/// wraps in unsigned arithmetic, which is the formula for months 1 to 12.
///
/// @param year the full year
/// @param month the month, 1 to 12
/// @param day the day of the month
/// @return the days
int days_from_civil(int year, unsigned month, unsigned day) noexcept {
    year -= month <= 2U ? 1 : 0;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
    const unsigned shifted = month + (month > 2U ? static_cast<unsigned>(-3) : 9U);
    const unsigned day_of_year = (153U * shifted + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return era * 146097 + static_cast<int>(day_of_era) - 719468;
}

/// An RFC 1123 date in GMT, such as Sun, 06 Nov 1994 08:49:37 GMT. The
/// weekday comes from the date, not from a clock.
///
/// @param moment the UTC date and time
/// @return the date
std::string rfc1123(const UtcMoment& moment) {
    static constexpr const char* kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static constexpr const char* kMonths[] = {
        "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
    };
    const unsigned month_number =
        moment.month >= 1 && moment.month <= 12 ? static_cast<unsigned>(moment.month) : 1U;
    const unsigned day_number =
        moment.day >= 1 && moment.day <= 31 ? static_cast<unsigned>(moment.day) : 1U;
    int days = days_from_civil(moment.year, month_number, day_number);
    int day_index = days % 7;
    if (day_index < 0)
        day_index += 7;
    day_index = (day_index + 4) % 7;
    std::string text = kDays[day_index];
    text += ", ";
    text += two_digits(static_cast<int>(day_number));
    text += " ";
    text += kMonths[month_number - 1U];
    text += " ";
    text += year_text(moment.year);
    text += " ";
    text += two_digits(moment.hour);
    text += ":";
    text += two_digits(moment.minute);
    text += ":";
    text += two_digits(moment.second);
    text += " GMT";
    return text;
}

/// The battle's kind as a photo's Source line shows it.
///
/// @param kind the kind
/// @return Skirmish, Campaign, Replay or Multiplayer
const char* kind_label(GameKind kind) noexcept {
    switch (kind) {
    case GameKind::campaign:
        return "Campaign";
    case GameKind::replay:
        return "Replay";
    case GameKind::multiplayer:
        return "Multiplayer";
    case GameKind::skirmish:
        return "Skirmish";
    }
    return "Skirmish";
}

/// The battle's kind as the JSON record spells it.
///
/// @param kind the kind
/// @return the lower-case kind
const char* kind_id(GameKind kind) noexcept {
    switch (kind) {
    case GameKind::campaign:
        return "campaign";
    case GameKind::replay:
        return "replay";
    case GameKind::multiplayer:
        return "multiplayer";
    case GameKind::skirmish:
        return "skirmish";
    }
    return "skirmish";
}

/// The description. A team is named only for a player who has one.
///
/// @param facts the photo's facts
/// @return the one line
std::string description_text(const PhotoFacts& facts) {
    std::string line;
    for (const PlayerFact& player : facts.players) {
        if (!line.empty())
            line += " vs ";
        std::string side = game_utf8(player.side);
        if (player.team.has_value())
            side += ", team " + std::to_string(*player.team);
        if (facts.player_names)
            line += game_utf8(player.name) + " (" + side + ")";
        else
            line += side;
    }
    return line;
}

/// The OpenAnnihilation record, version 1, compact JSON.
///
/// @param facts the photo's facts
/// @param clock the game time already formatted
/// @return the JSON text
std::string record_text(const PhotoFacts& facts, const std::string& clock) {
    oa::formats::json::JsonWriter writer;
    writer.begin_object();
    writer.key("photo");
    writer.integer(1);
    writer.key("engine");
    writer.string(game_utf8(facts.engine));
    writer.key("mod");
    writer.begin_object();
    writer.key("id");
    writer.string(game_utf8(facts.mod_id));
    writer.key("version");
    writer.string(game_utf8(facts.mod_version));
    writer.end_object();
    writer.key("map");
    writer.begin_object();
    writer.key("name");
    writer.string(game_utf8(facts.map));
    writer.end_object();
    writer.key("game");
    writer.begin_object();
    writer.key("kind");
    writer.string(kind_id(facts.kind));
    writer.key("tick");
    writer.integer(static_cast<int64_t>(facts.tick));
    writer.key("time");
    writer.string(clock);
    writer.key("speed");
    writer.integer(facts.speed);
    writer.end_object();
    writer.key("players");
    writer.begin_array();
    for (const PlayerFact& player : facts.players) {
        writer.begin_object();
        if (facts.player_names) {
            writer.key("name");
            writer.string(game_utf8(player.name));
        }
        writer.key("side");
        writer.string(game_utf8(player.side));
        writer.key("colour");
        writer.integer(player.colour);
        writer.key("team");
        if (player.team.has_value())
            writer.integer(*player.team);
        else
            writer.null();
        writer.end_object();
    }
    writer.end_array();
    if (facts.replay.has_value()) {
        writer.key("replay");
        writer.begin_object();
        if (facts.player_names) {
            writer.key("file");
            writer.string(file_name_only(facts.replay->file));
        }
        writer.key("sha256");
        writer.string(game_utf8(facts.replay->sha256));
        writer.key("tick");
        writer.integer(facts.replay->tick);
        writer.end_object();
    }
    writer.key("camera");
    writer.begin_object();
    writer.key("x");
    writer.integer(facts.camera.x);
    writer.key("z");
    writer.integer(facts.camera.z);
    writer.key("zoom");
    writer.raw(decimal_text(facts.camera.zoom));
    writer.key("yaw");
    writer.raw(decimal_text(facts.camera.yaw));
    writer.key("pitch");
    writer.raw(decimal_text(facts.camera.pitch));
    writer.end_object();
    if (facts.look.has_value()) {
        writer.key("look");
        writer.begin_object();
        writer.key("preset");
        writer.string(game_utf8(facts.look->preset));
        writer.key("vignette");
        writer.integer(facts.look->vignette);
        writer.key("roundness");
        writer.integer(facts.look->roundness);
        writer.key("grain");
        writer.integer(facts.look->grain);
        writer.key("grain_size");
        writer.integer(facts.look->grain_size);
        writer.key("grain_pixels");
        writer.integer(facts.look->grain_pixels);
        writer.key("frame");
        writer.string(facts.look->frame.empty() ? "none" : game_utf8(facts.look->frame));
        writer.key("tilt_shift");
        if (facts.look->tilt_shift.has_value()) {
            const TiltShiftFact& tilt = *facts.look->tilt_shift;
            writer.begin_object();
            writer.key("centre");
            writer.integer(tilt.centre);
            writer.key("width");
            writer.integer(tilt.width);
            writer.key("width_pixels");
            writer.integer(tilt.width_pixels);
            writer.key("radius");
            writer.integer(tilt.radius);
            writer.key("radius_pixels");
            writer.integer(tilt.radius_pixels);
            writer.end_object();
        } else {
            writer.null();
        }
        writer.key("haze");
        if (facts.look->haze.has_value())
            writer.integer(*facts.look->haze);
        else
            writer.null();
        writer.key("depth_of_field");
        if (facts.look->depth_of_field.has_value()) {
            const DepthOfFieldFact& field = *facts.look->depth_of_field;
            writer.begin_object();
            writer.key("radius");
            writer.integer(field.radius);
            writer.key("radius_pixels");
            writer.integer(field.radius_pixels);
            writer.end_object();
        } else {
            writer.null();
        }
        writer.end_object();
    }
    writer.key("size");
    writer.begin_object();
    writer.key("width");
    writer.integer(facts.size.width);
    writer.key("height");
    writer.integer(facts.size.height);
    writer.key("smooth");
    writer.integer(facts.size.smooth);
    writer.end_object();
    writer.end_object();
    return writer.text();
}

/// Appends one chunk.
///
/// @param[in,out] chunks the list
/// @param kind tEXt or iTXt
/// @param keyword the keyword
/// @param text the text
void add_chunk(
    std::vector<PhotoText>& chunks, ChunkKind kind, std::string keyword, std::string text
) {
    PhotoText chunk;
    chunk.kind = kind;
    chunk.keyword = std::move(keyword);
    chunk.text = std::move(text);
    chunks.push_back(std::move(chunk));
}

} // namespace

std::vector<PhotoText> photo_texts(const PhotoFacts& facts) {
    // shots_folder, account and host are never read into a chunk.
    const std::string clock = clock_text(facts.tick);
    const std::string map = game_utf8(facts.map);
    std::string title = map.empty() ? clock : map + ", " + clock;
    std::vector<PhotoText> chunks;
    chunks.reserve(6);
    add_chunk(chunks, ChunkKind::utf8, "Title", std::move(title));
    add_chunk(chunks, ChunkKind::utf8, "Description", description_text(facts));
    add_chunk(
        chunks, ChunkKind::latin1, "Software", "Open Annihilation " + game_utf8(facts.engine)
    );
    add_chunk(chunks, ChunkKind::latin1, "Creation Time", rfc1123(facts.created));
    add_chunk(chunks, ChunkKind::latin1, "Source", kind_label(facts.kind));
    add_chunk(chunks, ChunkKind::utf8, "OpenAnnihilation", record_text(facts, clock));
    return chunks;
}

} // namespace oa::present::photo
