// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Replays section's model: which replays show, in which order, grouped
// by day, and what their rows and the details card say.

#include "oa/ui/library/replays.hpp"

#include "oa/data/content_bundles.hpp"
#include "oa/ui/library/text.hpp"
#include "visible.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::library {

namespace {

namespace replays_data = oa::data::replays;

/// The most players a row names; more are counted.
constexpr std::size_t most_named_players = 3;
/// Minutes in an hour, and seconds in a minute.
constexpr uint64_t sixty = 60;
/// Milliseconds in a second.
constexpr uint64_t milliseconds_per_second = 1000;
/// Days in a week.
constexpr int32_t week_days = 7;
/// The weekday of 1970-01-01, counted from Monday: a Thursday.
constexpr int32_t first_day_weekday = 3;

/// The weekdays' words, from Monday.
constexpr std::array<std::string_view, 7> weekday_words{
    words::weekday_monday,
    words::weekday_tuesday,
    words::weekday_wednesday,
    words::weekday_thursday,
    words::weekday_friday,
    words::weekday_saturday,
    words::weekday_sunday,
};

/// The months' words, from January.
constexpr std::array<std::string_view, 12> month_words{
    words::month_january,
    words::month_february,
    words::month_march,
    words::month_april,
    words::month_may,
    words::month_june,
    words::month_july,
    words::month_august,
    words::month_september,
    words::month_october,
    words::month_november,
    words::month_december,
};

/// A civil date.
struct CivilDate {
    int32_t year{};   ///< the year
    uint32_t month{}; ///< 1 to 12
    uint32_t day{};   ///< 1 to 31
};

/// Returns the civil date of a day number, the inverse of day_number.
///
/// @param days days since 1970-01-01
/// @return the date
CivilDate civil_of(int32_t days) {
    const int64_t shifted = static_cast<int64_t>(days) + 719468;
    const int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto day_of_era = static_cast<uint32_t>(shifted - era * 146097);
    const uint32_t year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const uint32_t day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const uint32_t month_place = (5 * day_of_year + 2) / 153;
    CivilDate date;
    date.day = day_of_year - (153 * month_place + 2) / 5 + 1;
    date.month = month_place < 10 ? month_place + 3 : month_place - 9;
    date.year = static_cast<int32_t>(
        static_cast<int64_t>(year_of_era) + era * 400 + (date.month <= 2 ? 1 : 0)
    );
    return date;
}

/// The text a `.oademo` names the program that recorded it by, before the version.
constexpr std::string_view program_prefix = "open-annihilation ";

/// Returns the name a replay's row shows for its map: the map's, or for a
/// file that does not say, the file's name.
///
/// @param row the replay
/// @return the name
std::string_view shown_map(const ReplayRow& row) {
    if (!row.replay.map.empty())
        return row.replay.map;
    const std::string_view path = row.replay.path;
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// Writes who recorded a replay as the player knows them: a `.oademo` names
/// the program by its identifier, and the card writes it as "Open Annihilation
/// 0.8.14". A `.tad`'s recorder stays as it was recorded.
///
/// @param library the Library
/// @param replay the replay
/// @return the words; empty when the replay names none
std::string recorded_by_text(const Library& library, const replays_data::Replay& replay) {
    const std::string_view by = replay.recorded_by;
    if (replay.file == replays_data::FileKind::oademo && by.size() > program_prefix.size() &&
        by.substr(0, program_prefix.size()) == program_prefix)
        return filled(
            library.text,
            words::replay_program,
            {{"version", std::string(by.substr(program_prefix.size()))}}
        );
    return std::string(by);
}

/// Writes a label as the card sets it: it begins with a capital. A word that
/// is written in lower case for use inside a sentence ("mod") is a label too.
///
/// @param label the label as looked up
/// @return the label with its first letter capitalised, when it is an ASCII letter
std::string capitalised(std::string_view label) {
    std::string out(label);
    if (!out.empty() && out.front() >= 'a' && out.front() <= 'z')
        out.front() = static_cast<char>(out.front() - 'a' + 'A');
    return out;
}

/// Tells whether a replay is a watcher's, whom the row does not name.
///
/// @param player the player
/// @return true for a watcher
bool watches(const replays_data::Player& player) {
    return player.kind == "watcher" || player.side == "watcher";
}

/// Returns the players a row names: every one who played.
///
/// @param row the replay
/// @return the players, in the recording's order
std::vector<const replays_data::Player*> playing(const ReplayRow& row) {
    std::vector<const replays_data::Player*> shown_players;
    for (const replays_data::Player& player : row.replay.players)
        if (!watches(player))
            shown_players.push_back(&player);
    return shown_players;
}

/// Tells whether one replay comes before another, newest first.
///
/// @param left one replay
/// @param right the other
/// @return true when `left` is shown first
bool newer(const ReplayRow& left, const ReplayRow& right) {
    if (left.day != right.day)
        return left.day > right.day;
    if (left.minute != right.minute)
        return left.minute > right.minute;
    if (left.replay.recorded != right.replay.recorded)
        return left.replay.recorded > right.replay.recorded;
    return left.replay.path < right.replay.path;
}

/// Tells whether the visible list holds a replay.
///
/// @param library the Library
/// @param path the replay's path
/// @return true when its row shows
bool shows(const Library& library, std::string_view path) {
    return replay_place(library, path).has_value();
}

/// Joins two parts with the section's separator.
///
/// @param library the Library
/// @param left the first part
/// @param right the second
/// @return "left · right", or the one part that is not empty
std::string joined(const Library& library, std::string_view left, std::string_view right) {
    if (left.empty())
        return std::string(right);
    if (right.empty())
        return std::string(left);
    return filled(library.text, words::replay_joined, {{"left", left}, {"right", right}});
}

/// Writes a day as a short date: "2 Nov", or "2 Nov 2025" outside this year.
///
/// @param library the Library
/// @param day the day
/// @return the words
std::string date_text(const Library& library, int32_t day) {
    const CivilDate date = civil_of(day);
    const CivilDate now = civil_of(library.inputs.replays.today);
    const int32_t weekday = ((day + first_day_weekday) % week_days + week_days) % week_days;
    const std::string weekday_word(
        shown(library.text, weekday_words[static_cast<std::size_t>(weekday)])
    );
    const std::string month_word(shown(library.text, month_words[(date.month - 1) % 12]));
    const std::string day_digits = std::to_string(date.day);
    if (date.year == now.year)
        return filled(
            library.text,
            words::replay_day,
            {{"weekday", weekday_word}, {"day", day_digits}, {"month", month_word}}
        );
    const std::string year_digits = std::to_string(date.year);
    return filled(
        library.text,
        words::replay_day_year,
        {{"weekday", weekday_word},
         {"day", day_digits},
         {"month", month_word},
         {"year", year_digits}}
    );
}

/// Writes a mod as the card names it: "Ridge 2.8 (rev 1)".
///
/// @param library the Library
/// @param replay the replay
/// @return the words; None without a mod
std::string mod_text(const Library& library, const replays_data::Replay& replay) {
    if (replay.mod_id.empty() && replay.mod_name.empty())
        return std::string(shown(library.text, words::replay_none));
    std::string name = replay.mod_name.empty() ? replay.mod_id : replay.mod_name;
    if (!replay.mod_version.empty())
        name += " " + replay.mod_version;
    if (replay.mod_revision.has_value())
        return filled(
            library.text,
            words::replay_mod_revision,
            {{"mod", name}, {"revision", std::to_string(*replay.mod_revision)}}
        );
    return name;
}

/// Writes a list of names as the card does, or None.
///
/// @param library the Library
/// @param names the names
/// @return the words
std::string names_text(const Library& library, const std::vector<std::string>& names) {
    if (names.empty())
        return std::string(shown(library.text, words::replay_none));
    return join_names(library.text, names);
}

/// Returns a replay's file name, after its last folder.
///
/// @param path the path below Recordings
/// @return the name
std::string_view file_name(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

/// The empty section's line for how long recorded games are kept.
///
/// @param inputs the section's inputs
/// @return the English sentence
std::string_view kept_sentence(const ReplayInputs& inputs) {
    if (inputs.keep_forever)
        return words::replays_kept_forever;
    if (inputs.keep_days == replays_data::keep_span_90)
        return words::replays_kept_90_days;
    if (inputs.keep_days == replays_data::keep_span_year)
        return words::replays_kept_year;
    return words::replays_kept_days;
}

} // namespace

void update_replays(Library& library) {
    ReplayView& view = library.replays;
    const std::optional<std::string> selected_before = view.selected;
    const std::vector<ReplayRow>& rows = library.inputs.replays.rows;
    view.visible.clear();
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const ReplayRow& row = rows[index];
        if (!replay_in_filter(library, row, view.filter))
            continue;
        if (!view.kind.empty() && row.replay.kind != view.kind)
            continue;
        if (!replay_matches(row, library.query))
            continue;
        view.visible.push_back(index);
    }
    const auto order = [&](std::size_t left_index, std::size_t right_index) {
        const ReplayRow& left = rows[left_index];
        const ReplayRow& right = rows[right_index];
        switch (view.sort) {
        case ReplaySort::newest:
            break;
        case ReplaySort::longest:
            if (left.replay.length_ms != right.replay.length_ms)
                return left.replay.length_ms > right.replay.length_ms;
            break;
        case ReplaySort::map: {
            const std::string left_map = folded(shown_map(left));
            const std::string right_map = folded(shown_map(right));
            if (left_map != right_map)
                return left_map < right_map;
            break;
        }
        case ReplaySort::players: {
            const std::size_t left_count = playing(left).size();
            const std::size_t right_count = playing(right).size();
            if (left_count != right_count)
                return left_count > right_count;
            break;
        }
        }
        return newer(left, right);
    };
    std::sort(view.visible.begin(), view.visible.end(), order);

    // The marks keep only what shows; the selection stays while it shows.
    std::erase_if(view.marked, [&](const std::string& path) { return !shows(library, path); });
    if (view.selected && !shows(library, *view.selected))
        view.selected.reset();
    if (!view.selected && !view.visible.empty())
        view.selected = rows[view.visible.front()].replay.path;
    if (view.selected && view.marked.empty())
        view.marked.push_back(*view.selected);
    if (view.anchor && !shows(library, *view.anchor))
        view.anchor.reset();
    // Delete's confirm is about the replay the card showed. Another one clears it.
    if (view.selected != selected_before) {
        view.delete_armed = false;
        view.delete_failure.clear();
    }
}

void set_replay_filter(Library& library, ReplayFilter filter) {
    library.replays.filter = filter;
    update_replays(library);
}

void set_replay_kind(Library& library, std::string_view kind) {
    library.replays.kind = kind;
    update_replays(library);
}

void set_replay_sort(Library& library, ReplaySort sort) {
    library.replays.sort = sort;
    update_replays(library);
}

void select_replay(Library& library, std::string_view path, ReplayPick pick) {
    library.replays.delete_armed = false;
    library.replays.delete_failure.clear();
    update_replays(library);
    ReplayView& view = library.replays;
    const std::optional<std::size_t> place = replay_place(library, path);
    if (!place)
        return;
    const std::string chosen(path);
    if (pick == ReplayPick::extend && view.anchor) {
        const std::optional<std::size_t> from = replay_place(library, *view.anchor);
        if (from) {
            const std::size_t first = std::min(*from, *place);
            const std::size_t last = std::max(*from, *place);
            view.marked.clear();
            for (std::size_t at = first; at <= last; ++at)
                view.marked.push_back(library.inputs.replays.rows[view.visible[at]].replay.path);
            view.selected = chosen;
            return;
        }
    }
    if (pick == ReplayPick::toggle) {
        const auto found = std::find(view.marked.begin(), view.marked.end(), chosen);
        if (found == view.marked.end())
            view.marked.push_back(chosen);
        else if (view.marked.size() > 1)
            view.marked.erase(found);
        view.selected = chosen;
        view.anchor = chosen;
        return;
    }
    view.selected = chosen;
    view.marked = {chosen};
    view.anchor = chosen;
}

void move_replay_selection(Library& library, int delta) {
    library.replays.delete_armed = false;
    library.replays.delete_failure.clear();
    update_replays(library);
    const ReplayView& view = library.replays;
    if (view.visible.empty())
        return;
    std::size_t at = 0;
    if (view.selected) {
        if (const std::optional<std::size_t> place = replay_place(library, *view.selected))
            at = *place;
    }
    const auto last = static_cast<int64_t>(view.visible.size()) - 1;
    const int64_t moved = std::clamp(static_cast<int64_t>(at) + delta, int64_t{0}, last);
    const std::string path =
        library.inputs.replays.rows[view.visible[static_cast<std::size_t>(moved)]].replay.path;
    select_replay(library, path, ReplayPick::only);
}

void refuse_replay(Library& library, std::string_view path, std::string sentence) {
    library.replays.refusal = std::move(sentence);
    library.replays.refused_path = path;
}

const ReplayRow* selected_replay(const Library& library) {
    if (!library.replays.selected)
        return nullptr;
    return find_replay(library, *library.replays.selected);
}

const ReplayRow* find_replay(const Library& library, std::string_view path) {
    for (const ReplayRow& row : library.inputs.replays.rows)
        if (row.replay.path == path)
            return &row;
    return nullptr;
}

std::optional<std::size_t> replay_place(const Library& library, std::string_view path) {
    const ReplayView& view = library.replays;
    const std::vector<ReplayRow>& rows = library.inputs.replays.rows;
    for (std::size_t place = 0; place < view.visible.size(); ++place)
        if (view.visible[place] < rows.size() && rows[view.visible[place]].replay.path == path)
            return place;
    return std::nullopt;
}

bool replay_in_filter(const Library& library, const ReplayRow& row, ReplayFilter filter) {
    const int32_t today = library.inputs.replays.today;
    const bool this_month = row.day > today - month_days;
    switch (filter) {
    case ReplayFilter::this_month:
        return this_month;
    case ReplayFilter::older:
        return !this_month;
    case ReplayFilter::kept:
        return row.replay.kept;
    case ReplayFilter::imported:
        return replays_data::imported(row.replay);
    }
    return false;
}

std::size_t replay_filter_count(const Library& library, ReplayFilter filter) {
    std::size_t count = 0;
    for (const ReplayRow& row : library.inputs.replays.rows)
        if (replay_in_filter(library, row, filter))
            ++count;
    return count;
}

bool replay_matches(const ReplayRow& row, std::string_view query) {
    const std::string search = folded(query);
    std::vector<std::string_view> terms;
    std::size_t at = 0;
    while (at < search.size()) {
        while (at < search.size() && search[at] == ' ')
            ++at;
        std::size_t end = at;
        while (end < search.size() && search[end] != ' ')
            ++end;
        if (end > at)
            terms.push_back(std::string_view(search).substr(at, end - at));
        at = end;
    }
    if (terms.empty())
        return true;
    // The name a row shows (the file's, for a file with no map), the mod, the
    // planet and the players.
    std::vector<std::string> fields{
        folded(shown_map(row)), folded(row.replay.mod_name), folded(row.replay.planet)
    };
    for (const replays_data::Player& player : row.replay.players)
        fields.push_back(folded(player.name));
    for (const std::string_view term : terms) {
        const bool found = std::any_of(fields.begin(), fields.end(), [&](const std::string& field) {
            return field.find(term) != std::string::npos;
        });
        if (!found)
            return false;
    }
    return true;
}

std::vector<ReplayKindItem> replay_kind_items(const Library& library) {
    std::vector<ReplayKindItem> items;
    items.push_back({{}, std::string(shown(library.text, words::replay_kind_all))});
    for (const std::string_view kind : engine_replay_kinds)
        items.push_back({std::string(kind), replay_kind_title(library, kind)});
    for (const replays_data::Kind& kind : library.inputs.replays.kinds) {
        if (kind.id.empty() ||
            std::find(engine_replay_kinds.begin(), engine_replay_kinds.end(), kind.id) !=
                engine_replay_kinds.end())
            continue;
        items.push_back({kind.id, replay_kind_title(library, kind.id)});
    }
    return items;
}

std::string replay_kind_title(const Library& library, std::string_view kind) {
    if (kind == replays_data::kind_multiplayer)
        return std::string(shown(library.text, words::replay_kind_multiplayer));
    if (kind == replays_data::kind_skirmish)
        return std::string(shown(library.text, words::replay_kind_skirmish));
    if (kind == replays_data::kind_campaign)
        return std::string(shown(library.text, words::replay_kind_campaign));
    for (const replays_data::Kind& registered : library.inputs.replays.kinds)
        if (registered.id == kind)
            return std::string(shown(library.text, registered.title));
    return std::string(kind);
}

std::string replay_sort_title(const Library& library, ReplaySort sort) {
    switch (sort) {
    case ReplaySort::newest:
        return std::string(shown(library.text, words::replay_sort_newest));
    case ReplaySort::longest:
        return std::string(shown(library.text, words::replay_sort_longest));
    case ReplaySort::map:
        return std::string(shown(library.text, words::replay_sort_map));
    case ReplaySort::players:
        return std::string(shown(library.text, words::replay_sort_players));
    }
    return {};
}

std::string replay_filter_title(const Library& library, ReplayFilter filter) {
    switch (filter) {
    case ReplayFilter::this_month:
        return std::string(shown(library.text, words::replay_filter_this_month));
    case ReplayFilter::older:
        return std::string(shown(library.text, words::replay_filter_older));
    case ReplayFilter::kept:
        return std::string(shown(library.text, words::replay_filter_kept));
    case ReplayFilter::imported:
        return std::string(shown(library.text, words::replay_filter_imported));
    }
    return {};
}

std::vector<ReplayDay> replay_days(const Library& library) {
    const ReplayView& view = library.replays;
    std::vector<ReplayDay> days;
    if (view.visible.empty())
        return days;
    if (view.sort != ReplaySort::newest) {
        days.push_back({{}, 0, view.visible.size()});
        return days;
    }
    std::vector<int32_t> day_numbers;
    day_numbers.reserve(view.visible.size());
    for (const std::size_t index : view.visible)
        day_numbers.push_back(library.inputs.replays.rows[index].day);
    for (const replays_data::DayGroup& group : replays_data::group_by_day(day_numbers))
        days.push_back({replay_day_heading(library, group.day), group.first, group.count});
    return days;
}

std::string replay_day_heading(const Library& library, int32_t day) {
    const int32_t today = library.inputs.replays.today;
    if (day == today)
        return std::string(shown(library.text, words::replay_today));
    if (day == today - 1)
        return std::string(shown(library.text, words::replay_yesterday));
    return date_text(library, day);
}

std::string replay_length_text(uint64_t length_ms) {
    if (length_ms == 0)
        return "–";
    const uint64_t seconds = length_ms / milliseconds_per_second;
    const uint64_t hours = seconds / (sixty * sixty);
    const uint64_t minutes = seconds / sixty % sixty;
    const uint64_t rest = seconds % sixty;
    const auto two = [](uint64_t value) { return (value < 10 ? "0" : "") + std::to_string(value); };
    if (hours > 0)
        return std::to_string(hours) + ":" + two(minutes) + ":" + two(rest);
    return std::to_string(minutes) + ":" + two(rest);
}

std::string replay_time_text(int32_t minute) {
    const int32_t day_minutes = 24 * 60;
    const int32_t at = ((minute % day_minutes) + day_minutes) % day_minutes;
    const int32_t hours = at / 60;
    const int32_t minutes = at % 60;
    return (hours < 10 ? "0" : "") + std::to_string(hours) + ":" + (minutes < 10 ? "0" : "") +
           std::to_string(minutes);
}

std::string replay_players_text(const Library& library, const ReplayRow& row) {
    const std::vector<const replays_data::Player*> shown_players = playing(row);
    if (shown_players.empty())
        return {};
    if (shown_players.size() > most_named_players)
        return filled(
            library.text,
            words::replay_players_many,
            {{"count", std::to_string(shown_players.size())}}
        );
    std::string text = shown_players.front()->name;
    for (std::size_t index = 1; index < shown_players.size(); ++index)
        text = filled(
            library.text,
            words::replay_versus,
            {{"left", text}, {"right", shown_players[index]->name}}
        );
    return text;
}

std::string replay_kind_line(const Library& library, const ReplayRow& row) {
    const std::string kind = replay_kind_title(library, row.replay.kind);
    if (row.replay.planet.empty())
        return kind;
    return filled(
        library.text, words::replay_kind_planet, {{"kind", kind}, {"planet", row.replay.planet}}
    );
}

std::string replay_card_kind(const Library& library, const ReplayRow& row) {
    const std::string kind = replay_kind_title(library, row.replay.kind);
    if (row.replay.planet.empty())
        return kind;
    if (row.replay.sector.empty())
        return replay_kind_line(library, row);
    return filled(
        library.text,
        words::replay_kind_place,
        {{"kind", kind}, {"planet", row.replay.planet}, {"sector", row.replay.sector}}
    );
}

std::string replay_reading(const Library& library, const ReplayRow& row) {
    std::string line =
        row.replay.map.empty() ? std::string(file_name(row.replay.path)) : row.replay.map;
    line = joined(library, line, replay_kind_line(library, row));
    line = joined(library, line, replay_players_text(library, row));
    if (!replays_data::damaged(row.replay))
        line = joined(library, line, replay_length_text(row.replay.length_ms));
    line = joined(library, line, replay_time_text(row.minute));
    line = joined(library, line, size_text(row.replay.size, library.text));
    if (row.replay.kept)
        line = joined(library, line, shown(library.text, words::replay_kept_mark));
    if (row.replay.after_crash)
        line = joined(library, line, shown(library.text, words::replay_after_crash));
    if (replays_data::damaged(row.replay))
        line = joined(library, line, shown(library.text, words::replay_damaged));
    const std::string needed = replay_need_chip(library, row);
    if (!needed.empty())
        line = joined(library, line, needed);
    return line;
}

std::string replay_title(const ReplayRow& row) {
    return std::string(shown_map(row));
}

std::string replay_when_line(const Library& library, const ReplayRow& row) {
    const std::string when =
        replay_day_heading(library, row.day) + " " + replay_time_text(row.minute);
    // A length that is not known is left out, not written as a dash.
    if (replays_data::damaged(row.replay) || row.replay.length_ms == 0)
        return when;
    return joined(library, when, replay_length_text(row.replay.length_ms));
}

namespace {

/// A map named only by its title is today's "map is not installed", not a
/// package the catalogue can fetch. A release that is installed but is not the
/// mod being played is not missing either: Watch plays the replay with it.
bool content_gap_shown(const replays_data::ContentGap& gap) {
    return !gap.play_installed &&
           !(gap.kind == replays_data::GapKind::map && gap.sha256.empty() && gap.id.empty());
}

/// The name a sentence uses for one gap.
std::string gap_title(const replays_data::ContentGap& gap) {
    if (gap.kind == replays_data::GapKind::map) {
        if (!gap.pack_name.empty())
            return gap.pack_name;
        if (!gap.map_name.empty())
            return gap.map_name;
    }
    if (!gap.name.empty())
        return gap.name;
    return gap.id;
}

/// The map a card names, which may differ from the pack.
std::string gap_map_title(const replays_data::ContentGap& gap) {
    if (!gap.map_name.empty())
        return gap.map_name;
    if (!gap.name.empty())
        return gap.name;
    return gap.id;
}

/// Joins sentences with a space, skipping an empty one.
void add_sentence(std::string& note, std::string sentence) {
    if (sentence.empty())
        return;
    if (!note.empty())
        note.push_back(' ');
    note += std::move(sentence);
}

/// A catalogue sentence. An empty version uses the sentence that names none.
std::string
catalogue_words(const Library& library, std::string_view name, std::string_view version) {
    if (version.empty())
        return filled(library.text, words::replay_not_in_catalogue_name, {{"name", name}});
    return filled(
        library.text, words::replay_not_in_catalogue, {{"name", name}, {"version", version}}
    );
}

/// An offline sentence. An empty version uses the sentence that names none.
std::string offline_words(const Library& library, std::string_view name, std::string_view version) {
    if (version.empty())
        return filled(library.text, words::replay_offline_get_name, {{"name", name}});
    return filled(library.text, words::replay_offline_get, {{"name", name}, {"version", version}});
}

/// One gap's sentences, apart from the offline lines the card says once.
std::string gap_sentence(const Library& library, const replays_data::ContentGap& gap) {
    const std::string name = gap_title(gap);
    if (gap.block == replays_data::GapBlock::unlisted)
        return catalogue_words(library, name, gap.version);
    if (gap.block == replays_data::GapBlock::offline)
        return {};
    const std::string revision =
        gap.revision.has_value() ? std::to_string(*gap.revision) : std::string{};
    switch (gap.kind) {
    case replays_data::GapKind::mod_other:
        if (gap.version.empty() && !gap.revision.has_value()) {
            return filled(
                library.text,
                words::replay_recorded_have_name,
                {{"name", name}, {"have", gap.have_version}}
            );
        }
        if (gap.revision.has_value()) {
            return filled(
                library.text,
                words::replay_recorded_have_rev,
                {{"name", name},
                 {"version", gap.version},
                 {"revision", revision},
                 {"have", gap.have_version}}
            );
        }
        return filled(
            library.text,
            words::replay_recorded_have,
            {{"name", name}, {"version", gap.version}, {"have", gap.have_version}}
        );
    case replays_data::GapKind::mod_missing:
        if (gap.version.empty() && !gap.revision.has_value())
            return filled(library.text, words::replay_recorded_with_name, {{"name", name}});
        if (gap.revision.has_value()) {
            return filled(
                library.text,
                words::replay_recorded_with_rev,
                {{"name", name}, {"version", gap.version}, {"revision", revision}}
            );
        }
        return filled(
            library.text, words::replay_recorded_with, {{"name", name}, {"version", gap.version}}
        );
    case replays_data::GapKind::map: {
        std::string sentence =
            filled(library.text, words::replay_needs_the_map, {{"name", gap_map_title(gap)}});
        if (!gap.pack_name.empty()) {
            add_sentence(
                sentence, filled(library.text, words::replay_from_pack, {{"pack", gap.pack_name}})
            );
        }
        return sentence;
    }
    case replays_data::GapKind::mutator:
        return filled(library.text, words::replay_needs_the_mutator, {{"name", name}});
    case replays_data::GapKind::expansion_off:
    case replays_data::GapKind::expansion_missing:
        return filled(library.text, words::replay_needs_name_sentence, {{"name", name}});
    case replays_data::GapKind::engine:
        return filled(library.text, words::replay_needs_program, {{"version", gap.version}});
    }
    return {};
}

/// How many of the shown gaps can be fetched, and how many are only turned on.
struct ContentCounts {
    int fetchable{};
    int turn_on{};
    bool engine{};
    bool other{};
    bool blocked{};
    /// An expansion is missing and the catalogue has not said whether it offers it.
    bool unready{};
    const replays_data::ContentGap* one_fetch{};
    const replays_data::ContentGap* other_gap{};
};

ContentCounts count_gaps(const ReplayRow& row) {
    ContentCounts counts;
    for (const replays_data::ContentGap& gap : row.gaps) {
        if (!content_gap_shown(gap))
            continue;
        if (gap.block != replays_data::GapBlock::none)
            counts.blocked = true;
        if (gap.kind == replays_data::GapKind::engine)
            counts.engine = true;
        if (gap.other_release) {
            counts.other = true;
            counts.other_gap = &gap;
        }
        if (gap.fetchable && !gap.other_release) {
            ++counts.fetchable;
            counts.one_fetch = &gap;
        }
        if (gap.turn_on && !gap.fetchable)
            ++counts.turn_on;
        if (gap.kind == replays_data::GapKind::expansion_missing && !gap.fetchable &&
            gap.block == replays_data::GapBlock::none)
            counts.unready = true;
    }
    return counts;
}

/// The words on Watch when the replay needs content. Empty when Watch is
/// left out.
std::string watch_words(const Library& library, const ContentCounts& counts) {
    if (counts.engine)
        return std::string(shown(library.text, words::replay_check_updates));
    if (counts.fetchable > 1 || (counts.fetchable == 1 && counts.turn_on > 0))
        return std::string(shown(library.text, words::replay_get_them));
    if (counts.fetchable == 1 && counts.one_fetch != nullptr &&
        counts.one_fetch->kind == replays_data::GapKind::mod_missing &&
        !counts.one_fetch->version.empty()) {
        return filled(
            library.text,
            words::replay_get_release,
            {{"name", gap_title(*counts.one_fetch)}, {"version", counts.one_fetch->version}}
        );
    }
    if (counts.fetchable == 1)
        return std::string(shown(library.text, words::replay_get_it));
    if (counts.turn_on > 0)
        return std::string(shown(library.text, words::replay_turn_on));
    if (counts.other && counts.other_gap != nullptr) {
        return filled(
            library.text,
            words::replay_get_release,
            {{"name", gap_title(*counts.other_gap)}, {"version", counts.other_gap->version}}
        );
    }
    return std::string(shown(library.text, words::action_watch));
}

/// The chip's rank. A lower rank is the one the row shows.
int chip_rank(replays_data::GapKind kind) {
    switch (kind) {
    case replays_data::GapKind::mod_missing:
    case replays_data::GapKind::mod_other:
        return 0;
    case replays_data::GapKind::map:
        return 1;
    case replays_data::GapKind::mutator:
        return 2;
    case replays_data::GapKind::expansion_off:
    case replays_data::GapKind::expansion_missing:
        return 3;
    case replays_data::GapKind::engine:
        return 4;
    }
    return 5;
}

/// What the replay still needs, as the card says it: "You're offline." and
/// what to get when the registry could not be reached, then a sentence for
/// each other gap. Empty when it needs nothing.
std::string needs_note(const Library& library, const ReplayRow& row) {
    std::string note;
    bool offline = false;
    for (const replays_data::ContentGap& gap : row.gaps) {
        if (!content_gap_shown(gap))
            continue;
        if (gap.block == replays_data::GapBlock::offline)
            offline = true;
    }
    if (offline) {
        add_sentence(note, std::string(shown(library.text, words::replay_offline)));
        for (const replays_data::ContentGap& gap : row.gaps) {
            if (!content_gap_shown(gap) || gap.block != replays_data::GapBlock::offline)
                continue;
            add_sentence(note, offline_words(library, gap_title(gap), gap.version));
        }
    }
    for (const replays_data::ContentGap& gap : row.gaps) {
        if (!content_gap_shown(gap) || gap.block == replays_data::GapBlock::offline)
            continue;
        add_sentence(note, gap_sentence(library, gap));
    }
    return note;
}

} // namespace

std::string replay_note(const Library& library, const ReplayRow& row) {
    const bool failed = !library.replays.delete_failure.empty() && library.replays.selected &&
                        *library.replays.selected == row.replay.path;
    const std::string failure = failed ? library.replays.delete_failure : std::string{};
    if (replays_data::damaged(row.replay)) {
        // Two sentences, as a person reads them. A failed Delete is said first.
        const std::string damage = std::string(shown(library.text, words::replay_cannot_watch)) +
                                   " " + std::string(shown(library.text, row.replay.damage));
        return failure.empty() ? damage : failure + " " + damage;
    }
    if (!library.replays.refusal.empty() && library.replays.refused_path == row.replay.path)
        return failure.empty() ? library.replays.refusal : failure + " " + library.replays.refusal;
    // A failed Delete is said first, then what the replay still needs.
    std::string note = failure;
    add_sentence(note, needs_note(library, row));
    return note;
}

std::string replay_need_chip(const Library& library, const ReplayRow& row) {
    if (replays_data::damaged(row.replay))
        return {};
    const replays_data::ContentGap* chosen = nullptr;
    int rank = 5;
    for (const replays_data::ContentGap& gap : row.gaps) {
        if (!content_gap_shown(gap))
            continue;
        const int place = chip_rank(gap.kind);
        if (chosen != nullptr && place >= rank)
            continue;
        chosen = &gap;
        rank = place;
    }
    if (chosen == nullptr)
        return {};
    switch (chosen->kind) {
    case replays_data::GapKind::mod_missing:
    case replays_data::GapKind::mod_other:
        if (chosen->version.empty())
            return filled(library.text, words::replay_needs_name, {{"name", gap_title(*chosen)}});
        return filled(
            library.text,
            words::replay_needs_release,
            {{"name", gap_title(*chosen)}, {"version", chosen->version}}
        );
    case replays_data::GapKind::map:
        return std::string(shown(library.text, words::replay_needs_map));
    case replays_data::GapKind::mutator:
        return std::string(shown(library.text, words::replay_needs_mutator));
    case replays_data::GapKind::expansion_off:
    case replays_data::GapKind::expansion_missing:
        return filled(library.text, words::replay_needs_name, {{"name", gap_title(*chosen)}});
    case replays_data::GapKind::engine:
        return filled(library.text, words::replay_needs_version, {{"version", chosen->version}});
    }
    return {};
}

std::string replay_caution(const Library& library, const ReplayRow& row) {
    const ReplayInputs& inputs = library.inputs.replays;
    if (inputs.fetching && inputs.fetch_path == row.replay.path && !inputs.fetch_sentence.empty())
        return inputs.fetch_sentence;
    if (replays_data::damaged(row.replay))
        return {};
    if (!library.replays.refusal.empty() && library.replays.refused_path == row.replay.path)
        return {};
    return needs_note(library, row);
}

std::string replay_watch_short(const Library& library) {
    const ReplayRow* row = selected_replay(library);
    if (row == nullptr)
        return {};
    const ReplayInputs& inputs = library.inputs.replays;
    if (inputs.fetching && inputs.fetch_path == row->replay.path)
        return std::string(shown(library.text, words::action_cancel));
    const ContentCounts counts = count_gaps(*row);
    if (counts.engine)
        return std::string(shown(library.text, words::replay_check_updates));
    if (counts.fetchable > 1 || (counts.fetchable >= 1 && counts.turn_on > 0))
        return std::string(shown(library.text, words::replay_get_them));
    if (counts.fetchable == 1 || counts.other)
        return std::string(shown(library.text, words::replay_get_it));
    if (counts.turn_on > 0)
        return std::string(shown(library.text, words::replay_turn_on));
    return std::string(shown(library.text, words::action_watch));
}

std::vector<ReplayLinePiece> replay_line_pieces(const Library& library, const ReplayRow& row) {
    std::vector<ReplayLinePiece> pieces;
    const std::vector<const replays_data::Player*> named = playing(row);
    const std::string length = replays_data::damaged(row.replay)
                                   ? std::string()
                                   : replay_length_text(row.replay.length_ms);
    // Past three players the dots stand together and the count follows them.
    // Up to three, each name has its own dot, in front of it, so that a colour
    // can be read against the player. The pattern that joins the names is the
    // language's own: the names go in as marks, and each mark is then
    // replaced by that player's dot and name.
    constexpr char first_mark = '\x01';
    std::string text;
    if (named.empty()) {
        text = length;
    } else if (named.size() > most_named_players) {
        for (const replays_data::Player* player : named)
            pieces.push_back({player->colour, {}});
        text = replay_players_text(library, row);
        if (!length.empty())
            text = joined(library, text, length);
    } else {
        text = std::string(1, first_mark);
        for (std::size_t index = 1; index < named.size(); ++index)
            text = filled(
                library.text,
                words::replay_versus,
                {{"left", text}, {"right", std::string(1, static_cast<char>(first_mark + index))}}
            );
        if (!length.empty())
            text = joined(library, text, length);
    }
    std::string literal;
    const auto flush = [&] {
        if (!literal.empty())
            pieces.push_back({std::nullopt, std::move(literal)});
        literal.clear();
    };
    for (const char letter : text) {
        const auto mark = static_cast<std::size_t>(static_cast<unsigned char>(letter));
        if (named.size() <= most_named_players && mark >= 1 && mark <= named.size()) {
            flush();
            pieces.push_back({named[mark - 1]->colour, {}});
            pieces.push_back({std::nullopt, named[mark - 1]->name});
            continue;
        }
        literal += letter;
    }
    flush();
    return pieces;
}

std::vector<ReplayFact> replay_facts(const Library& library, const ReplayRow& row) {
    const replays_data::Replay& replay = row.replay;
    std::vector<ReplayFact> facts;
    const auto add = [&](std::string_view key, std::string_view label, std::string value) {
        facts.push_back(
            {std::string(key), capitalised(shown(library.text, label)), std::move(value)}
        );
    };
    const std::string when =
        replay_day_heading(library, row.day) + " " + replay_time_text(row.minute);
    if (!replays_data::damaged(replay)) {
        add("length", words::replay_fact_length, replay_length_text(replay.length_ms));
        add("recorded",
            words::replay_fact_recorded,
            joined(library, when, recorded_by_text(library, replay)));
        add("mod", words::replay_fact_mod, mod_text(library, replay));
        std::vector<std::string> expansions;
        for (const std::string& id : replay.expansions) {
            const oa::data::content_bundles::Bundle* bundle =
                oa::data::content_bundles::find_bundle(id);
            expansions.push_back(
                bundle != nullptr ? std::string(shown(library.text, bundle->english_name)) : id
            );
        }
        add("expansions", words::replay_fact_expansions, names_text(library, expansions));
        add("mutators", words::replay_fact_mutators, names_text(library, replay.mutators));
    } else {
        add("recorded", words::replay_fact_recorded, when);
    }
    add("file",
        words::replay_fact_file,
        joined(library, size_text(replay.size, library.text), file_name(replay.path)));
    const ReplayInputs& inputs = library.inputs.replays;
    if (replay.kept || inputs.keep_forever)
        add("kept",
            words::replay_fact_kept,
            std::string(shown(library.text, words::replay_kept_always)));
    else if (replays_data::imported(replay))
        add("kept",
            words::replay_fact_kept,
            std::string(shown(library.text, words::replay_kept_imported)));
    else if (replays_data::downloaded(replay))
        add("kept",
            words::replay_fact_kept,
            std::string(shown(library.text, words::replay_kept_downloaded)));
    else if (replays_data::exported(replay))
        add("kept",
            words::replay_fact_kept,
            std::string(shown(library.text, words::replay_kept_exported)));
    else
        add("kept-until",
            words::replay_fact_kept_until,
            date_text(library, row.day + inputs.keep_days));
    return facts;
}

bool marked_kept(const Library& library) {
    const ReplayView& view = library.replays;
    if (view.marked.empty())
        return false;
    for (const std::string& path : view.marked) {
        const ReplayRow* row = find_replay(library, path);
        if (row == nullptr || !row->replay.kept)
            return false;
    }
    return true;
}

bool marked_all_imported(const Library& library) {
    const ReplayView& view = library.replays;
    if (view.marked.empty())
        return false;
    for (const std::string& path : view.marked) {
        const ReplayRow* row = find_replay(library, path);
        if (row == nullptr || !replays_data::imported(row->replay))
            return false;
    }
    return true;
}

std::vector<ReplayButton> replay_buttons(const Library& library) {
    std::vector<ReplayButton> buttons;
    const ReplayRow* row = selected_replay(library);
    if (row == nullptr)
        return buttons;
    const ContentCounts counts = count_gaps(*row);
    const bool fetching =
        library.inputs.replays.fetching && library.inputs.replays.fetch_path == row->replay.path;
    const bool content = counts.fetchable > 0 || counts.turn_on > 0 || counts.engine ||
                         counts.other || counts.blocked || counts.unready || fetching;
    if (fetching) {
        buttons.push_back(
            {Action::watch_replay,
             !library.inputs.in_match,
             std::string(shown(library.text, words::action_cancel))}
        );
    } else if (!content) {
        buttons.push_back(
            {Action::watch_replay,
             !replays_data::damaged(row->replay) && !library.inputs.in_match,
             std::string(shown(library.text, words::action_watch))}
        );
    } else if (!counts.blocked && !replays_data::damaged(row->replay)) {
        const std::string words = watch_words(library, counts);
        // A missing expansion the catalogue has not answered yet is not Watch.
        const bool plain_watch = words == shown(library.text, words::action_watch);
        if (!(plain_watch && counts.unready && counts.fetchable == 0 && counts.turn_on == 0 &&
              !counts.engine && !counts.other)) {
            buttons.push_back({Action::watch_replay, !library.inputs.in_match, words});
        }
    }
    bool can_export = !replays_data::damaged(row->replay);
    if (can_export) {
        for (const std::string& path : library.replays.marked) {
            const ReplayRow* marked = find_replay(library, path);
            if (marked != nullptr && replays_data::damaged(marked->replay)) {
                can_export = false;
                break;
            }
        }
    }
    buttons.push_back(
        {Action::export_replays, can_export, std::string(shown(library.text, words::action_export))}
    );
    // An imported replay is kept already, as its card says: nothing to keep.
    if (!marked_all_imported(library)) {
        const bool kept = marked_kept(library);
        buttons.push_back(
            {Action::keep_replays,
             !library.replays.marked.empty(),
             std::string(shown(library.text, kept ? words::action_kept : words::action_keep))}
        );
    }
    buttons.push_back(
        {Action::delete_replays,
         !library.replays.marked.empty(),
         std::string(shown(library.text, words::action_delete_replay)),
         true}
    );
    if (library.replays.delete_armed) {
        const bool several = library.replays.marked.size() > 1;
        buttons.push_back(
            {Action::confirm_delete_replays,
             !library.replays.marked.empty(),
             std::string(
                 shown(library.text, several ? words::action_delete_them : words::action_delete_it)
             ),
             true}
        );
    }
    buttons.push_back(
        {Action::open_replay_folder,
         true,
         std::string(shown(library.text, words::action_open_folder))}
    );
    return buttons;
}

ReplayEmpty replay_empty(const Library& library) {
    ReplayEmpty empty;
    const ReplayInputs& inputs = library.inputs.replays;
    // Open a replay file waits until a read has ended, and a search that
    // matches nothing does not offer it.
    const bool offer_open = inputs.open_file && !inputs.reading;
    if (inputs.rows.empty() && inputs.reading) {
        // The folder is being read and no row has arrived: not "No replays yet."
        empty.title = std::string(shown(library.text, words::replays_reading_folder));
        empty.reading = true;
        return empty;
    }
    if (inputs.rows.empty()) {
        empty.title = std::string(shown(library.text, words::replays_none_yet));
        if (inputs.recording_on) {
            const std::string_view sentence = kept_sentence(inputs);
            empty.line = std::string(shown(library.text, sentence));
        } else {
            empty.line = std::string(shown(library.text, words::replays_recording_off));
            empty.turn_on = true;
        }
        empty.open_file = offer_open;
        return empty;
    }
    if (library.query.find_first_not_of(' ') != std::string::npos) {
        empty.title = filled(library.text, words::empty_matches, {{"query", library.query}});
        return empty;
    }
    if (!library.replays.kind.empty() && replay_filter_count(library, library.replays.filter) > 0) {
        empty.title = std::string(shown(library.text, words::replays_none_kind));
        return empty;
    }
    switch (library.replays.filter) {
    case ReplayFilter::this_month:
        empty.title = std::string(shown(library.text, words::replays_none_this_month));
        break;
    case ReplayFilter::older:
        empty.title = std::string(shown(library.text, words::replays_none_older));
        break;
    case ReplayFilter::kept:
        empty.title = std::string(shown(library.text, words::replays_none_kept));
        break;
    case ReplayFilter::imported:
        empty.title = std::string(shown(library.text, words::replays_none_imported));
        empty.open_file = offer_open;
        break;
    }
    return empty;
}

std::string replay_status(const Library& library) {
    const ReplayInputs& inputs = library.inputs.replays;
    if (inputs.reading) {
        // Until the folder's files are counted, there is no total to say.
        if (inputs.total == 0)
            return std::string(shown(library.text, words::replays_reading_folder));
        return filled(
            library.text,
            words::replays_reading,
            {{"read", std::to_string(inputs.read)}, {"total", std::to_string(inputs.total)}}
        );
    }
    const std::vector<std::size_t>& visible = library.replays.visible;
    if (visible.empty())
        return {};
    uint64_t bytes = 0;
    for (const std::size_t index : visible)
        bytes += inputs.rows[index].replay.size;
    const std::string size = size_text(bytes, library.text);
    if (visible.size() == 1)
        return filled(library.text, words::replays_count_one, {{"size", size}});
    return filled(
        library.text,
        words::replays_count_many,
        {{"count", std::to_string(visible.size())}, {"size", size}}
    );
}

} // namespace oa::ui::library
