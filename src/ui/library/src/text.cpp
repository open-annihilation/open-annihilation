// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's sizes, ages, versions, rules hashes and engine requirements,
// written from the patterns in text.hpp.
#include "oa/ui/library/text.hpp"

#include "oa/data/mutators/verdicts.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

/// Bytes in a KiB, MiB and GiB.
constexpr uint64_t kib = uint64_t{1} << 10;
constexpr uint64_t mib = uint64_t{1} << 20;
constexpr uint64_t gib = uint64_t{1} << 30;

/// Below this size, MB are written to one decimal.
constexpr uint64_t one_decimal_mb_limit = 100 * mib;

/// Seconds in a minute, an hour and a day, and the age from which days are counted.
constexpr int64_t seconds_per_minute = 60;
constexpr int64_t seconds_per_hour = 60 * seconds_per_minute;
constexpr int64_t seconds_per_day = 24 * seconds_per_hour;
constexpr int64_t hours_limit = 2 * seconds_per_day;

/// How many hexadecimal digits of a rules hash its text shows, and how many make one group.
constexpr std::size_t rules_text_digits = 8;
constexpr std::size_t rules_group_digits = 4;

/// Divides, rounding halves up.
///
/// @param value the dividend
/// @param unit the divisor, not zero, at most 2^62
/// @return value / unit, rounded to the nearest whole number, halves up
uint64_t rounded(uint64_t value, uint64_t unit) {
    const uint64_t rest = value % unit;
    return value / unit + (2 * rest >= unit ? 1 : 0);
}

/// Divides to tenths, rounding halves up.
///
/// @param value the dividend
/// @param unit the divisor, not zero, at most 2^32
/// @return value / unit in tenths
uint64_t rounded_tenths(uint64_t value, uint64_t unit) {
    const uint64_t whole = value / unit;
    const uint64_t rest = value % unit;
    return whole * 10 + (rest * 10 + unit / 2) / unit;
}

/// Writes tenths as a number with one decimal.
///
/// @param tenths the number, in tenths
/// @param text how texts are looked up
/// @return such as "41.0"
std::string tenths_text(uint64_t tenths, const TextHooks& text) {
    const std::string whole = std::to_string(tenths / 10);
    const std::string tenth = std::to_string(tenths % 10);
    return filled(text, words::one_decimal, {{"whole", whole}, {"tenth", tenth}});
}

/// A size's number and its unit's pattern.
struct SizeParts {
    std::string number{};
    std::string_view pattern{};
};

/// How a size is written in the unit and precision a reference size takes.
enum class SizeScale : uint8_t { kilobytes, megabytes_tenths, megabytes, gigabytes_tenths };

/// Chooses how a size is written.
///
/// @param bytes the size
/// @return its scale
SizeScale scale_of(uint64_t bytes) {
    if (bytes < mib)
        return SizeScale::kilobytes;
    if (bytes < one_decimal_mb_limit)
        return SizeScale::megabytes_tenths;
    if (bytes < gib)
        return SizeScale::megabytes;
    return SizeScale::gigabytes_tenths;
}

/// Writes a size at a scale.
///
/// @param bytes the size
/// @param scale the unit and precision
/// @param at_least_one a size in KB is at least 1
/// @param text how texts are looked up
/// @return the number and the unit's pattern
SizeParts size_parts(uint64_t bytes, SizeScale scale, bool at_least_one, const TextHooks& text) {
    switch (scale) {
    case SizeScale::kilobytes: {
        uint64_t whole = rounded(bytes, kib);
        if (at_least_one && whole == 0)
            whole = 1;
        return {std::to_string(whole), words::size_kb};
    }
    case SizeScale::megabytes_tenths:
        return {tenths_text(rounded_tenths(bytes, mib), text), words::size_mb};
    case SizeScale::megabytes:
        return {std::to_string(rounded(bytes, mib)), words::size_mb};
    case SizeScale::gigabytes_tenths:
        return {tenths_text(rounded_tenths(bytes, gib), text), words::size_gb};
    }
    return {};
}

/// The words for one comparison of an engine requirement.
///
/// @param comparison the comparison
/// @param compact the short words
/// @return the pattern, with a {version} place
std::string_view comparison_pattern(formats::oamod::EngineComparison comparison, bool compact) {
    using formats::oamod::EngineComparison;
    switch (comparison) {
    case EngineComparison::at_least:
        return compact ? words::engine_at_least_short : words::engine_at_least;
    case EngineComparison::above:
        return compact ? words::engine_above_short : words::engine_above;
    case EngineComparison::at_most:
        return compact ? words::engine_at_most_short : words::engine_at_most;
    case EngineComparison::below:
        return compact ? words::engine_below_short : words::engine_below;
    case EngineComparison::exactly:
        return compact ? words::engine_exactly_short : words::engine_exactly;
    }
    return words::engine_at_least;
}

/// Describes an engine requirement.
///
/// @param range the requirement
/// @param compact the short words
/// @param text how texts are looked up
/// @return its comparisons, joined
std::string
range_text(const formats::oamod::EngineRange& range, bool compact, const TextHooks& text) {
    std::string joined;
    for (const auto& [comparison, version] : range.terms) {
        if (!joined.empty()) {
            joined += shown(text, words::list_separator);
            joined.push_back(' ');
        }
        const std::string number = formats::oamod::engine_version_text(version);
        joined += filled(text, comparison_pattern(comparison, compact), {{"version", number}});
    }
    return joined;
}

/// Writes a version and its revision above 1.
///
/// @param version the version, as written
/// @param revision the revision; 0 when none
/// @param text how texts are looked up
/// @return such as "4.8 · rev 3", "4.8" or "rev 3"
std::string
version_and_revision(std::string_view version, int64_t revision, const TextHooks& text) {
    if (revision <= 1)
        return std::string(version);
    const std::string number = std::to_string(revision);
    if (version.empty())
        return filled(text, words::revision_short, {{"revision", number}});
    return filled(text, words::version_with_revision, {{"version", version}, {"revision", number}});
}

} // namespace

std::string_view shown(const TextHooks& text, std::string_view english) {
    if (text.shown == nullptr)
        return english;
    return text.shown(text.context, english);
}

std::string
filled(const TextHooks& text, std::string_view pattern, std::initializer_list<Place> places) {
    const std::string_view source = shown(text, pattern);
    std::string result;
    std::size_t at = 0;
    while (at < source.size()) {
        const std::size_t open = source.find('{', at);
        const std::size_t close = open == std::string_view::npos ? open : source.find('}', open);
        if (close == std::string_view::npos) {
            result.append(source.substr(at));
            break;
        }
        result.append(source.substr(at, open - at));
        const std::string_view name = source.substr(open + 1, close - open - 1);
        bool found = false;
        for (const auto& [place, value] : places) {
            if (place == name) {
                result.append(value);
                found = true;
                break;
            }
        }
        if (!found)
            result.append(source.substr(open, close - open + 1));
        at = close + 1;
    }
    return result;
}

std::string size_text(uint64_t bytes, const TextHooks& text) {
    const SizeParts parts = size_parts(bytes, scale_of(bytes), true, text);
    return filled(text, parts.pattern, {{"size", parts.number}});
}

std::string progress_text(uint64_t done, uint64_t total, const TextHooks& text) {
    const SizeScale scale = scale_of(total);
    const SizeParts done_parts = size_parts(done, scale, false, text);
    const std::string whole = size_text(total, text);
    return filled(text, words::progress, {{"done", done_parts.number}, {"total", whole}});
}

std::string age_text(int64_t seconds, const TextHooks& text) {
    if (seconds < seconds_per_minute)
        return std::string(shown(text, words::age_just_now));
    if (seconds < seconds_per_hour) {
        const std::string count = std::to_string(seconds / seconds_per_minute);
        return filled(text, words::age_minutes, {{"count", count}});
    }
    if (seconds < hours_limit) {
        const std::string count = std::to_string(seconds / seconds_per_hour);
        return filled(text, words::age_hours, {{"count", count}});
    }
    const std::string count = std::to_string(seconds / seconds_per_day);
    return filled(text, words::age_days, {{"count", count}});
}

std::string rules_text(const base::sha256::Digest& digest) {
    const auto digits = base::sha256::to_hex(digest);
    std::string written;
    for (std::size_t index = 0; index < rules_text_digits; ++index) {
        if (index != 0 && index % rules_group_digits == 0)
            written += ' ';
        written += digits[index];
    }
    return written;
}

std::string version_text(const Listing& listing, const TextHooks& text) {
    std::string written = version_and_revision(listing.version, listing.revision, text);
    if (listing.kind != Kind::map_pack)
        return written;
    const std::string count = std::to_string(listing.maps.size());
    const std::string maps = filled(
        text, listing.maps.size() == 1 ? words::map_count_one : words::map_count, {{"count", count}}
    );
    if (written.empty())
        return maps;
    return filled(text, words::version_with_maps, {{"version", written}, {"maps", maps}});
}

std::string version_text(const Installed& installed, const TextHooks& text) {
    return version_and_revision(installed.version, installed.revision, text);
}

std::string release_text(std::string_view version, int64_t revision, const TextHooks& text) {
    if (revision <= 0)
        return std::string(version);
    const std::string number = std::to_string(revision);
    if (version.empty())
        return filled(text, words::revision_short, {{"revision", number}});
    return filled(text, words::version_revision_fact, {{"version", version}, {"revision", number}});
}

ChangeTexts
change_texts(const Installed& installed, const UpdateNote& update, const TextHooks& text) {
    const bool same_version = installed.version == update.to_version;
    // One side's long and short names: its version where the versions
    // differ, else its revision, and the old or new copy where neither is
    // there to name it.
    const auto side = [&](std::string_view version,
                          int64_t revision,
                          std::string_view copy,
                          std::string_view copy_short) -> std::pair<std::string, std::string> {
        if (!same_version && !version.empty())
            return {std::string(version), std::string(version)};
        if (revision > 0) {
            const std::string number = std::to_string(revision);
            return {
                filled(text, words::revision_long, {{"revision", number}}),
                filled(text, words::revision_short, {{"revision", number}}),
            };
        }
        return {std::string(shown(text, copy)), std::string(shown(text, copy_short))};
    };
    ChangeTexts names;
    std::tie(names.from, names.from_short) = side(
        installed.version, installed.revision, words::change_old_copy, words::change_old_copy_short
    );
    std::tie(names.to, names.to_short) = side(
        update.to_version, update.to_revision, words::change_new_copy, words::change_new_copy_short
    );
    return names;
}

std::string engine_text(const formats::oamod::EngineRange& range, const TextHooks& text) {
    return range_text(range, false, text);
}

std::string range_words(std::string_view range_text, const TextHooks& text) {
    if (range_text.empty())
        return {};
    const std::optional<formats::oamut::VersionRange> range =
        formats::oamut::parse_version_range(range_text);
    if (!range)
        return std::string(range_text);
    std::string joined;
    for (const auto& [comparison, version] : range->terms) {
        if (!joined.empty()) {
            joined += shown(text, words::list_separator);
            joined.push_back(' ');
        }
        const std::string number = formats::oamut::version_text(version);
        joined += filled(text, comparison_pattern(comparison, false), {{"version", number}});
    }
    return joined;
}

std::string engine_short_text(const formats::oamod::EngineRange& range, const TextHooks& text) {
    return range_text(range, true, text);
}

std::string_view tab_text(Tab tab, bool compact, const TextHooks& text) {
    switch (tab) {
    case Tab::active:
        return shown(text, words::tab_active);
    case Tab::mods:
        return shown(text, words::tab_mods);
    case Tab::maps:
        return shown(text, words::tab_maps);
    case Tab::mutators:
        return shown(text, words::tab_mutators);
    case Tab::brains:
        return shown(text, words::tab_brains);
    case Tab::languages:
        return shown(text, compact ? words::tab_languages_short : words::tab_languages);
    case Tab::updates:
        return shown(text, compact ? words::tab_updates_short : words::tab_updates);
    case Tab::registries:
        return shown(text, compact ? words::tab_registries_short : words::tab_registries);
    }
    return {};
}

std::string_view filter_text(Filter filter, const TextHooks& text) {
    switch (filter) {
    case Filter::all:
        return shown(text, words::filter_all);
    case Filter::newest:
        return shown(text, words::filter_new);
    case Filter::installed:
        return shown(text, words::filter_installed);
    case Filter::updates:
        return shown(text, words::filter_updates);
    }
    return {};
}

std::string_view state_text(State state, const TextHooks& text) {
    switch (state) {
    case State::get:
        return shown(text, words::state_get);
    case State::installed:
        return shown(text, words::state_installed);
    case State::update:
        return shown(text, words::state_update);
    case State::playing:
        return shown(text, words::state_playing);
    }
    return {};
}

std::string_view action_text(Action action, const TextHooks& text) {
    switch (action) {
    case Action::get:
        return shown(text, words::action_get);
    case Action::update:
        return shown(text, words::action_update);
    case Action::update_all:
        return shown(text, words::action_update_all);
    case Action::cancel:
        return shown(text, words::action_cancel);
    case Action::retry:
        return shown(text, words::action_retry);
    case Action::roll_back:
        return shown(text, words::action_roll_back);
    case Action::play_now:
        return shown(text, words::action_play_now);
    case Action::switch_off:
        return shown(text, words::action_switch_off);
    case Action::open_folder:
        return shown(text, words::action_open_folder);
    case Action::homepage:
        return shown(text, words::action_homepage);
    case Action::details:
        return shown(text, words::action_details);
    case Action::back:
        return shown(text, words::action_back);
    case Action::check_now:
        return shown(text, words::action_refresh);
    case Action::settings:
        return shown(text, words::action_settings);
    case Action::close:
        return shown(text, words::action_close);
    case Action::add_registry:
        return shown(text, words::registry_add);
    case Action::add_mirror:
        return shown(text, words::registry_add_mirror);
    case Action::remove_registry:
        return shown(text, words::registry_remove);
    case Action::set_registry:
        return shown(text, words::registry_on);
    case Action::refresh_registry:
        return shown(text, words::registry_refresh);
    case Action::uninstall:
        return shown(text, words::action_uninstall);
    case Action::use_now:
        return shown(text, words::action_use_now);
    case Action::change_base_game:
        return shown(text, words::active_change);
    case Action::copy_base_game:
        return shown(text, words::base_use);
    case Action::add_expansion:
        return shown(text, words::active_add_from);
    case Action::ask_expansion_off:
        return shown(text, words::action_turn_off);
    case Action::turn_on:
        return shown(text, words::action_turn_on);
    case Action::check_with_mod:
        return shown(text, words::action_check_with_mod);
    case Action::use_by_default:
        return shown(text, words::action_use_by_default);
    case Action::get_with:
        return shown(text, words::action_get_with);
    }
    return {};
}

namespace {

/// Joins phrases with a comma between all but the last pair, which uses `last`.
///
/// @param text how texts are looked up
/// @param parts the phrases
/// @param last the pattern for the last pair, `names_and` or `names_or`
/// @return the joined phrases; empty when there are none
std::string
join_parts(const TextHooks& text, const std::vector<std::string>& parts, std::string_view last) {
    if (parts.empty())
        return {};
    if (parts.size() == 1)
        return parts.front();
    std::string joined = parts.front();
    for (std::size_t index = 1; index + 1 < parts.size(); ++index)
        joined = filled(text, words::names_comma, {{"left", joined}, {"right", parts[index]}});
    return filled(text, last, {{"left", joined}, {"right", parts.back()}});
}

/// Writes one non-zero count, singular at one.
///
/// @param text how texts are looked up
/// @param count the count; zero writes nothing
/// @param one the singular pattern
/// @param many the plural pattern
/// @return the phrase; empty when the count is zero
std::string
count_phrase(const TextHooks& text, uint32_t count, std::string_view one, std::string_view many) {
    if (count == 0)
        return {};
    return filled(text, count == 1 ? one : many, {{"count", std::to_string(count)}});
}

/// Writes a compatible verdict's counts, non-zero ones in ChangeCounts order.
///
/// @param counts the counts
/// @param text how texts are looked up
/// @return the summary; empty when every count is zero
std::string counts_summary(const oa::data::mutators::ChangeCounts& counts, const TextHooks& text) {
    std::vector<std::string> parts;
    const auto add = [&](uint32_t count, std::string_view one, std::string_view many) {
        std::string phrase = count_phrase(text, count, one, many);
        if (!phrase.empty())
            parts.push_back(std::move(phrase));
    };
    add(counts.units_changed, words::changes_units_one, words::changes_units);
    add(counts.units_added, words::adds_units_one, words::adds_units);
    add(counts.units_removed, words::removes_units_one, words::removes_units);
    add(counts.weapons_changed, words::changes_weapons_one, words::changes_weapons);
    add(counts.weapons_added, words::adds_weapons_one, words::adds_weapons);
    add(counts.weapons_removed, words::removes_weapons_one, words::removes_weapons);
    add(counts.features_changed, words::changes_features_one, words::changes_features);
    add(counts.features_added, words::adds_features_one, words::adds_features);
    add(counts.features_removed, words::removes_features_one, words::removes_features);
    add(counts.menu_entries_changed, words::changes_menus_one, words::changes_menus);
    add(counts.files_added, words::adds_files_one, words::adds_files);
    add(counts.files_hidden, words::hides_files_one, words::hides_files);
    return join_parts(text, parts, words::names_and);
}

} // namespace

Verdict verdict_of_record(const oa::data::mutators::VerdictRecord& record, const TextHooks& text) {
    Verdict verdict;
    verdict.reason = record.reason;
    switch (record.verdict) {
    case oa::data::mutators::Verdict::compatible:
        verdict.state = Verdict::State::compatible;
        verdict.summary = counts_summary(record.counts, text);
        if (verdict.summary.empty())
            verdict.summary = std::string(shown(text, words::changes_other_data));
        break;
    case oa::data::mutators::Verdict::no_effect:
        verdict.state = Verdict::State::no_effect;
        break;
    case oa::data::mutators::Verdict::incompatible:
        verdict.state = Verdict::State::incompatible;
        break;
    case oa::data::mutators::Verdict::failed:
        verdict.state = Verdict::State::failed;
        break;
    }
    return verdict;
}

void apply_map_fact(
    Verdict& verdict,
    const oa::data::mutators::VerdictRecord& record,
    bool declares_maps,
    const TextHooks& text
) {
    if (!declares_maps || verdict.state != Verdict::State::compatible)
        return;
    if (oa::data::mutators::any(record.counts))
        return;
    verdict.summary = std::string(shown(text, words::changes_map_on_start));
}

std::string registry_initials(std::string_view name) {
    std::vector<std::string_view> words_found;
    std::size_t start = 0;
    while (start < name.size()) {
        while (start < name.size() && name[start] == ' ')
            ++start;
        std::size_t end = start;
        while (end < name.size() && name[end] != ' ')
            ++end;
        if (end > start)
            words_found.push_back(name.substr(start, end - start));
        start = end;
    }
    const auto upper = [](std::string_view character) {
        std::string text(character);
        if (text.size() == 1 && text[0] >= 'a' && text[0] <= 'z')
            text[0] = static_cast<char>(text[0] - 'a' + 'A');
        return text;
    };
    const auto first = [](std::string_view word) {
        return word.substr(0, std::max(oa::ui::kit::character_bytes(word), std::size_t{1}));
    };
    std::string letters;
    if (words_found.size() >= 2) {
        for (std::size_t index = 0; index < words_found.size() && index < 3; ++index)
            letters += upper(first(words_found[index]));
        return letters;
    }
    if (words_found.empty())
        return letters;
    std::string_view word = words_found.front();
    for (int32_t taken = 0; taken < 2 && !word.empty(); ++taken) {
        const std::string_view character = first(word);
        letters += upper(character);
        word.remove_prefix(character.size());
    }
    return letters;
}

std::string registry_age_text(int64_t seconds, const TextHooks& text) {
    constexpr int64_t minute = 60;
    constexpr int64_t hour = 60 * minute;
    constexpr int64_t day = 24 * hour;
    if (seconds < minute)
        return std::string(shown(text, words::age_just_now));
    if (seconds < hour)
        return filled(text, words::age_minutes, {{"count", std::to_string(seconds / minute)}});
    if (seconds < day)
        return filled(text, words::age_hours, {{"count", std::to_string(seconds / hour)}});
    if (seconds < 2 * day)
        return std::string(shown(text, words::registry_one_day));
    return filled(text, words::age_days, {{"count", std::to_string(seconds / day)}});
}

namespace {

/// Returns a count of things with its noun, one or many.
///
/// @param count how many
/// @param one the noun for one, in English
/// @param many the noun for any other count, in English
/// @param text how texts are looked up
/// @return such as "1 package" or "31 packages"
std::string counted_things(
    std::size_t count, std::string_view one, std::string_view many, const TextHooks& text
) {
    const std::string number = std::to_string(count);
    return filled(text, count == 1 ? one : many, {{"n", number}});
}

/// Joins names as a sentence lists them: "A", "A and B", "A, B and C".
///
/// @param names the names
/// @return the list
std::string listed_names(const std::vector<std::string>& names) {
    std::string text;
    for (std::size_t index = 0; index < names.size(); ++index) {
        if (index > 0)
            text += index + 1 == names.size() ? " and " : ", ";
        text += names[index];
    }
    return text;
}

} // namespace

std::string registry_refresh_line(const Registry& registry, const TextHooks& text) {
    const std::string separator = " · ";
    if (registry.unreachable) {
        std::string line(shown(text, words::registry_unreachable));
        if (registry.checked_ago)
            line += separator + registry_age_text(*registry.checked_ago, text);
        return line;
    }
    if (registry.checked_ago)
        return filled(
            text,
            words::registry_refreshed,
            {{"age", registry_age_text(*registry.checked_ago, text)}}
        );
    return std::string(shown(text, words::registry_never));
}

std::string registry_subtitle(const Registry& registry, const TextHooks& text) {
    const std::string separator = " · ";
    std::string line = counted_things(
        registry.packages, words::registry_package_one, words::registry_package_many, text
    );
    if (!registry.built_in)
        line += separator + std::string(shown(text, words::registry_not_reviewed));
    if (registry.mirrors > 0)
        line += separator +
                counted_things(
                    registry.mirrors, words::registry_mirror_one, words::registry_mirror_many, text
                );
    return line;
}

std::string registry_note(const std::vector<Registry>& registries, const TextHooks& text) {
    std::vector<std::string> names;
    for (const Registry& registry : registries)
        if (registry.built_in)
            names.push_back(registry.name);
    if (names.empty())
        return std::string(shown(text, words::registry_note_unnamed));
    return filled(text, words::registry_note, {{"names", listed_names(names)}});
}

} // namespace oa::ui::library
