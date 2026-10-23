// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's search over made-up packages: a name beats a tag, a tag an
// author or publisher, and those a summary; several words; a word in no
// field hides the entry; ASCII folding; tag prefixes, including a translated
// label of a known id and a different case of that id; the 64-byte cap at a
// character boundary; and ties ordered by name and then registry.
#include "oa/test/check.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;
using lib::Kind;
using lib::MatchClass;

/// The built-in registry's id and two added ones'.
constexpr std::string_view core = "fixture-core";
constexpr std::string_view first_added = "a-registry";
constexpr std::string_view second_added = "b-registry";

/// A mod listing with a name and nothing else to search.
///
/// @param registry the registry's id
/// @param key the key
/// @param name the name
/// @return the listing
lib::Listing mod(std::string_view registry, std::string_view key, std::string_view name) {
    lib::Listing made;
    made.registry = std::string(registry);
    made.kind = Kind::mod;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = "1.0";
    made.size = 1024;
    return made;
}

/// Inputs with the three registries and some listings.
///
/// @param listings the listings
/// @return the inputs
lib::Inputs inputs_with(std::vector<lib::Listing> listings) {
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = std::string(core);
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    lib::Registry first;
    first.id = std::string(first_added);
    first.name = "A Registry";
    lib::Registry second;
    second.id = std::string(second_added);
    second.name = "B Registry";
    inputs.registries = {built_in, first, second};
    inputs.listings = std::move(listings);
    inputs.engine = {0, 8, 0};
    return inputs;
}

/// Returns an empty Library on its Mods tab, where every test here searches.
///
/// @return the Library
lib::Library on_mods() {
    lib::Library library;
    library.tab = lib::Tab::mods;
    return library;
}

/// The keys of the visible entries, in order, after a search.
///
/// @param library the Library
/// @param query the search
/// @return each visible entry's key and registry
std::vector<std::string> found(lib::Library& library, std::string_view query) {
    lib::set_query(library, query);
    std::vector<std::string> keys;
    for (const std::size_t index : library.visible)
        keys.push_back(library.entries[index].id.key + "@" + library.entries[index].id.registry);
    return keys;
}

using Keys = std::vector<std::string>;

void test_field_order() {
    lib::Listing name = mod(core, "name-hit", "Zeta Alpha");
    name.author = "Someone";
    name.summary = "Plain.";
    lib::Listing tag = mod(core, "tag-hit", "Tag Holder");
    tag.tags = {"units", "alphabet"};
    lib::Listing author = mod(core, "author-hit", "Author Holder");
    author.author = "Alpha Team";
    lib::Listing publisher = mod(core, "publisher-hit", "Publisher Holder");
    publisher.publisher = "Alpha House";
    lib::Listing summary = mod(core, "summary-hit", "Summary Holder");
    summary.summary = "An alpha build.";
    lib::Listing nothing = mod(core, "nothing", "Nothing Here");
    lib::Library library = on_mods();
    lib::refresh(library, inputs_with({summary, publisher, author, tag, name, nothing}));

    const Keys by_field = {
        "name-hit@fixture-core",
        "tag-hit@fixture-core",
        "author-hit@fixture-core",
        "publisher-hit@fixture-core",
        "summary-hit@fixture-core",
    };
    OA_CHECK(found(library, "alpha") == by_field);
    // An empty search shows every entry, by name, the base game entry first.
    OA_CHECK(found(library, "").size() == 7);
    OA_CHECK(found(library, "   ").size() == 7);
    OA_CHECK(found(library, "").front() == "@base-game@");

    const auto class_of = [&](std::string_view key, std::string_view query) {
        const lib::Entry* entry =
            lib::find_entry(library, {std::string(core), Kind::mod, std::string(key)});
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? lib::rank(*entry, query) : std::nullopt;
    };
    OA_CHECK(class_of("name-hit", "alpha").value_or(lib::Rank{}).worst == MatchClass::name);
    OA_CHECK(class_of("tag-hit", "alpha").value_or(lib::Rank{}).worst == MatchClass::tag);
    OA_CHECK(class_of("author-hit", "alpha").value_or(lib::Rank{}).worst == MatchClass::author);
    OA_CHECK(class_of("publisher-hit", "alpha").value_or(lib::Rank{}).worst == MatchClass::author);
    OA_CHECK(class_of("summary-hit", "alpha").value_or(lib::Rank{}).worst == MatchClass::summary);
    OA_CHECK(!class_of("nothing", "alpha"));
    // The name is still the best field when a tag or the summary has the word too.
    OA_CHECK(class_of("name-hit", "zeta").value_or(lib::Rank{}).worst == MatchClass::name);
}

void test_several_words() {
    lib::Listing summary_word = mod(core, "x", "Alpha Kit");
    summary_word.summary = "Made by a team.";
    lib::Listing author_words = mod(core, "y", "Other");
    author_words.author = "Alpha Team";
    lib::Listing tag_and_publisher = mod(core, "z", "Third");
    tag_and_publisher.tags = {"alpha"};
    tag_and_publisher.publisher = "Team Fixture";
    lib::Listing name_words = mod(core, "w", "Alpha Team Kit");
    lib::Listing one_word = mod(core, "v", "Alpha Only");
    lib::Library library = on_mods();
    lib::refresh(
        library, inputs_with({summary_word, author_words, tag_and_publisher, name_words, one_word})
    );

    // By the worst class, then the sum of the classes.
    const Keys by_class = {
        "w@fixture-core",
        "z@fixture-core",
        "y@fixture-core",
        "x@fixture-core",
    };
    OA_CHECK(found(library, "alpha team") == by_class);
    // Spaces around and between the words do not count.
    OA_CHECK(found(library, "  team   alpha ") == by_class);

    // A word in no field hides the entry, whatever the other words find.
    OA_CHECK(found(library, "alpha missing").empty());
    const lib::Entry* only = lib::find_entry(library, {std::string(core), Kind::mod, "v"});
    OA_CHECK(only != nullptr && !lib::rank(*only, "alpha team"));
    OA_CHECK(only != nullptr && lib::rank(*only, "alpha only"));
    const lib::Entry* mixed = lib::find_entry(library, {std::string(core), Kind::mod, "z"});
    const std::optional<lib::Rank> ranked =
        mixed != nullptr ? lib::rank(*mixed, "alpha team") : std::nullopt;
    OA_CHECK(ranked && ranked->worst == MatchClass::author && ranked->total == 3);
}

void test_folding() {
    lib::Listing kit = mod(core, "kit", "Alpha Kit");
    lib::Listing accented = mod(core, "accented", "\xc3\x89mile Mod"); // capital E with acute
    lib::Library library = on_mods();
    lib::refresh(library, inputs_with({kit, accented}));
    OA_CHECK(found(library, "ALPHA kIt") == Keys({"kit@fixture-core"}));
    // Only A to Z fold: a capital E with acute matches itself, not its small letter.
    OA_CHECK(found(library, "\xc3\x89MILE") == Keys({"accented@fixture-core"}));
    OA_CHECK(found(library, "\xc3\xa9mile").empty());
}

void test_tag_prefix() {
    lib::Listing balance = mod(core, "balance", "First Mod");
    balance.tags = {"balance"};
    lib::Listing other = mod(core, "other", "Second Mod");
    other.tags = {"rebalance"};
    lib::Library library = on_mods();
    lib::refresh(library, inputs_with({balance, other}));
    OA_CHECK(found(library, "bal") == Keys({"balance@fixture-core"}));
    OA_CHECK(found(library, "balance") == Keys({"balance@fixture-core"}));
    OA_CHECK(found(library, "reb") == Keys({"other@fixture-core"}));
    // A tag is not searched inside.
    OA_CHECK(found(library, "ance").empty());
}

void test_query_cap() {
    lib::Library library = on_mods();
    lib::refresh(library, inputs_with({mod(core, "kit", "Alpha Kit")}));
    const std::string ascii(65, 'a');
    lib::set_query(library, ascii);
    OA_CHECK(library.query == std::string(64, 'a'));
    lib::set_query(library, std::string(64, 'b'));
    OA_CHECK(library.query.size() == 64);
    // A two-byte character across the cap is left out whole.
    lib::set_query(library, std::string(63, 'a') + "\xc3\xa9");
    OA_CHECK(library.query == std::string(63, 'a'));
    // So is a three-byte one.
    lib::set_query(library, std::string(62, 'a') + "\xe2\x86\x92");
    OA_CHECK(library.query == std::string(62, 'a'));
    lib::set_query(library, std::string(61, 'a') + "\xe2\x86\x92");
    OA_CHECK(library.query == std::string(61, 'a') + "\xe2\x86\x92");
    // A word cut by the cap is searched for what is left of it; a word past the cap is not.
    lib::set_query(library, "kit " + std::string(56, ' ') + "zzzzz");
    OA_CHECK(library.query.size() == 64 && library.visible.empty());
    lib::set_query(library, "kit " + std::string(60, ' ') + "zzzz");
    OA_CHECK(library.query.size() == 64 && library.visible.size() == 1);
}

void test_ties() {
    lib::Inputs inputs = inputs_with({
        mod(second_added, "same", "Same Mod"),
        mod(first_added, "same", "Same Mod"),
        mod(core, "same", "same mod"),
        mod(core, "aaa", "Aaa Same"),
        mod(core, "beta", "Beta Same"),
    });
    lib::Library library = on_mods();
    lib::refresh(library, std::move(inputs));
    // Names that start with the word first; then by name, built-in registries first, then registry.
    const Keys word_first = {
        "same@fixture-core",
        "same@a-registry",
        "same@b-registry",
        "aaa@fixture-core",
        "beta@fixture-core",
    };
    OA_CHECK(found(library, "same") == word_first);
    const Keys by_name = {
        "@base-game@",
        "aaa@fixture-core",
        "beta@fixture-core",
        "same@fixture-core",
        "same@a-registry",
        "same@b-registry",
    };
    OA_CHECK(found(library, "") == by_name);
}

/// Translates the Economy tag and leaves every other word as written.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view economy_in_german(void*, std::string_view english) {
    if (english == lib::words::tag_economy)
        return "Wirtschaft";
    return english;
}

void test_translated_tag() {
    lib::Listing known = mod(core, "quarry", "Quarry");
    known.tags = {"economy"};
    lib::Listing capital = mod(core, "capital", "Capital Tag");
    capital.tags = {"Economy"};
    lib::Library library = on_mods();
    library.text.shown = economy_in_german;
    lib::refresh(library, inputs_with({known, capital}));
    // The shown label matches the id economy. A different case is not that id.
    OA_CHECK(found(library, "Wirtschaft") == Keys({"quarry@fixture-core"}));
    OA_CHECK(found(library, "econ") == Keys({"capital@fixture-core", "quarry@fixture-core"}));
}

/// Translates Economy and Default, and leaves every other word as written.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view brain_words_in_german(void*, std::string_view english) {
    if (english == lib::words::tag_economy)
        return "Wirtschaft";
    if (english == lib::words::chip_default)
        return "Standard";
    return english;
}

/// A brain listing with one tag and nothing else filled in.
///
/// @param key the key
/// @param name the name
/// @param tag the tag id
/// @return the listing
lib::Listing brain(std::string_view key, std::string_view name, std::string_view tag) {
    lib::Listing made = mod(core, key, name);
    made.kind = Kind::brain;
    lib::BrainFacts facts;
    facts.tags = {std::string(tag)};
    made.brain = std::move(facts);
    return made;
}

/// A translated brain tag finds that brain, and "default" finds the built-in
/// player, in English and under the translated Default.
void test_brain_search() {
    lib::Library library;
    library.text.shown = brain_words_in_german;
    lib::refresh(library, inputs_with({brain("wide-watch", "Wide Watch", "economy")}));
    lib::set_tab(library, lib::Tab::brains);
    OA_CHECK(found(library, "Wirtschaft") == Keys({"wide-watch@fixture-core"}));
    OA_CHECK(found(library, "default") == Keys({"@built-in@"}));
    OA_CHECK(found(library, "Standard") == Keys({"@built-in@"}));
    OA_CHECK(found(library, "").front() == "@built-in@");
}

void test_installed_names() {
    lib::Inputs inputs = inputs_with({});
    lib::Installed own;
    own.kind = Kind::mod;
    own.key = "own-mod";
    own.name = "Own Alpha";
    inputs.installed.push_back(own);
    lib::Library library = on_mods();
    lib::refresh(library, std::move(inputs));
    OA_CHECK(found(library, "alpha") == Keys({"own-mod@"}));
}

} // namespace

int main() {
    test_field_order();
    test_several_words();
    test_folding();
    test_tag_prefix();
    test_translated_tag();
    test_brain_search();
    test_query_cap();
    test_ties();
    test_installed_names();
    return oa::test::check_exit_status();
}
