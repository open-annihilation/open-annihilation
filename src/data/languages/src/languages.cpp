// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/languages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::languages {

namespace {

#include "registry.inc"
#include "pack_notices.inc"

static_assert(!kLanguages.empty() && kLanguages[0].tag == "en", "English comes first");

/// Tells whether every compiled offered language is marked available.
///
/// @return true when each one is
constexpr bool offered_languages_are_available() {
    for (const Language& language : kOfferedLanguages)
        if (language.source != Source::available)
            return false;
    return true;
}

static_assert(offered_languages_are_available(), "an offered language is available");

/// Lowers an ASCII letter; any other byte is kept.
///
/// @param letter the byte
/// @return the byte, A to Z lowered
constexpr char lower(char letter) noexcept {
    return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
}

/// Tells whether two texts are the same, ignoring the case of ASCII
/// letters and reading '_' as '-'.
///
/// @param left a text
/// @param right a text
/// @return true when they match
bool same_tag(std::string_view left, std::string_view right) noexcept {
    const auto fold = [](char letter) { return letter == '_' ? '-' : lower(letter); };
    return left.size() == right.size() &&
           std::equal(left.begin(), left.end(), right.begin(), [&](char a, char b) {
               return fold(a) == fold(b);
           });
}

/// Tells whether a pattern is a whole tag or its leading subtags.
///
/// @param tag a normalised tag, as "de-AT"
/// @param pattern an entry's locale, as "de"
/// @return true when the pattern is the tag, or the tag's start ending at a '-'
bool leads(std::string_view tag, std::string_view pattern) noexcept {
    if (pattern.empty() || pattern.size() > tag.size())
        return false;
    if (!same_tag(tag.substr(0, pattern.size()), pattern))
        return false;
    return pattern.size() == tag.size() || tag[pattern.size()] == '-';
}

/// Tells whether a byte may stand in a locale's tag.
///
/// @param letter the byte
/// @return true for ASCII letters, digits, '-' and '_'
constexpr bool tag_character(char letter) noexcept {
    return (letter >= 'a' && letter <= 'z') || (letter >= 'A' && letter <= 'Z') ||
           (letter >= '0' && letter <= '9') || letter == '-' || letter == '_';
}

/// One pack entry's strings and the language view that points at them.
///
/// The view is filled only after the strings and the view vectors are
/// finished. Nothing here is changed or freed afterwards, and the deque
/// that holds these never drops or moves one, so the view stays readable
/// for the whole run.
struct Owned {
    std::string tag;
    std::string endonym;
    std::string english_name;
    std::string word;
    std::vector<std::string> locales;
    std::vector<std::string> fallbacks;
    std::vector<std::string_view> locale_views;
    std::vector<std::string_view> fallback_views;
    Language language{};
};

/// The live languages. Changed and read on the thread that draws the
/// interface only. Pack entries are appended to `owned` and stay there
/// after they leave `live`.
struct Store {
    std::deque<Owned> owned;
    std::vector<const Language*> live;
    uint64_t generation = 0;
    bool seeded = false;
};

/// The one registry.
///
/// @return it
Store& registry() {
    static Store store;
    return store;
}

/// A pack entry after the rules have filled its names and locales.
struct Prepared {
    LanguageEntry entry;
    Source source{Source::installed};
};

/// Tells whether a tag is one of the compiled offered languages.
///
/// @param tag a tag
/// @return true when kOfferedLanguages has it
bool offered_tag(std::string_view tag) noexcept {
    for (const Language& offered : kOfferedLanguages)
        if (same_tag(offered.tag, tag))
            return true;
    return false;
}

/// Tells whether a prepared list already holds a tag.
///
/// @param prepared the entries kept so far
/// @param tag a tag
/// @return true when one of them has it
bool tag_taken(const std::vector<Prepared>& prepared, std::string_view tag) noexcept {
    for (const Prepared& have : prepared)
        if (same_tag(have.entry.tag, tag))
            return true;
    return false;
}

/// Tells whether two lists of text hold the same texts in the same order.
///
/// @param left views
/// @param right strings
/// @return true when they match
bool same_texts(
    std::span<const std::string_view> left, const std::vector<std::string>& right
) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index)
        if (left[index] != right[index])
            return false;
    return true;
}

/// Tells whether a live language is the same entry as a prepared one.
///
/// @param language a live language
/// @param item a prepared entry
/// @return true when every field matches
bool same_entry(const Language& language, const Prepared& item) noexcept {
    return language.source == item.source && language.tag == item.entry.tag &&
           language.endonym == item.entry.endonym &&
           language.english_name == item.entry.english_name &&
           language.game_name == item.entry.word && language.needs == item.entry.needs &&
           same_texts(language.locales, item.entry.locales) &&
           same_texts(language.fallbacks, item.entry.fallbacks);
}

/// Applies the field rules and drops an entry the registry does not keep.
///
/// A tag that is empty or not what normalised_locale leaves, an empty word
/// and a built-in tag are dropped. An empty English name becomes the tag,
/// and an empty name in itself becomes the English name. Locales that do
/// not normalise are dropped. Fallbacks are copied as written.
///
/// @param entry the entry as it was passed
/// @return the entry to keep; empty when it is dropped
std::optional<LanguageEntry> accepted_entry(const LanguageEntry& entry) {
    if (entry.word.empty() || entry.tag.empty() || normalised_locale(entry.tag) != entry.tag)
        return std::nullopt;
    for (const Language& built : kLanguages)
        if (same_tag(built.tag, entry.tag))
            return std::nullopt;
    LanguageEntry kept;
    kept.tag = entry.tag;
    kept.english_name = entry.english_name.empty() ? entry.tag : entry.english_name;
    kept.endonym = entry.endonym.empty() ? kept.english_name : entry.endonym;
    kept.word = entry.word;
    kept.needs = entry.needs;
    kept.fallbacks = entry.fallbacks;
    for (const std::string& locale : entry.locales) {
        std::string normalised = normalised_locale(locale);
        if (!normalised.empty())
            kept.locales.push_back(std::move(normalised));
    }
    return kept;
}

/// Appends a pack entry and returns the language view that points at it.
///
/// @param item the entry
/// @return the view; it stays readable for the whole run
const Language* append_entry(const Prepared& item) {
    Owned& owned = registry().owned.emplace_back();
    owned.tag = item.entry.tag;
    owned.endonym = item.entry.endonym;
    owned.english_name = item.entry.english_name;
    owned.word = item.entry.word;
    owned.locales = item.entry.locales;
    owned.fallbacks = item.entry.fallbacks;
    owned.locale_views.reserve(owned.locales.size());
    for (const std::string& locale : owned.locales)
        owned.locale_views.emplace_back(locale);
    owned.fallback_views.reserve(owned.fallbacks.size());
    for (const std::string& fallback : owned.fallbacks)
        owned.fallback_views.emplace_back(fallback);
    Language& language = owned.language;
    language.tag = owned.tag;
    language.endonym = owned.endonym;
    language.english_name = owned.english_name;
    language.game_name = owned.word;
    language.locales = owned.locale_views;
    language.fallbacks = owned.fallback_views;
    language.needs = item.entry.needs;
    language.source = item.source;
    return &language;
}

/// Returns the live entry equal to a prepared one, or appends a new one.
///
/// @param item the entry
/// @return the language the list keeps
const Language* live_or_new(const Prepared& item) {
    Store& store = registry();
    if (store.seeded) {
        for (const Language* language : store.live)
            if (same_entry(*language, item))
                return language;
    }
    return append_entry(item);
}

/// Orders two languages by their names' UTF-8 bytes, then by tag.
///
/// @param left a language
/// @param right a language
/// @return true when left comes first
bool by_endonym(const Language* left, const Language* right) noexcept {
    if (left->endonym != right->endonym)
        return left->endonym < right->endonym;
    return left->tag < right->tag;
}

/// Replaces the live list, and advances the generation when it differs.
/// The first list advances it from zero.
///
/// @param next the new order
void publish(std::vector<const Language*> next) {
    Store& store = registry();
    if (!store.seeded || next != store.live) {
        store.live = std::move(next);
        ++store.generation;
    }
    store.seeded = true;
}

/// Fills the registry from the compiled languages when nothing has yet.
void ensure_seeded() {
    if (!registry().seeded)
        set_pack_languages({}, {});
}

} // namespace

std::span<const Language* const> known_languages() noexcept {
    ensure_seeded();
    const std::vector<const Language*>& live = registry().live;
    return {live.data(), live.size()};
}

const Language& english() noexcept {
    return kLanguages[0];
}

bool meets(TextNeeds needs) noexcept {
    switch (needs) {
    case TextNeeds::game_fonts:
    case TextNeeds::modern_fonts:
        return true;
    case TextNeeds::text_shaping:
        // Shaping is part of a build that linked HarfBuzz. One that has not
        // keeps a shaping language in the registry and does not offer it.
#if defined(OA_TEXT_SHAPING)
        return true;
#else
        return false;
#endif
    case TextNeeds::more_font_faces:
        return false;
    }
    return false;
}

bool drawable(const Language& language) noexcept {
    return meets(language.needs);
}

bool playable(const Language& language) noexcept {
    return language.source != Source::available && drawable(language);
}

bool turns_unicode_chat_on(const Language& language, bool pack_asks) noexcept {
    return pack_asks || language.needs == TextNeeds::modern_fonts ||
           language.needs == TextNeeds::text_shaping;
}

const Language* find_by_tag(std::string_view tag) noexcept {
    ensure_seeded();
    for (const Language* language : registry().live)
        if (same_tag(language->tag, tag))
            return language;
    return nullptr;
}

const Language* find_by_game_name(std::string_view name) noexcept {
    if (name.empty())
        return nullptr;
    ensure_seeded();
    for (const Language* language : registry().live) {
        if (!playable(*language))
            continue;
        if (same_tag(language->game_name, name) || same_tag(language->english_name, name))
            return language;
    }
    return nullptr;
}

FallbackChain fallback_chain(const Language& language) noexcept {
    FallbackChain chain{};
    const auto add = [&chain](const Language* entry) {
        if (entry == nullptr || chain.count >= chain.languages.size())
            return;
        const auto held = chain.view();
        if (std::find(held.begin(), held.end(), entry) == held.end())
            chain.languages[chain.count++] = entry;
    };
    add(&language);
    // Room is kept for English, which ends every chain.
    for (const std::string_view tag : language.fallbacks)
        if (chain.count + 1 < chain.languages.size())
            add(find_by_tag(tag));
    const Language* last = &english();
    const auto held = chain.view();
    if (std::find(held.begin(), held.end(), last) != held.end()) {
        // English listed among the fallbacks moves to the end.
        std::size_t kept = 0;
        for (std::size_t index = 0; index < chain.count; ++index)
            if (chain.languages[index] != last)
                chain.languages[kept++] = chain.languages[index];
        chain.count = kept;
    }
    chain.languages[chain.count++] = last;
    return chain;
}

std::string normalised_locale(std::string_view locale) {
    if (locale.size() > most_locale_bytes)
        return {};
    // The encoding and the modifier go: "de_DE.UTF-8@euro" is "de_DE".
    locale = locale.substr(0, locale.find_first_of(".@"));
    while (!locale.empty() && (locale.front() == ' ' || locale.front() == '\t'))
        locale.remove_prefix(1);
    while (!locale.empty() && (locale.back() == ' ' || locale.back() == '\t'))
        locale.remove_suffix(1);
    if (locale.empty() || locale == "C" || locale == "POSIX")
        return {};
    if (!std::all_of(locale.begin(), locale.end(), tag_character) || locale.front() == '-' ||
        locale.front() == '_')
        return {};
    std::string tag(locale);
    std::replace(tag.begin(), tag.end(), '_', '-');
    return tag;
}

void set_pack_languages(
    std::span<const LanguageEntry> installed, std::span<const LanguageEntry> available
) {
    // Installed entries are kept first, so one of them wins over an
    // available entry with the same tag. A compiled offered language wins
    // over an available entry passed for its tag. The first of two entries
    // with one tag wins.
    std::vector<Prepared> prepared;
    const auto take = [&prepared](const LanguageEntry& entry, Source source) {
        std::optional<LanguageEntry> kept = accepted_entry(entry);
        if (!kept)
            return;
        if (source == Source::available && offered_tag(kept->tag))
            return;
        if (tag_taken(prepared, kept->tag))
            return;
        prepared.push_back(Prepared{std::move(*kept), source});
    };
    for (const LanguageEntry& entry : installed)
        take(entry, Source::installed);
    for (const LanguageEntry& entry : available)
        take(entry, Source::available);

    std::vector<const Language*> shown;
    std::vector<const Language*> waiting;
    shown.reserve(kLanguages.size());
    for (std::size_t index = 1; index < kLanguages.size(); ++index)
        shown.push_back(&kLanguages[index]);
    for (const Prepared& item : prepared) {
        const Language* language = live_or_new(item);
        if (item.source == Source::installed)
            shown.push_back(language);
        else
            waiting.push_back(language);
    }
    for (const Language& offered : kOfferedLanguages)
        if (!tag_taken(prepared, offered.tag))
            waiting.push_back(&offered);

    std::sort(shown.begin(), shown.end(), by_endonym);
    std::sort(waiting.begin(), waiting.end(), by_endonym);
    std::vector<const Language*> order;
    order.reserve(1 + shown.size() + waiting.size());
    order.push_back(&kLanguages[0]);
    order.insert(order.end(), shown.begin(), shown.end());
    order.insert(order.end(), waiting.begin(), waiting.end());
    publish(std::move(order));
}

uint64_t registry_generation() noexcept {
    ensure_seeded();
    return registry().generation;
}

const Language* match_locale(std::string_view locale, bool with_available) {
    const std::string tag = normalised_locale(locale);
    if (tag.empty())
        return nullptr;
    ensure_seeded();
    const Language* best = nullptr;
    std::size_t best_length = 0;
    // A longer match among the languages not offered chooses none.
    for (const std::string_view pattern : kUnofferedLocales)
        if (pattern.size() > best_length && leads(tag, pattern))
            best_length = pattern.size();
    for (const Language* language : registry().live) {
        // A playable language counts. An available language counts only when
        // the caller asks, and only when this build can draw it.
        const bool counts =
            playable(*language) ||
            (with_available && language->source == Source::available && meets(language->needs));
        if (!counts)
            continue;
        for (const std::string_view pattern : language->locales)
            if (pattern.size() > best_length && leads(tag, pattern)) {
                best = language;
                best_length = pattern.size();
            }
    }
    return best;
}

const Language* wanted_language(std::span<const std::string> locales) {
    for (const std::string& locale : locales)
        if (const Language* language = match_locale(locale, true))
            return language;
    return nullptr;
}

const Language& preferred_language(std::span<const std::string> locales) {
    for (const std::string& locale : locales)
        if (const Language* language = match_locale(locale))
            return *language;
    return english();
}

const Language& chosen_language(std::string_view choice, const Language& system) noexcept {
    if (const Language* language = find_by_tag(choice); language != nullptr && playable(*language))
        return *language;
    return system;
}

const PackNotice* pack_notice(std::string_view tag) noexcept {
    for (const PackNoticeRow& row : kPackNotices)
        if (same_tag(row.tag, tag))
            return &row.notice;
    return nullptr;
}

} // namespace oa::data::languages
