// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's search, run on the player's machine: which entries a tab's
// filter and a search keep and how they rank, and the order of the visible
// list.
#include "oa/data/content_bundles.hpp"
#include "oa/data/languages.hpp"
#include "oa/ui/library/text.hpp"
#include "visible.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::library {

namespace {

/// The bits that mark a UTF-8 continuation byte, and their value in one.
constexpr unsigned char utf8_continuation_mask = 0xc0;
constexpr unsigned char utf8_continuation_bits = 0x80;

/// The byte a search splits its words on.
constexpr char word_separator = ' ';

/// The groups of the order a list takes with no search.
enum class Group : uint8_t {
    update,      ///< an update is offered
    installed,   ///< installed or playing, up to date
    get,         ///< can be installed
    get_blocked, ///< cannot be installed here
};

/// Tells which group an entry sorts in with no search.
///
/// @param entry the entry
/// @return its group
Group group_of(const Entry& entry) {
    switch (entry.state) {
    case State::update:
        return Group::update;
    case State::installed:
    case State::playing:
        return Group::installed;
    case State::get:
        break;
    }
    return entry.can_act ? Group::get : Group::get_blocked;
}

/// Splits a folded search into its words.
///
/// @param query the folded search
/// @return the words, none empty
std::vector<std::string_view> split_words(std::string_view query) {
    std::vector<std::string_view> found;
    std::size_t at = 0;
    while (at < query.size()) {
        const std::size_t end = std::min(query.find(word_separator, at), query.size());
        if (end > at)
            found.push_back(query.substr(at, end - at));
        at = end + 1;
    }
    return found;
}

/// Tells whether a text holds a word.
///
/// @param text the folded text
/// @param word the folded word
/// @return true when the word is in the text
bool holds(std::string_view text, std::string_view word) {
    return text.find(word) != std::string_view::npos;
}

/// An entry's fields as the search reads them.
///
/// The name, author, publisher and summary are folded. The tags are the ids
/// as written, so a label can be looked up from the catalogue id.
struct Fields {
    std::string name{};
    std::vector<std::string> tags{};
    std::string author{};
    std::string publisher{};
    std::string summary{};
    /// True for a brain, so a tag's label is a brain tag's.
    bool brain{};
    /// True for a key layout, so a tag's label is a key-layout tag's.
    bool keys{};
};

/// Folds an entry's fields.
///
/// @param entry the entry
/// @return its fields; a package no registry lists has only its name,
///     and the base game has its name and the summary on its row
Fields fields_of(const Entry& entry) {
    Fields fields;
    if (is_built_in_language(entry.id)) {
        std::string name(entry_name(entry));
        const std::string_view english = entry_summary(entry);
        if (!english.empty()) {
            name.push_back(' ');
            name.append(english);
        }
        fields.name = folded(name);
        const std::string_view tag = built_in_language_tag(entry.id);
        if (!tag.empty())
            fields.tags.emplace_back(tag);
        return fields;
    }
    fields.name = folded(entry_name(entry));
    // No Mod has no catalogue listing. Its row still carries the summary
    // ("Total Annihilation as it was made"), and a search of those words
    // has to find it.
    if (is_base_game(entry.id)) {
        fields.summary = folded(entry_summary(entry));
        return fields;
    }
    if (is_built_in_brain(entry.id)) {
        fields.summary = folded(entry_summary(entry));
        fields.brain = true;
        return fields;
    }
    if (is_built_in_layout(entry.id)) {
        fields.summary = folded(entry_summary(entry));
        fields.keys = true;
        return fields;
    }
    if (entry.id.kind == Kind::mutator) {
        if (const std::vector<std::string>* tags = mutator_tag_list(entry))
            fields.tags = *tags;
        if (entry.listing == nullptr)
            return fields;
    } else if (entry.id.kind == Kind::brain) {
        fields.brain = true;
        if (const BrainFacts* facts = brain_facts(entry))
            fields.tags = facts->tags;
        if (entry.listing == nullptr)
            return fields;
    } else if (entry.id.kind == Kind::key_layout) {
        fields.keys = true;
        if (const KeyLayoutFacts* facts = key_facts(entry))
            fields.tags = facts->tags;
        if (entry.listing == nullptr)
            return fields;
    } else if (entry.listing == nullptr) {
        return fields;
    } else {
        fields.tags = entry.listing->tags;
    }
    fields.author = folded(entry.listing->author);
    fields.publisher = folded(entry.listing->publisher);
    fields.summary = folded(entry.listing->summary);
    return fields;
}

/// Finds the best field a word is in.
///
/// @param fields the entry's fields; the name, author and summary are folded,
///     and the tags are the ids as written
/// @param word the folded word
/// @param text how a tag's label is looked up
/// @return its class, or nothing when no field holds it
std::optional<MatchClass>
class_of(const Fields& fields, std::string_view word, const TextHooks& text) {
    if (holds(fields.name, word))
        return MatchClass::name;
    for (const std::string& tag : fields.tags) {
        const std::string id = folded(tag);
        if (std::string_view(id).substr(0, word.size()) == word)
            return MatchClass::tag;
        const std::string label = folded(
            fields.brain ? brain_tag_label(tag, text)
                         : (fields.keys ? key_tag_label(tag, text) : tag_label(tag, text))
        );
        if (label != id && std::string_view(label).substr(0, word.size()) == word)
            return MatchClass::tag;
    }
    if (holds(fields.author, word) || holds(fields.publisher, word))
        return MatchClass::author;
    if (holds(fields.summary, word))
        return MatchClass::summary;
    return std::nullopt;
}

/// An entry the list keeps, with what it sorts by.
struct Candidate {
    std::size_t index{}; ///< into the entries
    Rank rank{};
    Group group{Group::get};
    std::string name{}; ///< folded
};

/// Returns the day an entry's listing was published.
///
/// @param entry the entry
/// @return the day as written, `YYYY-MM-DD`; empty without a listing or a day
std::string_view published_day(const Entry& entry) {
    return entry.listing != nullptr ? std::string_view(entry.listing->published)
                                    : std::string_view();
}

/// Orders two candidates of the same name by their registries: built-in
/// registries first, then by registry id, then by kind and key, which leave
/// no two entries equal.
///
/// @param library the Library
/// @param left one candidate
/// @param right another
/// @return true when `left` comes first
bool before_by_registry(const Library& library, const Candidate& left, const Candidate& right) {
    const Entry& left_entry = library.entries[left.index];
    const Entry& right_entry = library.entries[right.index];
    if (left_entry.built_in != right_entry.built_in)
        return left_entry.built_in;
    return left_entry.id < right_entry.id;
}

/// Orders two candidates as New does: the later published day first, then
/// by name, then by registry. A day written `YYYY-MM-DD` orders as its text.
///
/// @param library the Library
/// @param left one candidate
/// @param right another
/// @return true when `left` comes first
bool before_newest(const Library& library, const Candidate& left, const Candidate& right) {
    const std::string_view left_day = published_day(library.entries[left.index]);
    const std::string_view right_day = published_day(library.entries[right.index]);
    if (left_day != right_day)
        return left_day > right_day;
    if (left.name != right.name)
        return left.name < right.name;
    return before_by_registry(library, left, right);
}

/// Orders two candidates as the list does with no search.
///
/// Under New, newest first (before_newest). Under the other filters, by
/// group, then name, then registry.
///
/// @param library the Library
/// @param left one candidate
/// @param right another
/// @return true when `left` comes first
bool before_without_search(const Library& library, const Candidate& left, const Candidate& right) {
    if (shown_filter(library) == Filter::newest)
        return before_newest(library, left, right);
    if (left.group != right.group)
        return left.group < right.group;
    if (left.name != right.name)
        return left.name < right.name;
    return before_by_registry(library, left, right);
}

/// Orders two candidates as the list does with a search.
///
/// By the worst class among the words, then their sum, then names that
/// start with the first word, then as with no search.
///
/// @param library the Library
/// @param left one candidate
/// @param right another
/// @return true when `left` comes first
bool before_with_search(const Library& library, const Candidate& left, const Candidate& right) {
    if (left.rank.worst != right.rank.worst)
        return left.rank.worst < right.rank.worst;
    if (left.rank.total != right.rank.total)
        return left.rank.total < right.rank.total;
    if (left.rank.name_starts != right.rank.name_starts)
        return left.rank.name_starts;
    return before_without_search(library, left, right);
}

/// Tells whether a tab holds entries of a kind.
///
/// @param tab the tab
/// @param kind the kind
/// @return true for its own kind on Mods, Maps, Mutators, Brains and
///     Languages, every kind on Updates, and none on Active or Registries
bool tab_holds(Tab tab, Kind kind) {
    switch (tab) {
    case Tab::active:
        return false;
    case Tab::mods:
        return kind == Kind::mod;
    case Tab::maps:
        return kind == Kind::map_pack;
    case Tab::mutators:
        return kind == Kind::mutator;
    case Tab::brains:
        return kind == Kind::brain;
    case Tab::keys:
        return kind == Kind::key_layout;
    case Tab::languages:
        return kind == Kind::language;
    case Tab::updates:
        return true;
    case Tab::registries:
        return false;
    }
    return false;
}

/// Tells whether a filter other than New keeps an entry of the tab's kind.
///
/// @param filter the filter
/// @param entry the entry
/// @return true when it keeps it
bool state_kept(Filter filter, const Entry& entry) {
    switch (filter) {
    case Filter::all:
    case Filter::newest:
        return true;
    case Filter::installed:
        return entry.state != State::get;
    case Filter::updates:
        return entry.state == State::update;
    }
    return false;
}

/// Tells whether the base game entry shows under a filter.
///
/// It shows on Mods, under All and Installed, and under no other tab or
/// filter. New never holds it: it is not a package a registry published.
///
/// @param library the Library
/// @param filter the filter
/// @return true when the list keeps it
bool shows_base_game(const Library& library, Filter filter) {
    return library.tab == Tab::mods && (filter == Filter::all || filter == Filter::installed);
}

/// Tells whether a built-in language shows under a filter.
///
/// It shows on Languages, under All and Installed, and under no other tab
/// or filter. New never holds it: it is not a package a registry published.
///
/// @param library the Library
/// @param filter the filter
/// @return true when the list keeps it
bool shows_built_in_language(const Library& library, Filter filter) {
    return library.tab == Tab::languages && (filter == Filter::all || filter == Filter::installed);
}

/// Tells whether the built-in computer player shows under a filter.
///
/// It shows on Brains, under All and Installed, and under no other tab or
/// filter. New never holds it: it is not a package a registry published.
///
/// @param library the Library
/// @param filter the filter
/// @return true when the list keeps it
bool shows_built_in_brain(const Library& library, Filter filter) {
    return library.tab == Tab::brains && (filter == Filter::all || filter == Filter::installed);
}

/// Tells whether OA default shows under a filter.
///
/// It shows on Keys, under All and Installed, and under no other tab or
/// filter. New never holds it: it is not a package a registry published.
///
/// @param library the Library
/// @param filter the filter
/// @return true when the list keeps it
bool shows_built_in_layout(const Library& library, Filter filter) {
    return library.tab == Tab::keys && (filter == Filter::all || filter == Filter::installed);
}

/// Tells whether the tab's filter offers the tag that is stored.
///
/// Counts never apply the tag, and the chip row lists only the tags of this
/// filter. One scan decides it: a tag left from another tab or filter would
/// otherwise drop every row while those counts stay, and with an empty
/// search the list would read "Nothing to show."
///
/// @param library the Library
/// @return true when the stored tag is one tags_in_tab names
bool tag_is_offered(const Library& library) {
    if (library.tag.empty())
        return false;
    for (const auto& counted : tags_in_tab(library))
        if (counted.first == library.tag)
            return true;
    return false;
}

} // namespace

std::string folded(std::string_view text) {
    std::string result(text);
    for (char& byte : result)
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    return result;
}

std::string_view capped_query(std::string_view query) {
    if (query.size() <= max_query_bytes)
        return query;
    std::size_t cut = max_query_bytes;
    while (cut > 0 && (static_cast<unsigned char>(query[cut]) & utf8_continuation_mask) ==
                          utf8_continuation_bits)
        --cut;
    return query.substr(0, cut);
}

std::optional<Rank> rank(const Entry& entry, std::string_view query, const TextHooks& text) {
    const std::string search = folded(capped_query(query));
    const std::vector<std::string_view> terms = split_words(search);
    if (terms.empty())
        return Rank{};
    Fields fields = fields_of(entry);
    // "default" finds the built-in player, and the installed brain that is
    // the default, including under a translation. It is not a chip.
    if (is_built_in_brain(entry.id) || entry.is_default) {
        fields.tags.emplace_back("default");
        fields.tags.emplace_back(shown(text, words::chip_default));
    }
    // "built" and "in" find OA default, including under a translation of
    // Built in. The row still shows the sentence the catalogue gives it.
    if (is_built_in_layout(entry.id)) {
        const std::string built_in = folded(shown(text, words::registry_built_in));
        if (!built_in.empty()) {
            if (!fields.summary.empty())
                fields.summary.push_back(' ');
            fields.summary.append(built_in);
        }
    }
    Rank result;
    for (const std::string_view word : terms) {
        const std::optional<MatchClass> found = class_of(fields, word, text);
        if (!found)
            return std::nullopt;
        result.worst = std::max(result.worst, *found);
        result.total += static_cast<uint32_t>(*found);
    }
    result.name_starts =
        std::string_view(fields.name).substr(0, terms.front().size()) == terms.front();
    return result;
}

std::vector<bool> kept_by_filter(const Library& library, Filter filter) {
    std::vector<bool> kept(library.entries.size(), false);
    if (filter != Filter::newest) {
        for (std::size_t index = 0; index < library.entries.size(); ++index) {
            const Entry& entry = library.entries[index];
            if (is_base_game(entry.id))
                kept[index] = shows_base_game(library, filter);
            else if (is_built_in_language(entry.id))
                kept[index] = shows_built_in_language(library, filter);
            else if (is_built_in_brain(entry.id))
                kept[index] = shows_built_in_brain(library, filter);
            else if (is_built_in_layout(entry.id))
                kept[index] = shows_built_in_layout(library, filter);
            else
                kept[index] = tab_holds(library.tab, entry.id.kind) && state_kept(filter, entry);
        }
        return kept;
    }
    // New: the tab's listings with a published day, newest first, then cut.
    // The base game entry is not among them.
    std::vector<Candidate> dated;
    for (std::size_t index = 0; index < library.entries.size(); ++index) {
        const Entry& entry = library.entries[index];
        if (is_base_game(entry.id) || is_built_in_language(entry.id) ||
            is_built_in_brain(entry.id) || is_built_in_layout(entry.id))
            continue;
        if (tab_holds(library.tab, entry.id.kind) && !published_day(entry).empty())
            dated.push_back({index, {}, Group::get, folded(entry_name(entry))});
    }
    std::sort(dated.begin(), dated.end(), [&](const Candidate& left, const Candidate& right) {
        return before_newest(library, left, right);
    });
    for (std::size_t place = 0; place < dated.size() && place < newest_count; ++place)
        kept[dated[place].index] = true;
    return kept;
}

bool has_shown_tag(const Library& library, const Entry& entry) {
    if (library.tag.empty())
        return true;
    const std::vector<std::string>* tags = nullptr;
    if (entry.id.kind == Kind::mutator)
        tags = mutator_tag_list(entry);
    else if (entry.id.kind == Kind::brain) {
        if (const BrainFacts* facts = brain_facts(entry))
            tags = &facts->tags;
    } else if (entry.id.kind == Kind::key_layout) {
        if (const KeyLayoutFacts* facts = key_facts(entry))
            tags = &facts->tags;
    } else if (entry.listing != nullptr)
        tags = &entry.listing->tags;
    if (tags == nullptr)
        return false;
    return std::find(tags->begin(), tags->end(), library.tag) != tags->end();
}

void update_visible(Library& library) {
    const std::vector<bool> by_filter = kept_by_filter(library, shown_filter(library));
    const bool offered = tag_is_offered(library);
    std::vector<Candidate> kept;
    for (std::size_t index = 0; index < library.entries.size(); ++index) {
        const Entry& entry = library.entries[index];
        if (!by_filter[index])
            continue;
        if (offered && !has_shown_tag(library, entry))
            continue;
        const std::optional<Rank> ranked = rank(entry, library.query, library.text);
        if (!ranked)
            continue;
        kept.push_back({index, *ranked, group_of(entry), folded(entry_name(entry))});
    }
    const bool searching = !split_words(folded(library.query)).empty();
    std::sort(kept.begin(), kept.end(), [&](const Candidate& left, const Candidate& right) {
        return searching ? before_with_search(library, left, right)
                         : before_without_search(library, left, right);
    });
    // The base game entry leads the Mods list whenever the filter and the
    // search keep it, ahead of the order the packages take.
    const auto base_game = std::find_if(kept.begin(), kept.end(), [&](const Candidate& candidate) {
        return is_base_game(library.entries[candidate.index].id);
    });
    if (base_game != kept.end() && base_game != kept.begin())
        std::rotate(kept.begin(), base_game, base_game + 1);
    // Built-in languages lead the Languages list, in the order Settings ›
    // Language gives them, whenever the filter and the search keep them.
    if (library.tab == Tab::languages) {
        std::vector<std::string> order;
        const std::span<const data::languages::Language* const> languages =
            data::languages::known_languages();
        for (const data::languages::Language* language : languages) {
            if (language == nullptr || language->source != data::languages::Source::built_in ||
                !data::languages::playable(*language))
                continue;
            order.push_back(std::string(built_in_language_prefix) + std::string(language->tag));
        }
        std::vector<Candidate> leading;
        std::vector<bool> taken(kept.size(), false);
        for (const std::string& key : order) {
            for (std::size_t place = 0; place < kept.size(); ++place) {
                if (taken[place])
                    continue;
                if (library.entries[kept[place].index].id.key == key) {
                    leading.push_back(kept[place]);
                    taken[place] = true;
                    break;
                }
            }
        }
        if (!leading.empty()) {
            for (std::size_t place = 0; place < kept.size(); ++place)
                if (!taken[place])
                    leading.push_back(kept[place]);
            kept = std::move(leading);
        }
    }
    // The built-in computer player leads the Brains list whenever the filter
    // and the search keep it, ahead of the order the packages take.
    if (library.tab == Tab::brains) {
        const auto built_in =
            std::find_if(kept.begin(), kept.end(), [&](const Candidate& candidate) {
                return is_built_in_brain(library.entries[candidate.index].id);
            });
        if (built_in != kept.end() && built_in != kept.begin())
            std::rotate(kept.begin(), built_in, built_in + 1);
    }
    // OA default leads the Keys list. Layouts made for the mod being played
    // come next, ahead of the order the other packages take.
    if (library.tab == Tab::keys) {
        const auto built_in =
            std::find_if(kept.begin(), kept.end(), [&](const Candidate& candidate) {
                return is_built_in_layout(library.entries[candidate.index].id);
            });
        if (built_in != kept.end() && built_in != kept.begin())
            std::rotate(kept.begin(), built_in, built_in + 1);
        auto rest = kept.begin();
        if (rest != kept.end() && is_built_in_layout(library.entries[rest->index].id))
            ++rest;
        std::stable_partition(rest, kept.end(), [&](const Candidate& candidate) {
            const Entry& entry = library.entries[candidate.index];
            const KeyLayoutFacts* facts = key_facts(entry);
            return facts != nullptr && layout_made_for_played(library.inputs, facts->works_with);
        });
    }
    library.visible.clear();
    for (const Candidate& candidate : kept)
        library.visible.push_back(candidate.index);
}

std::size_t tab_count(const Library& library, Tab tab) {
    if (tab == Tab::registries)
        return library.inputs.registries.size();
    if (tab == Tab::active)
        return 0;
    // Updates counts the updates that can be installed, as the OA button's
    // dot and UPDATE ALL do, not every package its filters can show.
    if (tab == Tab::updates)
        return update_count(library);
    std::size_t count = 0;
    for (const Entry& entry : library.entries)
        if (tab_holds(tab, entry.id.kind) && !is_base_game(entry.id) &&
            !is_built_in_language(entry.id) && !is_built_in_brain(entry.id) &&
            !is_built_in_layout(entry.id))
            ++count;
    return count;
}

namespace {

/// Tells whether an installed package is the base game entry.
///
/// @param item the package
/// @return true for the base game
bool installed_base_game(const Installed& item) {
    return is_base_game(EntryId{item.registry, item.kind, item.key});
}

/// Tells whether an installed package is a language built into the game.
///
/// @param item the package
/// @return true for a built-in language
bool installed_built_in_language(const Installed& item) {
    return is_built_in_language(EntryId{item.registry, item.kind, item.key});
}

/// Tells whether a map pack the Active tab lists is already one of the
/// installed packages.
///
/// @param inputs the inputs
/// @param pack the pack
/// @return true when an installed map pack is the same package
bool map_pack_already_installed(const Inputs& inputs, const InstalledMaps& pack) {
    for (const Installed& item : inputs.installed) {
        if (item.kind != Kind::map_pack)
            continue;
        if (!pack.key.empty()) {
            if (item.key == pack.key && item.registry == pack.registry)
                return true;
        } else if (item.registry.empty() && item.name == pack.name) {
            return true;
        }
    }
    return false;
}

} // namespace

std::size_t added_package_count(const Inputs& inputs) {
    std::size_t total = 0;
    for (const Installed& item : inputs.installed) {
        if (item.kind != Kind::mod && item.kind != Kind::map_pack && item.kind != Kind::language &&
            item.kind != Kind::mutator)
            continue;
        if (installed_base_game(item) || installed_built_in_language(item))
            continue;
        ++total;
    }
    for (const InstalledMaps& pack : inputs.map_packs) {
        if (!map_pack_already_installed(inputs, pack))
            ++total;
    }
    for (const std::string& downloaded : inputs.content.downloaded) {
        const oa::data::content_bundles::Bundle* bundle =
            oa::data::content_bundles::find_bundle(downloaded);
        if (bundle != nullptr && bundle->kind == oa::data::content_bundles::Kind::expansion)
            ++total;
    }
    return total;
}

} // namespace oa::ui::library
