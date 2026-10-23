// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's entries built from its inputs, browsing, each tab's filter
// and the filters' counts, selection, what the details and the footer say
// about the entries, and what the Active tab says of the player's setup.
#include "oa/ui/library/library.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/kit/tag_icon.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "visible.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

namespace hacks = data::mod_profile::registry;

using Phase = QueueItem::Phase;

/// A whole percentage.
constexpr uint64_t percent_whole = 100;

/// Returns the language pack the game is showing its words from.
///
/// @param library the Library
/// @return the entry marked in use; null when the language is built in or not installed
const Entry* language_in_use(const Library& library) {
    for (const Entry& entry : library.entries)
        if (entry.id.kind == Kind::language && entry.installed != nullptr &&
            entry.installed->in_use)
            return &entry;
    return nullptr;
}

/// Returns an entry's badge: its listing's, else its installed copy's.
///
/// @param entry the entry
/// @return the badge, which points into the Library's inputs; null without one
const Badge* badge_of(const Entry& entry) {
    if (entry.listing != nullptr && entry.listing->badge)
        return &*entry.listing->badge;
    if (entry.installed != nullptr && entry.installed->badge)
        return &*entry.installed->badge;
    return nullptr;
}

/// Returns who made an entry, as the details name an author.
///
/// @param entry the entry
/// @return the author; empty when nobody is named
std::string author_of(const Entry& entry) {
    std::string author;
    if (entry.listing != nullptr)
        author = entry.listing->author.empty() ? entry.listing->publisher : entry.listing->author;
    if (author.empty() && entry.installed != nullptr)
        author = entry.installed->author;
    return author;
}

/// Returns an author with space and tab taken off either end.
///
/// @param author the author
/// @return the name inside that space
std::string_view trimmed_author(std::string_view author) noexcept {
    while (!author.empty() && (author.front() == ' ' || author.front() == '\t'))
        author.remove_prefix(1);
    while (!author.empty() && (author.back() == ' ' || author.back() == '\t'))
        author.remove_suffix(1);
    return author;
}

/// Tells whether an author is a name, ignoring ASCII case.
///
/// @param author the author, already trimmed
/// @param lower the name in lower case
/// @return true when they are the same letters
bool author_is(std::string_view author, std::string_view lower) noexcept {
    if (author.size() != lower.size())
        return false;
    for (std::size_t index = 0; index < author.size(); ++index) {
        unsigned char byte = static_cast<unsigned char>(author[index]);
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<unsigned char>(byte - 'A' + 'a');
        if (byte != static_cast<unsigned char>(lower[index]))
            return false;
    }
    return true;
}

/// Returns the author a mark is matched against.
///
/// The listing's author when that listing names one, otherwise the installed
/// manifest's. Empty when neither names one. The publisher is not read, so
/// a package that names no author shows no mark.
///
/// @param entry the entry
/// @return the author
std::string_view maker_author(const Entry& entry) noexcept {
    if (entry.listing != nullptr && !entry.listing->author.empty())
        return entry.listing->author;
    if (entry.installed != nullptr)
        return entry.installed->author;
    return {};
}

/// Returns a coverage as the Active tile states it.
///
/// @param text how texts are looked up
/// @param coverage the fraction, from 0 to 1
/// @return such as "Coverage 92%"
std::string coverage_line(const TextHooks& text, double coverage) {
    const double clamped = std::clamp(coverage, 0.0, 1.0);
    const auto percent = static_cast<uint64_t>(clamped * static_cast<double>(percent_whole) + 0.5);
    return filled(text, words::active_coverage, {{"percent", std::to_string(percent)}});
}

/// Finds a registry by its id.
///
/// @param inputs the inputs
/// @param id the registry's id
/// @return the registry, or null when the inputs do not name it
const Registry* find_registry(const Inputs& inputs, std::string_view id) {
    for (const Registry& registry : inputs.registries)
        if (registry.id == id)
            return &registry;
    return nullptr;
}

/// Tells whether a queue item is still to finish.
///
/// @param phase the item's phase
/// @return true from waiting to installing, and while held
bool pending(Phase phase) {
    return phase != Phase::failed && phase != Phase::installed;
}

/// Tells whether a queue item is being worked on now.
///
/// @param phase the item's phase
/// @return true while downloading, verifying, checking or installing
bool active(Phase phase) {
    return phase == Phase::downloading || phase == Phase::verifying || phase == Phase::checking ||
           phase == Phase::installing;
}

/// Lists the hacks a listing turns on that this Open Annihilation does not carry out.
///
/// @param listing the listing
/// @return the hack ids, in catalogue order
std::vector<std::string_view> missing_hacks(const Listing& listing) {
    std::vector<std::string_view> missing;
    for (const std::string& id : listing.hack_ids) {
        const hacks::Entry* entry = hacks::find_entry(id);
        if (entry == nullptr || !entry->implemented)
            missing.push_back(id);
    }
    return missing;
}

/// Names missing hacks: up to named_missing_hacks of them, then how many more.
///
/// @param missing the hack ids
/// @param text how texts are looked up
/// @return the list, such as "a.b, c.d, e.f · 2 more"
std::string
missing_hacks_text(const std::vector<std::string_view>& missing, const TextHooks& text) {
    std::string named;
    const std::size_t shown_count = std::min(missing.size(), named_missing_hacks);
    for (std::size_t index = 0; index < shown_count; ++index) {
        if (index != 0) {
            named += shown(text, words::list_separator);
            named.push_back(' ');
        }
        named += missing[index];
    }
    if (missing.size() <= named_missing_hacks)
        return named;
    const std::string more = std::to_string(missing.size() - named_missing_hacks);
    return filled(text, words::hacks_and_more, {{"hacks", named}, {"count", more}});
}

/// Writes why GET or UPDATE is off for an entry.
///
/// The reasons are tried in order: a game is being played; the registry's
/// downloads are off; for GET, the engine requirement, the base game and
/// the hacks; for UPDATE, what the update note says.
///
/// @param library the Library
/// @param entry the entry, with its state and registry fields set
/// @return the reason; empty when nothing blocks it
std::string blocked_reason(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    const Inputs& inputs = library.inputs;
    if (entry.state != State::get && entry.state != State::update)
        return {};
    if (inputs.in_match)
        return std::string(shown(text, words::blocked_in_match));
    const Registry* registry = find_registry(inputs, entry.id.registry);
    if (registry != nullptr && !registry->downloads_on) {
        if (!registry->downloads_off_reason.empty())
            return registry->downloads_off_reason;
        return filled(text, words::blocked_downloads_off, {{"registry", entry.registry_name}});
    }
    // A brain that does not fit the game being played says so on the row.
    // That is information: it does not block GET or UPDATE.
    if (entry.id.kind == Kind::brain)
        return {};
    if (entry.state == State::update) {
        const UpdateNote& update = *entry.update;
        switch (update.blocked) {
        case UpdateNote::Blocked::none:
            return {};
        case UpdateNote::Blocked::engine:
            return filled(text, words::blocked_engine, {{"range", update.blocked_detail}});
        case UpdateNote::Blocked::hacks:
            return filled(text, words::blocked_hacks, {{"hacks", update.blocked_detail}});
        case UpdateNote::Blocked::base:
            return filled(text, words::blocked_base, {{"base", update.blocked_detail}});
        }
        return {};
    }
    const Listing& listing = *entry.listing;
    if (const std::optional<formats::oamod::EngineRange> range =
            formats::oamod::parse_engine_range(listing.requires_engine)) {
        if (!formats::oamod::engine_range_met(*range, inputs.engine))
            return filled(text, words::blocked_engine, {{"range", engine_text(*range, text)}});
    }
    if (!listing.base.empty() && listing.base != supported_base)
        return filled(text, words::blocked_base, {{"base", listing.base}});
    const std::vector<std::string_view> missing = missing_hacks(listing);
    if (!missing.empty())
        return filled(text, words::blocked_hacks, {{"hacks", missing_hacks_text(missing, text)}});
    return {};
}

/// Returns the first prerequisite no installed mutator satisfies.
///
/// A prerequisite is met by an installed mutator of the same id whose
/// version the range holds. An empty range is any version. The name is
/// returned when one is known, otherwise the id. An empty string means
/// every prerequisite is met.
///
/// @param inputs the Library's inputs
/// @param facts the mutator's facts
/// @return the first unmet prerequisite, or empty
std::string first_unmet_need(const Inputs& inputs, const MutatorFacts& facts) {
    for (const Prerequisite& need : facts.needs) {
        bool met = false;
        for (const Installed& item : inputs.installed) {
            if (item.kind != Kind::mutator || item.key != need.id)
                continue;
            formats::oamut::Condition condition;
            condition.id = need.id;
            condition.range_text = need.range;
            if (!need.range.empty()) {
                const std::optional<formats::oamut::VersionRange> range =
                    formats::oamut::parse_version_range(need.range);
                if (!range)
                    continue;
                condition.range = *range;
            }
            if (formats::oamut::condition_met(condition, need.id, item.version)) {
                met = true;
                break;
            }
        }
        if (!met)
            return need.name.empty() ? need.id : need.name;
    }
    return {};
}

/// Sets an entry's state, registry fields and whether it can act.
///
/// A mutator's compatibility and its first unmet prerequisite are recorded
/// on the entry. Neither blocks GET or UPDATE, and neither dims the row.
///
/// @param library the Library whose inputs the entry points into
/// @param[in,out] entry the entry, with its pointers set
void settle_entry(const Library& library, Entry& entry) {
    if (entry.update != nullptr)
        entry.state = State::update;
    else if (entry.installed != nullptr && entry.installed->playing)
        entry.state = State::playing;
    else if (entry.installed != nullptr)
        entry.state = State::installed;
    else
        entry.state = State::get;

    if (!entry.id.registry.empty()) {
        const Registry* registry = find_registry(library.inputs, entry.id.registry);
        entry.built_in = registry != nullptr && registry->built_in;
        entry.not_reviewed = !entry.built_in;
        entry.registry_name = registry != nullptr ? registry->name : entry.id.registry;
    }
    if (entry.id.kind == Kind::mutator) {
        if (const MutatorFacts* facts = mutator_facts(entry)) {
            entry.not_compatible = !works_with_played(library.inputs, *facts);
            if (facts->verdict && facts->verdict->state == Verdict::State::incompatible)
                entry.not_compatible = true;
            entry.needs_missing = first_unmet_need(library.inputs, *facts);
        }
    } else if (entry.id.kind == Kind::brain) {
        if (const BrainFacts* facts = brain_facts(entry))
            entry.not_compatible =
                !brain_unfit_reason(library.inputs, *facts, library.text).empty();
    }
    entry.blocked = blocked_reason(library, entry);
    entry.can_act = entry.blocked.empty();
}

/// Tells whether a mod is played: an installed mod marked playing, or a
/// mod the inputs name outside the packages.
///
/// @param library the Library
/// @return true when one is
bool mod_is_played(const Library& library) {
    if (library.inputs.playing_mod)
        return true;
    for (const Installed& installed : library.inputs.installed)
        if (installed.kind == Kind::mod && installed.playing)
            return true;
    return false;
}

/// Sets the base game entry: played while no mod is, otherwise installed,
/// with its name, summary and badge, and with no package behind it.
///
/// @param library the Library
/// @param[in,out] entry the base game entry
void settle_base_game(const Library& library, Entry& entry) {
    entry.listing = nullptr;
    entry.installed = nullptr;
    entry.update = nullptr;
    entry.queued = nullptr;
    entry.replaces_registry_name.clear();
    entry.built_in = false;
    entry.not_reviewed = false;
    entry.registry_name.clear();
    entry.blocked.clear();
    entry.can_act = true;
    entry.state = mod_is_played(library) ? State::installed : State::playing;
    entry.badge = library.inputs.base_game_badge ? &*library.inputs.base_game_badge : nullptr;
    entry.shown_name = std::string(shown(library.text, words::no_mod));
    const ActiveContent face = active_content(library);
    entry.shown_summary =
        filled(library.text, words::active_as_made, {{"game", std::string(face.name)}});
}

/// Makes sure the base game entry is present, and settles it.
///
/// An entry a package already took under the same id is kept as the base
/// game entry: a package key cannot be base_game_key, and the entry offers
/// none of a package's actions.
///
/// @param[in,out] library the Library, its other entries already settled
void ensure_base_game(Library& library) {
    const EntryId id = base_game_id();
    std::optional<std::size_t> place;
    for (std::size_t index = 0; index < library.entries.size(); ++index)
        if (library.entries[index].id == id)
            place = index;
    if (!place) {
        Entry added;
        added.id = id;
        library.entries.push_back(std::move(added));
        place = library.entries.size() - 1;
    }
    settle_base_game(library, library.entries[*place]);
}

/// Tells whether a built-in language is the one the game shows.
///
/// A pack in use (`LanguageInUse::pack`) means none of them is. A tag names
/// one directly. With no tag, an empty name is English, and any other name
/// is that language's name in itself.
///
/// @param library the Library
/// @param tag the language's BCP-47 tag
/// @param endonym its name in itself
/// @return true when that language is in use
bool built_in_language_in_use(
    const Library& library, std::string_view tag, std::string_view endonym
) {
    const LanguageInUse& shown = library.inputs.language;
    if (!shown.pack.empty())
        return false;
    if (!shown.tag.empty())
        return shown.tag == tag;
    if (shown.name.empty())
        return tag == data::languages::english().tag;
    return shown.name == endonym;
}

/// Sets a built-in language entry: its name in itself and in English, in
/// use or not, and with no package behind it.
///
/// @param library the Library
/// @param language the language, one known_languages holds as built in
/// @param[in,out] entry the entry
void settle_built_in_language(
    const Library& library, const data::languages::Language& language, Entry& entry
) {
    entry.listing = nullptr;
    entry.installed = nullptr;
    entry.update = nullptr;
    entry.queued = nullptr;
    entry.replaces_registry_name.clear();
    entry.built_in = false;
    entry.not_reviewed = false;
    entry.registry_name.clear();
    entry.blocked.clear();
    entry.can_act = true;
    entry.badge = nullptr;
    entry.shown_name = std::string(language.endonym);
    entry.shown_summary = language.english_name == language.endonym
                              ? std::string{}
                              : std::string(language.english_name);
    entry.state = built_in_language_in_use(library, language.tag, language.endonym)
                      ? State::playing
                      : State::installed;
}

/// Makes sure each built-in, playable language has an entry, and settles it.
///
/// The list is the one Settings › Language shows above its packs:
/// known_languages, kept where the source is built in and the language is
/// playable. A package cannot take one of these ids.
///
/// @param[in,out] library the Library, its other entries already settled
void ensure_built_in_languages(Library& library) {
    const std::span<const data::languages::Language* const> languages =
        data::languages::known_languages();
    for (const data::languages::Language* language : languages) {
        if (language == nullptr || language->source != data::languages::Source::built_in ||
            !data::languages::playable(*language))
            continue;
        const EntryId id = built_in_language_id(language->tag);
        std::optional<std::size_t> place;
        for (std::size_t index = 0; index < library.entries.size(); ++index)
            if (library.entries[index].id == id)
                place = index;
        if (!place) {
            Entry added;
            added.id = id;
            library.entries.push_back(std::move(added));
            place = library.entries.size() - 1;
        }
        settle_built_in_language(library, *language, library.entries[*place]);
    }
}

/// Sets the built-in computer player: installed, with its name and summary,
/// and with no package behind it.
///
/// @param library the Library
/// @param[in,out] entry the built-in entry
void settle_built_in_brain(const Library& library, Entry& entry) {
    entry.listing = nullptr;
    entry.installed = nullptr;
    entry.update = nullptr;
    entry.queued = nullptr;
    entry.replaces_registry_name.clear();
    entry.built_in = false;
    entry.not_reviewed = false;
    entry.registry_name.clear();
    entry.blocked.clear();
    entry.can_act = true;
    entry.not_compatible = false;
    entry.needs_missing.clear();
    entry.is_default = false;
    entry.badge = nullptr;
    entry.shown_name = std::string(shown(library.text, words::built_in_brain_name));
    entry.shown_summary = std::string(shown(library.text, words::built_in_brain_summary));
    entry.state = State::installed;
}

/// Makes sure the built-in computer player is present, and settles it.
///
/// An entry a package already took under the same id is kept as the built-in
/// player: a package key cannot be built_in_brain_key, and the entry offers
/// none of a package's actions. The default is the installed brain whose key
/// is Inputs::default_brain, or this entry when that names none.
///
/// @param[in,out] library the Library, its other entries already settled
void ensure_built_in_brain(Library& library) {
    const EntryId id = built_in_brain_id();
    std::optional<std::size_t> place;
    for (std::size_t index = 0; index < library.entries.size(); ++index)
        if (library.entries[index].id == id)
            place = index;
    if (!place) {
        Entry added;
        added.id = id;
        library.entries.push_back(std::move(added));
        place = library.entries.size() - 1;
    }
    settle_built_in_brain(library, library.entries[*place]);
    const std::string& wanted = library.inputs.default_brain;
    bool named = false;
    for (Entry& entry : library.entries) {
        entry.is_default = false;
        if (wanted.empty() || entry.id.kind != Kind::brain || entry.installed == nullptr)
            continue;
        if (entry.id.key != wanted)
            continue;
        entry.is_default = true;
        named = true;
    }
    if (!named)
        library.entries[*place].is_default = true;
}

/// Makes the selection one of the visible entries.
///
/// A selection that still shows stays. Otherwise the first visible entry
/// is selected, or nothing when none shows, and the details close.
///
/// @param[in,out] library the Library, its visible list up to date
void settle_selection(Library& library) {
    if (library.selected) {
        for (const std::size_t index : library.visible)
            if (library.entries[index].id == *library.selected)
                return;
    }
    if (library.visible.empty())
        library.selected.reset();
    else
        library.selected = library.entries[library.visible.front()].id;
    library.details_open = false;
}

/// Recomputes the visible entries and keeps the selection among them.
///
/// @param[in,out] library the Library
void recompute(Library& library) {
    update_visible(library);
    settle_selection(library);
}

/// Writes a queue item's name and version.
///
/// @param item the item
/// @param text how texts are looked up
/// @return such as "Fixture Mod 4.9"; the name alone when it has no version
std::string package_text(const QueueItem& item, const TextHooks& text) {
    if (item.version.empty())
        return item.name;
    return filled(text, words::queue_package, {{"name", item.name}, {"version", item.version}});
}

/// Names a queue item's phase in one or a few words.
///
/// @param phase the phase
/// @param text how texts are looked up
/// @return the words, such as "checking"
std::string_view phase_text(Phase phase, const TextHooks& text) {
    switch (phase) {
    case Phase::waiting:
        return shown(text, words::phase_waiting);
    case Phase::downloading:
        return shown(text, words::phase_downloading);
    case Phase::verifying:
        return shown(text, words::phase_verifying);
    case Phase::checking:
        return shown(text, words::phase_checking);
    case Phase::installing:
        return shown(text, words::phase_installing);
    case Phase::held:
        return shown(text, words::phase_held);
    case Phase::failed:
        return shown(text, words::phase_failed);
    case Phase::installed:
        return shown(text, words::phase_installed);
    }
    return {};
}

/// Picks the queue item the footer's first line describes.
///
/// @param queue the queue
/// @return its index; the queue is not empty
std::size_t headline_item(const std::vector<QueueItem>& queue) {
    const auto first_where = [&](auto&& wanted) -> std::optional<std::size_t> {
        for (std::size_t index = 0; index < queue.size(); ++index)
            if (wanted(queue[index].phase))
                return index;
        return std::nullopt;
    };
    if (const auto index = first_where([](Phase phase) { return phase == Phase::downloading; }))
        return *index;
    if (const auto index = first_where(active))
        return *index;
    if (const auto index = first_where([](Phase phase) { return phase == Phase::failed; }))
        return *index;
    if (const auto index = first_where([](Phase phase) { return phase == Phase::held; }))
        return *index;
    if (const auto index = first_where([](Phase phase) { return phase == Phase::waiting; }))
        return *index;
    return queue.size() - 1;
}

/// Writes the footer's first line for a queue item.
///
/// @param item the item
/// @param text how texts are looked up
/// @return the line
std::string first_queue_line(const QueueItem& item, const TextHooks& text) {
    switch (item.phase) {
    case Phase::waiting:
        return filled(text, words::queue_waiting, {{"package", package_text(item, text)}});
    case Phase::downloading:
        if (item.total_bytes == 0)
            return filled(
                text, words::queue_downloading_unsized, {{"package", package_text(item, text)}}
            );
        return filled(
            text,
            words::queue_downloading,
            {{"package", package_text(item, text)},
             {"progress", progress_text(item.done_bytes, item.total_bytes, text)}}
        );
    case Phase::verifying:
        return filled(text, words::queue_verifying, {{"name", item.name}});
    case Phase::checking:
        return filled(text, words::queue_checking, {{"name", item.name}});
    case Phase::installing:
        return filled(text, words::queue_installing, {{"name", item.name}});
    case Phase::held:
        return filled(text, words::queue_held, {{"name", item.name}});
    case Phase::failed:
        if (item.retryable)
            return filled(text, words::queue_failed_retry, {{"problem", item.problem}});
        return item.problem;
    case Phase::installed:
        switch (item.update_rules) {
        case UpdateNote::Rules::same:
            break;
        case UpdateNote::Rules::changes:
            return filled(
                text,
                words::queue_updated_changes,
                {{"name", item.name}, {"new", item.update_to}, {"old", item.update_from}}
            );
        case UpdateNote::Rules::unknown:
            return filled(
                text,
                words::queue_updated_unknown,
                {{"name", item.name}, {"new", item.update_to}, {"old", item.update_from}}
            );
        }
        return filled(text, words::queue_installed, {{"name", item.name}});
    }
    return {};
}

/// Writes the footer's line for a small screen.
///
/// @param item the item the first line describes
/// @param text how texts are looked up
/// @return the line
std::string compact_queue_line(const QueueItem& item, const TextHooks& text) {
    if (item.phase == Phase::downloading) {
        uint64_t percent = 0;
        if (item.total_bytes != 0 && item.done_bytes >= item.total_bytes)
            percent = percent_whole;
        else if (
            item.total_bytes != 0 &&
            item.done_bytes <= std::numeric_limits<uint64_t>::max() / percent_whole
        )
            percent = item.done_bytes * percent_whole / item.total_bytes;
        else if (item.total_bytes != 0)
            percent = item.done_bytes / (item.total_bytes / percent_whole);
        const std::string number = std::to_string(percent);
        return filled(text, words::queue_percent, {{"name", item.name}, {"percent", number}});
    }
    if (item.phase == Phase::installed && item.update_rules != UpdateNote::Rules::same)
        return filled(text, words::queue_rules_changed, {{"name", item.name}});
    return filled(
        text, words::queue_phase, {{"name", item.name}, {"phase", phase_text(item.phase, text)}}
    );
}

/// Writes the line under an entry's name while its queue item is unfinished.
///
/// @param item the item
/// @param text how texts are looked up
/// @return the line
std::string queue_status(const QueueItem& item, const TextHooks& text) {
    switch (item.phase) {
    case Phase::waiting:
        return std::string(shown(text, words::status_waiting));
    case Phase::downloading:
        if (item.total_bytes == 0)
            return std::string(shown(text, words::status_downloading_unsized));
        return filled(
            text,
            words::status_downloading,
            {{"progress", progress_text(item.done_bytes, item.total_bytes, text)}}
        );
    case Phase::verifying:
        return std::string(shown(text, words::status_verifying));
    case Phase::checking:
        return std::string(shown(text, words::status_checking));
    case Phase::installing:
        return std::string(shown(text, words::status_installing));
    case Phase::held:
        return filled(text, words::queue_held, {{"name", item.name}});
    case Phase::failed:
        if (item.problem.empty())
            return std::string(shown(text, words::status_failed));
        return item.problem;
    case Phase::installed:
        break;
    }
    return {};
}

/// Writes the Rules fact of an update.
///
/// @param library the Library
/// @param entry the entry, with an update
/// @return the fact
Fact update_rules_fact(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    const UpdateNote& update = *entry.update;
    const ChangeTexts names = change_texts(*entry.installed, update, text);
    const std::string from = update.from_rules ? rules_text(*update.from_rules) : std::string();
    const std::string to = update.to_rules ? rules_text(*update.to_rules) : std::string();
    Fact fact;
    fact.long_label = shown(text, words::fact_rules);
    fact.short_label = fact.long_label;
    switch (update.rules) {
    case UpdateNote::Rules::same:
        fact.long_value = from.empty() || to.empty()
                              ? std::string(shown(text, words::rules_same_short))
                              : filled(text, words::rules_same, {{"from", from}, {"to", to}});
        fact.short_value = shown(text, words::rules_same_short);
        break;
    case UpdateNote::Rules::changes:
        fact.long_value = filled(
            text,
            words::rules_changes,
            {{"from", from}, {"to", to}, {"old", names.from}, {"new", names.to}}
        );
        fact.short_value = filled(text, words::rules_changes_short, {{"old", names.from_short}});
        fact.mark = Mark::warn;
        break;
    case UpdateNote::Rules::unknown:
        fact.long_value =
            filled(text, words::rules_unknown, {{"old", names.from}, {"new", names.to}});
        fact.short_value = shown(text, words::rules_unknown_short);
        fact.mark = Mark::warn;
        break;
    }
    return fact;
}

/// Makes a fact whose long and short forms share a label.
///
/// @param label the label, looked up
/// @param long_value the long value
/// @param short_value the short value
/// @param mark how the value is marked
/// @return the fact
Fact fact_of(std::string_view label, std::string long_value, std::string short_value, Mark mark) {
    Fact fact;
    fact.long_label = label;
    fact.short_label = label;
    fact.long_value = std::move(long_value);
    fact.short_value = std::move(short_value);
    fact.mark = mark;
    return fact;
}

/// Joins phrases with a comma between all but the last pair.
///
/// @param text how texts are looked up
/// @param parts the phrases
/// @param last the pattern for the last pair
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

/// Tells whether an installed mutator meets one prerequisite.
///
/// @param inputs the Library's inputs
/// @param need the prerequisite
/// @return true when an installed copy of that id is inside the range
bool prerequisite_met(const Inputs& inputs, const Prerequisite& need) {
    for (const Installed& item : inputs.installed) {
        if (item.kind != Kind::mutator || item.key != need.id)
            continue;
        formats::oamut::Condition condition;
        condition.id = need.id;
        condition.range_text = need.range;
        if (!need.range.empty()) {
            const std::optional<formats::oamut::VersionRange> range =
                formats::oamut::parse_version_range(need.range);
            if (!range)
                continue;
            condition.range = *range;
        }
        if (formats::oamut::condition_met(condition, need.id, item.version))
            return true;
    }
    return false;
}

/// The name a Checked fact fills for the mod being played.
///
/// @param inputs the Library's inputs
/// @param text how texts are looked up
/// @return the played mod's name, else the playing mod's, else "No Mod"
std::string played_mod_name(const Inputs& inputs, const TextHooks& text) {
    if (!inputs.played.name.empty())
        return inputs.played.name;
    if (inputs.playing_mod && !inputs.playing_mod->name.empty())
        return inputs.playing_mod->name;
    return std::string(shown(text, words::no_mod));
}

/// Writes a mutator's version for its "not installed" status.
///
/// A revision above 0 is included, including 1, as "2.0 · rev 1". A revision
/// of 0 is the version alone.
///
/// @param version the version, as written
/// @param revision the packaging revision
/// @param text how texts are looked up
/// @return the version
std::string
mutator_status_version(std::string_view version, int64_t revision, const TextHooks& text) {
    if (revision <= 0)
        return std::string(version);
    const std::string number = std::to_string(revision);
    if (version.empty())
        return filled(text, words::revision_short, {{"revision", number}});
    return filled(text, words::version_with_revision, {{"version", version}, {"revision", number}});
}

/// Appends a mutator's facts after Author: Works with, Checked, Needs,
/// Options, Changes and Rules. From is written by the caller.
///
/// @param library the Library
/// @param entry the mutator
/// @param[in,out] list the facts so far
void append_mutator_facts(const Library& library, const Entry& entry, std::vector<Fact>& list) {
    const TextHooks& text = library.text;
    const Listing* listing = entry.listing;
    const MutatorFacts* mutator = mutator_facts(entry);
    const MutatorFacts empty;
    const MutatorFacts& facts = mutator != nullptr ? *mutator : empty;

    std::vector<std::string> mod_names;
    bool any_mod = facts.works_with.empty();
    for (const WorksWith& item : facts.works_with) {
        if (item.id == "*")
            any_mod = true;
        else if (!item.name.empty())
            mod_names.push_back(item.name);
        else if (!item.id.empty())
            mod_names.push_back(item.id);
    }
    const std::string works = any_mod ? std::string(shown(text, words::works_with_any))
                                      : join_parts(text, mod_names, words::names_and);
    if (!works.empty()) {
        list.push_back(fact_of(
            shown(text, words::fact_works_with),
            works,
            works,
            entry.not_compatible ? Mark::warn : Mark::none
        ));
    }

    if (entry.installed != nullptr && facts.verdict) {
        const CheckedWords checked =
            checked_text(*facts.verdict, played_mod_name(library.inputs, text), text);
        list.push_back(
            fact_of(shown(text, words::fact_checked), checked.value, checked.value, checked.mark)
        );
    }

    for (const Prerequisite& need : facts.needs) {
        const std::string name = need.name.empty() ? need.id : need.name;
        if (name.empty())
            continue;
        const std::string range = range_words(need.range, text);
        const bool met = prerequisite_met(library.inputs, need);
        // An empty range would leave a blank in "{name} {range}". The met
        // line reuses the row's "{name} ✓".
        const std::string value =
            range.empty() ? filled(
                                text,
                                met ? words::name_met : words::needs_mutator_missing_name,
                                {{"name", name}}
                            )
                          : filled(
                                text,
                                met ? words::needs_mutator_met : words::needs_mutator_missing,
                                {{"name", name}, {"range", range}}
                            );
        list.push_back(
            fact_of(shown(text, words::fact_needs), value, value, met ? Mark::good : Mark::warn)
        );
    }
    if (listing != nullptr) {
        if (const std::optional<formats::oamod::EngineRange> range =
                formats::oamod::parse_engine_range(listing->requires_engine)) {
            const bool met = formats::oamod::engine_range_met(*range, library.inputs.engine);
            list.push_back(fact_of(
                shown(text, words::fact_needs),
                filled(text, words::needs_engine, {{"range", engine_text(*range, text)}}),
                filled(
                    text, words::needs_engine_short, {{"range", engine_short_text(*range, text)}}
                ),
                met ? Mark::good : Mark::warn
            ));
        }
    }

    for (const OptionLine& option : facts.options) {
        if (option.label.empty() && option.values.empty())
            continue;
        const std::string values = join_parts(text, option.values, words::names_or);
        const std::string value =
            filled(text, words::option_values, {{"label", option.label}, {"values", values}});
        list.push_back(fact_of(shown(text, words::fact_options), value, value, Mark::none));
    }

    std::vector<std::string> layers;
    for (const std::string& id : facts.layers) {
        const std::string_view word = layer_word(id);
        if (!word.empty())
            layers.push_back(std::string(shown(text, word)));
        else if (!id.empty())
            layers.push_back(id);
    }
    if (!layers.empty()) {
        const std::string changes = join_parts(text, layers, words::names_and);
        list.push_back(fact_of(shown(text, words::fact_changes), changes, changes, Mark::none));
    }

    const std::string rules(shown(text, words::rules_mutator));
    list.push_back(fact_of(shown(text, words::fact_rules), rules, rules, Mark::none));
}

/// Tells whether a finished queue item still stands in for the installed copy.
///
/// The install can finish before the installed packages are read again, and
/// the note from the last search stays. While that reading is still the one
/// the item finished against, the item's release is the copy in place. A
/// later reading is the copy on disk: a roll back, or an older item, no
/// longer hides the note. When the inputs name no reading, or the item names
/// none, the finished release hides the note on its own.
///
/// @param library the Library, with its inputs stored
/// @param item the queue item
/// @return true when this finished item still counts as the copy in place
bool finished_item_stands_in(const Library& library, const QueueItem& item) {
    if (item.phase != Phase::installed || item.release <= 0)
        return false;
    if (library.inputs.installed_generation == 0 || item.finished_scan == 0)
        return true;
    return item.finished_scan == library.inputs.installed_generation;
}

/// Tells whether an update is already in place.
///
/// The installed release, or a finished queue item that still stands in for
/// it, both above 0 and at least the note's, means the player already has
/// that catalogue release. A release of 0 names none, and a held item is not
/// finished. The listing's release is the newest the catalogue names, not
/// the copy in place.
///
/// @param library the Library, with its inputs stored
/// @param update the note
/// @return true when that release is already installed or queued as installed
bool update_already_in_place(const Library& library, const UpdateNote& update) {
    for (const Installed& installed : library.inputs.installed) {
        if (installed.registry == update.registry && installed.kind == update.kind &&
            installed.key == update.key && installed.release > 0 &&
            installed.release >= update.to_release)
            return true;
    }
    for (const QueueItem& item : library.inputs.queue) {
        if (item.registry == update.registry && item.kind == update.kind &&
            item.key == update.key && finished_item_stands_in(library, item) &&
            item.release >= update.to_release)
            return true;
    }
    return false;
}

} // namespace

std::string queue_status_text(const QueueItem& item, const TextHooks& text) {
    return queue_status(item, text);
}

PackageMaker package_maker(std::string_view author) noexcept {
    author = trimmed_author(author);
    if (author_is(author, "openannihilation") || author_is(author, "open annihilation") ||
        author_is(author, "oa team"))
        return PackageMaker::open_annihilation;
    if (author_is(author, "cavedog entertainment"))
        return PackageMaker::cavedog;
    return PackageMaker::none;
}

PackageMaker entry_maker(const Entry& entry) noexcept {
    if (!entry.built_in)
        return PackageMaker::none;
    return package_maker(maker_author(entry));
}

void refresh(Library& library, Inputs inputs) {
    library.inputs = std::move(inputs);
    library.entries.clear();
    const Inputs& in = library.inputs;
    std::map<EntryId, std::size_t> by_id;
    const auto entry_for = [&](const EntryId& id) -> Entry& {
        const auto [found, added] = by_id.emplace(id, library.entries.size());
        if (added) {
            Entry entry;
            entry.id = id;
            library.entries.push_back(std::move(entry));
        }
        return library.entries[found->second];
    };
    for (const Listing& listing : in.listings) {
        Entry& entry = entry_for({listing.registry, listing.kind, listing.key});
        if (entry.listing == nullptr)
            entry.listing = &listing;
    }
    for (const Installed& installed : in.installed) {
        Entry& entry = entry_for({installed.registry, installed.kind, installed.key});
        if (entry.installed == nullptr)
            entry.installed = &installed;
    }
    for (const UpdateNote& update : in.updates) {
        const auto found = by_id.find({update.registry, update.kind, update.key});
        if (found == by_id.end())
            continue;
        Entry& entry = library.entries[found->second];
        if (entry.installed != nullptr && entry.update == nullptr &&
            !update_already_in_place(library, update))
            entry.update = &update;
    }
    for (const QueueItem& item : in.queue) {
        // An expansion is drawn on the Active tab, not on a package row that
        // happens to share its key.
        if (item.expansion)
            continue;
        const auto found = by_id.find({item.registry, item.kind, item.key});
        if (found != by_id.end())
            library.entries[found->second].queued = &item;
    }
    for (Entry& entry : library.entries)
        settle_entry(library, entry);
    for (Entry& entry : library.entries) {
        if (entry.state != State::get)
            continue;
        for (const Installed& installed : in.installed) {
            if (installed.kind != entry.id.kind || installed.key != entry.id.key)
                continue;
            if (installed.registry.empty() || installed.registry == entry.id.registry)
                continue;
            const Registry* registry = find_registry(in, installed.registry);
            entry.replaces_registry_name =
                registry != nullptr ? registry->name : installed.registry;
            break;
        }
    }
    note_content(library);
    adopt_downloaded_expansions(library);
    if (library.inputs.content.expansion_cleared)
        library.expansion_note = true;
    ensure_base_game(library);
    ensure_built_in_languages(library);
    ensure_built_in_brain(library);
    recompute(library);
}

void set_tab(Library& library, Tab tab) {
    library.tab = tab;
    recompute(library);
}

void set_filter(Library& library, Filter filter) {
    library.filters[static_cast<std::size_t>(library.tab)] = filter;
    recompute(library);
}

Filter shown_filter(const Library& library) {
    return library.filters[static_cast<std::size_t>(library.tab)];
}

void set_tag(Library& library, std::string_view tag) {
    library.tag = tag;
    recompute(library);
}

void set_query(Library& library, std::string_view query) {
    library.query = capped_query(query);
    recompute(library);
}

void select(Library& library, const EntryId& id) {
    update_visible(library);
    for (const std::size_t index : library.visible) {
        if (library.entries[index].id == id) {
            library.selected = id;
            break;
        }
    }
    settle_selection(library);
}

void move_selection(Library& library, int delta) {
    update_visible(library);
    if (library.visible.empty()) {
        settle_selection(library);
        return;
    }
    std::optional<std::size_t> at;
    if (library.selected) {
        for (std::size_t row = 0; row < library.visible.size(); ++row)
            if (library.entries[library.visible[row]].id == *library.selected)
                at = row;
    }
    if (!at) {
        settle_selection(library);
        return;
    }
    const auto last = static_cast<int64_t>(library.visible.size()) - 1;
    const int64_t row = std::clamp(static_cast<int64_t>(*at) + delta, int64_t{0}, last);
    library.selected = library.entries[library.visible[static_cast<std::size_t>(row)]].id;
}

void open_details(Library& library) {
    recompute(library);
    library.details_open = library.selected.has_value();
}

void close_details(Library& library) {
    recompute(library);
    library.details_open = false;
}

const Entry* find_entry(const Library& library, const EntryId& id) {
    for (const Entry& entry : library.entries)
        if (entry.id == id)
            return &entry;
    return nullptr;
}

const Entry* selected_entry(const Library& library) {
    if (!library.selected)
        return nullptr;
    return find_entry(library, *library.selected);
}

std::string_view entry_name(const Entry& entry) {
    if (is_built_in_brain(entry.id) || is_built_in_language(entry.id))
        return entry.shown_name;
    if (is_base_game(entry.id))
        return entry.shown_name.empty() ? words::no_mod : std::string_view(entry.shown_name);
    if (entry.listing != nullptr)
        return entry.listing->name;
    if (entry.installed != nullptr)
        return entry.installed->name;
    return entry.id.key;
}

bool works_with_played(const Inputs& inputs, const MutatorFacts& facts) {
    if (facts.works_with.empty())
        return true;
    for (const WorksWith& item : facts.works_with) {
        if (item.id == "*")
            return true;
        if (item.id == supported_base) {
            if (inputs.played.id.empty())
                return true;
            continue;
        }
        formats::oamut::Condition condition;
        condition.id = item.id;
        condition.range_text = item.range;
        if (!item.range.empty()) {
            const std::optional<formats::oamut::VersionRange> range =
                formats::oamut::parse_version_range(item.range);
            if (!range)
                continue;
            condition.range = *range;
        }
        if (formats::oamut::condition_met(condition, inputs.played.id, inputs.played.version))
            return true;
    }
    return false;
}

std::string_view entry_summary(const Entry& entry) {
    if (is_built_in_brain(entry.id) || is_built_in_language(entry.id))
        return entry.shown_summary;
    if (is_base_game(entry.id))
        return entry.shown_summary;
    if (entry.listing != nullptr && !entry.listing->summary.empty())
        return entry.listing->summary;
    if (entry.installed != nullptr)
        return entry.installed->description;
    return {};
}

namespace {

/// Returns the tags an entry shows.
///
/// A mutator's come from its facts, the listing's before the installed
/// copy's, and never from the listing's own tag list as well. Any other
/// kind uses the listing's tags.
///
/// @param entry the entry
/// @return the tags, or null when it has none to show
const std::vector<std::string>* tags_of(const Entry& entry) {
    if (entry.id.kind == Kind::mutator)
        return mutator_tag_list(entry);
    if (entry.id.kind == Kind::brain) {
        if (const BrainFacts* facts = brain_facts(entry))
            return &facts->tags;
        return nullptr;
    }
    if (entry.listing != nullptr)
        return &entry.listing->tags;
    return nullptr;
}

} // namespace

std::vector<std::pair<std::string, std::size_t>> tags_in_tab(const Library& library) {
    const std::vector<bool> kept = kept_by_filter(library, shown_filter(library));
    std::map<std::string, std::size_t> counts;
    for (std::size_t index = 0; index < library.entries.size(); ++index) {
        if (!kept[index])
            continue;
        const std::vector<std::string>* tags = tags_of(library.entries[index]);
        if (tags == nullptr)
            continue;
        for (const std::string& tag : *tags)
            ++counts[tag];
    }
    if (library.tab != Tab::mutators && library.tab != Tab::brains)
        return {counts.begin(), counts.end()};
    std::vector<std::pair<std::string, std::size_t>> ordered;
    if (library.tab == Tab::brains) {
        for (const std::string_view id : formats::oabrain::brain_tags) {
            const auto found = counts.find(std::string(id));
            if (found == counts.end())
                continue;
            ordered.push_back(*found);
            counts.erase(found);
        }
        for (const auto& counted : counts)
            ordered.push_back(counted);
        return ordered;
    }
    for (const std::string_view id : kit::tag_ids) {
        const auto found = counts.find(std::string(id));
        if (found == counts.end())
            continue;
        ordered.push_back(*found);
        counts.erase(found);
    }
    for (const auto& counted : counts)
        ordered.push_back(counted);
    return ordered;
}

std::size_t filter_count(const Library& library, Filter filter) {
    const std::vector<bool> kept = kept_by_filter(library, filter);
    std::size_t packages = 0;
    for (std::size_t index = 0; index < kept.size(); ++index)
        if (kept[index] && !is_base_game(library.entries[index].id) &&
            !is_built_in_language(library.entries[index].id) &&
            !is_built_in_brain(library.entries[index].id))
            ++packages;
    return packages;
}

std::string filter_caption(const Library& library, Filter filter) {
    return filled(
        library.text,
        words::filter_with_count,
        {{"filter", filter_text(filter, library.text)},
         {"count", std::to_string(filter_count(library, filter))}}
    );
}

std::size_t update_count(const Library& library) {
    return static_cast<std::size_t>(
        std::count_if(library.entries.begin(), library.entries.end(), [](const Entry& entry) {
            return entry.state == State::update && entry.can_act;
        })
    );
}

std::size_t updates_shown(const Library& library) {
    return static_cast<std::size_t>(
        std::count_if(library.visible.begin(), library.visible.end(), [&](std::size_t index) {
            return library.entries[index].state == State::update;
        })
    );
}

ActiveSetup active_setup(const Library& library) {
    const TextHooks& text = library.text;
    const Inputs& inputs = library.inputs;
    ActiveSetup setup;

    const bool unloaded = library.content.unload_mod;
    const Entry* playing = nullptr;
    if (!unloaded) {
        for (const Entry& entry : library.entries)
            if (entry.id.kind == Kind::mod && entry.installed != nullptr &&
                entry.installed->playing) {
                playing = &entry;
                break;
            }
    }
    if (playing != nullptr) {
        setup.mod_name = entry_name(*playing);
        setup.mod_version = version_text(*playing->installed, text);
        setup.mod_release =
            release_text(playing->installed->version, playing->installed->revision, text);
        setup.mod_badge = badge_of(*playing);
        const std::string author = author_of(*playing);
        if (!author.empty())
            setup.mod_author = filled(text, words::byline, {{"author", author}});
        setup.mod_state = std::string(shown(text, words::status_playing));
    } else if (!unloaded && inputs.playing_mod) {
        setup.mod_name = inputs.playing_mod->name;
        setup.mod_version = inputs.playing_mod->version;
        setup.mod_release = inputs.playing_mod->version;
        if (inputs.playing_mod->badge)
            setup.mod_badge = &*inputs.playing_mod->badge;
        setup.mod_state = std::string(shown(text, words::status_playing));
    }
    if (unloaded || (playing == nullptr && !inputs.playing_mod)) {
        const ActiveContent face = active_content(library);
        setup.base_title = std::string(shown(text, words::no_mod));
        setup.base_look = filled(text, words::active_as_made, {{"game", face.name}});
        setup.mod_line = shown(text, words::no_mod);
    } else if (setup.mod_version.empty()) {
        setup.mod_line = setup.mod_name;
    } else {
        setup.mod_line = filled(
            text, words::active_package, {{"name", setup.mod_name}, {"version", setup.mod_version}}
        );
    }

    const LanguageInUse& language = inputs.language;
    const bool built_in = language.pack.empty();
    if (!language.name.empty())
        setup.language_name = language.name;
    else if (!language.pack.empty())
        setup.language_name = language.pack;
    else
        setup.language_name = std::string(shown(text, words::language_english));
    if (!language.english.empty() && language.english != setup.language_name)
        setup.language_english = language.english;
    if (!language.pack.empty() && language.pack != setup.language_name &&
        language.pack != setup.language_english)
        setup.language_pack = language.pack;
    if (built_in) {
        setup.language_note = std::string(shown(text, words::registry_built_in));
    } else if (const Entry* used = language_in_use(library)) {
        std::string shown_version = language.version;
        int64_t shown_revision = language.revision;
        if (used->installed != nullptr) {
            if (!used->installed->version.empty())
                shown_version = used->installed->version;
            if (used->installed->revision > 0)
                shown_revision = used->installed->revision;
        }
        setup.language_version = release_text(shown_version, shown_revision, text);
        setup.language_badge = badge_of(*used);
        if (used->listing != nullptr && used->listing->coverage)
            setup.language_coverage = coverage_line(text, *used->listing->coverage);
    } else if (!language.version.empty() || language.revision > 0) {
        setup.language_version = release_text(language.version, language.revision, text);
    }
    if (!language.pack.empty()) {
        setup.language_line = language.version.empty()
                                  ? language.pack
                                  : filled(
                                        text,
                                        words::active_package,
                                        {{"name", language.pack}, {"version", language.version}}
                                    );
    } else {
        setup.language_line =
            filled(text, words::active_language_built_in, {{"language", setup.language_name}});
    }

    for (const InstalledMaps& pack : inputs.map_packs) {
        EntryId pack_id;
        if (!pack.key.empty()) {
            for (const Entry& entry : library.entries) {
                if (entry.id.kind == Kind::map_pack && entry.id.key == pack.key &&
                    entry.id.registry == pack.registry) {
                    pack_id = entry.id;
                    break;
                }
            }
        }
        if (pack_id.key.empty()) {
            for (const Entry& entry : library.entries) {
                if (entry.id.kind == Kind::map_pack && entry_name(entry) == pack.name) {
                    pack_id = entry.id;
                    break;
                }
            }
        }
        for (const MapLine& map : pack.maps) {
            ActiveMap line;
            line.name = map.name;
            line.players = filled(
                text,
                map.players == 1 ? words::active_players_one : words::active_players_many,
                {{"count", std::to_string(map.players)}}
            );
            line.size = map.size;
            line.description = map.description;
            line.line = filled(
                text,
                words::active_map,
                {{"map", line.name}, {"players", line.players}, {"size", line.size}}
            );
            line.pack_name = pack.name;
            line.pack_id = pack_id;
            if (map.preview && !map.preview->rgba.empty())
                line.preview = &*map.preview;
            // One map in the pack: the title is the way to the pack, so the
            // From line would only repeat a name the tile already shows.
            if (pack.maps.size() > 1)
                line.from = filled(text, words::active_from_pack, {{"pack", pack.name}});
            setup.maps.push_back(std::move(line));
        }
    }
    std::stable_sort(
        setup.maps.begin(), setup.maps.end(), [](const ActiveMap& left, const ActiveMap& right) {
            const std::string left_name = folded(left.name);
            const std::string right_name = folded(right.name);
            if (left_name != right_name)
                return left_name < right_name;
            return folded(left.pack_name) < folded(right.pack_name);
        }
    );
    return setup;
}

bool can_switch(const Library& library) noexcept {
    return !library.inputs.in_match && !library.inputs.mod_from_command_line;
}

std::vector<ActionButton> entry_actions(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    const bool in_match = library.inputs.in_match;
    const QueueItem* item = entry.queued;
    std::vector<ActionButton> buttons;
    const auto add = [&](Action action, bool enabled) {
        buttons.push_back({action, enabled, std::string(action_text(action, text))});
    };
    // The base game is not a package. While it is played it offers nothing;
    // while a mod is, PLAY NOW follows SWITCH OFF's rule and does its work.
    if (is_base_game(entry.id)) {
        if (entry.state != State::playing)
            add(Action::play_now, can_switch(library));
        return buttons;
    }
    // A built-in language is not a package. USE NOW switches to it; the one
    // in use offers nothing, and neither is downloaded or uninstalled.
    if (is_built_in_language(entry.id)) {
        if (entry.state != State::playing)
            add(Action::use_now, true);
        return buttons;
    }
    // The built-in computer player is not a package. It is not got, updated
    // or uninstalled.
    if (is_built_in_brain(entry.id))
        return buttons;
    // A mutator is got, checked, uninstalled and opened. Nothing switches it on.
    // GET WITH takes GET's place when the plan brings a prerequisite, or is
    // blocked before the mutator itself can be the only download.
    if (entry.id.kind == Kind::mutator) {
        if (entry.state == State::get || entry.state == State::update) {
            if (item != nullptr && pending(item->phase))
                add(Action::cancel, true);
            else if (item != nullptr && item->phase == Phase::failed && item->retryable)
                add(Action::retry, entry.can_act);
            else if (entry.state == State::update)
                add(Action::update, entry.can_act);
            else {
                const GetPlan plan = get_plan(library, entry);
                const bool ends_with_self = !plan.steps.empty() && plan.steps.back().id == entry.id;
                std::size_t extras = 0;
                std::string lead;
                if (ends_with_self) {
                    extras = plan.steps.size() - 1;
                    lead = plan.steps.front().name;
                } else if (!plan.steps.empty()) {
                    extras = plan.steps.size() + (plan.blocked.empty() ? 0 : 1);
                    lead = plan.steps.front().name;
                } else {
                    extras = 1;
                    lead = entry.needs_missing.empty() ? std::string(entry_name(entry))
                                                       : entry.needs_missing;
                }
                const bool brings = extras > 0 || (!plan.blocked.empty() && !ends_with_self);
                if (!brings)
                    add(Action::get, entry.can_act);
                else {
                    const std::string caption =
                        extras <= 1 ? filled(text, words::action_get_with, {{"name", lead}})
                                    : filled(
                                          text,
                                          words::action_get_with_more,
                                          {{"count", std::to_string(extras)}}
                                      );
                    buttons.push_back({Action::get_with, plan.blocked.empty(), caption});
                }
            }
        }
        const MutatorFacts* mutator = mutator_facts(entry);
        if (entry.installed != nullptr && mutator != nullptr && mutator->verdict) {
            const bool checking = mutator->verdict->state == Verdict::State::checking;
            add(Action::check_with_mod, !in_match && !checking);
        }
        if (entry.installed != nullptr) {
            add(Action::uninstall, uninstall_off_reason(library, entry).empty());
            add(Action::open_folder, true);
        }
        if (entry.listing != nullptr && !entry.listing->homepage.empty())
            add(Action::homepage, true);
        return buttons;
    }
    const ContentLines lines = content_lines(library, entry);
    if (!lines.turn_on.empty()) {
        buttons.push_back(
            {Action::turn_on,
             true,
             filled(text, words::action_turn_on, {{"name", lines.turn_on_name}})}
        );
    }
    if (entry.state == State::get || entry.state == State::update) {
        if (item != nullptr && pending(item->phase))
            add(Action::cancel, true);
        else if (item != nullptr && item->phase == Phase::failed && item->retryable)
            add(Action::retry, entry.can_act);
        else
            add(entry.state == State::get ? Action::get : Action::update, entry.can_act);
    }
    const Installed* installed = entry.installed;
    // The mod being played switches off where another mod plays now: the
    // game then plays none.
    if (installed != nullptr && entry.id.kind == Kind::mod) {
        if (installed->playing)
            add(Action::switch_off, can_switch(library));
        else
            add(Action::play_now, can_switch(library) && !lines.blocks_play);
    }
    if (installed != nullptr && entry.id.kind == Kind::language && !installed->in_use)
        add(Action::use_now, !lines.blocks_play);
    if (installed != nullptr && installed->kept)
        add(Action::roll_back, !in_match);
    if (installed != nullptr) {
        add(Action::uninstall, uninstall_off_reason(library, entry).empty());
        add(Action::open_folder, true);
    }
    if (entry.listing != nullptr && !entry.listing->homepage.empty())
        add(Action::homepage, true);
    return buttons;
}

bool can_check_now(const Library& library) noexcept {
    const Inputs& inputs = library.inputs;
    return inputs.can_check && !inputs.checking && !inputs.in_match;
}

std::vector<Fact> facts(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    const Listing* listing = entry.listing;
    const Installed* installed = entry.installed;
    std::vector<Fact> list;
    // The base game is not a package: its details are its summary alone.
    if (is_base_game(entry.id) || is_built_in_language(entry.id) || is_built_in_brain(entry.id))
        return list;

    const ContentLines lines = content_lines(library, entry);
    bool games_written = false;
    bool expansions_written = false;
    const auto write_games = [&] {
        if (games_written || lines.games.empty())
            return;
        list.push_back(
            fact_of(shown(text, words::fact_games), lines.games, lines.games, lines.games_mark)
        );
        games_written = true;
    };
    const auto write_expansions = [&] {
        if (expansions_written || lines.expansions.empty())
            return;
        list.push_back(fact_of(
            shown(text, words::fact_needs),
            lines.expansions,
            lines.expansions,
            lines.expansions_mark
        ));
        expansions_written = true;
    };

    // Which version is installed and which is newest, and who made it: each
    // left out when it has nothing to say.
    if (installed != nullptr) {
        const std::string release = release_text(installed->version, installed->revision, text);
        if (!release.empty())
            list.push_back(
                fact_of(shown(text, words::fact_installed), release, release, Mark::none)
            );
    }
    if (listing != nullptr) {
        const std::string release = release_text(listing->version, listing->revision, text);
        if (!release.empty())
            list.push_back(fact_of(shown(text, words::fact_latest), release, release, Mark::none));
    }
    std::string author;
    if (listing != nullptr)
        author = listing->author.empty() ? listing->publisher : listing->author;
    if (author.empty() && installed != nullptr)
        author = installed->author;
    if (!author.empty())
        list.push_back(fact_of(shown(text, words::fact_author), author, author, Mark::none));

    if (entry.id.kind == Kind::mutator) {
        append_mutator_facts(library, entry, list);
        if (entry.id.registry.empty()) {
            const std::string own(shown(text, words::from_own));
            list.push_back(fact_of(shown(text, words::fact_from), own, own, Mark::none));
        } else if (entry.not_reviewed) {
            const std::string from =
                filled(text, words::from_not_reviewed, {{"registry", entry.registry_name}});
            list.push_back(fact_of(shown(text, words::fact_from), from, from, Mark::warn));
        } else {
            list.push_back(fact_of(
                shown(text, words::fact_from), entry.registry_name, entry.registry_name, Mark::none
            ));
        }
        return list;
    }

    if (listing != nullptr) {
        const std::string size = size_text(listing->size, text);
        if (entry.update != nullptr) {
            const std::string download = size_text(entry.update->size, text);
            list.push_back(fact_of(
                shown(text, words::fact_size),
                filled(text, words::size_with_download, {{"size", size}, {"download", download}}),
                filled(text, words::size_download_short, {{"download", download}}),
                Mark::none
            ));
        } else {
            list.push_back(fact_of(shown(text, words::fact_size), size, size, Mark::none));
        }

        if ((listing->base.empty() && listing->kind == Kind::mod) ||
            listing->base == supported_base) {
            list.push_back(fact_of(
                shown(text, words::fact_base),
                std::string(shown(text, words::base_supported)),
                std::string(shown(text, words::base_supported_short)),
                Mark::none
            ));
        } else if (!listing->base.empty()) {
            list.push_back(
                fact_of(shown(text, words::fact_base), listing->base, listing->base, Mark::warn)
            );
        }
        write_games();

        if (const std::optional<formats::oamod::EngineRange> range =
                formats::oamod::parse_engine_range(listing->requires_engine)) {
            const bool met = formats::oamod::engine_range_met(*range, library.inputs.engine);
            list.push_back(fact_of(
                shown(text, words::fact_needs),
                filled(text, words::needs_engine, {{"range", engine_text(*range, text)}}),
                filled(
                    text, words::needs_engine_short, {{"range", engine_short_text(*range, text)}}
                ),
                met ? Mark::good : Mark::warn
            ));
        }
        write_expansions();

        if (listing->kind == Kind::mod) {
            const std::string game = std::to_string(listing->game_hacks);
            const std::string view = std::to_string(listing->view_hacks);
            std::string counts;
            std::string counts_short;
            if (listing->game_hacks == 0 && listing->view_hacks == 0 && listing->hack_ids.empty()) {
                counts = shown(text, words::hacks_none);
                counts_short = counts;
            } else {
                counts = filled(text, words::hacks_counts, {{"game", game}, {"view", view}});
                counts_short =
                    filled(text, words::hacks_counts_short, {{"game", game}, {"view", view}});
            }
            const std::vector<std::string_view> missing = missing_hacks(*listing);
            if (missing.empty()) {
                list.push_back(
                    fact_of(shown(text, words::fact_hacks), counts, counts_short, Mark::none)
                );
            } else {
                const std::string named = filled(
                    text, words::blocked_hacks, {{"hacks", missing_hacks_text(missing, text)}}
                );
                const std::string count = std::to_string(missing.size());
                list.push_back(fact_of(
                    shown(text, words::fact_hacks),
                    filled(
                        text, words::hacks_with_missing, {{"counts", counts}, {"missing", named}}
                    ),
                    filled(
                        text,
                        words::hacks_with_missing_short,
                        {{"counts", counts_short}, {"count", count}}
                    ),
                    Mark::warn
                ));
            }
        }
    }
    write_games();
    write_expansions();

    if (entry.id.kind == Kind::mod) {
        if (entry.update != nullptr && entry.installed != nullptr) {
            list.push_back(update_rules_fact(library, entry));
        } else {
            std::optional<base::sha256::Digest> rules;
            if (entry.installed != nullptr)
                rules = entry.installed->rules;
            else if (listing != nullptr)
                rules = listing->sim_hash;
            if (rules) {
                const std::string hash = rules_text(*rules);
                list.push_back(fact_of(shown(text, words::fact_rules), hash, hash, Mark::none));
            }
        }
    }

    if (listing != nullptr && listing->kind == Kind::map_pack) {
        const std::string count = std::to_string(listing->maps.size());
        const std::string maps = filled(
            text,
            listing->maps.size() == 1 ? words::map_count_one : words::map_count,
            {{"count", count}}
        );
        list.push_back(fact_of(shown(text, words::fact_maps), maps, maps, Mark::none));
    }

    if (listing != nullptr && listing->coverage) {
        const double clamped = std::clamp(*listing->coverage, 0.0, 1.0);
        const auto percent =
            static_cast<uint64_t>(clamped * static_cast<double>(percent_whole) + 0.5);
        const std::string value =
            filled(text, words::coverage_percent, {{"percent", std::to_string(percent)}});
        list.push_back(fact_of(shown(text, words::fact_coverage), value, value, Mark::none));
    }

    if (entry.id.registry.empty()) {
        const std::string own(shown(text, words::from_own));
        list.push_back(fact_of(shown(text, words::fact_from), own, own, Mark::none));
    } else if (entry.not_reviewed) {
        const std::string from =
            filled(text, words::from_not_reviewed, {{"registry", entry.registry_name}});
        list.push_back(fact_of(shown(text, words::fact_from), from, from, Mark::warn));
    } else {
        list.push_back(fact_of(
            shown(text, words::fact_from), entry.registry_name, entry.registry_name, Mark::none
        ));
    }
    return list;
}

std::string status_line(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    if (is_built_in_language(entry.id))
        return std::string(shown(text, words::status_built_in_language));
    if (entry.queued != nullptr && entry.queued->phase != Phase::installed)
        return queue_status(*entry.queued, text);
    if (!entry.can_act)
        return entry.blocked;
    switch (entry.state) {
    case State::update: {
        const UpdateNote& update = *entry.update;
        const std::string size = size_text(update.size, text);
        const bool same_version = entry.installed->version == update.to_version;
        // A catalogue that names no revision leaves it 0: such an update
        // of the same version is no revision of it, and says so plainly.
        if (same_version && update.to_revision > 0)
            return filled(
                text,
                words::status_revision_available,
                {{"revision", std::to_string(update.to_revision)}, {"size", size}}
            );
        if (!same_version && !update.to_version.empty())
            return filled(
                text,
                words::status_version_available,
                {{"version", update.to_version}, {"size", size}}
            );
        return filled(text, words::status_update_available, {{"size", size}});
    }
    case State::installed:
    case State::playing:
        // A folder with no oamod.yaml says so in its status.
        if (entry.installed != nullptr && entry.installed->without_manifest)
            return std::string(shown(text, words::status_without_manifest));
        return std::string(shown(
            text, entry.state == State::playing ? words::status_playing : words::status_installed
        ));
    case State::get:
        if (entry.id.kind == Kind::mutator && entry.listing != nullptr) {
            return filled(
                text,
                words::status_not_installed_version,
                {{"version",
                  mutator_status_version(entry.listing->version, entry.listing->revision, text)}}
            );
        }
        return filled(
            text, words::status_not_installed, {{"size", size_text(entry.listing->size, text)}}
        );
    }
    return {};
}

std::string byline(const Entry& entry, const TextHooks& text) {
    if (entry.listing == nullptr || entry.id.registry.empty())
        return {};
    const std::string& author =
        entry.listing->author.empty() ? entry.listing->publisher : entry.listing->author;
    if (author.empty())
        return {};
    return filled(text, words::byline, {{"author", author}});
}

std::string playing_note(const Library& library, const Entry& entry) {
    const TextHooks& text = library.text;
    if (entry.id.kind != Kind::mod)
        return {};
    // PLAY NOW on the base game is off for the same reason SWITCH OFF is,
    // and the note says so in the same words. While it is the one played
    // it offers nothing, so it has no note.
    if (is_base_game(entry.id)) {
        if (entry.state == State::playing || !library.inputs.mod_from_command_line)
            return {};
        return std::string(shown(text, words::note_command_line));
    }
    if (entry.installed == nullptr)
        return {};
    // While the command line chose the mod, PLAY NOW and SWITCH OFF are off,
    // and the note says why first.
    const std::string chosen = library.inputs.mod_from_command_line
                                   ? std::string(shown(text, words::note_command_line))
                                   : std::string();
    const auto after_chosen = [&chosen](std::string note) {
        if (chosen.empty() || note.empty())
            return chosen.empty() ? note : chosen;
        return chosen + " " + note;
    };
    const std::string name(entry_name(entry));
    if (entry.installed->playing) {
        const bool held = entry.queued != nullptr && entry.queued->phase == Phase::held;
        if (entry.update != nullptr || held)
            return after_chosen(filled(text, words::note_finishes, {{"name", name}}));
        return after_chosen({});
    }
    if (entry.update == nullptr)
        return after_chosen({});
    for (const Installed& installed : library.inputs.installed) {
        if (installed.kind == Kind::mod && installed.playing)
            return after_chosen(filled(
                text, words::note_playing_other, {{"playing", installed.name}, {"name", name}}
            ));
    }
    return after_chosen({});
}

std::string playing_note(const Library& library) {
    const Entry* entry = selected_entry(library);
    if (entry == nullptr)
        return {};
    return playing_note(library, *entry);
}

QueueLines queue_lines(const Library& library) {
    const TextHooks& text = library.text;
    const std::vector<QueueItem>& queue = library.inputs.queue;
    QueueLines lines;
    if (queue.empty())
        return lines;
    const std::size_t first = headline_item(queue);
    lines.first = first_queue_line(queue[first], text);
    lines.compact = compact_queue_line(queue[first], text);

    std::optional<std::size_t> next;
    std::size_t after_next = 0;
    for (std::size_t index = 0; index < queue.size(); ++index) {
        if (index == first || !pending(queue[index].phase))
            continue;
        if (!next)
            next = index;
        else
            ++after_next;
    }
    if (next) {
        const QueueItem& item = queue[*next];
        lines.second = filled(
            text,
            words::queue_then,
            {{"package", package_text(item, text)}, {"phase", phase_text(item.phase, text)}}
        );
        if (after_next != 0)
            lines.second = filled(
                text,
                words::queue_more,
                {{"line", lines.second}, {"count", std::to_string(after_next)}}
            );
    }
    return lines;
}

} // namespace oa::ui::library
