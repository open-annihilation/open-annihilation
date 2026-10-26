// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's model over made-up registries and packages: every state,
// every reason GET or UPDATE is off, built-in and added registries, a key
// installed from another registry, tabs, each tab's own filter, New, the
// filters' counts and tags, a tag left from another tab, an update a finished
// install has already reached, the order with no search, the update count, the
// Active tab's setup, the selection across refreshes, the buttons, the facts
// with the sample numbers, the status lines, the bylines, the playing note,
// the queue lines and every text.
#include "oa/data/languages.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"
#include "oa/ui/library/use_question.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;
using lib::Action;
using lib::Kind;
using lib::Mark;
using lib::State;
using Phase = lib::QueueItem::Phase;

/// Checks that a text is what a case expects, printing both when it is not.
///
/// @param actual the text the model wrote
/// @param expected the text the case expects
/// @param file the check's file
/// @param line the check's line
/// @param expression the text's expression, as written
void check_text(
    std::string_view actual,
    std::string_view expected,
    const char* file,
    int line,
    const char* expression
) {
    if (actual == expected)
        return;
    std::fprintf(
        stderr,
        "%s:%d: %s is \"%.*s\", expected \"%.*s\"\n",
        file,
        line,
        expression,
        static_cast<int>(actual.size()),
        actual.data(),
        static_cast<int>(expected.size()),
        expected.data()
    );
    ++oa::test::failed_checks();
}

#define OA_CHECK_TEXT(actual, expected)                                                            \
    check_text((actual), (expected), __FILE__, __LINE__, #actual)

constexpr uint64_t mib = uint64_t{1} << 20;

/// The built-in registry's id and the added one's.
constexpr std::string_view core = "fixture-core";
constexpr std::string_view example = "example-maps";

/// A digest whose first four bytes are given and whose others are zero.
///
/// @param b0 the first byte
/// @param b1 the second byte
/// @param b2 the third byte
/// @param b3 the fourth byte
/// @return the digest
oa::base::sha256::Digest digest(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    oa::base::sha256::Digest value{};
    value[0] = b0;
    value[1] = b1;
    value[2] = b2;
    value[3] = b3;
    return value;
}

/// The sample rules hashes "9c1f 4e0b" and "51d2 07aa".
const oa::base::sha256::Digest old_rules = digest(0x9c, 0x1f, 0x4e, 0x0b);
const oa::base::sha256::Digest new_rules = digest(0x51, 0xd2, 0x07, 0xaa);

/// The two registries: a built-in one and one the player added.
///
/// @return them
std::vector<lib::Registry> fixture_registries() {
    lib::Registry built_in;
    built_in.id = std::string(core);
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    lib::Registry added;
    added.id = std::string(example);
    added.name = "Example Maps";
    return {built_in, added};
}

/// A listing with a name, a version and a size.
///
/// @param registry the registry's id
/// @param kind the kind
/// @param key the key
/// @param name the name
/// @return the listing
lib::Listing
listing(std::string_view registry, Kind kind, std::string_view key, std::string_view name) {
    lib::Listing made;
    made.registry = std::string(registry);
    made.kind = kind;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = "1.0";
    made.revision = 1;
    made.release = 1;
    made.size = 64 * mib;
    made.author = "Fixture Author";
    return made;
}

/// An installed package.
///
/// @param registry where it came from; empty for the player's own
/// @param kind the kind
/// @param key the key
/// @param name the name
/// @return the package
lib::Installed
installed(std::string_view registry, Kind kind, std::string_view key, std::string_view name) {
    lib::Installed made;
    made.registry = std::string(registry);
    made.kind = kind;
    made.key = std::string(key);
    made.name = std::string(name);
    made.folder = std::string(key);
    made.version = "1.0";
    made.revision = 1;
    made.release = registry.empty() ? 0 : 1;
    return made;
}

/// An update of a package to revision 4 of version 4.8, 3.2 MB.
///
/// @param registry the registry's id
/// @param kind the kind
/// @param key the key
/// @return the update
lib::UpdateNote update(std::string_view registry, Kind kind, std::string_view key) {
    lib::UpdateNote made;
    made.registry = std::string(registry);
    made.kind = kind;
    made.key = std::string(key);
    made.to_release = 2;
    made.to_version = "4.8";
    made.to_revision = 4;
    made.size = 3355443; // 3.2 MiB
    return made;
}

/// A queue item.
///
/// @param registry the registry's id
/// @param key the key
/// @param name the name
/// @param phase the phase
/// @return the item, a mod at version 1.0
lib::QueueItem
queued(std::string_view registry, std::string_view key, std::string_view name, Phase phase) {
    lib::QueueItem made;
    made.registry = std::string(registry);
    made.kind = Kind::mod;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = "1.0";
    made.phase = phase;
    return made;
}

/// Inputs with the two registries, at engine 0.8.0, with a list four minutes old.
///
/// @return the inputs
lib::Inputs base_inputs() {
    lib::Inputs inputs;
    inputs.registries = fixture_registries();
    inputs.engine = {0, 8, 0};
    inputs.version_text = "v0.8.0";
    inputs.list_age_seconds = 240;
    return inputs;
}

/// Builds a Library from inputs, on its Mods tab.
///
/// @param inputs the inputs
/// @return the Library
lib::Library built(lib::Inputs inputs) {
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    lib::set_tab(library, lib::Tab::mods);
    return library;
}

/// Finds an entry.
///
/// @param library the Library
/// @param registry the registry's id; empty for the player's own
/// @param kind the kind
/// @param key the key
/// @return the entry, or null
const lib::Entry*
at(const lib::Library& library, std::string_view registry, Kind kind, std::string_view key) {
    return lib::find_entry(library, {std::string(registry), kind, std::string(key)});
}

/// The built-in languages, in the order Settings › Language lists them,
/// written as visible_keys writes a row.
///
/// @return the keys
std::vector<std::string> built_in_language_rows() {
    std::vector<std::string> keys;
    for (const oa::data::languages::Language* language : oa::data::languages::known_languages()) {
        if (language == nullptr || language->source != oa::data::languages::Source::built_in ||
            !oa::data::languages::playable(*language))
            continue;
        keys.push_back(lib::built_in_language_id(language->tag).key + "@");
    }
    return keys;
}

/// The keys of the visible entries, in order.
///
/// @param library the Library
/// @return the keys
std::vector<std::string> visible_keys(const lib::Library& library) {
    std::vector<std::string> keys;
    for (const std::size_t index : library.visible)
        keys.push_back(library.entries[index].id.key + "@" + library.entries[index].id.registry);
    return keys;
}

/// The actions of an entry's buttons, with whether each is enabled.
///
/// @param library the Library
/// @param entry the entry
/// @return each button's action and enabled flag
std::vector<std::pair<Action, bool>>
actions_of(const lib::Library& library, const lib::Entry& entry) {
    std::vector<std::pair<Action, bool>> found;
    for (const lib::ActionButton& button : lib::entry_actions(library, entry))
        found.emplace_back(button.action, button.enabled);
    return found;
}

/// Finds a fact by its long label.
///
/// @param list the facts
/// @param label the label
/// @return the fact, or null
const lib::Fact* fact(const std::vector<lib::Fact>& list, std::string_view label) {
    for (const lib::Fact& item : list)
        if (item.long_label == label)
            return &item;
    return nullptr;
}

/// An id of a hack this Open Annihilation carries out.
///
/// @return the id; empty when the registry has none
std::string_view implemented_hack() {
    namespace hacks = oa::data::mod_profile::registry;
    for (const hacks::Entry& entry : hacks::table().entries)
        if (entry.kind == hacks::EntryKind::hack && entry.implemented)
            return entry.id;
    return {};
}

void test_state_matrix() {
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "get-mod", "Get Mod"));
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "kept-mod", "Kept Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "kept-mod", "Kept Mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.updates.push_back(update(core, Kind::mod, "fixture-mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "played-mod", "Played Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "played-mod", "Played Mod"));
    inputs.installed.back().playing = true;
    inputs.listings.push_back(listing(core, Kind::mod, "played-update", "Played Update"));
    inputs.installed.push_back(installed(core, Kind::mod, "played-update", "Played Update"));
    inputs.installed.back().playing = true;
    inputs.updates.push_back(update(core, Kind::mod, "played-update"));
    inputs.listings.push_back(listing(core, Kind::mod, "blocked-update", "Blocked Update"));
    inputs.installed.push_back(installed(core, Kind::mod, "blocked-update", "Blocked Update"));
    inputs.updates.push_back(update(core, Kind::mod, "blocked-update"));
    inputs.updates.back().blocked = lib::UpdateNote::Blocked::engine;
    inputs.updates.back().blocked_detail = "0.9.0 or later";
    // Installed from a registry that no longer lists it.
    inputs.installed.push_back(installed(example, Kind::map_pack, "gone-maps", "Gone Maps"));
    // An update with no installed package is not an entry's.
    inputs.updates.push_back(update(core, Kind::mod, "get-mod"));
    const lib::Library library = built(std::move(inputs));

    // Eight packages, the base game, the built-in computer player, and the
    // built-in languages refresh adds.
    OA_CHECK(library.entries.size() == 8 + 1 + 1 + built_in_language_rows().size());
    const lib::Entry* built_in_player = lib::find_entry(library, lib::built_in_brain_id());
    OA_CHECK(built_in_player != nullptr && lib::is_built_in_brain(built_in_player->id));
    const lib::Entry* get = at(library, core, Kind::mod, "get-mod");
    OA_CHECK(get != nullptr && get->state == State::get && get->listing != nullptr);
    OA_CHECK(get != nullptr && get->installed == nullptr && get->update == nullptr && get->can_act);
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    OA_CHECK(own != nullptr && own->state == State::installed && own->listing == nullptr);
    const lib::Entry* kept = at(library, core, Kind::mod, "kept-mod");
    OA_CHECK(kept != nullptr && kept->state == State::installed && kept->listing != nullptr);
    OA_CHECK(kept != nullptr && kept->installed != nullptr && kept->update == nullptr);
    const lib::Entry* updated = at(library, core, Kind::mod, "fixture-mod");
    OA_CHECK(updated != nullptr && updated->state == State::update && updated->can_act);
    const lib::Entry* played = at(library, core, Kind::mod, "played-mod");
    OA_CHECK(played != nullptr && played->state == State::playing);
    const lib::Entry* played_update = at(library, core, Kind::mod, "played-update");
    OA_CHECK(played_update != nullptr && played_update->state == State::update);
    const lib::Entry* blocked = at(library, core, Kind::mod, "blocked-update");
    OA_CHECK(blocked != nullptr && blocked->state == State::update && !blocked->can_act);
    if (blocked != nullptr)
        OA_CHECK_TEXT(blocked->blocked, "Needs Open Annihilation 0.9.0 or later");
    const lib::Entry* gone = at(library, example, Kind::map_pack, "gone-maps");
    OA_CHECK(gone != nullptr && gone->state == State::installed && gone->listing == nullptr);
    if (gone != nullptr)
        OA_CHECK_TEXT(gone->registry_name, "Example Maps");
}

void test_queue_states() {
    lib::Inputs inputs = base_inputs();
    const std::vector<Phase> phases = {
        Phase::waiting,
        Phase::downloading,
        Phase::verifying,
        Phase::checking,
        Phase::installing,
        Phase::held,
        Phase::failed,
        Phase::installed,
    };
    for (std::size_t index = 0; index < phases.size(); ++index) {
        const std::string key = "queued-" + std::to_string(index);
        inputs.listings.push_back(listing(core, Kind::mod, key, "Queued " + std::to_string(index)));
        inputs.queue.push_back(queued(core, key, "Queued " + std::to_string(index), phases[index]));
    }
    inputs.queue[1].done_bytes = 19083674; // 18.2 MiB
    inputs.queue[1].total_bytes = 64 * mib;
    inputs.queue[6].problem = "Couldn't reach Example Maps";
    inputs.queue[6].retryable = true;
    const lib::Library library = built(std::move(inputs));

    const std::vector<std::string_view> lines = {
        "Waiting to download",
        "Downloading · 18.2 of 64.0 MB",
        "Verifying",
        "Checking",
        "Installing",
        "Waiting · finishes when you leave Queued 5",
        "Couldn't reach Example Maps",
        "Not installed · 64.0 MB",
    };
    for (std::size_t index = 0; index < phases.size(); ++index) {
        const lib::Entry* entry = at(library, core, Kind::mod, "queued-" + std::to_string(index));
        OA_CHECK(entry != nullptr && entry->queued != nullptr && entry->state == State::get);
        if (entry == nullptr)
            continue;
        OA_CHECK_TEXT(lib::status_line(library, *entry), lines[index]);
        const std::vector<std::pair<Action, bool>> actions = actions_of(library, *entry);
        OA_CHECK(!actions.empty());
        if (actions.empty())
            continue;
        if (index < 6)
            OA_CHECK(actions.front() == std::make_pair(Action::cancel, true));
        else if (index == 6)
            OA_CHECK(actions.front() == std::make_pair(Action::retry, true));
        else
            OA_CHECK(actions.front() == std::make_pair(Action::get, true));
    }
}

void test_blocked_reasons() {
    const std::string_view working = implemented_hack();
    OA_CHECK(!working.empty());

    lib::Inputs inputs = base_inputs();
    lib::Listing engine = listing(core, Kind::mod, "engine-mod", "Engine Mod");
    engine.requires_engine = ">= 0.9.0";
    engine.base = "other";
    inputs.listings.push_back(engine);
    lib::Listing met = listing(core, Kind::mod, "met-mod", "Met Mod");
    met.requires_engine = ">= 0.8.0";
    inputs.listings.push_back(met);
    lib::Listing unreadable = listing(core, Kind::mod, "unreadable-mod", "Unreadable Mod");
    unreadable.requires_engine = "soon";
    inputs.listings.push_back(unreadable);
    lib::Listing base = listing(core, Kind::mod, "base-mod", "Base Mod");
    base.base = "other";
    base.hack_ids = {"fixture.missing-one"};
    inputs.listings.push_back(base);
    lib::Listing hacks = listing(core, Kind::mod, "hacks-mod", "Hacks Mod");
    hacks.hack_ids = {
        "fixture.missing-one",
        std::string(working),
        "fixture.missing-two",
        "fixture.missing-three",
        "fixture.missing-four",
        "fixture.missing-five",
    };
    inputs.listings.push_back(hacks);
    lib::Listing two_hacks = listing(core, Kind::mod, "two-hacks-mod", "Two Hacks Mod");
    two_hacks.hack_ids = {"fixture.missing-one", std::string(working), "fixture.missing-two"};
    inputs.listings.push_back(two_hacks);
    lib::Listing fits = listing(core, Kind::mod, "fits-mod", "Fits Mod");
    fits.base = "ta-3.1c";
    fits.hack_ids = {std::string(working)};
    inputs.listings.push_back(fits);
    inputs.listings.push_back(listing(core, Kind::mod, "hacks-update", "Hacks Update"));
    inputs.installed.push_back(installed(core, Kind::mod, "hacks-update", "Hacks Update"));
    inputs.updates.push_back(update(core, Kind::mod, "hacks-update"));
    inputs.updates.back().blocked = lib::UpdateNote::Blocked::hacks;
    inputs.updates.back().blocked_detail = "fixture.missing-one";
    inputs.listings.push_back(listing(core, Kind::mod, "base-update", "Base Update"));
    inputs.installed.push_back(installed(core, Kind::mod, "base-update", "Base Update"));
    inputs.updates.push_back(update(core, Kind::mod, "base-update"));
    inputs.updates.back().blocked = lib::UpdateNote::Blocked::base;
    inputs.updates.back().blocked_detail = "other";
    lib::Library library = built(inputs);

    const auto blocked_of = [&](std::string_view key) -> std::string {
        const lib::Entry* entry = at(library, core, Kind::mod, key);
        OA_CHECK(entry != nullptr);
        if (entry == nullptr)
            return "(none)";
        OA_CHECK(entry->can_act == entry->blocked.empty());
        return entry->blocked;
    };
    // The engine requirement comes before the base, and the base before the hacks.
    OA_CHECK_TEXT(blocked_of("engine-mod"), "Needs Open Annihilation 0.9.0 or later");
    OA_CHECK_TEXT(blocked_of("met-mod"), "");
    OA_CHECK_TEXT(blocked_of("unreadable-mod"), "");
    OA_CHECK_TEXT(blocked_of("base-mod"), "Needs the base game other");
    OA_CHECK_TEXT(
        blocked_of("hacks-mod"),
        "Needs hacks this OA lacks: fixture.missing-one, fixture.missing-two, "
        "fixture.missing-three · 2 more"
    );
    OA_CHECK_TEXT(
        blocked_of("two-hacks-mod"),
        "Needs hacks this OA lacks: fixture.missing-one, fixture.missing-two"
    );
    OA_CHECK_TEXT(blocked_of("fits-mod"), "");
    OA_CHECK_TEXT(blocked_of("hacks-update"), "Needs hacks this OA lacks: fixture.missing-one");
    OA_CHECK_TEXT(blocked_of("base-update"), "Needs the base game other");
    OA_CHECK(lib::update_count(library) == 0);

    // A blocked GET is listed, with its reason as its status line.
    std::size_t blocked_shown = 0;
    for (const std::size_t index : library.visible)
        if (!library.entries[index].can_act)
            ++blocked_shown;
    OA_CHECK(blocked_shown == 6);
    const lib::Entry* engine_entry = at(library, core, Kind::mod, "engine-mod");
    if (engine_entry != nullptr) {
        OA_CHECK_TEXT(
            lib::status_line(library, *engine_entry), "Needs Open Annihilation 0.9.0 or later"
        );
        OA_CHECK(actions_of(library, *engine_entry).front() == std::make_pair(Action::get, false));
    }

    // Downloads turned off for the registry come before the package's own reasons.
    inputs.registries[0].downloads_on = false;
    lib::refresh(library, inputs);
    OA_CHECK_TEXT(
        blocked_of("engine-mod"), "Downloads from Fixture Core are off (Settings › Downloads)"
    );
    OA_CHECK_TEXT(
        blocked_of("hacks-update"), "Downloads from Fixture Core are off (Settings › Downloads)"
    );
    inputs.registries[0].downloads_off_reason = "Fixture Core needs an install ID";
    lib::refresh(library, inputs);
    OA_CHECK_TEXT(blocked_of("met-mod"), "Fixture Core needs an install ID");

    // A game being played comes first of all; installed entries are never blocked.
    inputs.in_match = true;
    lib::refresh(library, inputs);
    OA_CHECK_TEXT(blocked_of("engine-mod"), "Not during a game");
    OA_CHECK_TEXT(blocked_of("met-mod"), "Not during a game");
    OA_CHECK_TEXT(blocked_of("base-update"), "Not during a game");
}

void test_installed_never_blocked() {
    lib::Inputs inputs = base_inputs();
    inputs.registries[1].downloads_on = false;
    inputs.in_match = true;
    lib::Listing needs = listing(example, Kind::mod, "fixture-mod", "Fixture Mod");
    needs.requires_engine = ">= 9.0.0";
    needs.hack_ids = {"fixture.missing-one"};
    inputs.listings.push_back(needs);
    inputs.installed.push_back(installed(example, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.back().kept = lib::Kept{"0.9", 2};
    inputs.installed.push_back(installed(example, Kind::mod, "second-mod", "Second Mod"));
    inputs.installed.back().playing = true;
    inputs.installed.back().kept = lib::Kept{"0.9", 2};
    const lib::Library library = built(std::move(inputs));
    const lib::Entry* fixture = at(library, example, Kind::mod, "fixture-mod");
    const lib::Entry* second = at(library, example, Kind::mod, "second-mod");
    OA_CHECK(fixture != nullptr && fixture->state == State::installed);
    OA_CHECK(fixture != nullptr && fixture->can_act && fixture->blocked.empty());
    OA_CHECK(second != nullptr && second->state == State::playing);
    OA_CHECK(second != nullptr && second->can_act && second->blocked.empty());
    if (fixture != nullptr) {
        OA_CHECK_TEXT(lib::status_line(library, *fixture), "Installed");
        const std::vector<std::pair<Action, bool>> expected = {
            {Action::play_now, false},
            {Action::roll_back, false},
            {Action::uninstall, true},
            {Action::open_folder, true},
        };
        OA_CHECK(actions_of(library, *fixture) == expected);
    }
    if (second != nullptr) {
        OA_CHECK_TEXT(lib::status_line(library, *second), "Playing");
        const std::vector<std::pair<Action, bool>> expected = {
            {Action::switch_off, false},
            {Action::roll_back, false},
            {Action::uninstall, true},
            {Action::open_folder, true},
        };
        OA_CHECK(actions_of(library, *second) == expected);
    }
}

void test_registries() {
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.listings.push_back(listing(example, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.listings.push_back(listing(example, Kind::map_pack, "example-pack", "Example Pack"));
    inputs.installed.push_back(installed("", Kind::map_pack, "example-pack", "Example Pack"));
    // A registry the inputs no longer name.
    inputs.installed.push_back(installed("removed", Kind::mod, "removed-mod", "Removed Mod"));
    const lib::Library library = built(std::move(inputs));

    // Two registries with one key are two entries.
    const lib::Entry* built_in = at(library, core, Kind::mod, "fixture-mod");
    const lib::Entry* added = at(library, example, Kind::mod, "fixture-mod");
    OA_CHECK(built_in != nullptr && added != nullptr && built_in != added);
    if (built_in != nullptr) {
        OA_CHECK(built_in->built_in && !built_in->not_reviewed);
        OA_CHECK_TEXT(built_in->registry_name, "Fixture Core");
        OA_CHECK(built_in->state == State::installed);
        OA_CHECK(built_in->replaces_registry_name.empty());
    }
    if (added != nullptr) {
        OA_CHECK(!added->built_in && added->not_reviewed);
        OA_CHECK_TEXT(added->registry_name, "Example Maps");
        OA_CHECK(added->state == State::get);
        OA_CHECK_TEXT(added->replaces_registry_name, "Fixture Core");
    }
    // The player's own package: no registry, nothing replaced by name.
    const lib::Entry* own = at(library, "", Kind::map_pack, "example-pack");
    OA_CHECK(own != nullptr && !own->built_in && !own->not_reviewed && own->registry_name.empty());
    const lib::Entry* pack = at(library, example, Kind::map_pack, "example-pack");
    OA_CHECK(pack != nullptr && pack->state == State::get && pack->replaces_registry_name.empty());
    const lib::Entry* removed = at(library, "removed", Kind::mod, "removed-mod");
    OA_CHECK(removed != nullptr && !removed->built_in && removed->not_reviewed);
    if (removed != nullptr)
        OA_CHECK_TEXT(removed->registry_name, "removed");
}

void test_tabs_filters_tags() {
    lib::Inputs inputs = base_inputs();
    lib::Listing alpha = listing(core, Kind::mod, "alpha-mod", "Alpha Mod");
    alpha.tags = {"balance", "units"};
    inputs.listings.push_back(alpha);
    lib::Listing beta = listing(core, Kind::mod, "beta-mod", "Beta Mod");
    beta.tags = {"balance"};
    inputs.listings.push_back(beta);
    inputs.installed.push_back(installed(core, Kind::mod, "beta-mod", "Beta Mod"));
    lib::Listing gamma = listing(core, Kind::mod, "gamma-mod", "Gamma Mod");
    gamma.tags = {"total-conversion"};
    inputs.listings.push_back(gamma);
    inputs.installed.push_back(installed(core, Kind::mod, "gamma-mod", "Gamma Mod"));
    inputs.updates.push_back(update(core, Kind::mod, "gamma-mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "delta-mod", "Delta Mod"));
    inputs.installed.back().playing = true;
    inputs.listings.push_back(listing(example, Kind::map_pack, "example-pack", "Example Pack"));
    inputs.installed.push_back(installed(example, Kind::map_pack, "example-pack", "Example Pack"));
    inputs.updates.push_back(update(example, Kind::map_pack, "example-pack"));
    inputs.listings.push_back(listing(core, Kind::language, "xx", "Fixture Language"));
    lib::Library library = built(std::move(inputs));

    using Keys = std::vector<std::string>;
    OA_CHECK(library.tab == lib::Tab::mods && lib::shown_filter(library) == lib::Filter::all);
    const Keys every_mod = {
        "@base-game@",
        "gamma-mod@fixture-core",
        "beta-mod@fixture-core",
        "delta-mod@fixture-core",
        "alpha-mod@fixture-core",
    };
    OA_CHECK(visible_keys(library) == every_mod);
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(visible_keys(library) == Keys({"example-pack@example-maps"}));
    lib::set_tab(library, lib::Tab::languages);
    Keys language_rows = built_in_language_rows();
    language_rows.push_back("xx@fixture-core");
    OA_CHECK(visible_keys(library) == language_rows);

    lib::set_tab(library, lib::Tab::mods);
    lib::set_filter(library, lib::Filter::installed);
    const Keys installed_mods = {
        "@base-game@",
        "gamma-mod@fixture-core",
        "beta-mod@fixture-core",
        "delta-mod@fixture-core",
    };
    OA_CHECK(visible_keys(library) == installed_mods);
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"gamma-mod@fixture-core"}));

    // Each tab keeps its own filter: the Updates tab opens on Updates, of
    // every kind, whatever Mods shows.
    lib::set_filter(library, lib::Filter::installed);
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::updates);
    const Keys every_update = {
        "example-pack@example-maps",
        "gamma-mod@fixture-core",
    };
    OA_CHECK(visible_keys(library) == every_update);
    OA_CHECK(lib::update_count(library) == 2 && lib::updates_shown(library) == 2);
    lib::set_tab(library, lib::Tab::mods);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::installed);
    OA_CHECK(visible_keys(library) == installed_mods);

    // The Updates tab's filters work as the others do, across every kind.
    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::all);
    const Keys every_entry = {
        "example-pack@example-maps",
        "gamma-mod@fixture-core",
        "beta-mod@fixture-core",
        "delta-mod@fixture-core",
        "alpha-mod@fixture-core",
        "xx@fixture-core",
    };
    OA_CHECK(visible_keys(library) == every_entry);
    OA_CHECK(lib::updates_shown(library) == 2);
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(
        visible_keys(library) == Keys({
                                     "example-pack@example-maps",
                                     "gamma-mod@fixture-core",
                                     "beta-mod@fixture-core",
                                     "delta-mod@fixture-core",
                                 })
    );
    lib::set_tag(library, "total-conversion");
    OA_CHECK(visible_keys(library) == Keys({"gamma-mod@fixture-core"}));
    lib::set_tag(library, "");
    lib::set_filter(library, lib::Filter::updates);

    // Each filter's count is of the tab's entries it keeps, whatever the tag and the search.
    lib::set_tab(library, lib::Tab::mods);
    for (const std::string_view tag : {std::string_view(), std::string_view("units")}) {
        lib::set_tag(library, tag);
        for (const std::string_view query : {std::string_view(), std::string_view("gamma")}) {
            lib::set_query(library, query);
            OA_CHECK(lib::filter_count(library, lib::Filter::all) == 4);
            OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 0);
            OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 3);
            OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 1);
        }
    }
    lib::set_query(library, "");
    lib::set_tag(library, "");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::installed), "Installed (3)");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::newest), "New (0)");
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::all), "All (6)");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::updates), "Updates (2)");
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::all), "All (1)");
    lib::set_tab(library, lib::Tab::active);
    OA_CHECK(library.visible.empty() && !library.selected);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 0);
    OA_CHECK(lib::tags_in_tab(library).empty());

    lib::set_tab(library, lib::Tab::mods);
    lib::set_filter(library, lib::Filter::all);
    lib::set_tag(library, "units");
    OA_CHECK(visible_keys(library) == Keys({"alpha-mod@fixture-core"}));
    lib::set_tag(library, "balance");
    OA_CHECK(visible_keys(library) == Keys({"beta-mod@fixture-core", "alpha-mod@fixture-core"}));
    lib::set_tag(library, "");
    using Counts = std::vector<std::pair<std::string, std::size_t>>;
    OA_CHECK(
        lib::tags_in_tab(library) == Counts({{"balance", 2}, {"total-conversion", 1}, {"units", 1}})
    );
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(lib::tags_in_tab(library) == Counts({{"balance", 1}, {"total-conversion", 1}}));
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(lib::tags_in_tab(library).empty());

    lib::set_tab(library, lib::Tab::mods);
    lib::set_filter(library, lib::Filter::all);
    const std::string_view text = lib::tab_text(lib::Tab::languages, true);
    OA_CHECK_TEXT(text, "Lang");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::updates, true), "Upd");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::updates, false), "Updates");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::registries, true), "Reg");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::registries, false), "Registries");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::mods, true), "Mods");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::active, true), "Active");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::active, false), "Active");
    OA_CHECK_TEXT(lib::filter_text(lib::Filter::installed), "Installed");
    OA_CHECK_TEXT(lib::filter_text(lib::Filter::newest), "New");
    OA_CHECK_TEXT(lib::state_text(State::playing), "PLAYING");
}

void test_opens_on_active() {
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "alpha-mod", "Alpha Mod"));
    lib::Library library;
    lib::refresh(library, inputs);
    // A Library opens on Active, which lists no entry and selects none.
    OA_CHECK(library.tab == lib::Tab::active);
    OA_CHECK(library.visible.empty() && !library.selected);
    lib::open_details(library);
    OA_CHECK(!library.details_open);
    lib::set_query(library, "alpha");
    OA_CHECK(library.visible.empty());
    lib::set_query(library, "");
    lib::set_tab(library, lib::Tab::mods);
    // The base game entry leads Mods, ahead of the one package.
    OA_CHECK(library.selected == lib::base_game_id() && library.visible.size() == 2);
    // A refresh keeps the tab shown.
    lib::refresh(library, inputs);
    OA_CHECK(library.tab == lib::Tab::mods);
}

void test_new_filter() {
    lib::Inputs inputs = base_inputs();
    // Twelve dated mods, two of them on one day, and one with no day; the
    // undated one was released most often, which says nothing of when.
    const std::vector<std::pair<std::string, std::string>> dated = {
        {"m01", "2026-01-05"},
        {"m02", "2026-02-11"},
        {"m03", "2026-03-02"},
        {"m04", "2026-04-20"},
        {"m05", "2026-05-01"},
        {"m06", "2026-06-15"},
        {"m07", "2026-07-07"},
        {"m08", "2026-08-30"},
        {"zz-tie", "2026-09-09"},
        {"aa-tie", "2026-09-09"},
        {"m11", "2026-10-01"},
        {"m12", "2026-10-11"},
    };
    for (const auto& [key, day] : dated) {
        lib::Listing made = listing(core, Kind::mod, key, key);
        made.published = day;
        inputs.listings.push_back(std::move(made));
    }
    lib::Listing undated = listing(core, Kind::mod, "undated", "Undated");
    undated.release = 90;
    undated.revision = 40;
    inputs.listings.push_back(undated);
    lib::Listing pack = listing(example, Kind::map_pack, "fresh-pack", "Fresh Pack");
    pack.published = "2026-10-12";
    pack.tags = {"desert"};
    inputs.listings.push_back(pack);
    inputs.installed.push_back(installed(core, Kind::mod, "m01", "m01"));
    lib::Library library = built(std::move(inputs));

    using Keys = std::vector<std::string>;
    lib::set_filter(library, lib::Filter::newest);
    // The ten with the latest days, newest first, ties by name.
    const Keys newest_mods = {
        "m12@fixture-core",
        "m11@fixture-core",
        "aa-tie@fixture-core",
        "zz-tie@fixture-core",
        "m08@fixture-core",
        "m07@fixture-core",
        "m06@fixture-core",
        "m05@fixture-core",
        "m04@fixture-core",
        "m03@fixture-core",
    };
    OA_CHECK(visible_keys(library) == newest_mods);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == lib::newest_count);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 13);
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::newest), "New (10)");
    // A search narrows New and leaves its count; an old one is never found there.
    lib::set_query(library, "m0");
    OA_CHECK(
        visible_keys(library) == Keys({
                                     "m08@fixture-core",
                                     "m07@fixture-core",
                                     "m06@fixture-core",
                                     "m05@fixture-core",
                                     "m04@fixture-core",
                                     "m03@fixture-core",
                                 })
    );
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == lib::newest_count);
    lib::set_query(library, "undated");
    OA_CHECK(library.visible.empty());
    lib::set_query(library, "");

    // New on Maps holds maps alone; on Updates, every kind.
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::all);
    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(visible_keys(library) == Keys({"fresh-pack@example-maps"}));
    using Counts = std::vector<std::pair<std::string, std::size_t>>;
    OA_CHECK(lib::tags_in_tab(library) == Counts({{"desert", 1}}));
    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::newest);
    const std::vector<std::string> every_kind = visible_keys(library);
    OA_CHECK(every_kind.size() == lib::newest_count);
    OA_CHECK(!every_kind.empty() && every_kind.front() == "fresh-pack@example-maps");
    OA_CHECK(
        std::find(every_kind.begin(), every_kind.end(), "m03@fixture-core") == every_kind.end()
    );
    // Mods kept its own New.
    lib::set_tab(library, lib::Tab::mods);
    OA_CHECK(visible_keys(library) == newest_mods);
}

void test_active_setup() {
    lib::Inputs inputs = base_inputs();
    lib::Listing ridge = listing(core, Kind::mod, "ridge", "Ridge");
    ridge.badge = lib::Badge{std::vector<uint8_t>(4 * 4 * 4, 0x80), 4, 4};
    inputs.listings.push_back(ridge);
    lib::Installed ridge_installed = installed(core, Kind::mod, "ridge", "Ridge (folder)");
    ridge_installed.version = "4.8";
    ridge_installed.revision = 3;
    ridge_installed.playing = true;
    inputs.installed.push_back(ridge_installed);
    inputs.playing_mod = lib::PlayingMod{"Ridge (settings)", "4.8", std::nullopt};
    inputs.language = {"Deutsch", "Deutsch (Fixture)", "1.2"};
    inputs.map_packs.push_back({"zeta pack", {{"Last Map", 1, "8x8"}}});
    inputs.map_packs.push_back({"Alpha Pack", {{"Ridgeline", 4, "16x16"}, {"Twin", 2, "12x12"}}});
    lib::MapPicture minimap;
    minimap.width = 8;
    minimap.height = 2;
    minimap.rgba.assign(8U * 2U * 4U, 0);
    minimap.rgba[0] = 255;
    minimap.rgba[3] = 255;
    inputs.map_packs[0].maps[0].preview = std::move(minimap);
    inputs.map_packs[0].maps[0].description = "The last island.";
    lib::Installed alpha = installed(core, Kind::map_pack, "alpha", "Alpha Pack");
    alpha.badge = lib::Badge{std::vector<uint8_t>(4, 0xff), 1, 1};
    inputs.installed.push_back(std::move(alpha));
    lib::Library library = built(inputs);

    // The installed mod marked playing comes first, by its listing's name and badge.
    lib::ActiveSetup setup = lib::active_setup(library);
    OA_CHECK_TEXT(setup.mod_name, "Ridge");
    OA_CHECK_TEXT(setup.mod_version, "4.8 · rev 3");
    OA_CHECK_TEXT(setup.mod_release, "4.8 (rev 3)");
    OA_CHECK_TEXT(setup.mod_author, "by Fixture Author");
    OA_CHECK_TEXT(setup.mod_state, "Playing");
    OA_CHECK(setup.base_title.empty());
    OA_CHECK_TEXT(setup.mod_line, "Ridge 4.8 · rev 3");
    OA_CHECK(setup.mod_badge != nullptr && setup.mod_badge->width == 4);
    OA_CHECK_TEXT(setup.language_line, "Deutsch (Fixture) 1.2");
    // Folded title, then folded pack: Last Map, Ridgeline, Twin.
    OA_CHECK(setup.maps.size() == 3);
    if (setup.maps.size() == 3) {
        OA_CHECK_TEXT(setup.maps[0].name, "Last Map");
        OA_CHECK_TEXT(setup.maps[0].pack_name, "zeta pack");
        OA_CHECK_TEXT(setup.maps[0].players, "1 player");
        OA_CHECK_TEXT(setup.maps[0].line, "Last Map · 1 player · 8x8");
        OA_CHECK(setup.maps[0].from.empty());
        OA_CHECK_TEXT(setup.maps[0].description, "The last island.");
        OA_CHECK(setup.maps[0].preview != nullptr && setup.maps[0].preview->width == 8);
        OA_CHECK(setup.maps[0].pack_id.key.empty());
        OA_CHECK_TEXT(setup.maps[1].name, "Ridgeline");
        OA_CHECK_TEXT(setup.maps[1].pack_name, "Alpha Pack");
        OA_CHECK_TEXT(setup.maps[1].players, "4 players");
        OA_CHECK_TEXT(setup.maps[1].line, "Ridgeline · 4 players · 16x16");
        OA_CHECK_TEXT(setup.maps[1].from, "From Alpha Pack");
        OA_CHECK(setup.maps[1].preview == nullptr);
        OA_CHECK(setup.maps[1].pack_id.kind == Kind::map_pack);
        OA_CHECK(setup.maps[1].pack_id.key == "alpha");
        OA_CHECK(setup.maps[1].pack_id.registry == core);
        OA_CHECK_TEXT(setup.maps[2].name, "Twin");
        OA_CHECK_TEXT(setup.maps[2].pack_name, "Alpha Pack");
        OA_CHECK_TEXT(setup.maps[2].from, "From Alpha Pack");
        OA_CHECK(setup.maps[2].pack_id.key == "alpha");
    }

    // Without one, the mod the app says is played; a pack without a version is named alone.
    inputs.installed[0].playing = false;
    inputs.language = {"Deutsch", "Deutsch (Fixture)", ""};
    lib::refresh(library, inputs);
    setup = lib::active_setup(library);
    OA_CHECK_TEXT(setup.mod_line, "Ridge (settings) 4.8");
    OA_CHECK_TEXT(setup.mod_release, "4.8");
    OA_CHECK_TEXT(setup.mod_state, "Playing");
    OA_CHECK(setup.mod_author.empty());
    OA_CHECK(setup.mod_badge == nullptr);
    OA_CHECK_TEXT(setup.language_line, "Deutsch (Fixture)");

    // No mod, a language the game holds itself, English, and no map packs.
    inputs.playing_mod.reset();
    inputs.language = {"Français", "", ""};
    inputs.map_packs.clear();
    lib::refresh(library, inputs);
    setup = lib::active_setup(library);
    OA_CHECK(setup.mod_name.empty() && setup.mod_badge == nullptr);
    library.inputs.base_game_badge = lib::Badge{std::vector<uint8_t>(2 * 2 * 4, 9), 2, 2};
    setup = lib::active_setup(library);
    OA_CHECK(library.inputs.base_game_badge.has_value() && setup.mod_badge == nullptr);
    const lib::ActiveContent face = lib::active_content(library);
    OA_CHECK(face.badge == &*library.inputs.base_game_badge);
    OA_CHECK_TEXT(face.letters, "TA");
    library.inputs.base_game_badge.reset();
    setup = lib::active_setup(library);
    OA_CHECK(setup.mod_badge == nullptr);
    OA_CHECK(lib::active_content(library).badge == nullptr);
    OA_CHECK_TEXT(setup.mod_line, "No Mod");
    OA_CHECK_TEXT(setup.base_title, "No Mod");
    OA_CHECK_TEXT(setup.base_look, "Total Annihilation as it was made");
    OA_CHECK(setup.mod_state.empty() && setup.mod_release.empty() && setup.mod_author.empty());
    OA_CHECK_TEXT(setup.language_name, "Français");
    OA_CHECK_TEXT(setup.language_note, "Built in");
    OA_CHECK(setup.language_english.empty() && setup.language_version.empty());
    OA_CHECK_TEXT(setup.language_line, "Français · built in");
    OA_CHECK(setup.maps.empty());
    inputs.language = {};
    lib::refresh(library, inputs);
    setup = lib::active_setup(library);
    OA_CHECK_TEXT(setup.language_line, "English · built in");
    OA_CHECK_TEXT(setup.language_name, "English");
    OA_CHECK_TEXT(setup.language_note, "Built in");

    // A pack in use names itself, its English name, its revision and its coverage.
    inputs.language.name = "Deutsch";
    inputs.language.pack = "Deutsch (Fixture)";
    inputs.language.version = "1.2";
    inputs.language.english = "German";
    inputs.language.revision = 4;
    lib::Listing deutsch = listing(core, Kind::language, "de", "Deutsch");
    deutsch.coverage = 0.92;
    inputs.listings.push_back(deutsch);
    lib::Installed deutsch_installed = installed(core, Kind::language, "de", "Deutsch (folder)");
    deutsch_installed.version = "1.2";
    deutsch_installed.revision = 4;
    deutsch_installed.in_use = true;
    inputs.installed.push_back(deutsch_installed);
    lib::refresh(library, inputs);
    setup = lib::active_setup(library);
    OA_CHECK_TEXT(setup.language_name, "Deutsch");
    OA_CHECK_TEXT(setup.language_english, "German");
    OA_CHECK_TEXT(setup.language_pack, "Deutsch (Fixture)");
    OA_CHECK_TEXT(setup.language_version, "1.2 (rev 4)");
    OA_CHECK_TEXT(setup.language_coverage, "Coverage 92%");
    OA_CHECK(setup.language_note.empty());
    OA_CHECK(setup.language_badge == nullptr);
    OA_CHECK_TEXT(setup.language_line, "Deutsch (Fixture) 1.2");
}

void test_order_without_search() {
    lib::Inputs inputs = base_inputs();
    lib::Registry other;
    other.id = "another";
    other.name = "Another";
    inputs.registries.push_back(other);
    // Blocked GET, GET, installed, playing and update, named so names alone
    // would order them otherwise. The order is the group, then the name,
    // then the registry. Nothing about how often a package was fetched
    // enters it.
    lib::Listing blocked = listing(core, Kind::mod, "aa-blocked", "Aa Blocked");
    blocked.requires_engine = ">= 1.0.0";
    inputs.listings.push_back(blocked);
    inputs.listings.push_back(listing(core, Kind::mod, "bb-get", "bb Get"));
    inputs.listings.push_back(listing(example, Kind::mod, "same-name", "Same Name"));
    inputs.listings.push_back(listing("another", Kind::mod, "same-name", "Same Name"));
    inputs.listings.push_back(listing(core, Kind::mod, "same-name", "same name"));
    inputs.installed.push_back(installed("", Kind::mod, "cc-own", "Cc Own"));
    inputs.installed.push_back(installed(core, Kind::mod, "dd-playing", "DD Playing"));
    inputs.installed.back().playing = true;
    inputs.listings.push_back(listing(core, Kind::mod, "zz-update", "Zz Update"));
    inputs.installed.push_back(installed(core, Kind::mod, "zz-update", "Zz Update"));
    inputs.updates.push_back(update(core, Kind::mod, "zz-update"));
    const lib::Library library = built(std::move(inputs));
    using Keys = std::vector<std::string>;
    const Keys order = {
        "@base-game@",
        "zz-update@fixture-core",
        "cc-own@",
        "dd-playing@fixture-core",
        "bb-get@fixture-core",
        "same-name@fixture-core",
        "same-name@another",
        "same-name@example-maps",
        "aa-blocked@fixture-core",
    };
    OA_CHECK(visible_keys(library) == order);
}

void test_selection() {
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "alpha-mod", "Alpha Mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "beta-mod", "Beta Mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "gamma-mod", "Gamma Mod"));
    inputs.listings.push_back(listing(core, Kind::map_pack, "example-pack", "Example Pack"));
    lib::Library library = built(inputs);
    const lib::EntryId alpha{std::string(core), Kind::mod, "alpha-mod"};
    const lib::EntryId beta{std::string(core), Kind::mod, "beta-mod"};
    const lib::EntryId gamma{std::string(core), Kind::mod, "gamma-mod"};

    // The first refresh selects the first visible entry, the base game.
    const lib::EntryId base = lib::base_game_id();
    OA_CHECK(library.selected == base && !library.details_open);
    lib::move_selection(library, 1);
    OA_CHECK(library.selected == alpha);
    lib::move_selection(library, 5);
    OA_CHECK(library.selected == gamma);
    lib::move_selection(library, -9);
    OA_CHECK(library.selected == base);
    lib::select(library, beta);
    OA_CHECK(library.selected == beta);
    // An entry that is not visible is not selected.
    lib::select(library, {std::string(core), Kind::map_pack, "example-pack"});
    OA_CHECK(library.selected == beta);
    lib::open_details(library);
    OA_CHECK(library.details_open);
    const lib::Entry* selected = lib::selected_entry(library);
    OA_CHECK(selected != nullptr && lib::entry_name(*selected) == "Beta Mod");

    // A refresh keeps a selection that still shows, with its details.
    inputs.installed.push_back(installed(core, Kind::mod, "beta-mod", "Beta Mod"));
    lib::refresh(library, inputs);
    OA_CHECK(library.selected == beta && library.details_open);
    selected = lib::selected_entry(library);
    OA_CHECK(selected != nullptr && selected->state == State::installed);

    // When it is gone, the first visible entry is selected and the details close.
    inputs.installed.clear();
    inputs.listings.erase(inputs.listings.begin() + 1);
    lib::refresh(library, inputs);
    OA_CHECK(library.selected == base && !library.details_open);

    // A search or tab that hides it moves it too.
    lib::open_details(library);
    lib::set_query(library, "gamma");
    OA_CHECK(library.selected == gamma && !library.details_open);
    lib::set_query(library, "nothing at all");
    OA_CHECK(!library.selected && library.visible.empty());
    lib::open_details(library);
    OA_CHECK(!library.details_open);
    lib::move_selection(library, 1);
    OA_CHECK(!library.selected);
    lib::set_query(library, "");
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK((library.selected == lib::EntryId{std::string(core), Kind::map_pack, "example-pack"}));
    lib::open_details(library);
    lib::close_details(library);
    OA_CHECK(!library.details_open);
}

void test_entry_actions() {
    lib::Inputs inputs = base_inputs();
    lib::Listing get = listing(core, Kind::mod, "get-mod", "Get Mod");
    get.homepage = "https://example.org/get-mod";
    inputs.listings.push_back(get);
    inputs.listings.push_back(listing(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.back().kept = lib::Kept{"4.8", 2};
    inputs.updates.push_back(update(core, Kind::mod, "fixture-mod"));
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    inputs.installed.push_back(installed(core, Kind::map_pack, "example-pack", "Example Pack"));
    inputs.listings.push_back(listing(core, Kind::mod, "failed-mod", "Failed Mod"));
    inputs.queue.push_back(queued(core, "failed-mod", "Failed Mod", Phase::failed));
    inputs.queue.back().problem = "The file's SHA-256 didn't match";
    inputs.listings.push_back(listing(core, Kind::mod, "update-queued", "Update Queued"));
    inputs.installed.push_back(installed(core, Kind::mod, "update-queued", "Update Queued"));
    inputs.updates.push_back(update(core, Kind::mod, "update-queued"));
    inputs.queue.push_back(queued(core, "update-queued", "Update Queued", Phase::downloading));
    const lib::Library library = built(std::move(inputs));

    using Buttons = std::vector<std::pair<Action, bool>>;
    const lib::Entry* get_entry = at(library, core, Kind::mod, "get-mod");
    if (get_entry != nullptr) {
        OA_CHECK(
            actions_of(library, *get_entry) ==
            Buttons({{Action::get, true}, {Action::homepage, true}})
        );
        const std::vector<lib::ActionButton> buttons = lib::entry_actions(library, *get_entry);
        OA_CHECK_TEXT(buttons.front().text, "GET");
        OA_CHECK_TEXT(buttons.back().text, "HOMEPAGE");
    }
    const lib::Entry* update_entry = at(library, core, Kind::mod, "fixture-mod");
    if (update_entry != nullptr) {
        OA_CHECK(
            actions_of(library, *update_entry) == Buttons(
                                                      {{Action::update, true},
                                                       {Action::play_now, true},
                                                       {Action::roll_back, true},
                                                       {Action::uninstall, true},
                                                       {Action::open_folder, true}}
                                                  )
        );
        OA_CHECK_TEXT(lib::entry_actions(library, *update_entry)[2].text, "ROLL BACK");
    }
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    if (own != nullptr)
        OA_CHECK(
            actions_of(library, *own) ==
            Buttons(
                {{Action::play_now, true}, {Action::uninstall, true}, {Action::open_folder, true}}
            )
        );
    // A map pack is never played.
    const lib::Entry* pack = at(library, core, Kind::map_pack, "example-pack");
    if (pack != nullptr)
        OA_CHECK(
            actions_of(library, *pack) ==
            Buttons({{Action::uninstall, true}, {Action::open_folder, true}})
        );
    // A failure that cannot be retried offers GET again.
    const lib::Entry* failed = at(library, core, Kind::mod, "failed-mod");
    if (failed != nullptr) {
        OA_CHECK(actions_of(library, *failed) == Buttons({{Action::get, true}}));
        OA_CHECK_TEXT(lib::status_line(library, *failed), "The file's SHA-256 didn't match");
    }
    const lib::Entry* update_queued = at(library, core, Kind::mod, "update-queued");
    if (update_queued != nullptr)
        OA_CHECK(
            actions_of(library, *update_queued) == Buttons(
                                                       {{Action::cancel, true},
                                                        {Action::play_now, true},
                                                        {Action::uninstall, false},
                                                        {Action::open_folder, true}}
                                                   )
        );
    OA_CHECK_TEXT(lib::action_text(Action::update_all), "UPDATE ALL");
    OA_CHECK_TEXT(lib::action_text(Action::play_now), "PLAY NOW");
    OA_CHECK_TEXT(lib::action_text(Action::switch_off), "SWITCH OFF");
    OA_CHECK_TEXT(lib::action_text(Action::check_now), "Refresh");
    OA_CHECK_TEXT(lib::action_text(Action::settings), "SETTINGS…");
    OA_CHECK_TEXT(lib::action_text(Action::close), "CLOSE");
}

void test_switch_off() {
    // The mod being played offers SWITCH OFF where the others offer PLAY
    // NOW, whatever its registry, and a map pack offers neither.
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.push_back(installed(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.back().playing = true;
    inputs.installed.back().kept = lib::Kept{"4.8", 2};
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    inputs.installed.push_back(installed(core, Kind::map_pack, "example-pack", "Example Pack"));
    lib::Library library = built(inputs);
    using Buttons = std::vector<std::pair<Action, bool>>;
    const lib::EntryId played{std::string(core), Kind::mod, "fixture-mod"};
    const lib::EntryId own{"", Kind::mod, "own-mod"};
    const auto buttons_of = [&library](const lib::EntryId& id) {
        const lib::Entry* entry = lib::find_entry(library, id);
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? actions_of(library, *entry) : Buttons{};
    };
    OA_CHECK(
        buttons_of(played) == Buttons(
                                  {{Action::switch_off, true},
                                   {Action::roll_back, true},
                                   {Action::uninstall, true},
                                   {Action::open_folder, true}}
                              )
    );
    if (const lib::Entry* entry = lib::find_entry(library, played); entry != nullptr)
        OA_CHECK_TEXT(lib::entry_actions(library, *entry)[0].text, "SWITCH OFF");
    OA_CHECK(
        buttons_of(own) ==
        Buttons({{Action::play_now, true}, {Action::uninstall, true}, {Action::open_folder, true}})
    );
    OA_CHECK(
        buttons_of({std::string(core), Kind::map_pack, "example-pack"}) ==
        Buttons({{Action::uninstall, true}, {Action::open_folder, true}})
    );
    lib::select(library, played);
    OA_CHECK_TEXT(lib::playing_note(library), "");

    // While the command line chose the mod, neither can be pressed, and an
    // installed mod's note says why; a map pack's says nothing.
    inputs.mod_from_command_line = true;
    lib::refresh(library, inputs);
    OA_CHECK(
        buttons_of(played) == Buttons(
                                  {{Action::switch_off, false},
                                   {Action::roll_back, true},
                                   {Action::uninstall, true},
                                   {Action::open_folder, true}}
                              )
    );
    OA_CHECK(
        buttons_of(own) ==
        Buttons({{Action::play_now, false}, {Action::uninstall, true}, {Action::open_folder, true}})
    );
    OA_CHECK_TEXT(lib::playing_note(library), "The command line chose this run's mod.");
    lib::select(library, own);
    OA_CHECK_TEXT(lib::playing_note(library), "The command line chose this run's mod.");
    lib::set_tab(library, lib::Tab::maps);
    lib::select(library, {std::string(core), Kind::map_pack, "example-pack"});
    OA_CHECK(library.selected && library.selected->kind == Kind::map_pack);
    OA_CHECK_TEXT(lib::playing_note(library), "");

    // The note on an update of the mod being played follows it.
    inputs.updates.push_back(update(core, Kind::mod, "fixture-mod"));
    lib::set_tab(library, lib::Tab::mods);
    lib::refresh(library, inputs);
    lib::select(library, played);
    OA_CHECK(library.selected == played);
    OA_CHECK_TEXT(
        lib::playing_note(library),
        "The command line chose this run's mod. Finishes when you leave Fixture Mod."
    );
}

void test_own_mod_description() {
    // The details show the listing's summary, else the installed manifest's
    // description, as for a mod of the player's own; a folder with no
    // oamod.yaml says so in its status line, installed or played.
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.listings.back().summary = "The listing's summary.";
    inputs.installed.push_back(installed(core, Kind::mod, "fixture-mod", "Fixture Mod"));
    inputs.installed.back().description = "The manifest's description.";
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    inputs.installed.back().description = "A mod of the player's own.";
    inputs.installed.push_back(installed("", Kind::mod, "plain", "plain"));
    inputs.installed.back().version = "N/A";
    inputs.installed.back().without_manifest = true;
    inputs.installed.push_back(installed("", Kind::mod, "plain-played", "plain-played"));
    inputs.installed.back().without_manifest = true;
    inputs.installed.back().playing = true;
    inputs.installed.push_back(installed("", Kind::mod, "silent", "Silent"));
    const lib::Library library = built(inputs);
    const auto summary_of = [&library](std::string_view registry, std::string_view key) {
        const lib::Entry* entry = at(library, registry, Kind::mod, key);
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? std::string(lib::entry_summary(*entry)) : std::string("?");
    };
    const auto status_of = [&library](std::string_view key) {
        const lib::Entry* entry = at(library, "", Kind::mod, key);
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? lib::status_line(library, *entry) : std::string("?");
    };
    OA_CHECK_TEXT(summary_of(core, "fixture-mod"), "The listing's summary.");
    OA_CHECK_TEXT(summary_of("", "own-mod"), "A mod of the player's own.");
    OA_CHECK_TEXT(summary_of("", "plain"), "");
    OA_CHECK_TEXT(summary_of("", "silent"), "");
    OA_CHECK_TEXT(status_of("own-mod"), "Installed");
    OA_CHECK_TEXT(status_of("plain"), "No oamod.yaml present");
    OA_CHECK_TEXT(status_of("plain-played"), "No oamod.yaml present");

    // A listing with no summary leaves the manifest's description.
    inputs.listings.back().summary.clear();
    const lib::Library unsummed = built(inputs);
    if (const lib::Entry* entry = at(unsummed, core, Kind::mod, "fixture-mod"))
        OA_CHECK_TEXT(lib::entry_summary(*entry), "The manifest's description.");
}

void test_check_now() {
    // The header's refresh icon can be pressed while a registry is on to
    // ask, none is being asked, and no game is being played.
    lib::Inputs inputs = base_inputs();
    OA_CHECK(!lib::can_check_now(built(inputs)));
    inputs.can_check = true;
    OA_CHECK(lib::can_check_now(built(inputs)));
    inputs.checking = true;
    OA_CHECK(!lib::can_check_now(built(inputs)));
    inputs.checking = false;
    inputs.in_match = true;
    OA_CHECK(!lib::can_check_now(built(inputs)));
}

/// Inputs holding the sample mod: 4.8 revision 3 installed, 41.0 MB, revision 4 offered.
///
/// @return the inputs
lib::Inputs sample_inputs() {
    lib::Inputs inputs = base_inputs();
    lib::Listing sample = listing(core, Kind::mod, "fixture-mod", "Fixture Mod");
    sample.version = "4.8";
    sample.revision = 4;
    sample.size = 43011223;
    sample.base = "ta-3.1c";
    sample.requires_engine = ">= 0.8.0";
    sample.game_hacks = 8;
    sample.view_hacks = 4;
    sample.sim_hash = new_rules;
    sample.author = "";
    sample.publisher = "Fixture Publisher";
    sample.summary = "Balance changes for testing.";
    inputs.listings.push_back(sample);
    lib::Installed mod = installed(core, Kind::mod, "fixture-mod", "Fixture Mod");
    mod.version = "4.8";
    mod.revision = 3;
    mod.rules = old_rules;
    mod.kept = lib::Kept{"4.8", 2};
    inputs.installed.push_back(mod);
    lib::UpdateNote note = update(core, Kind::mod, "fixture-mod");
    note.from_rules = old_rules;
    note.to_rules = old_rules;
    note.rules = lib::UpdateNote::Rules::same;
    inputs.updates.push_back(note);
    return inputs;
}

void test_facts() {
    lib::Inputs inputs = sample_inputs();
    lib::Library library = built(inputs);
    const lib::EntryId id{std::string(core), Kind::mod, "fixture-mod"};
    const auto facts_now = [&]() {
        const lib::Entry* entry = lib::find_entry(library, id);
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? lib::facts(library, *entry) : std::vector<lib::Fact>();
    };

    std::vector<lib::Fact> list = facts_now();
    std::vector<std::string> labels;
    for (const lib::Fact& item : list)
        labels.push_back(item.long_label);
    OA_CHECK(
        labels ==
        std::vector<std::string>(
            {"Installed", "Latest", "Author", "Size", "Base", "Needs", "Hacks", "Rules", "From"}
        )
    );
    if (const lib::Fact* installed_fact = fact(list, "Installed")) {
        OA_CHECK_TEXT(installed_fact->long_value, "4.8 (rev 3)");
        OA_CHECK_TEXT(installed_fact->short_value, "4.8 (rev 3)");
        OA_CHECK(installed_fact->mark == Mark::none);
    }
    if (const lib::Fact* latest = fact(list, "Latest")) {
        OA_CHECK_TEXT(latest->long_value, "4.8 (rev 4)");
        OA_CHECK_TEXT(latest->short_value, "4.8 (rev 4)");
    }
    // The listing names no author: its publisher stands in.
    if (const lib::Fact* author = fact(list, "Author"))
        OA_CHECK_TEXT(author->long_value, "Fixture Publisher");
    if (const lib::Fact* size = fact(list, "Size")) {
        OA_CHECK_TEXT(size->long_value, "41.0 MB · 3.2 MB to download");
        OA_CHECK_TEXT(size->short_value, "3.2 MB to download");
    }
    if (const lib::Fact* base = fact(list, "Base")) {
        OA_CHECK_TEXT(base->long_value, "Total Annihilation 3.1c");
        OA_CHECK_TEXT(base->short_value, "TA 3.1c");
    }
    if (const lib::Fact* needs = fact(list, "Needs")) {
        OA_CHECK_TEXT(needs->long_value, "Open Annihilation 0.8.0 or later");
        OA_CHECK_TEXT(needs->short_value, "OA 0.8.0+");
        OA_CHECK(needs->mark == Mark::good);
    }
    if (const lib::Fact* hacks = fact(list, "Hacks")) {
        OA_CHECK_TEXT(hacks->long_value, "8 change the game · 4 view only");
        OA_CHECK_TEXT(hacks->short_value, "8 game · 4 view");
        OA_CHECK(hacks->mark == Mark::none);
    }
    if (const lib::Fact* rules = fact(list, "Rules")) {
        OA_CHECK_TEXT(rules->long_value, "9c1f 4e0b → 9c1f 4e0b same");
        OA_CHECK_TEXT(rules->short_value, "unchanged");
        OA_CHECK(rules->mark == Mark::none);
    }
    if (const lib::Fact* from = fact(list, "From")) {
        OA_CHECK_TEXT(from->long_value, "Fixture Core");
        OA_CHECK(from->mark == Mark::none);
    }

    // An update that changes the rules says so, and which saved games stop loading.
    inputs.updates[0].to_rules = new_rules;
    inputs.updates[0].rules = lib::UpdateNote::Rules::changes;
    lib::refresh(library, inputs);
    list = facts_now();
    if (const lib::Fact* rules = fact(list, "Rules")) {
        OA_CHECK_TEXT(
            rules->long_value,
            "9c1f 4e0b → 51d2 07aa · changes the game's rules; saved games from revision 3 "
            "won't load under revision 4"
        );
        OA_CHECK_TEXT(rules->short_value, "changes · rev 3 saves won't load");
        OA_CHECK(rules->mark == Mark::warn);
    }

    // Unknown when a hash is missing.
    inputs.updates[0].to_rules.reset();
    inputs.updates[0].rules = lib::UpdateNote::Rules::unknown;
    lib::refresh(library, inputs);
    list = facts_now();
    if (const lib::Fact* rules = fact(list, "Rules")) {
        OA_CHECK_TEXT(
            rules->long_value, "unknown · saved games from revision 3 may not load under revision 4"
        );
        OA_CHECK_TEXT(rules->short_value, "unknown · saves may not load");
        OA_CHECK(rules->mark == Mark::warn);
    }

    // A new version names the versions instead.
    inputs.updates[0].to_version = "4.9";
    inputs.updates[0].to_rules = new_rules;
    inputs.updates[0].rules = lib::UpdateNote::Rules::changes;
    lib::refresh(library, inputs);
    list = facts_now();
    if (const lib::Fact* rules = fact(list, "Rules")) {
        OA_CHECK_TEXT(
            rules->long_value,
            "9c1f 4e0b → 51d2 07aa · changes the game's rules; saved games from 4.8 won't load "
            "under 4.9"
        );
        OA_CHECK_TEXT(rules->short_value, "changes · 4.8 saves won't load");
    }

    // An engine that does not meet the requirement, missing hacks and an added registry.
    inputs = sample_inputs();
    inputs.engine = {0, 7, 3};
    inputs.listings[0].registry = std::string(example);
    inputs.listings[0].hack_ids = {std::string(implemented_hack()), "fixture.missing-one"};
    inputs.installed.clear();
    inputs.updates.clear();
    lib::refresh(library, inputs);
    const lib::Entry* added = at(library, example, Kind::mod, "fixture-mod");
    OA_CHECK(added != nullptr);
    if (added != nullptr) {
        list = lib::facts(library, *added);
        if (const lib::Fact* size = fact(list, "Size"))
            OA_CHECK_TEXT(size->long_value, "41.0 MB");
        if (const lib::Fact* needs = fact(list, "Needs"))
            OA_CHECK(needs->mark == Mark::warn);
        if (const lib::Fact* hacks = fact(list, "Hacks")) {
            OA_CHECK_TEXT(
                hacks->long_value,
                "8 change the game · 4 view only · Needs hacks this OA lacks: fixture.missing-one"
            );
            OA_CHECK_TEXT(hacks->short_value, "8 game · 4 view · 1 missing");
            OA_CHECK(hacks->mark == Mark::warn);
        }
        if (const lib::Fact* rules = fact(list, "Rules"))
            OA_CHECK_TEXT(rules->long_value, "51d2 07aa");
        if (const lib::Fact* from = fact(list, "From")) {
            OA_CHECK_TEXT(from->long_value, "Example Maps · Not reviewed by the OA team");
            OA_CHECK(from->mark == Mark::warn);
        }
    }

    // A map pack's maps, a language's coverage, and the player's own package.
    inputs = base_inputs();
    lib::Listing pack = listing(example, Kind::map_pack, "example-pack", "Example Pack");
    pack.version = "1.2";
    pack.maps = {{"First Map", 4, "16x16"}, {"Second Map", 2, "8x8"}};
    inputs.listings.push_back(pack);
    lib::Listing language = listing(example, Kind::language, "xx", "Fixture Language");
    language.coverage = 0.92;
    inputs.listings.push_back(language);
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    inputs.installed.back().rules = old_rules;
    lib::refresh(library, inputs);
    if (const lib::Entry* entry = at(library, example, Kind::map_pack, "example-pack")) {
        list = lib::facts(library, *entry);
        if (const lib::Fact* maps = fact(list, "Maps"))
            OA_CHECK_TEXT(maps->long_value, "2 maps");
        OA_CHECK(fact(list, "Hacks") == nullptr && fact(list, "Rules") == nullptr);
        OA_CHECK(fact(list, "Base") == nullptr);
        OA_CHECK_TEXT(lib::version_text(*entry->listing), "1.2 · 2 maps");
    }
    if (const lib::Entry* entry = at(library, example, Kind::language, "xx")) {
        list = lib::facts(library, *entry);
        if (const lib::Fact* coverage = fact(list, "Coverage"))
            OA_CHECK_TEXT(coverage->long_value, "92%");
        OA_CHECK(fact(list, "Coverage") != nullptr);
    }
    if (const lib::Entry* entry = at(library, "", Kind::mod, "own-mod")) {
        list = lib::facts(library, *entry);
        OA_CHECK(list.size() == 3);
        if (const lib::Fact* installed_fact = fact(list, "Installed"))
            OA_CHECK_TEXT(installed_fact->long_value, "1.0 (rev 1)");
        OA_CHECK(fact(list, "Latest") == nullptr && fact(list, "Author") == nullptr);
        if (const lib::Fact* rules = fact(list, "Rules"))
            OA_CHECK_TEXT(rules->long_value, "9c1f 4e0b");
        if (const lib::Fact* from = fact(list, "From"))
            OA_CHECK_TEXT(from->long_value, "Your own copy");
    }

    // A revision of 0 is left out of its fact, an empty version leaves the
    // revision alone, a side with neither is left out, and the installed
    // manifest's author stands in for a listing that names none.
    inputs = base_inputs();
    lib::Listing pack_r0 = listing(core, Kind::map_pack, "pack-r0", "Pack R0");
    pack_r0.revision = 0;
    pack_r0.author.clear();
    inputs.listings.push_back(pack_r0);
    lib::Installed pack_installed = installed(core, Kind::map_pack, "pack-r0", "Pack R0");
    pack_installed.version.clear();
    pack_installed.revision = 3;
    pack_installed.author = "The Pack Author";
    inputs.installed.push_back(pack_installed);
    lib::Listing language_none = listing(core, Kind::language, "xx", "No Version");
    language_none.version.clear();
    language_none.revision = 0;
    language_none.author.clear();
    inputs.listings.push_back(language_none);
    lib::refresh(library, inputs);
    if (const lib::Entry* entry = at(library, core, Kind::map_pack, "pack-r0")) {
        list = lib::facts(library, *entry);
        if (const lib::Fact* installed_fact = fact(list, "Installed"))
            OA_CHECK_TEXT(installed_fact->long_value, "rev 3");
        if (const lib::Fact* latest = fact(list, "Latest"))
            OA_CHECK_TEXT(latest->long_value, "1.0");
        if (const lib::Fact* author = fact(list, "Author"))
            OA_CHECK_TEXT(author->long_value, "The Pack Author");
        OA_CHECK(fact(list, "Installed") != nullptr && fact(list, "Latest") != nullptr);
        OA_CHECK(fact(list, "Author") != nullptr);
    }
    if (const lib::Entry* entry = at(library, core, Kind::language, "xx")) {
        list = lib::facts(library, *entry);
        OA_CHECK(fact(list, "Installed") == nullptr && fact(list, "Latest") == nullptr);
        OA_CHECK(fact(list, "Author") == nullptr);
        OA_CHECK(!list.empty() && list.front().long_label == "Size");
    }
}

void test_status_lines_and_bylines() {
    lib::Inputs inputs = sample_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "get-mod", "Get Mod"));
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod"));
    lib::Library library = built(inputs);
    const lib::Entry* entry = at(library, core, Kind::mod, "fixture-mod");
    if (entry != nullptr) {
        OA_CHECK_TEXT(
            lib::status_line(library, *entry), "Revision 4 available · 3.2 MB to download"
        );
        OA_CHECK_TEXT(lib::byline(*entry), "by Fixture Publisher");
    }
    const lib::Entry* get = at(library, core, Kind::mod, "get-mod");
    if (get != nullptr) {
        OA_CHECK_TEXT(lib::status_line(library, *get), "Not installed · 64.0 MB");
        OA_CHECK_TEXT(lib::byline(*get), "by Fixture Author");
    }
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    if (own != nullptr)
        OA_CHECK_TEXT(lib::byline(*own), "");

    lib::Listing named = listing(core, Kind::mod, "named", "Named");
    named.author = "Fixture Author";
    inputs.listings.push_back(named);
    lib::Listing unnamed = listing(core, Kind::mod, "unnamed", "Unnamed");
    unnamed.author.clear();
    unnamed.publisher.clear();
    inputs.listings.push_back(unnamed);
    lib::refresh(library, inputs);
    if (const lib::Entry* with_maker = at(library, core, Kind::mod, "named"))
        OA_CHECK_TEXT(lib::byline(*with_maker), "by Fixture Author");
    if (const lib::Entry* no_maker = at(library, core, Kind::mod, "unnamed"))
        OA_CHECK_TEXT(lib::byline(*no_maker), "");

    inputs.updates[0].to_version = "4.9";
    lib::refresh(library, inputs);
    entry = at(library, core, Kind::mod, "fixture-mod");
    if (entry != nullptr)
        OA_CHECK_TEXT(
            lib::status_line(library, *entry), "Version 4.9 available · 3.2 MB to download"
        );

    // No revision named, the version the same or none: an update, never "Revision 0".
    inputs.updates[0].to_version = "4.8";
    inputs.updates[0].to_revision = 0;
    lib::refresh(library, inputs);
    entry = at(library, core, Kind::mod, "fixture-mod");
    if (entry != nullptr)
        OA_CHECK_TEXT(lib::status_line(library, *entry), "Update available · 3.2 MB to download");
    inputs.updates[0].to_version.clear();
    lib::refresh(library, inputs);
    entry = at(library, core, Kind::mod, "fixture-mod");
    if (entry != nullptr)
        OA_CHECK_TEXT(lib::status_line(library, *entry), "Update available · 3.2 MB to download");
}

void test_playing_note() {
    lib::Inputs inputs = sample_inputs();
    inputs.installed[0].playing = true;
    lib::Library library = built(inputs);
    const lib::EntryId fixture{std::string(core), Kind::mod, "fixture-mod"};
    lib::select(library, fixture);
    OA_CHECK_TEXT(lib::playing_note(library), "Finishes when you leave Fixture Mod.");

    // A held update of the mod being played says the same.
    inputs.updates.clear();
    inputs.queue.push_back(queued(core, "fixture-mod", "Fixture Mod", Phase::held));
    lib::refresh(library, inputs);
    OA_CHECK(library.selected == fixture);
    OA_CHECK_TEXT(lib::playing_note(library), "Finishes when you leave Fixture Mod.");
    inputs.queue.clear();
    lib::refresh(library, inputs);
    OA_CHECK_TEXT(lib::playing_note(library), "");

    // Another mod is played.
    inputs = sample_inputs();
    inputs.installed.push_back(installed(core, Kind::mod, "second-mod", "Second Mod"));
    inputs.installed.back().playing = true;
    lib::refresh(library, inputs);
    lib::select(library, fixture);
    OA_CHECK(library.selected == fixture);
    OA_CHECK_TEXT(
        lib::playing_note(library),
        "You are playing Second Mod. Updating Fixture Mod doesn't restart the game."
    );
    lib::select(library, {std::string(core), Kind::mod, "second-mod"});
    OA_CHECK_TEXT(lib::playing_note(library), "");
}

void test_queue_lines() {
    lib::Inputs inputs = base_inputs();
    OA_CHECK(lib::queue_lines(built(inputs)).first.empty());

    lib::QueueItem done = queued(core, "done-mod", "Done Mod", Phase::installed);
    lib::QueueItem downloading = queued(core, "third-mod", "Third Mod", Phase::downloading);
    downloading.version = "3.1";
    downloading.done_bytes = 19083674; // 18.2 MiB
    downloading.total_bytes = 64 * mib;
    lib::QueueItem checking = queued(core, "second-mod", "Second Mod", Phase::checking);
    checking.version = "10.2.0";
    // The download comes first, though an item checked is earlier in the queue.
    inputs.queue = {done, checking, downloading};
    lib::QueueLines lines = lib::queue_lines(built(inputs));
    OA_CHECK_TEXT(lines.first, "Downloading Third Mod 3.1 · 18.2 of 64.0 MB");
    OA_CHECK_TEXT(lines.second, "Then Second Mod 10.2.0 · checking");
    OA_CHECK_TEXT(lines.compact, "Third Mod 28%");

    // With nothing downloading, the first item at work.
    inputs.queue = {queued(core, "a", "Waiting One", Phase::waiting), checking};
    lines = lib::queue_lines(built(inputs));
    OA_CHECK_TEXT(lines.first, "Checking Second Mod");
    OA_CHECK_TEXT(lines.second, "Then Waiting One 1.0 · waiting");
    OA_CHECK_TEXT(lines.compact, "Second Mod · checking");

    inputs.queue = {
        done,
        downloading,
        checking,
        queued(core, "a", "Waiting One", Phase::waiting),
        queued(core, "b", "Waiting Two", Phase::waiting)
    };
    lines = lib::queue_lines(built(inputs));
    OA_CHECK_TEXT(lines.first, "Downloading Third Mod 3.1 · 18.2 of 64.0 MB");
    OA_CHECK_TEXT(lines.second, "Then Second Mod 10.2.0 · checking · 2 more");
    OA_CHECK_TEXT(lines.compact, "Third Mod 28%");

    const auto first_line = [&](lib::QueueItem item) {
        lib::Inputs one = base_inputs();
        one.queue = {std::move(item)};
        return lib::queue_lines(built(std::move(one)));
    };
    OA_CHECK_TEXT(
        first_line(queued(core, "m", "Fixture Mod", Phase::verifying)).first,
        "Verifying Fixture Mod"
    );
    OA_CHECK_TEXT(
        first_line(queued(core, "m", "Fixture Mod", Phase::installing)).first,
        "Installing Fixture Mod"
    );
    OA_CHECK_TEXT(
        first_line(queued(core, "m", "Fixture Mod", Phase::waiting)).first,
        "Waiting to download Fixture Mod 1.0"
    );
    lines = first_line(queued(core, "m", "Fixture Mod", Phase::held));
    OA_CHECK_TEXT(lines.first, "Waiting · finishes when you leave Fixture Mod");
    OA_CHECK_TEXT(lines.compact, "Fixture Mod · after you leave it");
    lib::QueueItem failed = queued(example, "m", "Example Pack", Phase::failed);
    failed.problem = "Couldn't reach Example Maps";
    failed.retryable = true;
    lines = first_line(failed);
    OA_CHECK_TEXT(lines.first, "Couldn't reach Example Maps · RETRY");
    OA_CHECK_TEXT(lines.compact, "Example Pack · failed");
    failed.retryable = false;
    OA_CHECK_TEXT(first_line(failed).first, "Couldn't reach Example Maps");

    // After an update, the queue says whether it changed the game's rules.
    lib::QueueItem updated = queued(core, "m", "Fixture Mod", Phase::installed);
    updated.update_rules = lib::UpdateNote::Rules::changes;
    updated.update_from = "revision 3";
    updated.update_to = "revision 4";
    lines = first_line(updated);
    OA_CHECK_TEXT(
        lines.first,
        "Updated Fixture Mod · revision 4 changes the game's rules · ROLL BACK restores revision 3"
    );
    OA_CHECK_TEXT(lines.compact, "Fixture Mod · rules changed");
    updated.update_rules = lib::UpdateNote::Rules::unknown;
    updated.update_from = "4.8";
    updated.update_to = "4.9";
    lines = first_line(updated);
    OA_CHECK_TEXT(
        lines.first,
        "Updated Fixture Mod · 4.9 may change the game's rules · ROLL BACK restores 4.8"
    );
    OA_CHECK_TEXT(lines.compact, "Fixture Mod · rules changed");
    updated.update_rules = lib::UpdateNote::Rules::same;
    lines = first_line(updated);
    OA_CHECK_TEXT(lines.first, "Installed Fixture Mod");
    OA_CHECK_TEXT(lines.compact, "Fixture Mod · installed");
    OA_CHECK(lines.second.empty());

    // A failure comes before items that wait, and the last item installed comes last.
    inputs = base_inputs();
    inputs.queue = {queued(core, "a", "Waiting One", Phase::waiting), failed, done};
    lines = lib::queue_lines(built(inputs));
    OA_CHECK_TEXT(lines.first, "Couldn't reach Example Maps");
    OA_CHECK_TEXT(lines.second, "Then Waiting One 1.0 · waiting");
    inputs.queue = {done, queued(core, "b", "Later Mod", Phase::installed)};
    OA_CHECK_TEXT(lib::queue_lines(built(inputs)).first, "Installed Later Mod");
}

void test_change_texts() {
    lib::Installed mod = installed(core, Kind::mod, "fixture-mod", "Fixture Mod");
    mod.version = "4.8";
    mod.revision = 3;
    lib::UpdateNote note = update(core, Kind::mod, "fixture-mod");
    lib::ChangeTexts names = lib::change_texts(mod, note);
    OA_CHECK_TEXT(names.from, "revision 3");
    OA_CHECK_TEXT(names.to, "revision 4");
    OA_CHECK_TEXT(names.from_short, "rev 3");
    OA_CHECK_TEXT(names.to_short, "rev 4");
    note.to_version = "4.9";
    note.to_revision = 1;
    names = lib::change_texts(mod, note);
    OA_CHECK_TEXT(names.from, "4.8");
    OA_CHECK_TEXT(names.to, "4.9");
    OA_CHECK_TEXT(names.from_short, "4.8");
    OA_CHECK_TEXT(names.to_short, "4.9");

    // A side whose revision is 0 where its revision would name it is the
    // old or the new copy, never "revision 0"; so is a side with no version
    // where its version would, unless a revision names it.
    note.to_version = "4.8";
    note.to_revision = 0;
    names = lib::change_texts(mod, note);
    OA_CHECK_TEXT(names.from, "revision 3");
    OA_CHECK_TEXT(names.to, "the new copy");
    OA_CHECK_TEXT(names.from_short, "rev 3");
    OA_CHECK_TEXT(names.to_short, "new");
    mod.revision = 0;
    names = lib::change_texts(mod, note);
    OA_CHECK_TEXT(names.from, "the old copy");
    OA_CHECK_TEXT(names.from_short, "old");
    mod.version.clear();
    mod.revision = 2;
    note.to_version = "5.0";
    names = lib::change_texts(mod, note);
    OA_CHECK_TEXT(names.from, "revision 2");
    OA_CHECK_TEXT(names.to, "5.0");
}

void test_texts() {
    OA_CHECK_TEXT(lib::size_text(0), "1 KB");
    OA_CHECK_TEXT(lib::size_text(1), "1 KB");
    OA_CHECK_TEXT(lib::size_text(1535), "1 KB");
    OA_CHECK_TEXT(lib::size_text(1536), "2 KB");
    OA_CHECK_TEXT(lib::size_text(mib - 1), "1024 KB");
    OA_CHECK_TEXT(lib::size_text(mib), "1.0 MB");
    OA_CHECK_TEXT(lib::size_text(1101004), "1.0 MB");
    OA_CHECK_TEXT(lib::size_text(1101005), "1.1 MB");
    OA_CHECK_TEXT(lib::size_text(43011223), "41.0 MB");
    OA_CHECK_TEXT(lib::size_text(100 * mib - 1), "100.0 MB");
    OA_CHECK_TEXT(lib::size_text(100 * mib), "100 MB");
    OA_CHECK_TEXT(lib::size_text(197132288), "188 MB");
    OA_CHECK_TEXT(lib::size_text(1024 * mib - 1), "1024 MB");
    OA_CHECK_TEXT(lib::size_text(1024 * mib), "1.0 GB");
    OA_CHECK_TEXT(lib::size_text(1536 * mib), "1.5 GB");
    OA_CHECK_TEXT(lib::progress_text(19083674, 64 * mib), "18.2 of 64.0 MB");
    OA_CHECK_TEXT(lib::progress_text(512, 900 * 1024), "1 of 900 KB");
    OA_CHECK_TEXT(lib::progress_text(50 * mib, 188 * mib), "50 of 188 MB");

    OA_CHECK_TEXT(lib::age_text(-5), "just now");
    OA_CHECK_TEXT(lib::age_text(59), "just now");
    OA_CHECK_TEXT(lib::age_text(60), "1 min ago");
    OA_CHECK_TEXT(lib::age_text(240), "4 min ago");
    OA_CHECK_TEXT(lib::age_text(3599), "59 min ago");
    OA_CHECK_TEXT(lib::age_text(3600), "1 h ago");
    OA_CHECK_TEXT(lib::age_text(172799), "47 h ago");
    OA_CHECK_TEXT(lib::age_text(172800), "2 days ago");

    OA_CHECK_TEXT(lib::rules_text(old_rules), "9c1f 4e0b");
    OA_CHECK_TEXT(lib::rules_text(new_rules), "51d2 07aa");

    lib::Listing sample = listing(core, Kind::mod, "fixture-mod", "Fixture Mod");
    sample.version = "4.8";
    sample.revision = 3;
    OA_CHECK_TEXT(lib::version_text(sample), "4.8 · rev 3");
    sample.revision = 1;
    OA_CHECK_TEXT(lib::version_text(sample), "4.8");
    lib::Installed mod = installed(core, Kind::mod, "fixture-mod", "Fixture Mod");
    mod.version = "4.8";
    mod.revision = 3;
    OA_CHECK_TEXT(lib::version_text(mod), "4.8 · rev 3");
    mod.revision = 0;
    OA_CHECK_TEXT(lib::version_text(mod), "4.8");
    lib::Listing pack = listing(core, Kind::map_pack, "example-pack", "Example Pack");
    pack.version = "1.2";
    pack.maps = {{"Only Map", 2, "8x8"}};
    OA_CHECK_TEXT(lib::version_text(pack), "1.2 · 1 map");

    OA_CHECK_TEXT(lib::release_text("4.8", 3), "4.8 (rev 3)");
    OA_CHECK_TEXT(lib::release_text("4.8", 1), "4.8 (rev 1)");
    OA_CHECK_TEXT(lib::release_text("1.0", 0), "1.0");
    OA_CHECK_TEXT(lib::release_text("", 3), "rev 3");
    OA_CHECK_TEXT(lib::release_text("", 0), "");

    const auto range = [](std::string_view written) {
        return oa::formats::oamod::parse_engine_range(written).value_or(
            oa::formats::oamod::EngineRange{}
        );
    };
    OA_CHECK_TEXT(lib::engine_text(range(">= 0.8.0")), "0.8.0 or later");
    OA_CHECK_TEXT(lib::engine_short_text(range(">= 0.8.0")), "0.8.0+");
    OA_CHECK_TEXT(lib::engine_text(range(">= 0.8.0, < 0.9.0")), "0.8.0 or later, before 0.9.0");
    OA_CHECK_TEXT(lib::engine_short_text(range(">= 0.8.0, < 0.9.0")), "0.8.0+, before 0.9.0");
    OA_CHECK_TEXT(
        lib::engine_text(range("> 0.8.0, <= 1.0.0")), "later than 0.8.0, 1.0.0 or earlier"
    );
    OA_CHECK_TEXT(lib::engine_short_text(range("> 0.8.0, <= 1.0.0")), "after 0.8.0, up to 1.0.0");
    OA_CHECK_TEXT(lib::engine_text(range("= 0.8.1")), "exactly 0.8.1");
    OA_CHECK_TEXT(lib::engine_short_text(range("= 0.8.1")), "0.8.1 only");

    // A place no value names is kept as written.
    OA_CHECK_TEXT(
        lib::filled({}, "{name} and {other}", {{"name", "Fixture"}}), "Fixture and {other}"
    );
}

/// The Registries tab's initials, ages, refresh line, subtitle and note.
void test_registry_texts() {
    OA_CHECK_TEXT(lib::registry_initials("Core Prime"), "CP");
    OA_CHECK_TEXT(lib::registry_initials("Example Maps"), "EM");
    OA_CHECK_TEXT(lib::registry_initials("Kirby's laptop"), "KL");
    OA_CHECK_TEXT(lib::registry_initials("ridge"), "RI");
    OA_CHECK_TEXT(lib::registry_initials("North archive of maps"), "NAO");
    OA_CHECK_TEXT(lib::registry_initials("  spaced  out  "), "SO");
    OA_CHECK_TEXT(lib::registry_initials("简体中文"), "简体");
    OA_CHECK_TEXT(lib::registry_initials(""), "");

    OA_CHECK_TEXT(lib::registry_age_text(10), "just now");
    OA_CHECK_TEXT(lib::registry_age_text(4 * 60), "4 min ago");
    OA_CHECK_TEXT(lib::registry_age_text(3 * 3600 + 5), "3 h ago");
    OA_CHECK_TEXT(lib::registry_age_text(30 * 3600), "1 day ago");
    OA_CHECK_TEXT(lib::registry_age_text(5 * 86400), "5 days ago");

    lib::Registry built;
    built.name = "Core Prime";
    built.built_in = true;
    built.packages = 218;
    built.checked_ago = 4 * 60;
    OA_CHECK_TEXT(lib::registry_subtitle(built), "218 packages");
    OA_CHECK_TEXT(lib::registry_refresh_line(built), "refreshed 4 min ago");
    built.checked_ago.reset();
    OA_CHECK_TEXT(lib::registry_subtitle(built), "218 packages");
    OA_CHECK_TEXT(lib::registry_refresh_line(built), "never refreshed");
    built.checked_ago = 2 * 3600;
    built.unreachable = true;
    OA_CHECK_TEXT(lib::registry_subtitle(built), "218 packages");
    OA_CHECK_TEXT(lib::registry_refresh_line(built), "couldn't reach · 2 h ago");
    built.checked_ago.reset();
    OA_CHECK_TEXT(lib::registry_refresh_line(built), "couldn't reach");
    built.checked_ago = 2 * 86400;
    OA_CHECK_TEXT(lib::registry_refresh_line(built), "couldn't reach · 2 days ago");

    lib::Registry added;
    added.address = "maps.example.org";
    added.packages = 31;
    OA_CHECK_TEXT(lib::registry_subtitle(added), "31 packages · not reviewed");
    OA_CHECK_TEXT(lib::registry_refresh_line(added), "never refreshed");
    added.mirrors = 2;
    OA_CHECK_TEXT(lib::registry_subtitle(added), "31 packages · not reviewed · 2 mirrors");
    added.mirrors = 1;
    added.unreachable = true;
    added.checked_ago = 60;
    OA_CHECK_TEXT(lib::registry_subtitle(added), "31 packages · not reviewed · 1 mirror");
    OA_CHECK_TEXT(lib::registry_refresh_line(added), "couldn't reach · 1 min ago");
    added = {};
    added.address = "192.168.1.20";
    added.packages = 1;
    OA_CHECK_TEXT(lib::registry_subtitle(added), "1 package · not reviewed");

    lib::Registry core_prime;
    core_prime.name = "Core Prime";
    core_prime.built_in = true;
    lib::Registry north;
    north.name = "North";
    north.built_in = true;
    lib::Registry west;
    west.name = "West";
    west.built_in = true;
    constexpr std::string_view tail =
        " isn't reviewed by the OA team. OA still checks every package before using it.";
    OA_CHECK_TEXT(
        lib::registry_note({core_prime}),
        std::string("Content from registries other than Core Prime") + std::string(tail)
    );
    OA_CHECK_TEXT(
        lib::registry_note({core_prime, north}),
        std::string("Content from registries other than Core Prime and North") + std::string(tail)
    );
    OA_CHECK_TEXT(
        lib::registry_note({core_prime, north, west}),
        std::string("Content from registries other than Core Prime, North and West") +
            std::string(tail)
    );
    lib::Registry only_added;
    only_added.name = "Example";
    OA_CHECK_TEXT(
        lib::registry_note({only_added}),
        "Content from these registries isn't reviewed by the OA team. OA still checks "
        "every package before using it."
    );
}

/// A made-up language for the lookup: every known pattern turned around.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view reversed_words(void*, std::string_view english) {
    if (english == lib::words::queue_installed)
        return "{name} ist installiert";
    if (english == lib::words::size_mb)
        return "{size} Mo";
    if (english == lib::words::one_decimal)
        return "{whole},{tenth}";
    if (english == lib::words::action_get)
        return "HOLEN";
    return english;
}

/// A made-up lookup that translates the Economy tag and nothing else.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view economy_in_german(void*, std::string_view english) {
    if (english == lib::words::tag_economy)
        return "Wirtschaft";
    return english;
}

/// Tells whether the visible list holds a key.
///
/// @param library the Library
/// @param key the key, as visible_keys writes it
/// @return true when it shows
bool shows_key(const lib::Library& library, std::string_view key) {
    const std::vector<std::string> keys = visible_keys(library);
    return std::find(keys.begin(), keys.end(), key) != keys.end();
}

/// The base game entry's name and summary in another language.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view translated_base_game(void*, std::string_view english) {
    if (english == lib::words::no_mod)
        return "Jeu de base";
    if (english == lib::words::active_as_made)
        return "Sans mod";
    return english;
}

void test_base_game() {
    using Keys = std::vector<std::string>;
    using Buttons = std::vector<std::pair<Action, bool>>;

    // No package: it is first, marked PLAYING, and offers no action.
    const lib::Library none = built({});
    OA_CHECK(visible_keys(none) == Keys({"@base-game@"}));
    OA_CHECK(lib::filter_count(none, lib::Filter::all) == 0);
    OA_CHECK(lib::filter_count(none, lib::Filter::installed) == 0);
    OA_CHECK(lib::filter_count(none, lib::Filter::newest) == 0);
    OA_CHECK(lib::filter_count(none, lib::Filter::updates) == 0);
    OA_CHECK(lib::update_count(none) == 0);
    const lib::Entry* entry = lib::find_entry(none, lib::base_game_id());
    OA_CHECK(entry != nullptr && entry->state == State::playing);
    OA_CHECK(entry != nullptr && entry->listing == nullptr && entry->installed == nullptr);
    OA_CHECK(entry != nullptr && entry->update == nullptr && entry->queued == nullptr);
    OA_CHECK(entry != nullptr && !entry->built_in && !entry->not_reviewed);
    OA_CHECK(entry != nullptr && entry->registry_name.empty() && entry->badge == nullptr);
    if (entry != nullptr) {
        OA_CHECK(actions_of(none, *entry).empty());
        OA_CHECK(lib::facts(none, *entry).empty());
        OA_CHECK_TEXT(lib::entry_name(*entry), "No Mod");
        OA_CHECK_TEXT(lib::entry_summary(*entry), "Total Annihilation as it was made");
        OA_CHECK_TEXT(lib::status_line(none, *entry), "Playing");
    }
    OA_CHECK_TEXT(lib::playing_note(none), "");

    // The badge the app supplies is the one the row shows.
    lib::Inputs with_badge;
    with_badge.base_game_badge = lib::Badge{{1, 2, 3, 4}, 1, 1};
    lib::Library badged;
    lib::refresh(badged, std::move(with_badge));
    const lib::Entry* marked = lib::find_entry(badged, lib::base_game_id());
    OA_CHECK(marked != nullptr && marked->badge != nullptr && marked->badge->width == 1);
    OA_CHECK(marked != nullptr && marked->badge != nullptr && marked->badge->height == 1);
    if (marked != nullptr && marked->badge != nullptr && marked->badge->rgba.size() == 4)
        OA_CHECK(marked->badge->rgba[0] == 1 && marked->badge->rgba[3] == 4);

    // A mod played: INSTALLED, and PLAY NOW alone, on the same rule as SWITCH OFF.
    lib::Inputs inputs = base_inputs();
    inputs.installed.push_back(installed(core, Kind::mod, "ridge", "Ridge"));
    inputs.installed.back().playing = true;
    lib::Library library = built(inputs);
    entry = lib::find_entry(library, lib::base_game_id());
    OA_CHECK(entry != nullptr && entry->state == State::installed);
    if (entry != nullptr) {
        OA_CHECK(actions_of(library, *entry) == Buttons({{Action::play_now, true}}));
        OA_CHECK_TEXT(lib::entry_actions(library, *entry).front().text, "PLAY NOW");
        OA_CHECK(lib::facts(library, *entry).empty());
    }
    lib::select(library, lib::base_game_id());
    OA_CHECK_TEXT(lib::playing_note(library), "");
    inputs.in_match = true;
    lib::refresh(library, inputs);
    entry = lib::find_entry(library, lib::base_game_id());
    if (entry != nullptr)
        OA_CHECK(actions_of(library, *entry) == Buttons({{Action::play_now, false}}));
    inputs.in_match = false;
    inputs.mod_from_command_line = true;
    lib::refresh(library, inputs);
    lib::select(library, lib::base_game_id());
    entry = lib::find_entry(library, lib::base_game_id());
    if (entry != nullptr)
        OA_CHECK(actions_of(library, *entry) == Buttons({{Action::play_now, false}}));
    OA_CHECK_TEXT(lib::playing_note(library), "The command line chose this run's mod.");

    // A mod the inputs name, with no installed copy marked playing, is still one played.
    inputs.mod_from_command_line = false;
    inputs.installed.clear();
    inputs.playing_mod = lib::PlayingMod{"Ridge", "1", std::nullopt};
    lib::refresh(library, inputs);
    entry = lib::find_entry(library, lib::base_game_id());
    OA_CHECK(entry != nullptr && entry->state == State::installed);
    if (entry != nullptr)
        OA_CHECK(actions_of(library, *entry) == Buttons({{Action::play_now, true}}));

    // A folder keyed @base-game cannot take the entry: it stays the base game.
    inputs.playing_mod.reset();
    inputs.installed.push_back(installed("", Kind::mod, "@base-game", "Clash"));
    lib::refresh(library, inputs);
    entry = lib::find_entry(library, lib::base_game_id());
    OA_CHECK(entry != nullptr && entry->installed == nullptr && entry->state == State::playing);
    if (entry != nullptr)
        OA_CHECK(actions_of(library, *entry).empty());
    std::size_t clashes = 0;
    for (const lib::Entry& each : library.entries)
        if (lib::is_base_game(each.id))
            ++clashes;
    OA_CHECK(clashes == 1);

    // First under All and Installed. Absent from New, Updates, Maps, Active and Registries.
    // Never counted. A search of its name keeps it first, ahead of a name that also matches.
    inputs = base_inputs();
    lib::Listing dated = listing(core, Kind::mod, "dated", "Dated Mod");
    dated.published = "2026-10-01";
    inputs.listings.push_back(dated);
    lib::Listing kit = listing(core, Kind::mod, "game-kit", "Game Kit");
    kit.tags = {"units"};
    inputs.listings.push_back(kit);
    inputs.listings.push_back(listing(example, Kind::map_pack, "pack", "Pack"));
    inputs.installed.push_back(installed(core, Kind::mod, "dated", "Dated Mod"));
    inputs.updates.push_back(update(core, Kind::mod, "dated"));
    library = built(std::move(inputs));
    OA_CHECK(visible_keys(library).front() == "@base-game@");
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 2);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 1);
    OA_CHECK(lib::update_count(library) == 1);
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(visible_keys(library).front() == "@base-game@");
    OA_CHECK(shows_key(library, "dated@fixture-core"));
    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(visible_keys(library) == Keys({"dated@fixture-core"}));
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"dated@fixture-core"}));
    lib::set_filter(library, lib::Filter::all);
    lib::set_tag(library, "units");
    OA_CHECK(!shows_key(library, "@base-game@"));
    lib::set_tag(library, "");
    for (const std::string_view word : {"no", "mod", "annihilation", "made"}) {
        lib::set_query(library, word);
        const std::vector<std::string> found = visible_keys(library);
        OA_CHECK(!found.empty() && found.front() == "@base-game@");
    }
    lib::set_query(library, "game");
    OA_CHECK(shows_key(library, "game-kit@fixture-core"));
    lib::set_query(library, "zzzz");
    OA_CHECK(library.visible.empty());
    lib::set_query(library, "");
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(visible_keys(library) == Keys({"pack@example-maps"}));
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(!shows_key(library, "@base-game@"));
    lib::set_tab(library, lib::Tab::active);
    OA_CHECK(library.visible.empty());
    lib::set_tab(library, lib::Tab::registries);
    OA_CHECK(!shows_key(library, "@base-game@"));

    // The name and summary are the language's, and a search matches that name.
    lib::TextHooks hooks;
    hooks.shown = translated_base_game;
    lib::Library translated;
    translated.text = hooks;
    lib::refresh(translated, base_inputs());
    lib::set_tab(translated, lib::Tab::mods);
    const lib::Entry* french = lib::find_entry(translated, lib::base_game_id());
    OA_CHECK(french != nullptr);
    if (french != nullptr) {
        OA_CHECK_TEXT(lib::entry_name(*french), "Jeu de base");
        OA_CHECK_TEXT(lib::entry_summary(*french), "Sans mod");
    }
    lib::set_query(translated, "jeu");
    OA_CHECK(visible_keys(translated) == Keys({"@base-game@"}));
    lib::set_query(translated, "disable");
    OA_CHECK(translated.visible.empty());
}

void test_built_in_languages() {
    using Buttons = std::vector<std::pair<Action, bool>>;
    // Settings › Language's built-in order, then the packs. English is in
    // use when the inputs name no language. Counts stay counts of packages.
    const std::vector<std::string> built_in = built_in_language_rows();
    OA_CHECK(
        built_in ==
        std::vector<std::string>(
            {"@language:en@", "@language:de@", "@language:es@", "@language:fr@", "@language:it@"}
        )
    );
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::language, "zh-Hans", "简体中文"));
    inputs.listings.push_back(listing(core, Kind::language, "xx", "Fixture Language"));
    lib::Installed hans = installed(core, Kind::language, "zh-Hans", "简体中文");
    inputs.installed.push_back(hans);
    lib::Library library = built(inputs);
    lib::set_tab(library, lib::Tab::languages);

    using Keys = std::vector<std::string>;
    Keys all_rows = built_in;
    all_rows.push_back("zh-Hans@fixture-core");
    all_rows.push_back("xx@fixture-core");
    OA_CHECK(visible_keys(library) == all_rows);
    OA_CHECK(lib::tab_count(library, lib::Tab::languages) == 2);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 2);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 0);
    OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 0);

    const lib::Entry* english = lib::find_entry(library, lib::built_in_language_id("en"));
    const lib::Entry* german = lib::find_entry(library, lib::built_in_language_id("de"));
    OA_CHECK(english != nullptr && german != nullptr);
    if (english != nullptr) {
        OA_CHECK(english->state == State::playing);
        OA_CHECK_TEXT(lib::entry_name(*english), "English");
        OA_CHECK(lib::entry_summary(*english).empty());
        OA_CHECK_TEXT(lib::status_line(library, *english), "Built into the game");
        OA_CHECK(lib::facts(library, *english).empty());
        OA_CHECK(actions_of(library, *english).empty());
    }
    if (german != nullptr) {
        OA_CHECK(german->state == State::installed);
        OA_CHECK_TEXT(lib::entry_name(*german), "Deutsch");
        OA_CHECK_TEXT(lib::entry_summary(*german), "German");
        OA_CHECK_TEXT(lib::status_line(library, *german), "Built into the game");
        OA_CHECK(lib::facts(library, *german).empty());
        OA_CHECK(actions_of(library, *german) == Buttons({{Action::use_now, true}}));
        OA_CHECK_TEXT(lib::entry_actions(library, *german).front().text, "USE NOW");
    }

    // New and Updates do not list them. Mods does not either.
    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(library.visible.empty());
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(library.visible.empty());
    lib::set_filter(library, lib::Filter::installed);
    Keys installed_rows = built_in;
    installed_rows.push_back("zh-Hans@fixture-core");
    OA_CHECK(visible_keys(library) == installed_rows);
    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::all);
    OA_CHECK(visible_keys(library) == Keys({"zh-Hans@fixture-core", "xx@fixture-core"}));
    lib::set_tab(library, lib::Tab::mods);
    OA_CHECK(visible_keys(library) == Keys({"@base-game@"}));

    // Search finds a built-in language by either name and by its tag.
    lib::set_tab(library, lib::Tab::languages);
    lib::set_filter(library, lib::Filter::all);
    lib::set_query(library, "german");
    OA_CHECK(visible_keys(library) == Keys({"@language:de@"}));
    lib::set_query(library, "deutsch");
    OA_CHECK(visible_keys(library) == Keys({"@language:de@"}));
    lib::set_query(library, "de");
    OA_CHECK(visible_keys(library) == Keys({"@language:de@"}));
    lib::set_query(library, "");

    // Choosing Deutsch marks it in use and offers USE NOW on English.
    inputs.language.tag = "de";
    inputs.language.name = "Deutsch";
    inputs.language.english = "German";
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::languages);
    english = lib::find_entry(library, lib::built_in_language_id("en"));
    german = lib::find_entry(library, lib::built_in_language_id("de"));
    OA_CHECK(english != nullptr && english->state == State::installed);
    OA_CHECK(german != nullptr && german->state == State::playing);
    if (english != nullptr)
        OA_CHECK(actions_of(library, *english) == Buttons({{Action::use_now, true}}));
    if (german != nullptr)
        OA_CHECK(actions_of(library, *german).empty());
    OA_CHECK(visible_keys(library).front() == "@language:en@");

    // An installed pack offers USE NOW until it is the one in use. One that
    // is only listed keeps GET. After an install, USE NOW is there at once.
    const lib::Entry* pack = at(library, core, Kind::language, "zh-Hans");
    const lib::Entry* listed = at(library, core, Kind::language, "xx");
    OA_CHECK(pack != nullptr && listed != nullptr);
    if (pack != nullptr)
        OA_CHECK(
            actions_of(library, *pack) ==
            Buttons(
                {{Action::use_now, true}, {Action::uninstall, true}, {Action::open_folder, true}}
            )
        );
    if (listed != nullptr)
        OA_CHECK(actions_of(library, *listed) == Buttons({{Action::get, true}}));
    inputs.installed.back().in_use = true;
    inputs.language.tag = "zh-Hans";
    inputs.language.pack = "简体中文";
    inputs.language.name = "简体中文";
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::languages);
    pack = at(library, core, Kind::language, "zh-Hans");
    german = lib::find_entry(library, lib::built_in_language_id("de"));
    OA_CHECK(pack != nullptr && german != nullptr && german->state == State::installed);
    if (pack != nullptr) {
        OA_CHECK(
            actions_of(library, *pack) ==
            Buttons({{Action::uninstall, true}, {Action::open_folder, true}})
        );
    }
    if (german != nullptr)
        OA_CHECK(actions_of(library, *german) == Buttons({{Action::use_now, true}}));

    lib::Inputs gained = library.inputs;
    gained.installed.push_back(installed(core, Kind::language, "xx", "Fixture Language"));
    lib::refresh(library, std::move(gained));
    lib::set_tab(library, lib::Tab::languages);
    listed = at(library, core, Kind::language, "xx");
    OA_CHECK(listed != nullptr && listed->state == State::installed);
    if (listed != nullptr)
        OA_CHECK(
            actions_of(library, *listed) ==
            Buttons(
                {{Action::use_now, true}, {Action::uninstall, true}, {Action::open_folder, true}}
            )
        );
    OA_CHECK(lib::tab_count(library, lib::Tab::languages) == 2);
    OA_CHECK_TEXT(lib::action_text(Action::use_now), "USE NOW");
}

void test_tab_counts() {
    // A tab's count is its All count, before the search and the tag, and it
    // does not follow the tab that is open. Registries counts its rows.
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "counted-mod", "Counted Mod"));
    inputs.listings.push_back(listing(core, Kind::mod, "counted-other", "Counted Other"));
    inputs.listings.push_back(listing(core, Kind::map_pack, "counted-maps", "Counted Maps"));
    inputs.listings.push_back(listing(core, Kind::language, "counted-lang", "Counted Lang"));
    lib::Library library = built(inputs);
    lib::set_tab(library, lib::Tab::updates);
    lib::set_filter(library, lib::Filter::installed);
    lib::set_tag(library, "missing");
    lib::set_query(library, "nothing-matches");
    OA_CHECK(lib::tab_count(library, lib::Tab::active) == 0);
    OA_CHECK(lib::tab_count(library, lib::Tab::mods) == 2);
    OA_CHECK(lib::tab_count(library, lib::Tab::maps) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::mutators) == 0);
    OA_CHECK(lib::tab_count(library, lib::Tab::brains) == 0);
    OA_CHECK(lib::tab_count(library, lib::Tab::languages) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == lib::update_count(library));
    OA_CHECK(lib::tab_count(library, lib::Tab::registries) == 2);
    // Base Game is not a mod: Mods' count leaves it out.
    OA_CHECK(lib::find_entry(library, lib::base_game_id()) != nullptr);
}

void test_added_package_count() {
    // The button counts packages the player has added, not catalogue rows.
    lib::Inputs listed = base_inputs();
    listed.listings.push_back(listing(core, Kind::mod, "counted-mod", "Counted Mod"));
    listed.listings.push_back(listing(core, Kind::map_pack, "counted-maps", "Counted Maps"));
    OA_CHECK(lib::added_package_count(listed) == 0);

    listed.installed.push_back(installed(core, Kind::mod, "counted-mod", "Counted Mod"));
    OA_CHECK(lib::added_package_count(listed) == 1);

    listed.installed.push_back(installed({}, Kind::mod, std::string(lib::base_game_key), "Base"));
    listed.installed.push_back(
        installed({}, Kind::language, std::string(lib::built_in_language_prefix) + "en", "English")
    );
    OA_CHECK(lib::added_package_count(listed) == 1);

    lib::InstalledMaps own;
    own.name = "Fixture Isles";
    own.maps.push_back({"Isle Hop", 4, "16x16", {}, {}, {}});
    listed.map_packs.push_back(own);
    OA_CHECK(lib::added_package_count(listed) == 2);

    listed.installed.push_back(installed(core, Kind::map_pack, "counted-maps", "Counted Maps"));
    lib::InstalledMaps same;
    same.name = "Counted Maps";
    same.key = "counted-maps";
    same.registry = std::string(core);
    listed.map_packs.push_back(same);
    OA_CHECK(lib::added_package_count(listed) == 3);

    listed.installed.push_back(installed(core, Kind::language, "de", "Deutsch"));
    listed.language = {"Deutsch", "Deutsch", "1.2"};
    listed.playing_mod = lib::PlayingMod{"Basalt", "2.1", std::nullopt};
    // Bundles copied from an installation are the player's game, not added
    // packages; an expansion a registry delivered is.
    listed.content.copied = {"totala", "totala-demo", "cavedog-units", "campaign"};
    OA_CHECK(lib::added_package_count(listed) == 4);
    listed.content.downloaded = {"cavedog-units"};
    OA_CHECK(lib::added_package_count(listed) == 5);
    listed.installed.push_back(
        installed(core, Kind::mutator, "counted-mutator", "Counted Mutator")
    );
    OA_CHECK(lib::added_package_count(listed) == 6);
    listed.installed.push_back(
        installed(core, Kind::brain, "steady-commander", "Steady Commander")
    );
    OA_CHECK(lib::added_package_count(listed) == 6);
}

void test_tabs_list_what_their_counts_hold() {
    // Mods can read "Mods (n)" and "Installed (n)" while the list says
    // "Nothing to show." That line is drawn only when the visible list is
    // empty, the search is empty and the filter is not Updates. Counts
    // ignore the tag. A tag chosen where the filter offers it, then carried
    // to a tab that does not, used to empty the list. A finished install
    // used to leave Updates counting the note the queue had already reached.
    lib::Inputs inputs = base_inputs();
    lib::Listing alpha = listing(core, Kind::mod, "alpha-mod", "Alpha Mod");
    alpha.tags = {"balance"};
    inputs.listings.push_back(alpha);
    inputs.installed.push_back(installed(core, Kind::mod, "alpha-mod", "Alpha Mod"));
    lib::Listing ridge = listing(core, Kind::mod, "ridge", "Ridge");
    ridge.tags = {"units"};
    inputs.listings.push_back(ridge);
    lib::Listing shore = listing(core, Kind::map_pack, "shore", "Shore");
    shore.tags = {"naval"};
    inputs.listings.push_back(shore);
    inputs.installed.push_back(installed(core, Kind::map_pack, "shore", "Shore"));
    lib::Listing hans = listing(core, Kind::language, "zh-hans", "Simplified Chinese");
    hans.tags = {"translation"};
    hans.release = 6;
    hans.revision = 4;
    inputs.listings.push_back(hans);
    // The copy in the folder is still the previous release. The worker has
    // not rewritten it, and the download of release 6 has finished.
    lib::Installed hans_installed =
        installed(core, Kind::language, "zh-hans", "Simplified Chinese");
    hans_installed.release = 5;
    hans_installed.revision = 3;
    inputs.installed.push_back(hans_installed);
    lib::UpdateNote hans_update = update(core, Kind::language, "zh-hans");
    hans_update.to_release = 6;
    hans_update.to_revision = 4;
    inputs.updates.push_back(hans_update);

    lib::Library library = built(std::move(inputs));
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(lib::update_count(library) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == 1);
    lib::set_tag(library, "translation");
    using Keys = std::vector<std::string>;
    OA_CHECK(visible_keys(library) == Keys({"zh-hans@fixture-core"}));

    // The install finishes. The note is still in the inputs. The queue holds
    // the finished item at the catalogue release it installed.
    lib::Inputs finished = library.inputs;
    lib::QueueItem done = queued(core, "zh-hans", "Simplified Chinese", Phase::installed);
    done.kind = Kind::language;
    done.release = 6;
    finished.queue.push_back(std::move(done));
    lib::refresh(library, std::move(finished));
    const lib::Entry* language = at(library, core, Kind::language, "zh-hans");
    OA_CHECK(
        language != nullptr && language->state == State::installed && language->update == nullptr
    );
    OA_CHECK(lib::update_count(library) == 0);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == 0);
    OA_CHECK(library.visible.empty());
    OA_CHECK(library.tag == "translation");

    lib::set_tab(library, lib::Tab::mods);
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(lib::tab_count(library, lib::Tab::mods) == 2);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 1);
    OA_CHECK(visible_keys(library) == Keys({"@base-game@", "alpha-mod@fixture-core"}));
    OA_CHECK(library.tag == "translation");
    lib::set_filter(library, lib::Filter::all);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 2);
    OA_CHECK(
        visible_keys(library) ==
        Keys({"@base-game@", "alpha-mod@fixture-core", "ridge@fixture-core"})
    );

    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(lib::tab_count(library, lib::Tab::maps) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 1);
    OA_CHECK(visible_keys(library) == Keys({"shore@fixture-core"}));

    // A mod's tag, chosen next, still must not blank Languages or Maps.
    lib::set_tag(library, "balance");
    lib::set_tab(library, lib::Tab::languages);
    OA_CHECK(library.tag == "balance");
    OA_CHECK(lib::tab_count(library, lib::Tab::languages) == 1);
    Keys language_rows = built_in_language_rows();
    language_rows.push_back("zh-hans@fixture-core");
    OA_CHECK(visible_keys(library) == language_rows);
    language = at(library, core, Kind::language, "zh-hans");
    OA_CHECK(language != nullptr && language->state == State::installed);
    lib::set_tab(library, lib::Tab::maps);
    OA_CHECK(visible_keys(library) == Keys({"shore@fixture-core"}));

    // Where the tab offers the tag, it filters again. It was never cleared.
    lib::set_tab(library, lib::Tab::mods);
    lib::set_filter(library, lib::Filter::all);
    OA_CHECK(visible_keys(library) == Keys({"alpha-mod@fixture-core"}));

    // A search that matches nothing still empties the list. That line is
    // "Nothing matches …", which an empty search does not say.
    lib::set_filter(library, lib::Filter::installed);
    lib::set_query(library, "zzz");
    OA_CHECK(library.visible.empty());
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::mods) == 2);
    lib::set_query(library, "");
    // balance is offered by the installed mod, so the base game stays out.
    OA_CHECK(visible_keys(library) == Keys({"alpha-mod@fixture-core"}));

    // The worker has rewritten the copy and dropped the queue item. The last
    // search's note is still in the inputs, and the copy is already there.
    lib::Inputs caught_up = library.inputs;
    caught_up.queue.clear();
    for (lib::Installed& copy : caught_up.installed) {
        if (copy.key == "zh-hans") {
            copy.release = 6;
            copy.revision = 4;
        }
    }
    lib::refresh(library, std::move(caught_up));
    language = at(library, core, Kind::language, "zh-hans");
    OA_CHECK(
        language != nullptr && language->state == State::installed && language->update == nullptr
    );
    OA_CHECK(lib::update_count(library) == 0);

    // A later release is still an update. Held, and a finished item that
    // names no release, do not hide the note. The finished release does.
    lib::Inputs later = library.inputs;
    later.updates.clear();
    lib::UpdateNote next = update(core, Kind::language, "zh-hans");
    next.to_release = 7;
    later.updates.push_back(next);
    lib::QueueItem held = queued(core, "zh-hans", "Simplified Chinese", Phase::held);
    held.kind = Kind::language;
    held.release = 7;
    later.queue = {held};
    lib::refresh(library, later);
    OA_CHECK(lib::update_count(library) == 1);
    later.queue[0].phase = Phase::installed;
    later.queue[0].release = 0;
    lib::refresh(library, later);
    OA_CHECK(lib::update_count(library) == 1);
    later.queue[0].release = 7;
    lib::refresh(library, std::move(later));
    OA_CHECK(lib::update_count(library) == 0);
}

void test_rolled_back_update_returns() {
    // Install release 1, update to release 2, then roll back to release 1.
    // The finished queue item hides the note only while the installed copy
    // has not been read again. Once a later reading names release 1, Updates
    // counts the note and the row offers UPDATE.
    lib::Inputs inputs = base_inputs();
    lib::Listing ridge = listing(core, Kind::mod, "ridge", "Ridge");
    ridge.release = 2;
    inputs.listings.push_back(ridge);
    lib::Installed copy = installed(core, Kind::mod, "ridge", "Ridge");
    copy.release = 1;
    inputs.installed.push_back(copy);
    lib::UpdateNote note = update(core, Kind::mod, "ridge");
    note.to_release = 2;
    inputs.updates.push_back(note);
    inputs.installed_generation = 1;

    lib::Library library = built(std::move(inputs));
    lib::set_tab(library, lib::Tab::updates);
    const lib::Entry* row = at(library, core, Kind::mod, "ridge");
    OA_CHECK(row != nullptr && row->state == State::update);
    OA_CHECK(lib::update_count(library) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == 1);
    if (row != nullptr) {
        OA_CHECK_TEXT(lib::entry_actions(library, *row).front().text, "UPDATE");
        OA_CHECK(lib::entry_actions(library, *row).front().action == Action::update);
        OA_CHECK(lib::entry_actions(library, *row).front().enabled);
    }

    // The update finishes. The next reading of the installed copy still
    // names release 1, and the finished item is of that same reading.
    lib::Inputs finished = library.inputs;
    lib::QueueItem done = queued(core, "ridge", "Ridge", Phase::installed);
    done.release = 2;
    done.finished_scan = finished.installed_generation;
    finished.queue.push_back(std::move(done));
    lib::refresh(library, std::move(finished));
    row = at(library, core, Kind::mod, "ridge");
    OA_CHECK(row != nullptr && row->state == State::installed && row->update == nullptr);
    OA_CHECK(lib::update_count(library) == 0);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == 0);

    // Roll back. A later reading names release 1 again. The finished item
    // is of the earlier reading, so it no longer hides the note.
    lib::Inputs rolled = library.inputs;
    rolled.installed_generation = 2;
    lib::refresh(library, std::move(rolled));
    lib::set_tab(library, lib::Tab::updates);
    row = at(library, core, Kind::mod, "ridge");
    OA_CHECK(row != nullptr && row->state == State::update);
    OA_CHECK(lib::update_count(library) == 1);
    OA_CHECK(lib::tab_count(library, lib::Tab::updates) == 1);
    if (row != nullptr) {
        OA_CHECK_TEXT(lib::entry_actions(library, *row).front().text, "UPDATE");
        OA_CHECK(lib::entry_actions(library, *row).front().enabled);
    }
}

/// Tells whether an id list holds an id.
///
/// @param ids the ids
/// @param id the id
/// @return true when it is among them
bool holds_id(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// A pill press goes through turn_content_expansion and stays on the draft.
void test_expansion_pills() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"campaign", "music"};
    inputs.content.enabled = {"campaign"};
    lib::Library library;
    lib::refresh(library, inputs);

    const lib::ContentPress off = lib::press_expansion(library, "campaign");
    OA_CHECK(off.kind == lib::ContentPressKind::redraw);
    OA_CHECK(library.content.edited);
    OA_CHECK(!holds_id(library.content.expansions, "campaign"));
    OA_CHECK_TEXT(lib::active_content(library).hint, "Campaign turns off when the Library closes.");
    const lib::ContentPress again = lib::press_expansion(library, "campaign");
    OA_CHECK(again.kind == lib::ContentPressKind::redraw);
    OA_CHECK(holds_id(library.content.expansions, "campaign"));
    OA_CHECK(!library.content.edited);

    const lib::ContentPress missing = lib::press_expansion(library, "core-contingency");
    OA_CHECK(missing.kind == lib::ContentPressKind::add);
    OA_CHECK(missing.expansion == "core-contingency");
    OA_CHECK(!holds_id(library.content.expansions, "core-contingency"));

    inputs.content.copied = {"campaign", "core-contingency"};
    inputs.content.enabled = {};
    lib::refresh(library, inputs);
    const lib::ContentPress on = lib::press_expansion(library, "core-contingency");
    OA_CHECK(on.kind == lib::ContentPressKind::redraw);
    OA_CHECK(holds_id(library.content.expansions, "campaign"));
    OA_CHECK(holds_id(library.content.expansions, "core-contingency"));
    OA_CHECK_TEXT(
        lib::active_content(library).hint, "Core Contingency turns on when the Library closes."
    );

    const lib::ContentPress cascade = lib::press_expansion(library, "campaign");
    OA_CHECK(cascade.kind == lib::ContentPressKind::redraw);
    OA_CHECK(!holds_id(library.content.expansions, "campaign"));
    OA_CHECK(!holds_id(library.content.expansions, "core-contingency"));

    inputs.content.copied = {"campaign", "cavedog-units"};
    inputs.content.enabled = {};
    lib::refresh(library, inputs);
    const lib::ContentPress refused = lib::press_expansion(library, "cavedog-units");
    OA_CHECK(refused.kind == lib::ContentPressKind::none);
    OA_CHECK(!library.content.edited);

    inputs.content.copied = {"campaign"};
    inputs.content.enabled = {"campaign"};
    inputs.content.mod_required = {"campaign"};
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.folder = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    lib::refresh(library, inputs);
    const lib::ContentPress asked = lib::press_expansion(library, "campaign");
    OA_CHECK(asked.kind == lib::ContentPressKind::ask);
    OA_CHECK(asked.expansion == "campaign");
    OA_CHECK(!library.content.edited);
    OA_CHECK(library.content.asking);
    OA_CHECK(holds_id(library.content.seed_enabled, "campaign"));
    const lib::ContentPress during = lib::press_expansion(library, "campaign");
    OA_CHECK(during.kind == lib::ContentPressKind::none);
    const oa::ui::kit::Question question = lib::expansion_off_question(library);
    OA_CHECK(question.title == "Turn off Campaign?");
    OA_CHECK(question.buttons.size() == 2);
    OA_CHECK(question.buttons[0].id == lib::cancel_button_id);
    OA_CHECK(question.buttons[1].id == lib::turn_off_button_id);
    OA_CHECK(question.buttons[1].caption == "TURN OFF");
    OA_CHECK(question.paragraphs.size() == 2);
    if (question.paragraphs.size() == 2) {
        OA_CHECK(
            question.paragraphs[0].text ==
            "Ridge needs Campaign, so it is unloaded and the game plays with No Mod."
        );
        OA_CHECK(question.paragraphs[1].text.find("Nothing is deleted.") != std::string::npos);
    }
    lib::cancel_expansion_off(library);
    OA_CHECK(!library.content.asking && !library.content.edited);
    OA_CHECK(holds_id(library.content.seed_enabled, "campaign"));

    const lib::ContentPress asked_again = lib::press_expansion(library, "campaign");
    OA_CHECK(asked_again.kind == lib::ContentPressKind::ask);
    lib::accept_expansion_off(library);
    OA_CHECK(!library.content.asking && library.content.edited);
    OA_CHECK(!holds_id(library.content.expansions, "campaign"));
    OA_CHECK(library.content.last_id == "campaign" && !library.content.last_on);

    library.inputs.in_match = true;
    const lib::ContentPress blocked = lib::press_expansion(library, "music");
    OA_CHECK(blocked.kind == lib::ContentPressKind::none);
    library.inputs.in_match = false;
    library.inputs.content.locked = true;
    const lib::ContentPress locked = lib::press_expansion(library, "campaign");
    OA_CHECK(locked.kind == lib::ContentPressKind::none);

    lib::Inputs demo;
    demo.content.base_id = "totala-demo";
    lib::Library shown;
    lib::refresh(shown, std::move(demo));
    const lib::ActiveContent face = lib::active_content(shown);
    OA_CHECK_TEXT(face.name, "Total Annihilation");
    OA_CHECK_TEXT(face.version_line, "Demo");
    OA_CHECK_TEXT(face.expansions_heading, "EXPANSIONS");
    OA_CHECK_TEXT(face.hint, "This game has no expansions.");
    OA_CHECK(face.pills.empty());
}

/// Returns one expansion pill.
///
/// @param face the Active tab's content
/// @param id the expansion
/// @return the pill; null when the tile has none
const lib::ExpansionPill* pill_of(const lib::ActiveContent& face, std::string_view id) {
    for (const lib::ExpansionPill& pill : face.pills) {
        if (pill.id == id)
            return &pill;
    }
    return nullptr;
}

/// Ukrainian names of the bundles, as the interface catalogue gives them.
///
/// @param english the English text
/// @return the translation, or the English
std::string_view ukrainian_bundles(void*, std::string_view english) {
    if (english == "Campaign")
        return "Кампанія";
    if (english == "Music")
        return "Музика";
    if (english == "Cavedog units")
        return "Підрозділи Cavedog";
    if (english == "Cavedog tilesets")
        return "Набори тайлів Cavedog";
    return english;
}

/// The Active tab, its hints, the base game chooser and the question before
/// an expansion goes off name the bundles in the language shown.
void test_expansion_names_translated() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "campaign", "music", "cavedog-units", "cavedog-tilesets"};
    inputs.content.enabled = {"campaign", "music", "cavedog-tilesets"};
    lib::Library library;
    library.text.shown = ukrainian_bundles;
    lib::refresh(library, inputs);

    const lib::ActiveContent face = lib::active_content(library);
    const lib::ExpansionPill* campaign = pill_of(face, "campaign");
    const lib::ExpansionPill* music = pill_of(face, "music");
    const lib::ExpansionPill* units = pill_of(face, "cavedog-units");
    const lib::ExpansionPill* tilesets = pill_of(face, "cavedog-tilesets");
    OA_CHECK(campaign != nullptr && campaign->text == "Кампанія");
    OA_CHECK(music != nullptr && music->text == "Музика");
    OA_CHECK(units != nullptr && units->text == "Підрозділи Cavedog");
    OA_CHECK(tilesets != nullptr && tilesets->text == "Набори тайлів Cavedog");
    for (const lib::ExpansionPill& pill : face.pills)
        OA_CHECK(pill.text.find("Cavedog units") == std::string::npos);

    // The hint after a press names the expansion in the language.
    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::redraw);
    OA_CHECK_TEXT(lib::active_content(library).hint, "Кампанія turns off when the Library closes.");
    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::redraw);

    const lib::BaseOpen opened = lib::open_base_chooser(library);
    OA_CHECK(opened.opened);
    bool saw_retail = false;
    for (const lib::BaseGameRow& row : library.chooser.rows) {
        if (row.id != "totala")
            continue;
        saw_retail = true;
        OA_CHECK(
            row.expansions ==
            std::vector<std::string>({"Кампанія", "Музика", "Набори тайлів Cavedog"})
        );
    }
    OA_CHECK(saw_retail);
    lib::close_base_chooser(library);

    // The question before a mod's expansion goes off.
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.folder = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    inputs.content.mod_required = {"campaign"};
    lib::refresh(library, inputs);
    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
    const oa::ui::kit::Question question = lib::expansion_off_question(library);
    OA_CHECK_TEXT(question.title, "Turn off Кампанія?");
    OA_CHECK(!question.paragraphs.empty());
    if (!question.paragraphs.empty()) {
        OA_CHECK_TEXT(
            question.paragraphs.front().text,
            "Ridge needs Кампанія, so it is unloaded and the game plays with No Mod."
        );
    }
}

/// An offered expansion shows a download arrow. The pill turns on once the
/// copy is in the game's folder, and only on the pending draft.
void test_expansion_download() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    lib::Registry built_in;
    built_in.id = std::string(core);
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    lib::Registry added;
    added.id = std::string(example);
    added.name = "Example Maps";
    inputs.registries = {built_in, added};

    lib::DownloadableExpansion units;
    units.id = "cavedog-units";
    units.registry = std::string(core);
    units.name = "Cavedog units";
    units.extends = {"totala"};
    lib::DownloadableExpansion from_added = units;
    from_added.registry = std::string(example);
    lib::DownloadableExpansion nowhere;
    nowhere.id = "cavedog-units";
    nowhere.registry = std::string(core);
    nowhere.extends = {};
    inputs.downloadable_expansions = {nowhere, from_added, units};

    lib::Library library;
    lib::refresh(library, inputs);
    const lib::ActiveContent offered = lib::active_content(library);
    const lib::ExpansionPill* units_pill = pill_of(offered, "cavedog-units");
    const lib::ExpansionPill* tactics = pill_of(offered, "battle-tactics");
    OA_CHECK(units_pill != nullptr && units_pill->face == lib::ExpansionFace::off);
    OA_CHECK(units_pill != nullptr && units_pill->text == "Cavedog units");
    OA_CHECK(units_pill != nullptr && units_pill->arrow == lib::ExpansionArrow::get);
    OA_CHECK(units_pill != nullptr && units_pill->arrow_enabled);
    OA_CHECK(units_pill != nullptr && units_pill->download_registry == core);
    OA_CHECK(tactics != nullptr && tactics->face == lib::ExpansionFace::add);
    OA_CHECK(tactics != nullptr && tactics->text.find("ADD FROM") != std::string::npos);
    OA_CHECK(tactics != nullptr && !tactics->arrow);

    const lib::ContentPress add = lib::press_expansion(library, "battle-tactics");
    OA_CHECK(add.kind == lib::ContentPressKind::add);
    OA_CHECK(!holds_id(library.content.expansions, "battle-tactics"));

    const lib::ContentPress got = lib::press_expansion(library, "cavedog-units");
    OA_CHECK(got.kind == lib::ContentPressKind::download);
    OA_CHECK(got.expansion == "cavedog-units");
    OA_CHECK(got.registry == core);
    OA_CHECK(holds_id(library.content.downloads, "cavedog-units"));
    OA_CHECK(!library.content.edited);
    OA_CHECK(!library.content.applied);
    OA_CHECK(!holds_id(library.content.expansions, "cavedog-units"));

    lib::Inputs queued_inputs = library.inputs;
    lib::QueueItem downloading = queued(core, "cavedog-units", "Cavedog units", Phase::downloading);
    downloading.expansion = true;
    downloading.done_bytes = 19083674;
    downloading.total_bytes = 64 * mib;
    queued_inputs.queue.push_back(downloading);
    lib::refresh(library, queued_inputs);
    OA_CHECK(holds_id(library.content.downloads, "cavedog-units"));
    const lib::ActiveContent busy_face = lib::active_content(library);
    const lib::ExpansionPill* busy = pill_of(busy_face, "cavedog-units");
    OA_CHECK(busy != nullptr && busy->arrow == lib::ExpansionArrow::cancel && busy->arrow_enabled);
    OA_CHECK(lib::active_content(library).hint.find("Downloading") != std::string::npos);
    OA_CHECK(lib::queue_lines(library).first.find("Downloading") != std::string::npos);
    OA_CHECK(lib::queue_lines(library).first.find("Cavedog units") != std::string::npos);

    const lib::ContentPress stopped = lib::press_expansion(library, "cavedog-units");
    OA_CHECK(stopped.kind == lib::ContentPressKind::cancel_download);
    OA_CHECK(stopped.registry == core);
    OA_CHECK(!holds_id(library.content.downloads, "cavedog-units"));

    lib::Inputs failed_inputs = library.inputs;
    failed_inputs.queue.clear();
    lib::QueueItem failed = queued(core, "cavedog-units", "Cavedog units", Phase::failed);
    failed.expansion = true;
    failed.retryable = true;
    failed.problem = "The server could not be reached";
    failed_inputs.queue.push_back(failed);
    lib::refresh(library, failed_inputs);
    const lib::ActiveContent again_face = lib::active_content(library);
    const lib::ExpansionPill* again = pill_of(again_face, "cavedog-units");
    OA_CHECK(again != nullptr && again->arrow == lib::ExpansionArrow::retry);
    const lib::ContentPress retried = lib::press_expansion(library, "cavedog-units");
    OA_CHECK(retried.kind == lib::ContentPressKind::retry_download);
    OA_CHECK(holds_id(library.content.downloads, "cavedog-units"));

    lib::Inputs stuck_inputs = library.inputs;
    stuck_inputs.queue[0].retryable = false;
    lib::refresh(library, stuck_inputs);
    const lib::ActiveContent stuck_face = lib::active_content(library);
    const lib::ExpansionPill* stuck = pill_of(stuck_face, "cavedog-units");
    OA_CHECK(stuck != nullptr && !stuck->arrow);
    OA_CHECK(lib::active_content(library).hint == "The server could not be reached");
    OA_CHECK(lib::press_expansion(library, "cavedog-units").kind == lib::ContentPressKind::none);

    lib::Inputs during = library.inputs;
    during.queue[0].phase = Phase::downloading;
    during.queue[0].retryable = false;
    during.in_match = true;
    lib::refresh(library, during);
    OA_CHECK(
        lib::press_expansion(library, "cavedog-units").kind ==
        lib::ContentPressKind::cancel_download
    );
    library.content.downloads.clear();
    during.in_match = false;
    during.content.locked = true;
    during.queue.clear();
    lib::refresh(library, during);
    OA_CHECK(lib::press_expansion(library, "cavedog-units").kind == lib::ContentPressKind::none);

    lib::Inputs quiet = library.inputs;
    quiet.content.locked = false;
    quiet.queue.clear();
    quiet.registries[0].downloads_on = false;
    quiet.registries[1].downloads_on = false;
    library.content.downloads.clear();
    lib::refresh(library, quiet);
    const lib::ActiveContent off_face = lib::active_content(library);
    const lib::ExpansionPill* off = pill_of(off_face, "cavedog-units");
    OA_CHECK(off != nullptr && off->arrow == lib::ExpansionArrow::get && !off->arrow_enabled);
    OA_CHECK(lib::active_content(library).hint.find("Downloads from") != std::string::npos);
    OA_CHECK(lib::press_expansion(library, "cavedog-units").kind == lib::ContentPressKind::none);

    // The copy arrived without the expansions it needs. The pill stays off.
    lib::Inputs short_copy = library.inputs;
    short_copy.registries[0].downloads_on = true;
    short_copy.content.locked = false;
    short_copy.queue.clear();
    lib::refresh(library, short_copy);
    OA_CHECK(
        lib::press_expansion(library, "cavedog-units").kind == lib::ContentPressKind::download
    );
    short_copy.content.copied = {"cavedog-units"};
    lib::refresh(library, short_copy);
    OA_CHECK(!library.content.edited);
    OA_CHECK(!holds_id(library.content.downloads, "cavedog-units"));
    OA_CHECK(!holds_id(library.content.expansions, "cavedog-units"));
    const lib::ActiveContent unmet_face = lib::active_content(library);
    const lib::ExpansionPill* unmet = pill_of(unmet_face, "cavedog-units");
    OA_CHECK(unmet != nullptr && unmet->face == lib::ExpansionFace::off && !unmet->arrow);

    lib::Library arriving;
    lib::Inputs copied = inputs;
    lib::refresh(arriving, copied);
    OA_CHECK(
        lib::press_expansion(arriving, "cavedog-units").kind == lib::ContentPressKind::download
    );
    copied.content.copied = {"campaign", "core-contingency", "cavedog-units"};
    lib::refresh(arriving, copied);
    const lib::ActiveContent on_face = lib::active_content(arriving);
    const lib::ExpansionPill* on = pill_of(on_face, "cavedog-units");
    OA_CHECK(on != nullptr && on->face == lib::ExpansionFace::on && !on->arrow);
    OA_CHECK(holds_id(arriving.content.expansions, "cavedog-units"));
    OA_CHECK(holds_id(arriving.content.expansions, "campaign"));
    OA_CHECK(holds_id(arriving.content.expansions, "core-contingency"));
    OA_CHECK(arriving.content.edited);
    OA_CHECK(!arriving.content.applied);
    OA_CHECK(
        lib::active_content(arriving).hint.find("turns on when the Library closes") !=
        std::string::npos
    );
    OA_CHECK(!holds_id(arriving.content.downloads, "cavedog-units"));

    // A queue item for the expansion is not a mod row, even when a mod shares its key.
    lib::Inputs collided = copied;
    collided.content.copied.clear();
    collided.content.enabled.clear();
    lib::Listing same_key = listing(core, Kind::mod, "cavedog-units", "Not the expansion");
    collided.listings.push_back(same_key);
    lib::QueueItem row = queued(core, "cavedog-units", "Cavedog units", Phase::downloading);
    row.expansion = true;
    collided.queue = {row};
    lib::Library rows;
    lib::refresh(rows, std::move(collided));
    const lib::Entry* entry = at(rows, core, Kind::mod, "cavedog-units");
    OA_CHECK(entry != nullptr && entry->queued == nullptr);
    OA_CHECK(lib::queue_lines(rows).first.find("Cavedog units") != std::string::npos);
}

/// The base game chooser marks a game and keeps the choice on the draft.
void test_base_chooser() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "campaign", "core-contingency", "music"};
    inputs.content.enabled = {"campaign"};
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    lib::Library library;
    lib::refresh(library, inputs);

    library.inputs.in_match = true;
    OA_CHECK(!lib::open_base_chooser(library).opened);
    library.inputs.in_match = false;
    library.inputs.content.locked = true;
    OA_CHECK(!lib::open_base_chooser(library).opened);
    library.inputs.content.locked = false;
    library.inputs.mod_from_command_line = true;
    OA_CHECK(!lib::open_base_chooser(library).opened);
    library.inputs.mod_from_command_line = false;

    library.inputs.base_game_badge = lib::Badge{std::vector<uint8_t>(1 * 1 * 4, 9), 1, 1};
    const lib::BaseOpen opened = lib::open_base_chooser(library);
    OA_CHECK(opened.opened && library.chooser.open);
    OA_CHECK(library.chooser.marked == "totala");
    bool saw_demo = false;
    for (const lib::BaseGameRow& row : library.chooser.rows) {
        if (row.id == "totala") {
            OA_CHECK(row.state == lib::BaseGameState::in_use && row.marked);
            OA_CHECK(row.badge == &*library.inputs.base_game_badge);
            OA_CHECK(row.letters == "TA");
        }
        if (row.id == "totala-demo") {
            saw_demo = true;
            OA_CHECK(row.state == lib::BaseGameState::not_added);
            OA_CHECK(row.name == "Total Annihilation demo");
            OA_CHECK(row.version == "1997");
            OA_CHECK(row.letters == "DEMO");
            OA_CHECK(row.badge == &*library.inputs.base_game_badge);
            OA_CHECK(row.expansions.empty());
        }
    }
    OA_CHECK(saw_demo);

    lib::mark_base_game(library, "totala-demo");
    OA_CHECK(library.chooser.marked == "totala-demo");
    OA_CHECK(library.chooser.note.find("Ridge") != std::string::npos);
    OA_CHECK(library.chooser.note.find("Library closes") != std::string::npos);
    const lib::BaseUse needs = lib::use_marked_base_game(library);
    OA_CHECK(needs.kind == lib::BaseUseKind::needs_source);
    OA_CHECK(needs.id == "totala-demo");
    OA_CHECK(!library.content.edited && library.chooser.open);

    library.inputs.in_match = true;
    const lib::BaseUse blocked = lib::use_marked_base_game(library);
    OA_CHECK(blocked.kind == lib::BaseUseKind::refused);
    library.inputs.in_match = false;

    lib::mark_base_game(library, "totala");
    OA_CHECK(library.chooser.note.empty());
    const lib::BaseUse same = lib::use_marked_base_game(library);
    OA_CHECK(same.kind == lib::BaseUseKind::none);
    OA_CHECK(!library.chooser.open && !library.content.edited);

    inputs.content.copied.push_back("totala-demo");
    lib::refresh(library, inputs);
    OA_CHECK(lib::open_base_chooser(library).opened);
    lib::mark_base_game(library, "totala");
    lib::close_base_chooser(library);
    OA_CHECK(!library.chooser.open && !library.content.pending_base);

    OA_CHECK(lib::open_base_chooser(library).opened);
    lib::mark_base_game(library, "totala-demo");
    const lib::BaseUse applied = lib::use_marked_base_game(library);
    OA_CHECK(applied.kind == lib::BaseUseKind::applied);
    OA_CHECK(!library.chooser.open);
    OA_CHECK(library.content.pending_base);
    OA_CHECK(library.content.base_game == "totala-demo");
    OA_CHECK(library.content.expansions.empty());
    OA_CHECK(library.content.unload_mod);
    OA_CHECK(library.content.clear_reason == lib::PendingClear::base_game_chosen);
    OA_CHECK_TEXT(
        lib::active_content(library).hint, "This base game is used when the Library closes."
    );

    lib::Library asked;
    lib::Inputs pill_inputs;
    pill_inputs.content.base_id = "totala";
    pill_inputs.content.copied = {"campaign"};
    pill_inputs.content.enabled = {"campaign"};
    pill_inputs.content.mod_required = {"campaign"};
    pill_inputs.installed.push_back(ridge);
    lib::refresh(asked, std::move(pill_inputs));
    OA_CHECK(lib::press_expansion(asked, "campaign").kind == lib::ContentPressKind::ask);
    lib::accept_expansion_off(asked);
    OA_CHECK(asked.content.unload_mod);
    OA_CHECK(!asked.content.pending_base);
    OA_CHECK(asked.content.clear_reason == lib::PendingClear::required_expansion_off);

    lib::Inputs short_music;
    short_music.content.base_id = "totala";
    short_music.content.copied = {"music"};
    short_music.content.music_tracks = 17;
    short_music.content.music_track_count = 18;
    lib::Library music_library;
    lib::refresh(music_library, std::move(short_music));
    bool music_pill = false;
    for (const lib::ExpansionPill& pill : lib::active_content(music_library).pills) {
        if (pill.id != "music")
            continue;
        music_pill = true;
        OA_CHECK(pill.text == "17 of 18 tracks");
    }
    OA_CHECK(music_pill);
}

/// A catalogue, a picture or a copy that arrives while the chooser is open
/// keeps it open on the game the player marked. A different stored base game
/// closes it.
void test_base_chooser_through_refresh() {
    lib::Inputs inputs;
    inputs.registries = fixture_registries();
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "campaign"};
    inputs.content.enabled = {"campaign"};
    lib::Library library;
    lib::refresh(library, inputs);
    OA_CHECK(lib::open_base_chooser(library).opened);
    lib::mark_base_game(library, "totala-demo");
    OA_CHECK(library.chooser.marked == "totala-demo");
    OA_CHECK(!library.chooser.note.empty());

    // The catalogue the Library asked for as it opened, and the base game's
    // picture, arrive. The stored choice is the same.
    lib::Inputs arrived = inputs;
    arrived.listings.push_back(listing(core, Kind::mod, "ridge", "Ridge"));
    arrived.base_game_badge = lib::Badge{std::vector<uint8_t>(1 * 1 * 4, 9), 1, 1};
    lib::refresh(library, arrived);
    OA_CHECK(library.chooser.open);
    OA_CHECK(library.chooser.marked == "totala-demo");
    OA_CHECK(!library.chooser.note.empty());
    OA_CHECK(!library.chooser.rows.empty());
    for (const lib::BaseGameRow& row : library.chooser.rows) {
        OA_CHECK(row.marked == (row.id == "totala-demo"));
        OA_CHECK(row.badge == &*library.inputs.base_game_badge);
    }

    // USE THIS GAME still asks for the demo's installer.
    const lib::BaseUse needs = lib::use_marked_base_game(library);
    OA_CHECK(needs.kind == lib::BaseUseKind::needs_source);
    OA_CHECK(needs.id == "totala-demo");
    OA_CHECK(library.chooser.open);

    // A failed copy's words stay through a refresh too.
    lib::note_base_copy_problem(library, "The installer could not be read.");
    lib::refresh(library, arrived);
    OA_CHECK(library.chooser.open);
    OA_CHECK_TEXT(library.chooser.problem, "The installer could not be read.");
    lib::note_base_copy_problem(library, {});

    // The copy arrives. The chooser shows the demo as copied, and USE THIS
    // GAME keeps it on the pending choice.
    arrived.content.copied.push_back("totala-demo");
    lib::refresh(library, arrived);
    OA_CHECK(library.chooser.open);
    OA_CHECK(library.chooser.marked == "totala-demo");
    bool demo_copied = false;
    for (const lib::BaseGameRow& row : library.chooser.rows) {
        if (row.id == "totala-demo")
            demo_copied = row.state == lib::BaseGameState::copied;
    }
    OA_CHECK(demo_copied);
    const lib::BaseUse applied = lib::use_marked_base_game(library);
    OA_CHECK(applied.kind == lib::BaseUseKind::applied);
    OA_CHECK(!library.chooser.open);
    OA_CHECK(library.content.base_game == "totala-demo");
    OA_CHECK(library.content.pending_base);

    // A different stored base game closes the chooser and drops the draft.
    lib::Library stored;
    lib::refresh(stored, arrived);
    OA_CHECK(lib::open_base_chooser(stored).opened);
    lib::mark_base_game(stored, "totala-demo");
    lib::Inputs switched = arrived;
    switched.content.base_id = "totala-demo";
    switched.content.enabled.clear();
    lib::refresh(stored, std::move(switched));
    OA_CHECK(!stored.chooser.open);
    OA_CHECK(stored.chooser.rows.empty());
}

/// A catalogue or a picture that arrives while the expansion-off question
/// shows keeps the question, so TURN OFF still turns the expansion off and
/// CANCEL still leaves it on. The answer is kept through the next refresh. A
/// different stored choice drops the question.
void test_expansion_off_question_through_refresh() {
    lib::Inputs inputs;
    inputs.registries = fixture_registries();
    inputs.content.base_id = "totala";
    inputs.content.copied = {"campaign", "core-contingency"};
    inputs.content.enabled = {"campaign", "core-contingency"};
    inputs.content.mod_required = {"campaign"};
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.folder = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    lib::Library library;
    lib::refresh(library, inputs);
    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
    const oa::ui::kit::Question asked = lib::expansion_off_question(library);

    // The catalogue the Library asked for as it opened, and the base game's
    // picture, arrive while the question shows. The stored choice is the same.
    lib::Inputs arrived = inputs;
    arrived.listings.push_back(listing(core, Kind::mod, "ridge", "Ridge"));
    arrived.base_game_badge = lib::Badge{std::vector<uint8_t>(1 * 1 * 4, 9), 1, 1};
    lib::refresh(library, arrived);
    OA_CHECK(library.content.asking);
    OA_CHECK(library.content.ask_id == "campaign");
    OA_CHECK(!holds_id(library.content.proposed, "campaign"));
    OA_CHECK(holds_id(library.content.turned_off, "core-contingency"));
    OA_CHECK(!library.content.edited);
    const oa::ui::kit::Question still = lib::expansion_off_question(library);
    OA_CHECK_TEXT(still.title, asked.title);
    OA_CHECK(still.paragraphs.size() == asked.paragraphs.size());
    // The pills and CHANGE… still wait for the answer.
    OA_CHECK(lib::press_expansion(library, "core-contingency").kind == lib::ContentPressKind::none);
    OA_CHECK(!lib::open_base_chooser(library).opened);

    // TURN OFF turns Campaign off, Core Contingency with it, and unloads Ridge.
    lib::accept_expansion_off(library);
    OA_CHECK(!library.content.asking);
    OA_CHECK(library.content.edited);
    OA_CHECK(!holds_id(library.content.expansions, "campaign"));
    OA_CHECK(!holds_id(library.content.expansions, "core-contingency"));
    OA_CHECK(library.content.unload_mod);
    OA_CHECK(library.content.clear_reason == lib::PendingClear::required_expansion_off);

    // The answer is kept through the next refresh.
    lib::refresh(library, arrived);
    OA_CHECK(library.content.edited);
    OA_CHECK(!holds_id(library.content.expansions, "campaign"));
    OA_CHECK(library.content.unload_mod);
    OA_CHECK_TEXT(
        lib::active_content(library).hint,
        "Ridge was unloaded because Campaign is off. The game plays with No Mod."
    );
    OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");

    // CANCEL after a refresh leaves Campaign on, and the pill asks again.
    lib::Library kept_on;
    lib::refresh(kept_on, inputs);
    OA_CHECK(lib::press_expansion(kept_on, "campaign").kind == lib::ContentPressKind::ask);
    lib::refresh(kept_on, arrived);
    OA_CHECK(kept_on.content.asking);
    lib::cancel_expansion_off(kept_on);
    OA_CHECK(!kept_on.content.asking && !kept_on.content.edited);
    OA_CHECK(!kept_on.content.unload_mod);
    OA_CHECK(lib::press_expansion(kept_on, "campaign").kind == lib::ContentPressKind::ask);

    // A different stored choice drops the question, as it drops an edit, and
    // TURN OFF then changes nothing.
    lib::Library dropped;
    lib::refresh(dropped, inputs);
    OA_CHECK(lib::press_expansion(dropped, "campaign").kind == lib::ContentPressKind::ask);
    lib::Inputs switched = arrived;
    switched.content.enabled = {"campaign"};
    lib::refresh(dropped, std::move(switched));
    OA_CHECK(!dropped.content.asking);
    OA_CHECK(dropped.content.ask_id.empty() && dropped.content.proposed.empty());
    lib::accept_expansion_off(dropped);
    OA_CHECK(!dropped.content.edited && !dropped.content.unload_mod);
}

/// Returns a button of an action, or null.
///
/// @param buttons the buttons
/// @param action the action
/// @return the button
const lib::ActionButton* button_of(const std::vector<lib::ActionButton>& buttons, Action action) {
    for (const lib::ActionButton& button : buttons)
        if (button.action == action)
            return &button;
    return nullptr;
}

/// Tells whether the visible rows include an entry.
///
/// @param library the Library
/// @param key the key
/// @param registry the registry
/// @return true when a row shows it
bool row_visible(const lib::Library& library, std::string_view key, std::string_view registry) {
    const std::string wanted = std::string(key) + "@" + std::string(registry);
    const std::vector<std::string> keys = visible_keys(library);
    return std::find(keys.begin(), keys.end(), wanted) != keys.end();
}

/// Copies the retail expansions a package's TURN ON press can ask for.
///
/// @param[in,out] inputs the inputs
void copied_retail(lib::Inputs& inputs) {
    inputs.content.base_id = "totala";
    inputs.content.copied = {"campaign", "core-contingency", "battle-tactics", "music"};
}

/// A mod's games and the expansions it needs, on the row, the facts and TURN ON.
void test_package_needs() {
    const auto ridge_mod = [](lib::Inputs& inputs, bool installed_copy) {
        lib::Listing listed = listing(core, Kind::mod, "ridge", "Ridge");
        listed.supported_games = {"totala"};
        listed.required_expansions = {"battle-tactics"};
        inputs.listings.push_back(listed);
        if (!installed_copy)
            return;
        lib::Installed have = installed(core, Kind::mod, "ridge", "Ridge");
        have.supported_games = {"totala"};
        have.required_expansions = {"battle-tactics"};
        inputs.installed.push_back(have);
    };

    lib::Inputs inputs = base_inputs();
    inputs.registries[0].downloads_on = true;
    ridge_mod(inputs, true);
    copied_retail(inputs);
    inputs.content.enabled = {"campaign", "core-contingency"};
    lib::Library library = built(inputs);
    const lib::Entry* entry = at(library, core, Kind::mod, "ridge");
    OA_CHECK(entry != nullptr && row_visible(library, "ridge", core));
    if (entry == nullptr)
        return;
    OA_CHECK_TEXT(lib::content_lines(library, *entry).row, "Needs Battle Tactics, which is off");
    const std::vector<lib::ActionButton> before = lib::entry_actions(library, *entry);
    const lib::ActionButton* turn = button_of(before, Action::turn_on);
    const lib::ActionButton* play = button_of(before, Action::play_now);
    OA_CHECK(turn != nullptr && turn == &before.front() && turn->enabled);
    if (turn != nullptr)
        OA_CHECK_TEXT(turn->text, "TURN ON Battle Tactics");
    OA_CHECK(play != nullptr && !play->enabled);
    lib::select(library, entry->id);
    OA_CHECK_TEXT(
        lib::details_note(library),
        "Turning it on applies when the Library closes, and then PLAY NOW works."
    );
    OA_CHECK(lib::turn_on_required(library, *entry));
    OA_CHECK(holds_id(library.content.expansions, "battle-tactics"));
    OA_CHECK(holds_id(library.content.expansions, "campaign"));
    OA_CHECK(holds_id(library.content.expansions, "core-contingency"));
    const std::vector<lib::ActionButton> after = lib::entry_actions(library, *entry);
    OA_CHECK(button_of(after, Action::turn_on) == nullptr);
    const lib::ActionButton* play_after = button_of(after, Action::play_now);
    OA_CHECK(play_after != nullptr && play_after->enabled);
    OA_CHECK_TEXT(lib::content_lines(library, *entry).row, "Needs Battle Tactics · on");
    // A picture that arrives after TURN ON keeps it on the draft: the row
    // still reads on, and PLAY NOW stays on.
    lib::Inputs pictured = library.inputs;
    pictured.base_game_badge = lib::Badge{std::vector<uint8_t>(1 * 1 * 4, 9), 1, 1};
    lib::refresh(library, std::move(pictured));
    const lib::Entry* turned = at(library, core, Kind::mod, "ridge");
    OA_CHECK(turned != nullptr);
    if (turned != nullptr) {
        OA_CHECK_TEXT(lib::content_lines(library, *turned).row, "Needs Battle Tactics · on");
        const std::vector<lib::ActionButton> kept = lib::entry_actions(library, *turned);
        const lib::ActionButton* play_kept = button_of(kept, Action::play_now);
        OA_CHECK(play_kept != nullptr && play_kept->enabled);
    }

    // Battle Tactics pulls Campaign on with it, and not Core Contingency.
    lib::Inputs waiting = base_inputs();
    ridge_mod(waiting, true);
    copied_retail(waiting);
    lib::Library closed = built(std::move(waiting));
    const lib::Entry* tactics = at(closed, core, Kind::mod, "ridge");
    OA_CHECK(tactics != nullptr);
    if (tactics != nullptr) {
        OA_CHECK(lib::turn_on_required(closed, *tactics));
        OA_CHECK(holds_id(closed.content.expansions, "campaign"));
        OA_CHECK(holds_id(closed.content.expansions, "battle-tactics"));
        OA_CHECK(!holds_id(closed.content.expansions, "core-contingency"));
    }

    lib::Inputs demo = base_inputs();
    lib::Listing demo_listed = listing(core, Kind::mod, "ridge", "Ridge");
    demo_listed.supported_games = {"totala-demo"};
    demo.listings.push_back(demo_listed);
    lib::Installed demo_have = installed(core, Kind::mod, "ridge", "Ridge");
    demo_have.supported_games = {"totala-demo"};
    demo.installed.push_back(demo_have);
    copied_retail(demo);
    lib::Library demo_library = built(std::move(demo));
    const lib::Entry* demo_entry = at(demo_library, core, Kind::mod, "ridge");
    OA_CHECK(demo_entry != nullptr && row_visible(demo_library, "ridge", core));
    if (demo_entry != nullptr) {
        const lib::ContentLines lines = lib::content_lines(demo_library, *demo_entry);
        OA_CHECK_TEXT(lines.row, "For the Total Annihilation demo only");
        OA_CHECK(lines.turn_on.empty() && lines.blocks_play);
        OA_CHECK_TEXT(lines.games, "Total Annihilation demo");
        OA_CHECK(lines.games_mark == Mark::warn);
        const std::vector<lib::ActionButton> demo_buttons =
            lib::entry_actions(demo_library, *demo_entry);
        const lib::ActionButton* demo_play = button_of(demo_buttons, Action::play_now);
        OA_CHECK(
            demo_play != nullptr && !demo_play->enabled &&
            button_of(demo_buttons, Action::turn_on) == nullptr
        );
    }

    lib::Inputs met = base_inputs();
    lib::Listing met_listed = listing(core, Kind::mod, "ridge", "Ridge");
    met_listed.supported_games = {"totala"};
    met_listed.required_expansions = {"core-contingency"};
    met.listings.push_back(met_listed);
    lib::Installed met_have = installed(core, Kind::mod, "ridge", "Ridge");
    met_have.supported_games = {"totala"};
    met_have.required_expansions = {"core-contingency"};
    met.installed.push_back(met_have);
    copied_retail(met);
    met.content.enabled = {"campaign", "core-contingency"};
    lib::Library met_library = built(std::move(met));
    const lib::Entry* met_entry = at(met_library, core, Kind::mod, "ridge");
    OA_CHECK(met_entry != nullptr);
    if (met_entry != nullptr) {
        OA_CHECK_TEXT(
            lib::content_lines(met_library, *met_entry).row, "Needs Core Contingency · on"
        );
        const std::vector<lib::ActionButton> met_buttons =
            lib::entry_actions(met_library, *met_entry);
        const lib::ActionButton* met_play = button_of(met_buttons, Action::play_now);
        OA_CHECK(met_play != nullptr && met_play->enabled);
        OA_CHECK(button_of(met_buttons, Action::turn_on) == nullptr);
    }

    lib::Inputs offered = base_inputs();
    offered.registries[0].downloads_on = true;
    ridge_mod(offered, false);
    copied_retail(offered);
    lib::Library offer_library = built(std::move(offered));
    const lib::Entry* offer = at(offer_library, core, Kind::mod, "ridge");
    OA_CHECK(offer != nullptr && row_visible(offer_library, "ridge", core));
    if (offer != nullptr) {
        const lib::ContentLines lines = lib::content_lines(offer_library, *offer);
        OA_CHECK_TEXT(lines.row, "Needs Battle Tactics, which is off");
        OA_CHECK_TEXT(lines.games, "Total Annihilation ✓");
        OA_CHECK_TEXT(lines.expansions, "Battle Tactics off");
        const std::vector<lib::ActionButton> offer_buttons =
            lib::entry_actions(offer_library, *offer);
        const lib::ActionButton* get = button_of(offer_buttons, Action::get);
        OA_CHECK(get != nullptr && get->enabled);
        OA_CHECK(button_of(offer_buttons, Action::play_now) == nullptr);
        OA_CHECK(button_of(offer_buttons, Action::turn_on) != nullptr);
    }

    lib::Inputs packs = base_inputs();
    packs.registries[0].downloads_on = true;
    lib::Listing map_listed = listing(core, Kind::map_pack, "ridge-maps", "Ridge Maps");
    map_listed.supported_games = {"totala"};
    map_listed.required_expansions = {"battle-tactics"};
    lib::MapLine map_line;
    map_line.name = "Ridgeline";
    map_line.players = 2;
    map_line.size = "16x16";
    map_listed.maps.push_back(map_line);
    packs.listings.push_back(map_listed);
    lib::Installed map_have = installed(core, Kind::map_pack, "ridge-maps", "Ridge Maps");
    map_have.supported_games = {"totala"};
    map_have.required_expansions = {"battle-tactics"};
    packs.installed.push_back(map_have);
    lib::Listing language_listed = listing(core, Kind::language, "de", "Deutsch");
    language_listed.supported_games = {"totala"};
    language_listed.required_expansions = {"battle-tactics"};
    packs.listings.push_back(language_listed);
    lib::Installed language_have = installed(core, Kind::language, "de", "Deutsch");
    language_have.supported_games = {"totala"};
    language_have.required_expansions = {"battle-tactics"};
    packs.installed.push_back(language_have);
    copied_retail(packs);
    lib::Library pack_library = built(std::move(packs));
    lib::set_tab(pack_library, lib::Tab::maps);
    const lib::Entry* map_entry = at(pack_library, core, Kind::map_pack, "ridge-maps");
    OA_CHECK(map_entry != nullptr && row_visible(pack_library, "ridge-maps", core));
    if (map_entry != nullptr) {
        OA_CHECK_TEXT(
            lib::content_lines(pack_library, *map_entry).row, "Needs Battle Tactics, which is off"
        );
        const std::vector<lib::ActionButton> map_buttons =
            lib::entry_actions(pack_library, *map_entry);
        OA_CHECK(button_of(map_buttons, Action::play_now) == nullptr);
        OA_CHECK(button_of(map_buttons, Action::turn_on) != nullptr);
        lib::select(pack_library, map_entry->id);
        OA_CHECK_TEXT(
            lib::details_note(pack_library), "Turning it on applies when the Library closes."
        );
    }
    lib::set_tab(pack_library, lib::Tab::languages);
    const lib::Entry* language_entry = at(pack_library, core, Kind::language, "de");
    OA_CHECK(language_entry != nullptr && row_visible(pack_library, "de", core));
    if (language_entry != nullptr) {
        const std::vector<lib::ActionButton> language_buttons =
            lib::entry_actions(pack_library, *language_entry);
        const lib::ActionButton* use = button_of(language_buttons, Action::use_now);
        OA_CHECK(use != nullptr && !use->enabled);
        lib::select(pack_library, language_entry->id);
        OA_CHECK_TEXT(
            lib::details_note(pack_library),
            "Turning it on applies when the Library closes, and then USE NOW works."
        );
        OA_CHECK(lib::turn_on_required(pack_library, *language_entry));
        const std::vector<lib::ActionButton> used_buttons =
            lib::entry_actions(pack_library, *language_entry);
        const lib::ActionButton* used = button_of(used_buttons, Action::use_now);
        OA_CHECK(used != nullptr && used->enabled);
    }

    lib::Inputs missing = base_inputs();
    ridge_mod(missing, true);
    missing.content.base_id = "totala";
    missing.content.copied = {"campaign", "core-contingency"};
    lib::Library missing_library = built(std::move(missing));
    const lib::Entry* missing_entry = at(missing_library, core, Kind::mod, "ridge");
    OA_CHECK(missing_entry != nullptr);
    if (missing_entry != nullptr) {
        const lib::ContentLines lines = lib::content_lines(missing_library, *missing_entry);
        OA_CHECK_TEXT(lines.row, "Needs Battle Tactics, which is off");
        OA_CHECK(lines.turn_on.empty());
    }

    lib::Inputs locked = base_inputs();
    ridge_mod(locked, true);
    copied_retail(locked);
    locked.content.locked = true;
    lib::Library locked_library = built(locked);
    const lib::Entry* locked_entry = at(locked_library, core, Kind::mod, "ridge");
    OA_CHECK(locked_entry != nullptr);
    if (locked_entry != nullptr) {
        OA_CHECK(lib::content_lines(locked_library, *locked_entry).turn_on.empty());
        const std::vector<lib::ActionButton> locked_buttons =
            lib::entry_actions(locked_library, *locked_entry);
        const lib::ActionButton* locked_play = button_of(locked_buttons, Action::play_now);
        OA_CHECK(locked_play != nullptr && !locked_play->enabled);
    }
    locked.in_match = true;
    locked.content.locked = false;
    lib::Library during = built(locked);
    const lib::Entry* during_entry = at(during, core, Kind::mod, "ridge");
    OA_CHECK(during_entry != nullptr);
    if (during_entry != nullptr) {
        OA_CHECK(lib::content_lines(during, *during_entry).turn_on.empty());
        const std::vector<lib::ActionButton> during_buttons =
            lib::entry_actions(during, *during_entry);
        const lib::ActionButton* during_play = button_of(during_buttons, Action::play_now);
        OA_CHECK(during_play != nullptr && !during_play->enabled);
    }
    locked.in_match = false;
    locked.mod_from_command_line = true;
    locked.content.enabled = {"campaign", "core-contingency", "battle-tactics"};
    lib::Library commanded = built(std::move(locked));
    const lib::Entry* commanded_entry = at(commanded, core, Kind::mod, "ridge");
    OA_CHECK(commanded_entry != nullptr);
    if (commanded_entry != nullptr) {
        OA_CHECK(lib::content_lines(commanded, *commanded_entry).turn_on.empty());
        const std::vector<lib::ActionButton> commanded_buttons =
            lib::entry_actions(commanded, *commanded_entry);
        const lib::ActionButton* commanded_play = button_of(commanded_buttons, Action::play_now);
        OA_CHECK(commanded_play != nullptr && !commanded_play->enabled);
    }

    lib::Inputs cleared = base_inputs();
    cleared.content.expansion_cleared = true;
    lib::Library cleared_library = built(std::move(cleared));
    OA_CHECK(cleared_library.expansion_note);
    OA_CHECK_TEXT(
        lib::active_content(cleared_library).hint,
        "The mod was unloaded because an expansion it needs is off. The game plays with No Mod."
    );

    lib::Inputs played_inputs = base_inputs();
    lib::Installed played = installed(core, Kind::mod, "ridge", "Ridge");
    played.playing = true;
    played.required_expansions = {"campaign"};
    played.supported_games = {"totala"};
    played_inputs.installed.push_back(played);
    copied_retail(played_inputs);
    played_inputs.content.enabled = {"campaign"};
    played_inputs.content.mod_required = {"campaign"};
    lib::PlayingMod playing;
    playing.name = "Ridge";
    played_inputs.playing_mod = playing;
    lib::Library played_library = built(std::move(played_inputs));
    const lib::ContentPress asked = lib::press_expansion(played_library, "campaign");
    OA_CHECK(asked.kind == lib::ContentPressKind::ask);
    lib::accept_expansion_off(played_library);
    OA_CHECK_TEXT(
        lib::active_content(played_library).hint,
        "Ridge was unloaded because Campaign is off. The game plays with No Mod."
    );
    const lib::ContentPress restored = lib::press_expansion(played_library, "campaign");
    OA_CHECK(restored.kind == lib::ContentPressKind::redraw);
    OA_CHECK(played_library.content.unloaded_mod.empty());
    OA_CHECK(
        lib::active_content(played_library).hint == "Campaign turns on when the Library closes."
    );

    lib::Inputs ranged = base_inputs();
    lib::Listing ranged_listed = listing(core, Kind::mod, "ridge", "Ridge");
    ranged_listed.requires_engine = ">= 0.8.0";
    ranged_listed.base = "ta-3.1c";
    ranged_listed.supported_games = {"totala", "totala-demo"};
    ranged_listed.required_expansions = {"campaign"};
    ranged.listings.push_back(ranged_listed);
    lib::Installed ranged_have = installed(core, Kind::mod, "ridge", "Ridge");
    ranged_have.supported_games = {"totala", "totala-demo"};
    ranged_have.required_expansions = {"campaign"};
    ranged.installed.push_back(ranged_have);
    copied_retail(ranged);
    ranged.content.enabled = {"campaign"};
    lib::Library ranged_library = built(std::move(ranged));
    const lib::Entry* ranged_entry = at(ranged_library, core, Kind::mod, "ridge");
    OA_CHECK(ranged_entry != nullptr);
    if (ranged_entry != nullptr) {
        const std::vector<lib::Fact> facts = lib::facts(ranged_library, *ranged_entry);
        std::vector<std::string> labels;
        int needs_seen = 0;
        for (const lib::Fact& item : facts) {
            labels.push_back(item.long_label);
            if (item.long_label != "Needs")
                continue;
            if (needs_seen == 0)
                OA_CHECK_TEXT(item.long_value, "Open Annihilation 0.8.0 or later");
            if (needs_seen == 1) {
                OA_CHECK_TEXT(item.long_value, "Campaign ✓");
                OA_CHECK(item.mark == Mark::none);
            }
            ++needs_seen;
        }
        OA_CHECK(needs_seen == 2);
        OA_CHECK(
            std::find(labels.begin(), labels.end(), "Games") != labels.end() &&
            std::find(labels.begin(), labels.end(), "Base") <
                std::find(labels.begin(), labels.end(), "Games") &&
            std::find(labels.begin(), labels.end(), "Games") <
                std::find(labels.begin(), labels.end(), "Needs")
        );
        const lib::ContentLines lines = lib::content_lines(ranged_library, *ranged_entry);
        OA_CHECK_TEXT(lines.games, "Total Annihilation ✓ · Total Annihilation demo");
        OA_CHECK(!lines.row.empty());
        OA_CHECK_TEXT(lines.row, "Needs Campaign · on");
    }
}

/// A required expansion unloads the mod, then another base game is chosen.
/// The draft holds one choice, so the close stores it once.
void test_base_game_after_required_off() {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "totala-demo", "campaign"};
    inputs.content.enabled = {"campaign"};
    inputs.content.mod_required = {"campaign"};
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    lib::Library library;
    lib::refresh(library, inputs);

    OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
    lib::accept_expansion_off(library);
    OA_CHECK(library.content.unload_mod);
    OA_CHECK(!library.content.pending_base);
    OA_CHECK(!library.content.applied);
    OA_CHECK(library.content.clear_reason == lib::PendingClear::required_expansion_off);
    OA_CHECK(library.content.unloaded_mod == "Ridge");
    OA_CHECK(library.content.unloaded_for == "campaign");
    OA_CHECK_TEXT(
        lib::active_content(library).hint,
        "Ridge was unloaded because Campaign is off. The game plays with No Mod."
    );
    OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");

    OA_CHECK(lib::open_base_chooser(library).opened);
    lib::mark_base_game(library, "totala-demo");
    OA_CHECK(library.chooser.note.find("Ridge") == std::string::npos);
    OA_CHECK(library.chooser.note.find("Library closes") != std::string::npos);
    const lib::BaseUse used = lib::use_marked_base_game(library);
    OA_CHECK(used.kind == lib::BaseUseKind::applied);
    OA_CHECK(!library.chooser.open);
    OA_CHECK(library.content.base_game == "totala-demo");
    OA_CHECK(library.content.expansions.empty());
    OA_CHECK(library.content.pending_base);
    OA_CHECK(library.content.unload_mod);
    OA_CHECK(library.content.clear_reason == lib::PendingClear::required_expansion_off);
    OA_CHECK(library.content.unloaded_mod == "Ridge");
    OA_CHECK(library.content.unloaded_for == "campaign");
    OA_CHECK(library.content.edited);
    OA_CHECK(!library.content.applied);
    OA_CHECK_TEXT(
        lib::active_content(library).hint, "This base game is used when the Library closes."
    );
    OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");

    // The host applies this one draft on close, then a second close finds it applied.
    const bool first_close = library.content.edited && !library.content.applied;
    OA_CHECK(first_close);
    library.content.applied = true;
    library.content.edited = false;
    const bool second_close = library.content.edited && !library.content.applied;
    OA_CHECK(!second_close);
    OA_CHECK(library.content.clear_reason == lib::PendingClear::required_expansion_off);
    OA_CHECK(library.content.base_game == "totala-demo");
    OA_CHECK(library.content.unload_mod);
}

/// What closing the Library stores, read from the draft as the host's close
/// reads it. An edited draft that is not yet applied stores its base game and
/// expansions, and clears the mod while it unloads one, unless the command
/// line chose the mod. Any other draft stores nothing, so the stored choice
/// and the mod played stay.
struct Closing {
    bool stores{};
    std::string base_game{};
    std::vector<std::string> expansions{};
    bool clears_mod{};
};

/// Returns what closing the Library stores.
///
/// @param library the Library
/// @return what the close stores
Closing closing(const lib::Library& library) {
    const lib::ContentDraft& draft = library.content;
    Closing stored_on_close;
    stored_on_close.stores = draft.edited && !draft.applied;
    if (!stored_on_close.stores)
        return stored_on_close;
    stored_on_close.base_game = draft.base_game.empty() ? draft.seed_base : draft.base_game;
    stored_on_close.expansions = draft.expansions;
    stored_on_close.clears_mod =
        draft.clear_reason != lib::PendingClear::none && !library.inputs.mod_from_command_line;
    return stored_on_close;
}

/// Writes a base game's version, its expansions that are on and the mod line
/// as one text, for a message.
///
/// @param version the version line
/// @param on the expansions that are on
/// @param mod the mod line
/// @return the text, such as "3.1c with campaign music, Ridge"
std::string
described(std::string_view version, const std::vector<std::string>& on, std::string_view mod) {
    std::string text{version};
    for (std::size_t place = 0; place < on.size(); ++place)
        text += (place == 0 ? " with " : " ") + on[place];
    text += ", ";
    text += mod;
    return text;
}

/// Compares the Active tab with what the game plays once the Library closes:
/// the base game, every expansion pill and the mod.
///
/// @param library the Library
/// @param mod the mod line while the mod played stays
/// @return empty when the tile agrees; otherwise what each says
std::string tile_difference(const lib::Library& library, std::string_view mod) {
    const Closing stored_on_close = closing(library);
    const lib::ContentSnapshot& stored = library.inputs.content;
    std::string base = stored_on_close.stores ? stored_on_close.base_game : stored.base_id;
    if (base.empty())
        base = "totala";
    const std::vector<std::string>& on =
        stored_on_close.stores ? stored_on_close.expansions : stored.enabled;
    const std::string_view mod_after =
        stored_on_close.clears_mod ? std::string_view{"No Mod"} : mod;

    const lib::ActiveContent face = lib::active_content(library);
    const std::string_view version = base == "totala-demo" ? "Demo" : "3.1c";
    bool agrees = face.version_line == version;
    std::vector<std::string> shown_on;
    for (const lib::ExpansionPill& pill : face.pills) {
        const bool pill_on = pill.face == lib::ExpansionFace::on;
        if (pill_on)
            shown_on.push_back(pill.id);
        if (pill_on != holds_id(on, pill.id))
            agrees = false;
    }
    if (shown_on.size() != on.size())
        agrees = false;
    const std::string mod_line = lib::active_setup(library).mod_line;
    if (mod_line != mod_after)
        agrees = false;
    if (agrees)
        return {};
    return "the Active tab shows \"" + described(face.version_line, shown_on, mod_line) +
           "\", closing the Library leaves \"" + described(version, on, mod_after) + "\"";
}

/// Checks that the Active tab shows what the game plays once the Library
/// closes, printing the difference when it does not.
///
/// @param library the Library
/// @param mod the mod line while the mod played stays
/// @param steps what the player did, for the message
/// @return true when the tile agrees
bool tile_applies(const lib::Library& library, std::string_view mod, std::string_view steps) {
    const std::string difference = tile_difference(library, mod);
    if (difference.empty())
        return true;
    std::fprintf(
        stderr, "after %.*s: %s\n", static_cast<int>(steps.size()), steps.data(), difference.c_str()
    );
    return false;
}

/// Opens the base game chooser, marks a game and presses USE THIS GAME.
///
/// @param[in,out] library the Library
/// @param id the base game
/// @return what USE THIS GAME did; none when the chooser did not open
lib::BaseUseKind use_base_game(lib::Library& library, std::string_view id) {
    if (!lib::open_base_chooser(library).opened)
        return lib::BaseUseKind::none;
    lib::mark_base_game(library, id);
    const lib::BaseUse used = lib::use_marked_base_game(library);
    lib::close_base_chooser(library);
    return used.kind;
}

/// The inputs of a Library playing Ridge on 3.1c with the demo copied.
///
/// @param enabled the expansions stored as on
/// @param required the expansions Ridge needs
/// @return the inputs
lib::Inputs ridge_on_retail(std::vector<std::string> enabled, std::vector<std::string> required) {
    lib::Inputs inputs;
    inputs.content.base_id = "totala";
    inputs.content.copied = {"totala", "totala-demo", "campaign", "core-contingency", "music"};
    inputs.content.enabled = std::move(enabled);
    inputs.content.mod_required = std::move(required);
    lib::Installed ridge;
    ridge.kind = Kind::mod;
    ridge.key = "ridge";
    ridge.folder = "ridge";
    ridge.name = "Ridge";
    ridge.playing = true;
    inputs.installed.push_back(ridge);
    return inputs;
}

/// The same inputs once the catalogue and the base game's picture arrive.
///
/// @param inputs the inputs
/// @return them, with a listing and a picture
lib::Inputs arrived_over(const lib::Inputs& inputs) {
    lib::Inputs arrived = inputs;
    arrived.registries = fixture_registries();
    arrived.listings.push_back(listing(core, Kind::mod, "basalt", "Basalt"));
    arrived.base_game_badge = lib::Badge{std::vector<uint8_t>(1 * 1 * 4, 9), 1, 1};
    return arrived;
}

/// Choosing another base game and then the stored one again leaves the
/// Active tab on what closing the Library applies. Back on the stored base
/// game the mod plays again, and a refresh leaves the tile as it is.
void test_base_game_round_trip() {
    // Every copied expansion is on, so the round trip ends on the stored
    // choice: closing stores nothing, and Ridge stays.
    {
        const lib::Inputs inputs =
            ridge_on_retail({"campaign", "core-contingency", "music"}, {"campaign"});
        lib::Library library;
        lib::refresh(library, inputs);
        const std::string opened_hint = lib::active_content(library).hint;
        OA_CHECK(use_base_game(library, "totala-demo") == lib::BaseUseKind::applied);
        OA_CHECK(library.content.unload_mod && library.content.edited);
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");
        OA_CHECK(tile_applies(library, "Ridge", "the demo"));

        OA_CHECK(use_base_game(library, "totala") == lib::BaseUseKind::applied);
        OA_CHECK(!library.content.pending_base);
        OA_CHECK(!library.content.unload_mod);
        OA_CHECK(!library.content.edited);
        OA_CHECK(library.content.clear_reason == lib::PendingClear::none);
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "Ridge");
        OA_CHECK_TEXT(lib::active_content(library).hint, opened_hint);
        OA_CHECK(tile_applies(library, "Ridge", "the demo, then 3.1c"));

        lib::refresh(library, arrived_over(inputs));
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "Ridge");
        OA_CHECK(tile_applies(library, "Ridge", "the demo, 3.1c and a refresh"));
    }

    // Core Contingency is stored off. 3.1c again turns on all of its
    // expansions, and Ridge stays.
    {
        const lib::Inputs inputs = ridge_on_retail({"campaign"}, {"campaign"});
        lib::Library library;
        lib::refresh(library, inputs);
        OA_CHECK(use_base_game(library, "totala-demo") == lib::BaseUseKind::applied);
        OA_CHECK(use_base_game(library, "totala") == lib::BaseUseKind::applied);
        OA_CHECK(library.content.edited);
        OA_CHECK(!library.content.unload_mod);
        OA_CHECK(holds_id(library.content.expansions, "core-contingency"));
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "Ridge");
        OA_CHECK(!closing(library).clears_mod);
        OA_CHECK(tile_applies(library, "Ridge", "the demo, then 3.1c"));
        lib::refresh(library, arrived_over(inputs));
        OA_CHECK(tile_applies(library, "Ridge", "the demo, 3.1c and a refresh"));
    }

    // TURN OFF unloads Ridge for Campaign. The demo keeps it unloaded, and
    // 3.1c again turns Campaign back on, so Ridge plays again.
    {
        const lib::Inputs inputs =
            ridge_on_retail({"campaign", "core-contingency", "music"}, {"campaign"});
        lib::Library library;
        lib::refresh(library, inputs);
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
        lib::accept_expansion_off(library);
        OA_CHECK(tile_applies(library, "Ridge", "TURN OFF"));
        OA_CHECK(use_base_game(library, "totala-demo") == lib::BaseUseKind::applied);
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");
        OA_CHECK(tile_applies(library, "Ridge", "TURN OFF, then the demo"));
        OA_CHECK(use_base_game(library, "totala") == lib::BaseUseKind::applied);
        OA_CHECK(holds_id(library.content.expansions, "campaign"));
        OA_CHECK(!library.content.unload_mod && !library.content.edited);
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "Ridge");
        OA_CHECK(tile_applies(library, "Ridge", "TURN OFF, the demo, then 3.1c"));
        lib::refresh(library, arrived_over(inputs));
        OA_CHECK(tile_applies(library, "Ridge", "TURN OFF, the demo, 3.1c and a refresh"));
    }

    // Ridge needs Campaign, which is stored off. Turning Campaign on and then
    // off again with TURN OFF ends on the stored expansions, but Ridge is
    // unloaded, so closing still has that to apply, through a refresh too.
    {
        const lib::Inputs inputs = ridge_on_retail({"music"}, {"campaign"});
        lib::Library library;
        lib::refresh(library, inputs);
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::redraw);
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
        lib::accept_expansion_off(library);
        OA_CHECK(library.content.unload_mod);
        OA_CHECK(library.content.edited);
        OA_CHECK(closing(library).clears_mod);
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");
        OA_CHECK(tile_applies(library, "Ridge", "Campaign on, then TURN OFF"));
        lib::refresh(library, arrived_over(inputs));
        OA_CHECK_TEXT(lib::active_setup(library).mod_line, "No Mod");
        OA_CHECK(tile_applies(library, "Ridge", "Campaign on, TURN OFF and a refresh"));
    }
}

/// One thing the player does on the Active tab, for the walk below.
enum class ActiveStep : uint8_t {
    campaign,         ///< presses the Campaign pill
    core_contingency, ///< presses the Core Contingency pill
    turn_off,         ///< TURN OFF on the expansion-off question
    cancel,           ///< CANCEL on that question
    demo,             ///< uses the demo in the base game chooser
    retail,           ///< uses 3.1c in the base game chooser
    refresh,          ///< the catalogue and a picture arrive
};

/// How many kinds of step the walk takes.
constexpr std::size_t active_step_count = 7;

/// Takes one step on the Active tab.
///
/// @param[in,out] library the Library
/// @param step the step
/// @param arrived the inputs a refresh brings
/// @return the step's words, for a message
std::string_view take_step(lib::Library& library, ActiveStep step, const lib::Inputs& arrived) {
    switch (step) {
    case ActiveStep::campaign:
        static_cast<void>(lib::press_expansion(library, "campaign"));
        return "Campaign";
    case ActiveStep::core_contingency:
        static_cast<void>(lib::press_expansion(library, "core-contingency"));
        return "Core Contingency";
    case ActiveStep::turn_off:
        lib::accept_expansion_off(library);
        return "TURN OFF";
    case ActiveStep::cancel:
        lib::cancel_expansion_off(library);
        return "CANCEL";
    case ActiveStep::demo:
        static_cast<void>(use_base_game(library, "totala-demo"));
        return "the demo";
    case ActiveStep::retail:
        static_cast<void>(use_base_game(library, "totala"));
        return "3.1c";
    case ActiveStep::refresh:
        lib::refresh(library, arrived);
        return "a refresh";
    }
    return {};
}

/// Every sequence of five steps over pills, the expansion-off question, the
/// base game chooser and refreshes leaves the Active tab on what closing the
/// Library applies, after each step. Ridge is played over stored choices
/// that do and do not hold what it needs, and once as the command line's
/// mod, and one Library plays no mod.
void test_tile_applies_after_any_choices() {
    struct Start {
        lib::Inputs inputs{};
        std::string mod{};
    };

    std::vector<Start> starts;
    starts.push_back(
        {ridge_on_retail({"campaign", "core-contingency", "music"}, {"campaign"}), "Ridge"}
    );
    starts.push_back({ridge_on_retail({"campaign"}, {"campaign"}), "Ridge"});
    starts.push_back({ridge_on_retail({"campaign", "core-contingency"}, {}), "Ridge"});
    starts.push_back({ridge_on_retail({}, {"campaign"}), "Ridge"});
    lib::Inputs no_mod = ridge_on_retail({"campaign"}, {});
    no_mod.installed.clear();
    starts.push_back({std::move(no_mod), "No Mod"});
    lib::Inputs named = ridge_on_retail({"campaign", "core-contingency"}, {"campaign"});
    named.mod_from_command_line = true;
    starts.push_back({named, "Ridge"});

    // Closing the Library keeps the mod the command line chose, so Campaign,
    // which Ridge needs, stays on without asking. Other pills still turn.
    {
        lib::Library library;
        lib::refresh(library, named);
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::none);
        OA_CHECK(!library.content.asking && !library.content.edited);
        const lib::ActiveContent face = lib::active_content(library);
        const lib::ExpansionPill* campaign = pill_of(face, "campaign");
        OA_CHECK(campaign != nullptr && campaign->face == lib::ExpansionFace::on);
        OA_CHECK(
            lib::press_expansion(library, "core-contingency").kind == lib::ContentPressKind::redraw
        );
        OA_CHECK(tile_applies(library, "Ridge", "Campaign, then Core Contingency"));
    }

    constexpr std::size_t depth = 5;
    std::size_t sequences = 1;
    for (std::size_t place = 0; place < depth; ++place)
        sequences *= active_step_count;
    int32_t disagreements = 0;
    std::vector<std::string> printed;
    constexpr std::size_t most_printed = 5;
    for (const Start& start : starts) {
        const lib::Inputs arrived = arrived_over(start.inputs);
        for (std::size_t sequence = 0; sequence < sequences; ++sequence) {
            lib::Library library;
            lib::refresh(library, start.inputs);
            std::string steps;
            std::size_t rest = sequence;
            for (std::size_t place = 0; place < depth; ++place) {
                const auto step = static_cast<ActiveStep>(rest % active_step_count);
                rest /= active_step_count;
                if (!steps.empty())
                    steps += ", ";
                steps += take_step(library, step, arrived);
                const std::string difference = tile_difference(library, start.mod);
                if (difference.empty())
                    continue;
                // Sequences that share a first part fail on it alike, so
                // each part is printed once.
                if (printed.size() < most_printed && !holds_id(printed, steps)) {
                    std::fprintf(stderr, "after %s: %s\n", steps.c_str(), difference.c_str());
                    printed.push_back(steps);
                }
                ++disagreements;
                break;
            }
        }
    }
    if (disagreements > 0)
        std::fprintf(stderr, "%d sequences disagree\n", static_cast<int>(disagreements));
    OA_CHECK(disagreements == 0);
}

/// Tells whether the command line is what refuses a pill's press: the press
/// does nothing as the Library stands, and the same press, with the mod
/// chosen in the game instead, asks before unloading it. Neither press is
/// kept.
///
/// @param library the Library
/// @param id the expansion
/// @return true when the command line refuses it
bool command_line_refuses(const lib::Library& library, std::string_view id) {
    lib::Library pressed = library;
    if (lib::press_expansion(pressed, id).kind != lib::ContentPressKind::none)
        return false;
    lib::Library chosen_in_game = library;
    chosen_in_game.inputs.mod_from_command_line = false;
    return lib::press_expansion(chosen_in_game, id).kind == lib::ContentPressKind::ask;
}

/// Returns the name an expansion of 3.1c shows.
///
/// @param id the expansion
/// @return its name; the id itself for one the walk does not use
std::string expansion_name(std::string_view id) {
    if (id == "campaign")
        return "Campaign";
    if (id == "core-contingency")
        return "Core Contingency";
    if (id == "battle-tactics")
        return "Battle Tactics";
    if (id == "music")
        return "Music";
    return std::string(id);
}

/// Compares every pill's lock with whether the command line refuses its
/// press, and the words under the pills with the locked pills.
///
/// @param library the Library
/// @return empty when they agree; otherwise what differs
std::string lock_difference(const lib::Library& library) {
    const lib::ActiveContent face = lib::active_content(library);
    std::vector<std::string> locked_names;
    for (const lib::ExpansionPill& pill : face.pills) {
        const bool refused = command_line_refuses(library, pill.id);
        if (pill.locked != refused)
            return pill.id + (pill.locked ? " is locked, and its press is not refused"
                                          : " is not locked, and its press is refused");
        if (!pill.locked)
            continue;
        if (pill.enabled || pill.face != lib::ExpansionFace::on)
            return pill.id + " is locked, and takes a press or is off";
        locked_names.push_back(expansion_name(pill.id));
    }
    std::string expected;
    if (!locked_names.empty()) {
        std::string names = locked_names.front();
        for (std::size_t place = 1; place < locked_names.size(); ++place)
            names += (place + 1 == locked_names.size() ? " and " : ", ") + locked_names[place];
        expected = "The command line chose this run's mod, which needs " + names + ".";
    }
    if (face.lock != expected)
        return "the lock reads \"" + face.lock + "\", not \"" + expected + "\"";
    return {};
}

/// A pill whose expansion the command line's mod needs is locked, with the
/// reason under the pills, exactly while the command line is what refuses
/// its press. Its press stays refused and changes nothing.
void test_command_line_lock() {
    // Ridge needs Campaign. Turning off Core Contingency or turning on
    // Music still works.
    lib::Inputs named = ridge_on_retail({"campaign", "core-contingency"}, {"campaign"});
    named.mod_from_command_line = true;
    {
        lib::Library library;
        lib::refresh(library, named);
        const lib::ActiveContent face = lib::active_content(library);
        const lib::ExpansionPill* campaign = pill_of(face, "campaign");
        const lib::ExpansionPill* contingency = pill_of(face, "core-contingency");
        const lib::ExpansionPill* music = pill_of(face, "music");
        OA_CHECK(campaign != nullptr && campaign->locked && !campaign->enabled);
        OA_CHECK(campaign != nullptr && campaign->face == lib::ExpansionFace::on);
        OA_CHECK(contingency != nullptr && !contingency->locked && contingency->enabled);
        OA_CHECK(music != nullptr && !music->locked && music->enabled);
        OA_CHECK_TEXT(face.lock, "The command line chose this run's mod, which needs Campaign.");
        OA_CHECK(face.change_enabled == false);
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::none);
        OA_CHECK(!library.content.edited && !library.content.asking);
        OA_CHECK(!library.content.unload_mod);
        const lib::ActiveContent refused = lib::active_content(library);
        const lib::ExpansionPill* still = pill_of(refused, "campaign");
        OA_CHECK(still != nullptr && still->locked && still->face == lib::ExpansionFace::on);
        OA_CHECK(lock_difference(library).empty());
        OA_CHECK(lib::press_expansion(library, "music").kind == lib::ContentPressKind::redraw);
        OA_CHECK(lock_difference(library).empty());
        OA_CHECK_TEXT(
            lib::active_content(library).lock,
            "The command line chose this run's mod, which needs Campaign."
        );
    }

    // Ridge chosen in the game: no pill is locked, and Campaign asks first.
    {
        lib::Inputs in_game = named;
        in_game.mod_from_command_line = false;
        lib::Library library;
        lib::refresh(library, in_game);
        const lib::ActiveContent face = lib::active_content(library);
        for (const lib::ExpansionPill& pill : face.pills)
            OA_CHECK(!pill.locked && pill.enabled);
        OA_CHECK(face.lock.empty());
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::ask);
    }

    // Ridge needs Core Contingency, which needs Campaign: turning Campaign
    // off would turn Core Contingency off too, so both are locked.
    {
        lib::Inputs needs_both = named;
        needs_both.content.mod_required = {"core-contingency"};
        lib::Library library;
        lib::refresh(library, needs_both);
        const lib::ActiveContent face = lib::active_content(library);
        const lib::ExpansionPill* campaign = pill_of(face, "campaign");
        const lib::ExpansionPill* contingency = pill_of(face, "core-contingency");
        OA_CHECK(campaign != nullptr && campaign->locked);
        OA_CHECK(contingency != nullptr && contingency->locked);
        OA_CHECK_TEXT(
            face.lock,
            "The command line chose this run's mod, which needs Campaign and Core Contingency."
        );
        OA_CHECK(lock_difference(library).empty());
    }

    // Campaign is stored off: the player can turn it on, so it is not
    // locked. Once it is on in the pending choice, it is.
    {
        lib::Inputs stored_off = named;
        stored_off.content.enabled = {"music"};
        lib::Library library;
        lib::refresh(library, stored_off);
        const lib::ActiveContent stored = lib::active_content(library);
        const lib::ExpansionPill* before = pill_of(stored, "campaign");
        OA_CHECK(before != nullptr && !before->locked && before->enabled);
        OA_CHECK(stored.lock.empty());
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::redraw);
        const lib::ActiveContent after = lib::active_content(library);
        const lib::ExpansionPill* turned_on = pill_of(after, "campaign");
        OA_CHECK(turned_on != nullptr && turned_on->locked && !turned_on->enabled);
        OA_CHECK_TEXT(after.lock, "The command line chose this run's mod, which needs Campaign.");
        OA_CHECK(lib::press_expansion(library, "campaign").kind == lib::ContentPressKind::none);
        OA_CHECK(holds_id(library.content.expansions, "campaign"));
        OA_CHECK(lock_difference(library).empty());
    }

    // During a game and while --content named the choice every pill is
    // greyed, and none is locked. With no mod played (--base-game), nothing
    // is needed.
    {
        lib::Inputs in_match = named;
        in_match.in_match = true;
        lib::Inputs content_named = named;
        content_named.content.locked = true;
        lib::Inputs no_mod = named;
        no_mod.installed.clear();
        for (const lib::Inputs& inputs : {in_match, content_named, no_mod}) {
            lib::Library library;
            lib::refresh(library, inputs);
            const lib::ActiveContent face = lib::active_content(library);
            for (const lib::ExpansionPill& pill : face.pills)
                OA_CHECK(!pill.locked);
            OA_CHECK(face.lock.empty());
            OA_CHECK(lock_difference(library).empty());
        }
    }

    // Every sequence of four steps, from Ridge chosen by the command line and
    // in the game, needing what is stored on, off and nothing, and from no mod.
    std::vector<lib::Inputs> starts;
    starts.push_back(named);
    lib::Inputs both = named;
    both.content.mod_required = {"core-contingency"};
    starts.push_back(both);
    lib::Inputs off = named;
    off.content.enabled = {"music"};
    starts.push_back(off);
    lib::Inputs nothing_needed = named;
    nothing_needed.content.mod_required.clear();
    starts.push_back(nothing_needed);
    lib::Inputs no_mod = named;
    no_mod.installed.clear();
    starts.push_back(no_mod);
    lib::Inputs in_game = named;
    in_game.mod_from_command_line = false;
    starts.push_back(in_game);

    constexpr std::size_t depth = 4;
    std::size_t sequences = 1;
    for (std::size_t place = 0; place < depth; ++place)
        sequences *= active_step_count;
    int32_t disagreements = 0;
    int32_t locked_states = 0;
    for (const lib::Inputs& start : starts) {
        const lib::Inputs arrived = arrived_over(start);
        for (std::size_t sequence = 0; sequence < sequences; ++sequence) {
            lib::Library library;
            lib::refresh(library, start);
            std::string steps;
            std::size_t rest = sequence;
            for (std::size_t place = 0; place < depth; ++place) {
                const auto step = static_cast<ActiveStep>(rest % active_step_count);
                rest /= active_step_count;
                if (!steps.empty())
                    steps += ", ";
                steps += take_step(library, step, arrived);
                const std::string difference = lock_difference(library);
                if (!lib::active_content(library).lock.empty())
                    ++locked_states;
                if (difference.empty())
                    continue;
                if (disagreements < 5)
                    std::fprintf(stderr, "after %s: %s\n", steps.c_str(), difference.c_str());
                ++disagreements;
                break;
            }
        }
    }
    if (disagreements > 0)
        std::fprintf(
            stderr, "%d sequences disagree on the lock\n", static_cast<int>(disagreements)
        );
    OA_CHECK(disagreements == 0);
    OA_CHECK(locked_states > 0);
}

void test_mutators() {
    // Mutators sits between Maps and Brains, Brains between Mutators and
    // Keys, and Keys between Brains and Languages. Updates still opens on
    // Updates; Mutators, Brains and Keys open on All. Each of those tabs
    // keeps its full word.
    OA_CHECK(lib::tab_total == 9);
    const lib::Tab order[] = {
        lib::Tab::active,
        lib::Tab::mods,
        lib::Tab::maps,
        lib::Tab::mutators,
        lib::Tab::brains,
        lib::Tab::keys,
        lib::Tab::languages,
        lib::Tab::updates,
        lib::Tab::registries,
    };
    OA_CHECK(std::size(order) == lib::tab_total);
    for (std::size_t index = 0; index < std::size(order); ++index)
        OA_CHECK(static_cast<lib::Tab>(index) == order[index]);
    const lib::Library fresh;
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::mods)] == lib::Filter::all);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::mutators)] == lib::Filter::all);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::brains)] == lib::Filter::all);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::keys)] == lib::Filter::all);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::languages)] == lib::Filter::all);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::updates)] == lib::Filter::updates);
    OA_CHECK(fresh.filters[static_cast<std::size_t>(lib::Tab::registries)] == lib::Filter::all);
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::mutators, false), "Mutators");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::mutators, true), "Mutators");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::brains, false), "Brains");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::brains, true), "Brains");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::keys, false), "Keys");
    OA_CHECK_TEXT(lib::tab_text(lib::Tab::keys, true), "Keys");

    lib::Inputs inputs = base_inputs();
    lib::Listing fast = listing(core, Kind::mutator, "fast-builders", "Fast Builders");
    fast.published = "2026-02-01";
    fast.tags = {"not-a-fact"};
    lib::MutatorFacts fast_facts;
    fast_facts.tags = {"economy", "statistics"};
    fast_facts.works_with.push_back({"*", "", ""});
    fast.mutator = fast_facts;
    inputs.listings.push_back(fast);
    lib::Installed fast_held = installed(core, Kind::mutator, "fast-builders", "Fast Builders");
    lib::MutatorFacts fast_installed_facts;
    fast_installed_facts.tags = {"installed-only"};
    fast_installed_facts.works_with.push_back({"*", "", ""});
    fast_held.mutator = fast_installed_facts;
    inputs.installed.push_back(fast_held);
    inputs.updates.push_back(update(core, Kind::mutator, "fast-builders"));

    lib::Listing gatehouse = listing(core, Kind::mutator, "gatehouse", "Gatehouse");
    gatehouse.version = "4.1";
    lib::MutatorFacts gatehouse_facts;
    gatehouse_facts.tags = {"gameplay", "new-units"};
    gatehouse.mutator = gatehouse_facts;
    inputs.listings.push_back(gatehouse);
    lib::Installed gatehouse_held = installed(core, Kind::mutator, "gatehouse", "Gatehouse");
    gatehouse_held.version = "4.1";
    gatehouse_held.mutator = gatehouse_facts;
    inputs.installed.push_back(gatehouse_held);

    lib::Listing keep = listing(core, Kind::mutator, "gatehouse-keep", "Gatehouse Keep");
    keep.version = "2.0";
    keep.published = "2026-03-01";
    lib::MutatorFacts keep_facts;
    keep_facts.tags = {"economy"};
    lib::Prerequisite need;
    need.id = "gatehouse";
    need.range = ">= 4.0";
    need.name = "Gatehouse";
    keep_facts.needs.push_back(need);
    keep.mutator = keep_facts;
    inputs.listings.push_back(keep);

    lib::Listing walls = listing(core, Kind::mutator, "ridge-walls", "Ridge Walls");
    lib::MutatorFacts wall_facts;
    wall_facts.tags = {"commanders", "alpha-extra"};
    wall_facts.works_with.push_back({"ridge", "", "Ridge"});
    walls.mutator = wall_facts;
    inputs.listings.push_back(walls);

    lib::Listing grounded = listing(core, Kind::mutator, "grounded", "Grounded");
    lib::MutatorFacts grounded_facts;
    grounded_facts.tags = {"disable", "ai", "maps", "zebra"};
    grounded.mutator = grounded_facts;
    inputs.listings.push_back(grounded);

    lib::Installed own = installed({}, Kind::mutator, "homemade", "homemade");
    lib::MutatorFacts own_facts;
    own_facts.tags = {"statistics"};
    own.mutator = own_facts;
    inputs.installed.push_back(own);

    inputs.listings.push_back(listing(core, Kind::mod, "ridge", "Ridge"));
    inputs.installed.push_back(installed(core, Kind::mod, "ridge", "Ridge"));
    inputs.updates.push_back(update(core, Kind::mod, "ridge"));

    lib::Library library = built(std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::all);

    const auto entry_of = [&](std::string_view registry, std::string_view key) {
        return at(library, registry, Kind::mutator, key);
    };
    const lib::Entry* fast_row = entry_of(core, "fast-builders");
    const lib::Entry* own_row = entry_of("", "homemade");
    const lib::Entry* gatehouse_row = entry_of(core, "gatehouse");
    const lib::Entry* keep_row = entry_of(core, "gatehouse-keep");
    const lib::Entry* walls_row = entry_of(core, "ridge-walls");
    OA_CHECK(fast_row != nullptr && fast_row->state == State::update && fast_row->can_act);
    OA_CHECK(fast_row != nullptr && fast_row->blocked.empty() && !fast_row->not_compatible);
    OA_CHECK(own_row != nullptr && own_row->state == State::installed && own_row->can_act);
    OA_CHECK(own_row != nullptr && own_row->blocked.empty() && own_row->id.registry.empty());
    OA_CHECK(gatehouse_row != nullptr && gatehouse_row->state == State::installed);
    OA_CHECK(keep_row != nullptr && keep_row->state == State::get && keep_row->can_act);
    OA_CHECK(keep_row != nullptr && keep_row->blocked.empty() && keep_row->needs_missing.empty());
    OA_CHECK(walls_row != nullptr && walls_row->state == State::get && walls_row->not_compatible);
    OA_CHECK(walls_row != nullptr && walls_row->blocked.empty() && walls_row->can_act);
    if (walls_row != nullptr) {
        OA_CHECK(actions_of(library, *walls_row).front() == std::make_pair(lib::Action::get, true));
    }

    using Keys = std::vector<std::string>;
    const Keys every = {
        "fast-builders@fixture-core",
        "gatehouse@fixture-core",
        "homemade@",
        "gatehouse-keep@fixture-core",
        "grounded@fixture-core",
        "ridge-walls@fixture-core",
    };
    OA_CHECK(visible_keys(library) == every);
    OA_CHECK(lib::tab_count(library, lib::Tab::mutators) == 6);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 6);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 3);
    OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 2);
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::all), "All (6)");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::installed), "Installed (3)");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::updates), "Updates (1)");
    OA_CHECK_TEXT(lib::filter_caption(library, lib::Filter::newest), "New (2)");

    using Counts = std::vector<std::pair<std::string, std::size_t>>;
    const Counts tags = {
        {"statistics", 2},
        {"economy", 2},
        {"commanders", 1},
        {"new-units", 1},
        {"disable", 1},
        {"gameplay", 1},
        {"ai", 1},
        {"maps", 1},
        {"alpha-extra", 1},
        {"zebra", 1},
    };
    OA_CHECK(lib::tags_in_tab(library) == tags);

    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(
        visible_keys(library) == Keys({"gatehouse-keep@fixture-core", "fast-builders@fixture-core"})
    );
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(
        visible_keys(library) ==
        Keys({"fast-builders@fixture-core", "gatehouse@fixture-core", "homemade@"})
    );
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"fast-builders@fixture-core"}));
    lib::set_filter(library, lib::Filter::all);

    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"fast-builders@fixture-core", "ridge@fixture-core"}));
    OA_CHECK(lib::update_count(library) == 2);

    lib::set_tab(library, lib::Tab::mutators);
    lib::select(library, {std::string(core), Kind::mutator, "ridge-walls"});
    const std::string note = lib::details_note(library);
    OA_CHECK(note.find("Not compatible with this mod") != std::string::npos);
    OA_CHECK(note.find("Ridge") != std::string::npos);

    // A game, then downloads turned off, then an engine this build lacks.
    // An installed mutator is never blocked.
    lib::Inputs blocked_inputs = base_inputs();
    lib::Listing old = listing(core, Kind::mutator, "old-engine", "Old Engine");
    old.requires_engine = ">= 9.0.0";
    lib::MutatorFacts old_facts;
    old_facts.works_with.push_back({"*", "", ""});
    old.mutator = old_facts;
    blocked_inputs.listings.push_back(old);
    blocked_inputs.in_match = true;
    lib::Library blocked = built(blocked_inputs);
    const auto blocked_of = [&](const lib::Library& held) {
        const lib::Entry* entry = at(held, core, Kind::mutator, "old-engine");
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? entry->blocked : std::string("(none)");
    };
    OA_CHECK_TEXT(blocked_of(blocked), "Not during a game");
    blocked_inputs.in_match = false;
    blocked_inputs.registries[0].downloads_on = false;
    lib::refresh(blocked, blocked_inputs);
    OA_CHECK_TEXT(
        blocked_of(blocked), "Downloads from Fixture Core are off (Settings › Downloads)"
    );
    blocked_inputs.registries[0].downloads_on = true;
    lib::refresh(blocked, blocked_inputs);
    OA_CHECK_TEXT(blocked_of(blocked), "Needs Open Annihilation 9.0.0 or later");
    blocked_inputs.in_match = true;
    blocked_inputs.registries[0].downloads_on = false;
    blocked_inputs.installed.push_back(installed(core, Kind::mutator, "old-engine", "Old Engine"));
    lib::refresh(blocked, blocked_inputs);
    const lib::Entry* held = at(blocked, core, Kind::mutator, "old-engine");
    OA_CHECK(held != nullptr && held->state == State::installed);
    OA_CHECK(held != nullptr && held->can_act && held->blocked.empty());

    // The name is the one the inputs carry. 4.1 meets the range; 3.9 does not.
    lib::Inputs needed = base_inputs();
    lib::Listing unmet = listing(core, Kind::mutator, "gatehouse-keep", "Gatehouse Keep");
    unmet.mutator = keep_facts;
    needed.listings.push_back(unmet);
    lib::Library none_held = built(needed);
    const lib::Entry* absent = at(none_held, core, Kind::mutator, "gatehouse-keep");
    OA_CHECK(absent != nullptr && absent->needs_missing == "Gatehouse" && absent->can_act);
    needed.installed.push_back(installed(core, Kind::mutator, "gatehouse", "Gatehouse"));
    needed.installed.back().version = "3.9";
    lib::Library too_old = built(needed);
    const lib::Entry* short_version = at(too_old, core, Kind::mutator, "gatehouse-keep");
    OA_CHECK(short_version != nullptr && short_version->needs_missing == "Gatehouse");
    OA_CHECK(short_version != nullptr && short_version->can_act && short_version->blocked.empty());
    needed.installed.back().version = "4.1";
    lib::Library met = built(needed);
    const lib::Entry* satisfied = at(met, core, Kind::mutator, "gatehouse-keep");
    OA_CHECK(satisfied != nullptr && satisfied->needs_missing.empty() && satisfied->can_act);

    // An empty installed works-with list means any mod, ahead of the listing.
    lib::Inputs over = base_inputs();
    lib::Listing listed_walls = listing(core, Kind::mutator, "ridge-walls", "Ridge Walls");
    listed_walls.mutator = wall_facts;
    over.listings.push_back(listed_walls);
    lib::Installed held_walls = installed(core, Kind::mutator, "ridge-walls", "Ridge Walls");
    held_walls.mutator = lib::MutatorFacts{};
    over.installed.push_back(held_walls);
    const lib::Library overridden = built(std::move(over));
    const lib::Entry* freed = at(overridden, core, Kind::mutator, "ridge-walls");
    OA_CHECK(freed != nullptr && !freed->not_compatible && freed->can_act);

    lib::Inputs played = base_inputs();
    lib::MutatorFacts any;
    OA_CHECK(lib::works_with_played(played, any));
    any.works_with.push_back({"*", "", ""});
    OA_CHECK(lib::works_with_played(played, any));
    lib::MutatorFacts base_only;
    base_only.works_with.push_back({std::string(lib::supported_base), "", ""});
    OA_CHECK(lib::works_with_played(played, base_only));
    played.played.id = "ridge";
    OA_CHECK(!lib::works_with_played(played, base_only));
    lib::MutatorFacts ridge_only;
    ridge_only.works_with.push_back({"ridge", ">= 4.0", "Ridge"});
    played.played = {"ridge", "5.0", "Ridge"};
    OA_CHECK(lib::works_with_played(played, ridge_only));
    played.played.version = "3.9";
    OA_CHECK(!lib::works_with_played(played, ridge_only));
    played.played.version = "5b";
    OA_CHECK(!lib::works_with_played(played, ridge_only));
    played.played = {"other", "5.0", "Other"};
    OA_CHECK(!lib::works_with_played(played, ridge_only));

    OA_CHECK(lib::tag_label("statistics") == lib::words::tag_statistics);
    OA_CHECK(lib::tag_label("economy") == lib::words::tag_economy);
    OA_CHECK(lib::tag_label("commanders") == lib::words::tag_commanders);
    OA_CHECK(lib::tag_label("new-units") == lib::words::tag_new_units);
    OA_CHECK(lib::tag_label("disable") == lib::words::tag_disable);
    OA_CHECK(lib::tag_label("gameplay") == lib::words::tag_gameplay);
    OA_CHECK(lib::tag_label("ai") == lib::words::tag_ai);
    OA_CHECK(lib::tag_label("maps") == lib::words::tab_maps);
    OA_CHECK(lib::tag_label("balance") == "balance");
    OA_CHECK(lib::tag_label("Economy") == "Economy");
    lib::TextHooks hooks;
    hooks.shown = economy_in_german;
    OA_CHECK(lib::tag_label("economy", hooks) == "Wirtschaft");
    OA_CHECK(lib::tag_label("Economy", hooks) == "Economy");

    lib::Inputs capital_inputs = base_inputs();
    lib::Listing capital = listing(core, Kind::mod, "capital-mod", "Capital Mod");
    capital.tags = {"Economy"};
    capital_inputs.listings.push_back(capital);
    const lib::Library capital_library = built(std::move(capital_inputs));
    OA_CHECK(lib::tags_in_tab(capital_library) == Counts({{"Economy", 1}}));
}

/// Translates Rush and Economy, and leaves every other word as written.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view brain_tags_in_german(void*, std::string_view english) {
    if (english == lib::words::tag_rush)
        return "Ansturm";
    if (english == lib::words::tag_economy)
        return "Wirtschaft";
    return english;
}

/// The Brains tab: the built-in player first, the package states, fit as
/// information, and the nine tags' labels.
void test_brains() {
    lib::Inputs inputs = base_inputs();

    lib::Listing ridge = listing(core, Kind::brain, "ridge-raider", "Ridge Raider");
    ridge.published = "2026-02-01";
    lib::BrainFacts ridge_facts;
    ridge_facts.tags = {"rush"};
    ridge_facts.works_with.push_back({"ridge", "", "Ridge"});
    ridge.brain = ridge_facts;
    inputs.listings.push_back(ridge);

    lib::Installed steady = installed({}, Kind::brain, "steady-commander", "Steady Commander");
    lib::BrainFacts steady_facts;
    steady_facts.tags = {"classic"};
    steady_facts.works_with.push_back({"*", "", ""});
    steady.brain = steady_facts;
    inputs.installed.push_back(steady);

    lib::Listing tide = listing(core, Kind::brain, "tide-keeper", "Tide Keeper");
    lib::BrainFacts tide_facts;
    tide_facts.tags = {"turtle", "zebra"};
    tide_facts.requires_expansions = {"core-contingency"};
    tide.brain = tide_facts;
    inputs.listings.push_back(tide);
    lib::Installed tide_held = installed(core, Kind::brain, "tide-keeper", "Tide Keeper");
    tide_held.brain = tide_facts;
    inputs.installed.push_back(tide_held);
    inputs.updates.push_back(update(core, Kind::brain, "tide-keeper"));

    lib::Listing wide = listing(core, Kind::brain, "wide-watch", "Wide Watch");
    lib::BrainFacts wide_facts;
    wide_facts.tags = {"economy"};
    wide_facts.full_awareness = true;
    wide_facts.works_with.push_back({"*", "", ""});
    wide.brain = wide_facts;
    inputs.listings.push_back(wide);
    lib::Installed wide_held = installed(core, Kind::brain, "wide-watch", "Wide Watch");
    wide_held.brain = wide_facts;
    inputs.installed.push_back(wide_held);

    lib::Listing lantern = listing(core, Kind::brain, "lantern-scout", "Lantern Scout");
    lantern.published = "2026-04-01";
    lantern.requires_engine = ">= 9.0.0";
    lib::BrainFacts lantern_facts;
    lantern_facts.tags = {"experimental"};
    lantern_facts.requires_engine = ">= 9.0.0";
    lantern.brain = lantern_facts;
    inputs.listings.push_back(lantern);

    lib::Listing ridge_mod = listing(core, Kind::mod, "ridge", "Ridge");
    inputs.listings.push_back(ridge_mod);
    inputs.installed.push_back(installed(core, Kind::mod, "ridge", "Ridge"));
    inputs.updates.push_back(update(core, Kind::mod, "ridge"));

    lib::Library library = built(inputs);
    lib::set_tab(library, lib::Tab::brains);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::all);

    const lib::Entry* built_in = lib::find_entry(library, lib::built_in_brain_id());
    OA_CHECK(built_in != nullptr);
    OA_CHECK(visible_keys(library).front() == "@built-in@");
    if (built_in != nullptr) {
        OA_CHECK(built_in->state == State::installed);
        OA_CHECK(built_in->is_default);
        OA_CHECK(built_in->listing == nullptr && built_in->installed == nullptr);
        OA_CHECK(built_in->can_act && built_in->blocked.empty() && !built_in->not_compatible);
        OA_CHECK(actions_of(library, *built_in).empty());
        OA_CHECK_TEXT(lib::entry_name(*built_in), "Built-in computer player");
        OA_CHECK_TEXT(
            lib::entry_summary(*built_in),
            "Open Annihilation's own computer player. It plays as Total Annihilation's does."
        );
    }

    const auto brain_at = [&](std::string_view registry, std::string_view key) {
        return at(library, registry, Kind::brain, key);
    };
    const lib::Entry* ridge_row = brain_at(core, "ridge-raider");
    const lib::Entry* steady_row = brain_at("", "steady-commander");
    const lib::Entry* tide_row = brain_at(core, "tide-keeper");
    const lib::Entry* wide_row = brain_at(core, "wide-watch");
    const lib::Entry* lantern_row = brain_at(core, "lantern-scout");
    OA_CHECK(ridge_row != nullptr && ridge_row->state == State::get && ridge_row->not_compatible);
    OA_CHECK(ridge_row != nullptr && ridge_row->can_act && ridge_row->blocked.empty());
    OA_CHECK(
        steady_row != nullptr && steady_row->state == State::installed && !steady_row->is_default
    );
    OA_CHECK(tide_row != nullptr && tide_row->state == State::update && tide_row->not_compatible);
    OA_CHECK(tide_row != nullptr && tide_row->can_act && tide_row->blocked.empty());
    OA_CHECK(
        wide_row != nullptr && wide_row->state == State::installed && !wide_row->not_compatible
    );
    OA_CHECK(
        lantern_row != nullptr && lantern_row->state == State::get && lantern_row->not_compatible
    );
    OA_CHECK(lantern_row != nullptr && lantern_row->can_act && lantern_row->blocked.empty());
    if (ridge_row != nullptr)
        OA_CHECK(actions_of(library, *ridge_row).front() == std::make_pair(Action::get, true));
    if (lantern_row != nullptr)
        OA_CHECK(actions_of(library, *lantern_row).front() == std::make_pair(Action::get, true));
    if (tide_row != nullptr)
        OA_CHECK(actions_of(library, *tide_row).front() == std::make_pair(Action::update, true));

    using Keys = std::vector<std::string>;
    OA_CHECK(
        visible_keys(library) == Keys({
                                     "@built-in@",
                                     "tide-keeper@fixture-core",
                                     "steady-commander@",
                                     "wide-watch@fixture-core",
                                     "lantern-scout@fixture-core",
                                     "ridge-raider@fixture-core",
                                 })
    );
    OA_CHECK(lib::tab_count(library, lib::Tab::brains) == 5);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 5);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 3);
    OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 2);
    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(
        visible_keys(library) == Keys({"lantern-scout@fixture-core", "ridge-raider@fixture-core"})
    );
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(visible_keys(library).front() == "@built-in@");
    OA_CHECK(!shows_key(library, "ridge-raider@fixture-core"));
    OA_CHECK(!shows_key(library, "lantern-scout@fixture-core"));
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"tide-keeper@fixture-core"}));
    lib::set_filter(library, lib::Filter::all);

    lib::set_tab(library, lib::Tab::mods);
    OA_CHECK(!shows_key(library, "@built-in@"));
    OA_CHECK(!shows_key(library, "steady-commander@"));
    lib::set_tab(library, lib::Tab::updates);
    OA_CHECK(visible_keys(library) == Keys({"ridge@fixture-core", "tide-keeper@fixture-core"}));
    OA_CHECK(!shows_key(library, "@built-in@"));
    lib::set_tab(library, lib::Tab::brains);

    using Counts = std::vector<std::pair<std::string, std::size_t>>;
    OA_CHECK(
        lib::tags_in_tab(library) == Counts({
                                         {"classic", 1},
                                         {"economy", 1},
                                         {"rush", 1},
                                         {"turtle", 1},
                                         {"experimental", 1},
                                         {"zebra", 1},
                                     })
    );

    inputs.default_brain = "steady-commander";
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::brains);
    const lib::Entry* named_default = at(library, "", Kind::brain, "steady-commander");
    const lib::Entry* named_built_in = lib::find_entry(library, lib::built_in_brain_id());
    OA_CHECK(named_default != nullptr && named_default->is_default);
    OA_CHECK(named_built_in != nullptr && !named_built_in->is_default);
    if (named_built_in != nullptr) {
        const std::vector<std::pair<Action, bool>> use_default{{Action::use_by_default, true}};
        OA_CHECK(actions_of(library, *named_built_in) == use_default);
    }
    if (named_default != nullptr) {
        for (const auto& [action, enabled] : actions_of(library, *named_default)) {
            static_cast<void>(enabled);
            OA_CHECK(action != Action::use_by_default);
        }
    }
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 5);
    inputs.default_brain = "missing-brain";
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::brains);
    const lib::Entry* missing_default = lib::find_entry(library, lib::built_in_brain_id());
    OA_CHECK(missing_default != nullptr && missing_default->is_default);

    lib::Inputs plain = base_inputs();
    OA_CHECK(lib::brain_unfit_reason(plain, lib::BrainFacts{}).empty());
    lib::BrainFacts any;
    any.works_with.push_back({"*", "", ""});
    OA_CHECK(lib::brain_unfit_reason(plain, any).empty());
    OA_CHECK_TEXT(lib::brain_unfit_reason(plain, ridge_facts), "Made for Ridge");
    plain.played = {"ridge", "28", "Ridge"};
    OA_CHECK(lib::brain_unfit_reason(plain, ridge_facts).empty());
    plain.played = {};
    OA_CHECK_TEXT(lib::brain_unfit_reason(plain, tide_facts), "Needs Core Contingency");
    plain.content.enabled = {"core-contingency"};
    OA_CHECK(lib::brain_unfit_reason(plain, tide_facts).empty());
    plain.content.enabled.clear();
    OA_CHECK_TEXT(
        lib::brain_unfit_reason(plain, lantern_facts), "Needs Open Annihilation 9.0.0 or later"
    );
    OA_CHECK(lib::brain_unfit_reason(plain, wide_facts).empty());

    OA_CHECK(lib::brain_tag_label("classic") == lib::words::tag_classic);
    OA_CHECK(lib::brain_tag_label("economy") == lib::words::tag_economy);
    OA_CHECK(lib::brain_tag_label("rush") == lib::words::tag_rush);
    OA_CHECK(lib::brain_tag_label("turtle") == lib::words::tag_turtle);
    OA_CHECK(lib::brain_tag_label("air") == lib::words::tag_air);
    OA_CHECK(lib::brain_tag_label("naval") == lib::words::tag_naval);
    OA_CHECK(lib::brain_tag_label("learned") == lib::words::tag_learned);
    OA_CHECK(lib::brain_tag_label("teaching") == lib::words::tag_teaching);
    OA_CHECK(lib::brain_tag_label("experimental") == lib::words::tag_experimental);
    OA_CHECK(lib::brain_tag_label("zebra") == "zebra");
    OA_CHECK(lib::brain_tag_label("Rush") == "Rush");
    lib::TextHooks hooks;
    hooks.shown = brain_tags_in_german;
    OA_CHECK(lib::brain_tag_label("rush", hooks) == "Ansturm");
    OA_CHECK(lib::brain_tag_label("economy", hooks) == "Wirtschaft");
    OA_CHECK(lib::brain_tag_label("Rush", hooks) == "Rush");
}

/// Tells whether an entry's buttons include an action.
///
/// @param actions the buttons
/// @param wanted the action
/// @return true when one of them is it
bool has_action(const std::vector<std::pair<Action, bool>>& actions, Action wanted) {
    for (const auto& [action, enabled] : actions) {
        static_cast<void>(enabled);
        if (action == wanted)
            return true;
    }
    return false;
}

/// The Keys tab: OA default first and left out of every count, the package
/// states, fit as information, the use question and the Active tile.
void test_keys() {
    using Buttons = std::vector<std::pair<Action, bool>>;
    lib::Inputs inputs = base_inputs();

    lib::Listing lefty = listing(core, Kind::key_layout, "lefty-test", "Lefty Test");
    lefty.homepage = "https://example.test/lefty";
    lib::KeyLayoutFacts lefty_facts;
    lefty_facts.tags = {"left-hand"};
    lefty.keys = lefty_facts;
    inputs.listings.push_back(lefty);
    lib::Installed lefty_held = installed(core, Kind::key_layout, "lefty-test", "Lefty Test");
    lefty_held.keys = lefty_facts;
    inputs.installed.push_back(std::move(lefty_held));
    inputs.updates.push_back(update(core, Kind::key_layout, "lefty-test"));

    lib::Listing grid = listing(core, Kind::key_layout, "grid-test", "Grid Test");
    lib::KeyLayoutFacts grid_facts;
    grid_facts.tags = {"grid"};
    grid_facts.works_with.push_back({"ridge", "", "Ridge"});
    grid_facts.keyboard = "iso";
    grid_facts.pointer = "left-click";
    grid_facts.helpers = {"sticky-modifiers", "toggle:order.queue-modifier"};
    grid_facts.changes = 4;
    grid_facts.unknown_actions = 1;
    grid_facts.own_changes = 2;
    grid_facts.triggers = {"Q", "@KeyW", "MouseLeft", "Nope", "Ctrl"};
    grid.keys = grid_facts;
    inputs.listings.push_back(grid);
    lib::Installed grid_held = installed(core, Kind::key_layout, "grid-test", "Grid Test");
    grid_held.keys = grid_facts;
    inputs.installed.push_back(std::move(grid_held));

    lib::Listing wide = listing(core, Kind::key_layout, "wide-keys", "Wide Keys");
    lib::KeyLayoutFacts wide_facts;
    wide_facts.tags = {"classic"};
    wide_facts.works_with.push_back({"*", "", ""});
    wide.keys = wide_facts;
    inputs.listings.push_back(wide);
    lib::Installed wide_held = installed(core, Kind::key_layout, "wide-keys", "Wide Keys");
    wide_held.keys = wide_facts;
    inputs.installed.push_back(std::move(wide_held));

    lib::Listing lantern = listing(core, Kind::key_layout, "lantern-keys", "Lantern Keys");
    lantern.published = "2026-04-01";
    lantern.requires_engine = ">= 9.0.0";
    lib::KeyLayoutFacts lantern_facts;
    lantern_facts.tags = {"legacy"};
    lantern_facts.requires_engine = ">= 9.0.0";
    lantern.keys = lantern_facts;
    inputs.listings.push_back(std::move(lantern));

    lib::Listing ridge_keys = listing(core, Kind::key_layout, "ridge-keys", "Ridge Keys");
    ridge_keys.published = "2026-03-01";
    lib::KeyLayoutFacts ridge_key_facts;
    ridge_key_facts.works_with.push_back({"ridge", "", "Ridge"});
    ridge_keys.keys = ridge_key_facts;
    inputs.listings.push_back(std::move(ridge_keys));

    inputs.listings.push_back(listing(core, Kind::mod, "ridge", "Ridge"));
    inputs.installed.push_back(installed(core, Kind::mod, "ridge", "Ridge"));

    lib::Library library = built(inputs);
    lib::set_tab(library, lib::Tab::keys);
    OA_CHECK(lib::shown_filter(library) == lib::Filter::all);

    const lib::Entry* built_in = lib::find_entry(library, lib::built_in_layout_id());
    OA_CHECK(built_in != nullptr);
    OA_CHECK(visible_keys(library).front() == "@oa-default@");
    if (built_in != nullptr) {
        OA_CHECK(built_in->state == State::installed);
        OA_CHECK(built_in->listing == nullptr && built_in->installed == nullptr);
        OA_CHECK(built_in->can_act && built_in->blocked.empty() && !built_in->not_compatible);
        const Buttons built_in_actions{{Action::edit_keys, true}};
        OA_CHECK(actions_of(library, *built_in) == built_in_actions);
        OA_CHECK_TEXT(lib::entry_name(*built_in), "OA default");
        OA_CHECK_TEXT(lib::entry_summary(*built_in), "The keys Open Annihilation ships with.");
    }

    const auto layout_at = [&](std::string_view key) {
        return at(library, core, Kind::key_layout, key);
    };
    const lib::Entry* lefty_row = layout_at("lefty-test");
    const lib::Entry* grid_row = layout_at("grid-test");
    const lib::Entry* wide_row = layout_at("wide-keys");
    const lib::Entry* lantern_row = layout_at("lantern-keys");
    const lib::Entry* ridge_row = layout_at("ridge-keys");
    OA_CHECK(lefty_row != nullptr && lefty_row->state == State::update && lefty_row->can_act);
    OA_CHECK(lefty_row != nullptr && lefty_row->blocked.empty() && !lefty_row->not_compatible);
    OA_CHECK(
        grid_row != nullptr && grid_row->state == State::installed && grid_row->not_compatible
    );
    OA_CHECK(grid_row != nullptr && grid_row->can_act && grid_row->blocked.empty());
    OA_CHECK(
        wide_row != nullptr && wide_row->state == State::installed && !wide_row->not_compatible
    );
    OA_CHECK(
        lantern_row != nullptr && lantern_row->state == State::get && lantern_row->not_compatible
    );
    OA_CHECK(lantern_row != nullptr && lantern_row->can_act && lantern_row->blocked.empty());
    OA_CHECK(ridge_row != nullptr && ridge_row->state == State::get && ridge_row->not_compatible);
    OA_CHECK(ridge_row != nullptr && ridge_row->can_act && ridge_row->blocked.empty());

    using Keys = std::vector<std::string>;
    OA_CHECK(
        visible_keys(library) == Keys({
                                     "@oa-default@",
                                     "lefty-test@fixture-core",
                                     "grid-test@fixture-core",
                                     "wide-keys@fixture-core",
                                     "lantern-keys@fixture-core",
                                     "ridge-keys@fixture-core",
                                 })
    );
    OA_CHECK(lib::tab_count(library, lib::Tab::keys) == 5);
    OA_CHECK(lib::filter_count(library, lib::Filter::all) == 5);
    OA_CHECK(lib::filter_count(library, lib::Filter::installed) == 3);
    OA_CHECK(lib::filter_count(library, lib::Filter::updates) == 1);
    OA_CHECK(lib::filter_count(library, lib::Filter::newest) == 2);
    lib::set_filter(library, lib::Filter::newest);
    OA_CHECK(
        visible_keys(library) == Keys({"lantern-keys@fixture-core", "ridge-keys@fixture-core"})
    );
    OA_CHECK(!shows_key(library, "@oa-default@"));
    lib::set_filter(library, lib::Filter::installed);
    OA_CHECK(visible_keys(library).front() == "@oa-default@");
    lib::set_filter(library, lib::Filter::updates);
    OA_CHECK(visible_keys(library) == Keys({"lefty-test@fixture-core"}));
    lib::set_filter(library, lib::Filter::all);
    OA_CHECK(lib::added_package_count(library.inputs) == 1);

    if (lefty_row != nullptr) {
        const Buttons lefty_actions = actions_of(library, *lefty_row);
        const Buttons lefty_expected{
            {Action::update, true},
            {Action::use_layout, true},
            {Action::uninstall, true},
            {Action::open_folder, true},
            {Action::homepage, true},
        };
        OA_CHECK(lefty_actions == lefty_expected);
        OA_CHECK(!has_action(lefty_actions, Action::play_now));
        OA_CHECK(!has_action(lefty_actions, Action::roll_back));
    }
    if (lantern_row != nullptr) {
        const Buttons lantern_actions = actions_of(library, *lantern_row);
        const Buttons lantern_expected{{Action::get, true}};
        OA_CHECK(lantern_actions == lantern_expected);
        OA_CHECK(!has_action(lantern_actions, Action::play_now));
        OA_CHECK(!has_action(lantern_actions, Action::roll_back));
    }
    if (grid_row != nullptr) {
        const std::vector<lib::Fact> grid_facts_shown = lib::facts(library, *grid_row);
        const lib::Fact* works = fact(grid_facts_shown, "Works with");
        const lib::Fact* keyboard = fact(grid_facts_shown, "Keyboard");
        const lib::Fact* interface_fact = fact(grid_facts_shown, "Interface");
        const lib::Fact* asks = fact(grid_facts_shown, "Asks for");
        const lib::Fact* changes = fact(grid_facts_shown, "Changes");
        const lib::Fact* unknown = fact(grid_facts_shown, "Not in this version");
        const lib::Fact* own = fact(grid_facts_shown, "Your own changes");
        OA_CHECK(works != nullptr && works->long_value == "Ridge" && works->mark == Mark::warn);
        OA_CHECK(keyboard != nullptr && keyboard->long_value == "ISO");
        OA_CHECK(interface_fact != nullptr && interface_fact->long_value == "Left-click");
        OA_CHECK(asks != nullptr && asks->long_value == "Sticky modifiers and Latch Queue");
        OA_CHECK(changes != nullptr && changes->long_value == "Changes 4 actions");
        OA_CHECK(
            unknown != nullptr && unknown->long_value == "Not in this version: 1" &&
            unknown->mark == Mark::warn
        );
        OA_CHECK(own != nullptr && own->long_value == "· 2 changes of your own");
    }
    if (lantern_row != nullptr) {
        const lib::Fact* needs = fact(lib::facts(library, *lantern_row), "Needs");
        OA_CHECK(
            needs != nullptr && needs->long_value == "Open Annihilation 9.0.0 or later" &&
            needs->mark == Mark::warn
        );
    }

    const lib::Entry* ridge_mod = at(library, core, Kind::mod, "ridge");
    OA_CHECK(ridge_mod != nullptr);
    if (ridge_mod != nullptr) {
        std::vector<std::string> named;
        std::vector<lib::EntryId> opened;
        for (const lib::Fact& item : lib::facts(library, *ridge_mod)) {
            if (item.long_label != "Key layouts")
                continue;
            named.push_back(item.long_value);
            if (item.opens)
                opened.push_back(*item.opens);
        }
        OA_CHECK(named == std::vector<std::string>({"Grid Test", "Ridge Keys"}));
        OA_CHECK(opened.size() == 2);
        if (opened.size() == 2) {
            OA_CHECK(opened[0].key == "grid-test" && opened[0].kind == Kind::key_layout);
            OA_CHECK(opened[1].key == "ridge-keys" && opened[1].kind == Kind::key_layout);
        }
    }

    if (wide_row != nullptr) {
        lib::open_key_use(library, *wide_row);
        OA_CHECK(library.key_use.open && !library.key_use.show_own && !library.key_use.show_scope);
        OA_CHECK(library.key_use.helpers.empty() && library.key_use.pointer_note.empty());
        const std::optional<lib::KeyUseResult> plain = lib::key_use_result(library);
        OA_CHECK(plain.has_value() && !plain->keep && !plain->for_mod && plain->helpers_on.empty());
        lib::close_key_use(library);
        OA_CHECK(!library.key_use.open);
        OA_CHECK(!lib::key_use_result(library).has_value());
    }

    inputs.keys_own_changes = 4;
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::keys);
    const lib::Entry* lefty_again = at(library, core, Kind::key_layout, "lefty-test");
    OA_CHECK(lefty_again != nullptr);
    if (lefty_again != nullptr) {
        lib::open_key_use(library, *lefty_again);
        OA_CHECK(
            library.key_use.show_own && library.key_use.keep && library.key_use.own_count == 4
        );
        OA_CHECK_TEXT(library.key_use.saved_with, "OA default");
        OA_CHECK(!library.key_use.already);
        const std::optional<lib::KeyUseResult> kept = lib::key_use_result(library);
        OA_CHECK(kept.has_value() && kept->keep);
        lib::set_key_use_keep(library, false);
        const std::optional<lib::KeyUseResult> aside = lib::key_use_result(library);
        OA_CHECK(aside.has_value() && !aside->keep);
        lib::close_key_use(library);
    }
    const lib::Entry* grid_again = at(library, core, Kind::key_layout, "grid-test");
    OA_CHECK(grid_again != nullptr);
    if (grid_again != nullptr) {
        lib::open_key_use(library, *grid_again);
        OA_CHECK(library.key_use.already && library.key_use.already_count == 2);
        OA_CHECK_TEXT(library.key_use.already_name, "Grid Test");
        OA_CHECK_TEXT(
            library.key_use.pointer_note, "Made for the left-click interface; yours is right-click."
        );
        OA_CHECK(library.key_use.helpers.size() == 2);
        if (library.key_use.helpers.size() == 2) {
            OA_CHECK(library.key_use.helpers[0].name == "sticky-modifiers");
            OA_CHECK(library.key_use.helpers[0].on);
            OA_CHECK(library.key_use.helpers[1].name == "toggle.order.queue-modifier");
            OA_CHECK_TEXT(library.key_use.helpers[1].label, "Latch Queue");
        }
        const std::optional<lib::KeyUseResult> overwritten = lib::key_use_result(library);
        OA_CHECK(overwritten.has_value() && !overwritten->keep);
        lib::set_key_use_helper(library, 0, false);
        const std::optional<lib::KeyUseResult> one_off = lib::key_use_result(library);
        OA_CHECK(one_off.has_value() && one_off->helpers_on.size() == 1);
        if (one_off.has_value() && one_off->helpers_on.size() == 1)
            OA_CHECK(one_off->helpers_on.front() == "toggle:order.queue-modifier");
        lib::close_key_use(library);
    }

    inputs.played = {"ridge", "28", "Ridge"};
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::keys);
    OA_CHECK(
        visible_keys(library) == Keys({
                                     "@oa-default@",
                                     "grid-test@fixture-core",
                                     "ridge-keys@fixture-core",
                                     "lefty-test@fixture-core",
                                     "wide-keys@fixture-core",
                                     "lantern-keys@fixture-core",
                                 })
    );
    const lib::Entry* made_for = at(library, core, Kind::key_layout, "grid-test");
    OA_CHECK(made_for != nullptr);
    if (made_for != nullptr) {
        OA_CHECK(!made_for->not_compatible);
        OA_CHECK_TEXT(lib::layout_made_for_line(library, *made_for), "Made for Ridge");
        lib::open_key_use(library, *made_for);
        OA_CHECK(library.key_use.show_scope && library.key_use.for_mod);
        OA_CHECK_TEXT(library.key_use.mod_name, "Ridge");
        const std::optional<lib::KeyUseResult> only_mod = lib::key_use_result(library);
        OA_CHECK(only_mod.has_value() && only_mod->for_mod);
        lib::set_key_use_scope(library, false);
        const std::optional<lib::KeyUseResult> every = lib::key_use_result(library);
        OA_CHECK(every.has_value() && !every->for_mod);
        lib::set_key_use_scope(library, true);
        const std::optional<lib::KeyUseResult> again = lib::key_use_result(library);
        OA_CHECK(again.has_value() && again->for_mod);
        lib::close_key_use(library);
    }
    OA_CHECK(lib::tab_count(library, lib::Tab::keys) == 5);

    lib::set_tab(library, lib::Tab::active);
    lib::ActiveSetup playing = lib::active_setup(library);
    OA_CHECK_TEXT(playing.keys_name, "OA default");
    OA_CHECK_TEXT(playing.keys_badge, "Built in");
    OA_CHECK_TEXT(playing.keys_line, "· 4 changes of your own");
    OA_CHECK(playing.keys_with.empty());

    inputs.keys_layout = "grid-test";
    inputs.keys_own_changes = 9;
    lib::refresh(library, inputs);
    playing = lib::active_setup(library);
    OA_CHECK_TEXT(playing.keys_name, "Grid Test");
    OA_CHECK_TEXT(playing.keys_badge, "USING");
    OA_CHECK_TEXT(playing.keys_line, "· 2 changes of your own");

    inputs.keys_layout.clear();
    inputs.keys_for_mod = "grid-test";
    inputs.keys_for_mod_name.clear();
    lib::refresh(library, inputs);
    playing = lib::active_setup(library);
    OA_CHECK_TEXT(playing.keys_name, "Grid Test");
    OA_CHECK_TEXT(playing.keys_badge, "USING");
    OA_CHECK_TEXT(playing.keys_with, "With Ridge: Grid Test");
    OA_CHECK_TEXT(playing.keys_line, "· 2 changes of your own");

    lib::set_tab(library, lib::Tab::keys);
    inputs.keys_layout = "grid-test";
    inputs.keys_for_mod.clear();
    inputs.played = {};
    inputs.keys_own_changes = 0;
    lib::refresh(library, inputs);
    lib::set_tab(library, lib::Tab::keys);
    const lib::Entry* grid_used = at(library, core, Kind::key_layout, "grid-test");
    const lib::Entry* default_unused = lib::find_entry(library, lib::built_in_layout_id());
    OA_CHECK(grid_used != nullptr && default_unused != nullptr);
    if (grid_used != nullptr) {
        const Buttons used = actions_of(library, *grid_used);
        OA_CHECK(has_action(used, Action::edit_keys));
        OA_CHECK(!has_action(used, Action::use_layout));
        OA_CHECK(!has_action(used, Action::play_now));
    }
    if (default_unused != nullptr) {
        const Buttons default_actions{{Action::use_layout, true}};
        OA_CHECK(actions_of(library, *default_unused) == default_actions);
    }

    using Counts = std::vector<std::pair<std::string, std::size_t>>;
    OA_CHECK(
        lib::tags_in_tab(library) == Counts({
                                         {"classic", 1},
                                         {"grid", 1},
                                         {"left-hand", 1},
                                         {"legacy", 1},
                                     })
    );
    OA_CHECK(lib::key_tag_label("grid") == lib::words::tag_grid);
    OA_CHECK(lib::key_tag_label("classic") == lib::words::tag_classic);
    OA_CHECK(lib::key_tag_label("left-hand") == lib::words::tag_left_hand);
    OA_CHECK(lib::key_tag_label("zebra") == "zebra");
}

void test_lookup() {
    lib::TextHooks hooks;
    hooks.shown = reversed_words;
    OA_CHECK_TEXT(lib::size_text(43011223, hooks), "41,0 Mo");
    lib::Inputs inputs = base_inputs();
    inputs.listings.push_back(listing(core, Kind::mod, "get-mod", "Get Mod"));
    inputs.queue.push_back(queued(core, "other", "Fixture Mod", Phase::installed));
    lib::Library library;
    library.text = hooks;
    lib::refresh(library, std::move(inputs));
    OA_CHECK_TEXT(lib::queue_lines(library).first, "Fixture Mod ist installiert");
    const lib::Entry* get = at(library, core, Kind::mod, "get-mod");
    if (get != nullptr) {
        OA_CHECK_TEXT(lib::status_line(library, *get), "Not installed · 64,0 Mo");
        OA_CHECK_TEXT(lib::entry_actions(library, *get).front().text, "HOLEN");
    }
}

/// The long labels of an entry's facts, in order.
///
/// @param library the Library
/// @param entry the entry
/// @return the labels
std::vector<std::string> fact_labels(const lib::Library& library, const lib::Entry& entry) {
    std::vector<std::string> labels;
    for (const lib::Fact& item : lib::facts(library, entry))
        labels.push_back(item.long_label);
    return labels;
}

/// One installed mutator's facts, with a verdict put on the copy.
///
/// @param verdict the verdict; unset leaves the fact and the button out
/// @param update_it offer an update
/// @param in_match a game is being played
/// @return the Library, on the Mutators tab, with the entry selected
lib::Library mutator_library(std::optional<lib::Verdict> verdict, bool update_it, bool in_match) {
    lib::Inputs inputs = base_inputs();
    inputs.in_match = in_match;
    lib::Listing made = listing(core, Kind::mutator, "fast-builders", "Fast Builders");
    made.version = "2.0";
    made.revision = 1;
    made.author = "Ada";
    made.summary = "Builds faster.";
    made.homepage = "https://example.test/fast";
    made.requires_engine = ">= 0.8.0";
    lib::MutatorFacts facts;
    facts.works_with.push_back({"*", {}, {}});
    facts.needs.push_back({"gatehouse", ">= 4.0", "Gatehouse"});
    facts.options.push_back({"Income", {"frugal", "moderate", "wealthy"}});
    facts.layers = {"units", "files"};
    made.mutator = facts;
    inputs.listings.push_back(std::move(made));

    lib::Installed copy = installed(core, Kind::mutator, "fast-builders", "Fast Builders");
    copy.version = "1.0";
    copy.revision = 1;
    copy.author = "Ada";
    copy.description = "Builds faster.";
    copy.kept = lib::Kept{"0.9", 1};
    if (verdict)
        facts.verdict = std::move(verdict);
    copy.mutator = facts;
    inputs.installed.push_back(std::move(copy));
    lib::Installed gatehouse = installed(core, Kind::mutator, "gatehouse", "Gatehouse");
    gatehouse.version = "4.0";
    inputs.installed.push_back(std::move(gatehouse));
    if (update_it) {
        lib::UpdateNote note = update(core, Kind::mutator, "fast-builders");
        note.to_version = "2.0";
        note.to_revision = 1;
        inputs.updates.push_back(std::move(note));
    }
    lib::Library library = built(std::move(inputs));
    lib::set_tab(library, lib::Tab::mutators);
    return library;
}

/// The Checked fact in German: the base game's own sentence, and a mod's.
///
/// @param english the pattern
/// @return its translation, or the pattern
std::string_view checked_in_german(void*, std::string_view english) {
    if (english == lib::words::no_mod)
        return "Kein Mod";
    if (english == lib::words::checked_good_base)
        return "Ohne Mod: {summary}";
    if (english == lib::words::checked_good)
        return "Mit {mod}: {summary}";
    if (english == lib::words::checked_no_effect_base)
        return "Ohne Mod: keine Wirkung";
    if (english == lib::words::changes_other_data)
        return "ändert andere Spieldaten";
    if (english == lib::words::changes_map_on_start)
        return "ändert die Karte, wenn ein Gefecht beginnt";
    return english;
}

/// A compatible verdict with no counts, the map fact, and the base game's
/// own sentence beside a mod's.
void test_checked_words() {
    using Record = oa::data::mutators::VerdictRecord;
    using Dry = oa::data::mutators::Verdict;

    Record sides;
    sides.verdict = Dry::compatible;
    lib::Verdict other = lib::verdict_of_record(sides);
    OA_CHECK(other.state == lib::Verdict::State::compatible);
    OA_CHECK(other.summary == "changes other game data");
    lib::apply_map_fact(other, sides, false, {});
    OA_CHECK(other.summary == "changes other game data");

    lib::Verdict map = lib::verdict_of_record(sides);
    lib::apply_map_fact(map, sides, true, {});
    OA_CHECK(map.summary == "changes the map when a match starts");
    const lib::CheckedWords map_words = lib::checked_text(map, "No Mod", {});
    OA_CHECK(map_words.value == "With No Mod: changes the map when a match starts");
    OA_CHECK(map_words.mark == lib::Mark::good);

    Record units;
    units.verdict = Dry::compatible;
    units.counts.units_changed = 2;
    lib::Verdict counted = lib::verdict_of_record(units);
    lib::apply_map_fact(counted, units, true, {});
    OA_CHECK(counted.summary == "changes 2 units");

    Record quiet;
    quiet.verdict = Dry::no_effect;
    lib::Verdict none = lib::verdict_of_record(quiet);
    lib::apply_map_fact(none, quiet, true, {});
    OA_CHECK(none.state == lib::Verdict::State::no_effect);

    const lib::TextHooks german{nullptr, checked_in_german};
    const lib::Verdict german_other = lib::verdict_of_record(sides, german);
    OA_CHECK(
        lib::checked_text(german_other, "Kein Mod", german).value ==
        "Ohne Mod: ändert andere Spieldaten"
    );
    OA_CHECK(
        lib::checked_text(german_other, "Ridge 1.0", german).value ==
        "Mit Ridge 1.0: ändert andere Spieldaten"
    );
    OA_CHECK(lib::checked_text(none, "Kein Mod", german).value == "Ohne Mod: keine Wirkung");
    lib::Verdict german_map = lib::verdict_of_record(sides, german);
    lib::apply_map_fact(german_map, sides, true, german);
    OA_CHECK(
        lib::checked_text(german_map, "", german).value ==
        "Ohne Mod: ändert die Karte, wenn ein Gefecht beginnt"
    );
}

/// A mutator's details: facts in order, each verdict, the status and the actions.
void test_mutator_details() {
    using Record = oa::data::mutators::VerdictRecord;
    using Dry = oa::data::mutators::Verdict;

    Record one;
    one.verdict = Dry::compatible;
    one.counts.units_changed = 1;
    one.reason = "ignored";
    const lib::Verdict one_verdict = lib::verdict_of_record(one);
    OA_CHECK(one_verdict.state == lib::Verdict::State::compatible);
    OA_CHECK(one_verdict.reason == "ignored");
    OA_CHECK(one_verdict.summary == "changes 1 unit");

    Record several;
    several.verdict = Dry::compatible;
    several.counts.units_changed = 2;
    several.counts.weapons_added = 1;
    several.counts.files_hidden = 3;
    OA_CHECK(
        lib::verdict_of_record(several).summary ==
        "changes 2 units, adds 1 weapon and hides 3 files"
    );

    Record every;
    every.verdict = Dry::compatible;
    every.counts.units_changed = 2;
    every.counts.units_added = 1;
    every.counts.units_removed = 3;
    every.counts.weapons_changed = 4;
    every.counts.weapons_added = 5;
    every.counts.weapons_removed = 6;
    every.counts.features_changed = 7;
    every.counts.features_added = 8;
    every.counts.features_removed = 9;
    every.counts.menu_entries_changed = 1;
    every.counts.files_added = 2;
    every.counts.files_hidden = 1;
    OA_CHECK(
        lib::verdict_of_record(every).summary ==
        "changes 2 units, adds 1 unit, removes 3 units, changes 4 weapons, adds 5 weapons, "
        "removes 6 weapons, changes 7 features, adds 8 features, removes 9 features, "
        "changes 1 menu entry, adds 2 files and hides 1 file"
    );

    Record no_effect;
    no_effect.verdict = Dry::no_effect;
    no_effect.reason = "kept";
    const lib::Verdict no_effect_verdict = lib::verdict_of_record(no_effect);
    OA_CHECK(no_effect_verdict.state == lib::Verdict::State::no_effect);
    OA_CHECK(no_effect_verdict.reason == "kept");
    OA_CHECK(no_effect_verdict.summary.empty());

    Record incompatible;
    incompatible.verdict = Dry::incompatible;
    incompatible.reason = "needs a unit";
    const lib::Verdict incompatible_verdict = lib::verdict_of_record(incompatible);
    OA_CHECK(incompatible_verdict.state == lib::Verdict::State::incompatible);
    OA_CHECK(incompatible_verdict.reason == "needs a unit");

    Record failed;
    failed.verdict = Dry::failed;
    failed.reason = "the dry run stopped";
    const lib::Verdict failed_verdict = lib::verdict_of_record(failed);
    OA_CHECK(failed_verdict.state == lib::Verdict::State::failed);
    OA_CHECK(failed_verdict.reason == "the dry run stopped");

    // Listed, not installed. No verdict, so no Checked fact and no CHECK.
    lib::Inputs listed_inputs = base_inputs();
    lib::Listing listed = listing(core, Kind::mutator, "fast-builders", "Fast Builders");
    listed.version = "2.0";
    listed.revision = 1;
    listed.author = "Ada";
    listed.summary = "Builds faster.";
    listed.homepage = "https://example.test/fast";
    listed.requires_engine = ">= 0.8.0";
    lib::MutatorFacts listed_facts;
    listed_facts.works_with.push_back({"ridge", {}, "Ridge"});
    listed_facts.needs.push_back({"gatehouse", ">= 4.0", "Gatehouse"});
    listed_facts.options.push_back({"Income", {"frugal", "moderate", "wealthy"}});
    listed_facts.layers = {"files", "units"};
    listed.mutator = listed_facts;
    listed_inputs.listings.push_back(std::move(listed));
    lib::Listing walls = listing(core, Kind::mutator, "ridge-walls", "Ridge Walls");
    walls.mutator = lib::MutatorFacts{};
    walls.mutator->works_with.push_back({"ridge", {}, "Ridge"});
    listed_inputs.listings.push_back(std::move(walls));
    lib::Listing bare = listing(core, Kind::mutator, "bare", "Bare");
    bare.homepage.clear();
    bare.requires_engine.clear();
    bare.mutator = lib::MutatorFacts{};
    listed_inputs.listings.push_back(std::move(bare));
    lib::Library listed_library = built(std::move(listed_inputs));
    lib::set_tab(listed_library, lib::Tab::mutators);

    const lib::Entry* fast = at(listed_library, core, Kind::mutator, "fast-builders");
    OA_CHECK(fast != nullptr);
    if (fast != nullptr) {
        OA_CHECK(lib::status_line(listed_library, *fast) == "Not installed · 2.0 · rev 1");
        OA_CHECK(fast->not_compatible);
        OA_CHECK(fast->can_act);
        const std::vector<lib::Fact> facts = lib::facts(listed_library, *fast);
        OA_CHECK(
            fact_labels(listed_library, *fast) == std::vector<std::string>(
                                                      {"Latest",
                                                       "Author",
                                                       "Works with",
                                                       "Needs",
                                                       "Needs",
                                                       "Options",
                                                       "Changes",
                                                       "Rules",
                                                       "From"}
                                                  )
        );
        OA_CHECK(fact(facts, "Checked") == nullptr);
        OA_CHECK(fact(facts, "Size") == nullptr);
        OA_CHECK(fact(facts, "Installed") == nullptr);
        const lib::Fact* works = fact(facts, "Works with");
        OA_CHECK(works != nullptr && works->long_value == "Ridge" && works->mark == Mark::warn);
        OA_CHECK(works != nullptr && works->short_value == works->long_value);
        const lib::Fact* needs = fact(facts, "Needs");
        OA_CHECK(
            needs != nullptr && needs->long_value == "Gatehouse 4.0 or later: not installed" &&
            needs->mark == Mark::warn
        );
        OA_CHECK(facts.size() >= 5);
        if (facts.size() >= 5) {
            OA_CHECK(facts[4].long_value == "Open Annihilation 0.8.0 or later");
            OA_CHECK(facts[4].short_value == "OA 0.8.0+");
            OA_CHECK(facts[4].mark == Mark::good);
        }
        const lib::Fact* options = fact(facts, "Options");
        OA_CHECK(
            options != nullptr && options->long_value == "Income: frugal, moderate or wealthy" &&
            options->short_value == options->long_value
        );
        const lib::Fact* changes = fact(facts, "Changes");
        OA_CHECK(
            changes != nullptr && changes->long_value == "Files and Units" &&
            changes->short_value == changes->long_value
        );
        const lib::Fact* rules = fact(facts, "Rules");
        OA_CHECK(
            rules != nullptr && rules->long_value == "Changes the game: unranked on Core Prime" &&
            rules->short_value == rules->long_value
        );
        // Gatehouse is not listed, so GET WITH is off: nothing offers it in range.
        const std::vector<std::pair<Action, bool>> expected = {
            {Action::get_with, false},
            {Action::homepage, true},
        };
        OA_CHECK(actions_of(listed_library, *fast) == expected);
    }

    const lib::Entry* ridge_walls = at(listed_library, core, Kind::mutator, "ridge-walls");
    OA_CHECK(ridge_walls != nullptr && ridge_walls->not_compatible && ridge_walls->can_act);
    if (ridge_walls != nullptr) {
        const auto ridge_facts = lib::facts(listed_library, *ridge_walls);
        const lib::Fact* works = fact(ridge_facts, "Works with");
        OA_CHECK(works != nullptr && works->long_value == "Ridge" && works->mark == Mark::warn);
        OA_CHECK(actions_of(listed_library, *ridge_walls).front().first == Action::get);
        OA_CHECK(actions_of(listed_library, *ridge_walls).front().second);
    }

    // A prerequisite that names no version range is just the mutator's name.
    {
        lib::Inputs plain = base_inputs();
        lib::Listing needs_plain = listing(core, Kind::mutator, "needs-plain", "Needs Plain");
        lib::MutatorFacts plain_facts;
        plain_facts.needs.push_back({"gatehouse", "", "Gatehouse"});
        needs_plain.mutator = plain_facts;
        plain.listings.push_back(std::move(needs_plain));
        const lib::Library missing_range = built(plain);
        const lib::Entry* missing_entry = at(missing_range, core, Kind::mutator, "needs-plain");
        OA_CHECK(missing_entry != nullptr);
        if (missing_entry != nullptr) {
            const std::vector<lib::Fact> plain_list = lib::facts(missing_range, *missing_entry);
            const lib::Fact* needs = fact(plain_list, "Needs");
            OA_CHECK(
                needs != nullptr && needs->long_value == "Gatehouse: not installed" &&
                needs->short_value == needs->long_value && needs->mark == Mark::warn
            );
        }
        plain.installed.push_back(installed(core, Kind::mutator, "gatehouse", "Gatehouse"));
        const lib::Library met_range = built(std::move(plain));
        const lib::Entry* met_entry = at(met_range, core, Kind::mutator, "needs-plain");
        OA_CHECK(met_entry != nullptr);
        if (met_entry != nullptr) {
            const std::vector<lib::Fact> plain_list = lib::facts(met_range, *met_entry);
            const lib::Fact* needs = fact(plain_list, "Needs");
            OA_CHECK(
                needs != nullptr && needs->long_value == "Gatehouse ✓" &&
                needs->short_value == needs->long_value && needs->mark == Mark::good
            );
        }
    }

    const lib::Entry* bare_entry = at(listed_library, core, Kind::mutator, "bare");
    OA_CHECK(bare_entry != nullptr);
    if (bare_entry != nullptr) {
        const std::vector<std::string> labels = fact_labels(listed_library, *bare_entry);
        OA_CHECK(fact(lib::facts(listed_library, *bare_entry), "Options") == nullptr);
        OA_CHECK(fact(lib::facts(listed_library, *bare_entry), "Changes") == nullptr);
        OA_CHECK(fact(lib::facts(listed_library, *bare_entry), "Checked") == nullptr);
        OA_CHECK(fact(lib::facts(listed_library, *bare_entry), "Needs") == nullptr);
        for (const std::string& label : labels)
            OA_CHECK(
                label != "Size" && label != "Base" && label != "Games" && label != "Hacks" &&
                label != "Maps" && label != "Coverage"
            );
    }

    const auto checked_of = [](const lib::Verdict& verdict) {
        const lib::Library library = mutator_library(verdict, false, false);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr);
        if (entry == nullptr)
            return lib::Fact{};
        const std::vector<lib::Fact> facts = lib::facts(library, *entry);
        OA_CHECK(facts.size() >= 2 && facts[0].long_label == "Installed");
        OA_CHECK(facts.size() >= 2 && facts[0].long_value == "1.0 (rev 1)");
        OA_CHECK(facts.size() >= 2 && facts[1].long_label == "Latest");
        OA_CHECK(facts.size() >= 2 && facts[1].long_value == "2.0 (rev 1)");
        const lib::Fact* checked = fact(facts, "Checked");
        OA_CHECK(checked != nullptr);
        if (checked == nullptr)
            return lib::Fact{};
        OA_CHECK(checked->short_value == checked->long_value);
        return *checked;
    };

    lib::Verdict compatible;
    compatible.state = lib::Verdict::State::compatible;
    compatible.summary = "changes 2 units";
    const lib::Fact good = checked_of(compatible);
    OA_CHECK(good.long_value == "With No Mod: changes 2 units" && good.mark == Mark::good);

    lib::Verdict none = no_effect_verdict;
    const lib::Fact quiet = checked_of(none);
    OA_CHECK(quiet.long_value == "With No Mod: no effect" && quiet.mark == Mark::warn);

    const lib::Fact blocked = checked_of(incompatible_verdict);
    OA_CHECK(blocked.long_value == "With No Mod: needs a unit" && blocked.mark == Mark::bad);

    const lib::Fact stopped = checked_of(failed_verdict);
    OA_CHECK(
        stopped.long_value == "With No Mod: it stopped: the dry run stopped" &&
        stopped.mark == Mark::bad
    );

    lib::Verdict running;
    running.state = lib::Verdict::State::checking;
    const lib::Fact checking = checked_of(running);
    OA_CHECK(checking.long_value == "Checking with No Mod…" && checking.mark == Mark::none);

    lib::Verdict not_yet;
    not_yet.state = lib::Verdict::State::unchecked;
    const lib::Fact waiting = checked_of(not_yet);
    OA_CHECK(waiting.long_value == "Not checked with No Mod" && waiting.mark == Mark::none);

    // Unset verdict: no Checked fact and no CHECK WITH THIS MOD.
    {
        const lib::Library library = mutator_library(std::nullopt, false, false);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            OA_CHECK(fact(lib::facts(library, *entry), "Checked") == nullptr);
            for (const auto& [action, enabled] : actions_of(library, *entry)) {
                static_cast<void>(enabled);
                OA_CHECK(action != Action::check_with_mod);
            }
        }
    }

    // Incompatible, with an update and a kept copy: still UPDATE and CHECK, no ROLL BACK.
    {
        const lib::Library library = mutator_library(incompatible_verdict, true, false);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr && entry->not_compatible && entry->can_act);
        if (entry != nullptr) {
            const std::vector<std::pair<Action, bool>> expected = {
                {Action::update, true},
                {Action::check_with_mod, true},
                {Action::uninstall, true},
                {Action::open_folder, true},
                {Action::homepage, true},
            };
            OA_CHECK(actions_of(library, *entry) == expected);
        }
    }

    // Listed actions, a queue, a failure, an install, and CHECK off in a game and while checking.
    {
        lib::Inputs inputs = base_inputs();
        lib::Listing made = listing(core, Kind::mutator, "queued-one", "Queued One");
        made.homepage = "https://example.test/queued";
        made.mutator = lib::MutatorFacts{};
        inputs.listings.push_back(made);
        lib::QueueItem waiting_item = queued(core, "queued-one", "Queued One", Phase::waiting);
        waiting_item.kind = Kind::mutator;
        inputs.queue.push_back(waiting_item);
        lib::Listing failed_listing = listing(core, Kind::mutator, "failed-one", "Failed One");
        failed_listing.mutator = lib::MutatorFacts{};
        inputs.listings.push_back(failed_listing);
        lib::QueueItem failed_item = queued(core, "failed-one", "Failed One", Phase::failed);
        failed_item.kind = Kind::mutator;
        failed_item.retryable = true;
        inputs.queue.push_back(failed_item);
        lib::Library library = built(std::move(inputs));
        const lib::Entry* queued_entry = at(library, core, Kind::mutator, "queued-one");
        const lib::Entry* failed_entry = at(library, core, Kind::mutator, "failed-one");
        OA_CHECK(queued_entry != nullptr && failed_entry != nullptr);
        if (queued_entry != nullptr) {
            const std::vector<std::pair<Action, bool>> expected = {
                {Action::cancel, true},
                {Action::homepage, true},
            };
            OA_CHECK(actions_of(library, *queued_entry) == expected);
        }
        if (failed_entry != nullptr) {
            const std::vector<std::pair<Action, bool>> expected = {{Action::retry, true}};
            OA_CHECK(actions_of(library, *failed_entry) == expected);
        }
    }
    {
        const lib::Library library = mutator_library(compatible, false, false);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            const std::vector<std::pair<Action, bool>> expected = {
                {Action::check_with_mod, true},
                {Action::uninstall, true},
                {Action::open_folder, true},
                {Action::homepage, true},
            };
            OA_CHECK(actions_of(library, *entry) == expected);
        }
    }
    {
        const lib::Library library = mutator_library(compatible, false, true);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            const std::vector<std::pair<Action, bool>> actions = actions_of(library, *entry);
            bool saw_check = false;
            for (const auto& [action, enabled] : actions) {
                if (action == Action::check_with_mod) {
                    saw_check = true;
                    OA_CHECK(!enabled);
                }
                OA_CHECK(
                    action != Action::roll_back && action != Action::play_now &&
                    action != Action::switch_off && action != Action::use_now &&
                    action != Action::turn_on
                );
            }
            OA_CHECK(saw_check);
        }
    }
    {
        lib::Verdict checking_verdict;
        checking_verdict.state = lib::Verdict::State::checking;
        const lib::Library library = mutator_library(checking_verdict, false, false);
        const lib::Entry* entry = at(library, core, Kind::mutator, "fast-builders");
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            bool saw_check = false;
            for (const auto& [action, enabled] : actions_of(library, *entry)) {
                if (action == Action::check_with_mod) {
                    saw_check = true;
                    OA_CHECK(!enabled);
                }
            }
            OA_CHECK(saw_check);
        }
    }
}

/// A brain's facts, both forms, in order, and the actions and status lines
/// around Default. A verdict is put on by hand. Ridge Raider's GET stays on
/// while no mod is played.
void test_brain_details() {
    using Buttons = std::vector<std::pair<Action, bool>>;
    const auto same_forms = [](const lib::Fact& item) {
        OA_CHECK(item.short_label == item.long_label);
        OA_CHECK(item.short_value == item.long_value);
    };

    lib::Inputs rich_inputs = base_inputs();
    lib::Listing wide = listing(core, Kind::brain, "wide-watch", "Wide Watch");
    wide.homepage = "https://example.test/wide";
    lib::BrainFacts wide_facts;
    wide_facts.levels.push_back({"hold", "Hold", lib::BrainDifficulty::easy});
    wide_facts.levels.push_back({"push", "Push", lib::BrainDifficulty::medium});
    wide_facts.levels.push_back({"crush", "Crush", lib::BrainDifficulty::hard});
    wide_facts.works_with.push_back({"ridge", "", "Ridge"});
    wide_facts.works_with.push_back({"cinder", "", "Cinder"});
    wide_facts.requires_engine = ">= 0.8.0";
    wide_facts.requires_expansions = {"core-contingency"};
    wide_facts.full_awareness = true;
    wide_facts.network_bytes = 2 * mib;
    wide.brain = wide_facts;
    lib::Installed wide_held = installed(core, Kind::brain, "wide-watch", "Wide Watch");
    wide_held.brain = wide_facts;
    rich_inputs.listings.push_back(std::move(wide));
    rich_inputs.installed.push_back(std::move(wide_held));
    const lib::Library rich = built(std::move(rich_inputs));
    const lib::Entry* wide_row = at(rich, core, Kind::brain, "wide-watch");
    OA_CHECK(wide_row != nullptr);
    if (wide_row != nullptr) {
        const std::vector<lib::Fact> facts = lib::facts(rich, *wide_row);
        OA_CHECK(
            fact_labels(rich, *wide_row) == std::vector<std::string>(
                                                {"Installed",
                                                 "Latest",
                                                 "Author",
                                                 "Levels",
                                                 "Works with",
                                                 "Needs",
                                                 "Needs",
                                                 "Full awareness",
                                                 "Network",
                                                 "Size",
                                                 "From"}
                                            )
        );
        OA_CHECK(facts.size() == 11);
        if (facts.size() == 11) {
            OA_CHECK(facts[0].long_value == "1.0 (rev 1)");
            same_forms(facts[0]);
            OA_CHECK(facts[1].long_value == "1.0 (rev 1)");
            same_forms(facts[1]);
            OA_CHECK(facts[2].long_value == "Fixture Author");
            same_forms(facts[2]);
            OA_CHECK(facts[3].long_value == "Hold, Push and Crush");
            same_forms(facts[3]);
            OA_CHECK(facts[3].mark == Mark::none);
            OA_CHECK(facts[4].long_value == "Ridge or Cinder" && facts[4].mark == Mark::warn);
            same_forms(facts[4]);
            OA_CHECK(facts[5].long_value == "Open Annihilation 0.8.0 or later");
            OA_CHECK(facts[5].short_value == "OA 0.8.0+");
            OA_CHECK(facts[5].mark == Mark::good);
            OA_CHECK(facts[6].long_value == "Core Contingency" && facts[6].mark == Mark::warn);
            same_forms(facts[6]);
            OA_CHECK(facts[7].long_value == lib::words::full_awareness_sees);
            OA_CHECK(facts[7].mark == Mark::warn);
            same_forms(facts[7]);
            OA_CHECK(
                facts[8].long_value == "Uses a learned network (" + lib::size_text(2 * mib) + ")"
            );
            OA_CHECK(facts[8].mark == Mark::none);
            same_forms(facts[8]);
            OA_CHECK(facts[9].long_value == lib::size_text(64 * mib));
            same_forms(facts[9]);
            OA_CHECK(facts[10].long_value == "Fixture Core");
            same_forms(facts[10]);
        }
        for (const std::string& label : fact_labels(rich, *wide_row))
            OA_CHECK(
                label != "Base" && label != "Games" && label != "Hacks" && label != "Maps" &&
                label != "Coverage" && label != "Options" && label != "Changes" && label != "Rules"
            );
        OA_CHECK(fact(facts, "Checked") == nullptr);
        OA_CHECK_TEXT(lib::status_line(rich, *wide_row), "Installed");
    }

    lib::Inputs sparse_inputs = base_inputs();
    lib::Listing lantern = listing(core, Kind::brain, "lantern-scout", "Lantern Scout");
    lantern.homepage = "https://example.test/lantern";
    lantern.brain = lib::BrainFacts{};
    sparse_inputs.listings.push_back(std::move(lantern));
    const lib::Library sparse = built(std::move(sparse_inputs));
    const lib::Entry* lantern_row = at(sparse, core, Kind::brain, "lantern-scout");
    OA_CHECK(lantern_row != nullptr);
    if (lantern_row != nullptr) {
        const std::vector<lib::Fact> facts = lib::facts(sparse, *lantern_row);
        OA_CHECK(
            fact_labels(sparse, *lantern_row) ==
            std::vector<std::string>({"Latest", "Author", "Works with", "Size", "From"})
        );
        OA_CHECK(fact(facts, "Installed") == nullptr);
        OA_CHECK(fact(facts, "Levels") == nullptr);
        OA_CHECK(fact(facts, "Needs") == nullptr);
        OA_CHECK(fact(facts, "Full awareness") == nullptr);
        OA_CHECK(fact(facts, "Network") == nullptr);
        OA_CHECK(fact(facts, "Checked") == nullptr);
        const lib::Fact* works = fact(facts, "Works with");
        OA_CHECK(works != nullptr && works->long_value == "Any mod" && works->mark == Mark::none);
        OA_CHECK(works != nullptr && works->short_value == works->long_value);
        OA_CHECK_TEXT(lib::status_line(sparse, *lantern_row), "Not installed · 1.0 · rev 1");
        OA_CHECK(
            actions_of(sparse, *lantern_row) ==
            Buttons({{Action::get, true}, {Action::homepage, true}})
        );
    }

    lib::Inputs plain = base_inputs();
    const lib::Library built_in_library = built(std::move(plain));
    const lib::Entry* built_in = lib::find_entry(built_in_library, lib::built_in_brain_id());
    OA_CHECK(built_in != nullptr && built_in->is_default);
    if (built_in != nullptr) {
        const std::vector<lib::Fact> facts = lib::facts(built_in_library, *built_in);
        OA_CHECK(
            fact_labels(built_in_library, *built_in) ==
            std::vector<std::string>({"Levels", "Works with"})
        );
        OA_CHECK(facts.size() == 2);
        if (facts.size() == 2) {
            OA_CHECK(facts[0].long_value == "Easy, Medium and Hard");
            same_forms(facts[0]);
            OA_CHECK(facts[1].long_value == "Any mod" && facts[1].mark == Mark::none);
            same_forms(facts[1]);
        }
        OA_CHECK(actions_of(built_in_library, *built_in).empty());
        OA_CHECK_TEXT(lib::status_line(built_in_library, *built_in), "Built in · Default");
    }

    const auto brain_with = [](std::optional<lib::Verdict> verdict,
                               bool is_default,
                               bool in_match) {
        lib::Inputs inputs = base_inputs();
        inputs.in_match = in_match;
        if (is_default)
            inputs.default_brain = "steady-commander";
        lib::Listing listed = listing(core, Kind::brain, "steady-commander", "Steady Commander");
        listed.homepage = "https://example.test/steady";
        lib::BrainFacts facts;
        facts.works_with.push_back({"*", "", ""});
        listed.brain = facts;
        lib::Installed held = installed(core, Kind::brain, "steady-commander", "Steady Commander");
        held.brain = facts;
        if (verdict)
            held.brain->verdict = std::move(*verdict);
        inputs.listings.push_back(std::move(listed));
        inputs.installed.push_back(std::move(held));
        return built(std::move(inputs));
    };
    const auto steady_of = [](const lib::Library& library) {
        return at(library, core, Kind::brain, "steady-commander");
    };
    const auto checked_of = [&](const lib::Verdict& verdict) {
        const lib::Library library = brain_with(verdict, false, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr);
        if (entry == nullptr)
            return lib::Fact{};
        const std::vector<lib::Fact> facts = lib::facts(library, *entry);
        const lib::Fact* checked = fact(facts, "Checked");
        OA_CHECK(checked != nullptr);
        if (checked == nullptr)
            return lib::Fact{};
        OA_CHECK(checked->short_value == checked->long_value);
        OA_CHECK(checked->short_label == checked->long_label);
        return *checked;
    };

    lib::Verdict compatible;
    compatible.state = lib::Verdict::State::compatible;
    compatible.summary = "it fits this game";
    const lib::Fact good = checked_of(compatible);
    OA_CHECK(good.long_value == "With No Mod: it fits this game" && good.mark == Mark::good);

    lib::Verdict quiet;
    quiet.state = lib::Verdict::State::no_effect;
    const lib::Fact no_effect = checked_of(quiet);
    OA_CHECK(no_effect.long_value == "With No Mod: no effect" && no_effect.mark == Mark::warn);

    lib::Verdict blocked;
    blocked.state = lib::Verdict::State::incompatible;
    blocked.reason = "No build list for this mod's third side";
    const lib::Fact unfit = checked_of(blocked);
    OA_CHECK(
        unfit.long_value == "With No Mod: No build list for this mod's third side" &&
        unfit.mark == Mark::bad
    );
    {
        const lib::Library library = brain_with(blocked, false, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr && entry->not_compatible && entry->can_act);
    }

    lib::Verdict stopped;
    stopped.state = lib::Verdict::State::failed;
    stopped.reason = "broken on purpose";
    const lib::Fact failed = checked_of(stopped);
    OA_CHECK(
        failed.long_value == "With No Mod: it stopped: broken on purpose" &&
        failed.mark == Mark::bad
    );

    lib::Verdict running;
    running.state = lib::Verdict::State::checking;
    const lib::Fact checking = checked_of(running);
    OA_CHECK(checking.long_value == "Checking with No Mod…" && checking.mark == Mark::none);

    lib::Verdict not_yet;
    not_yet.state = lib::Verdict::State::unchecked;
    const lib::Fact waiting = checked_of(not_yet);
    OA_CHECK(waiting.long_value == "Not checked with No Mod" && waiting.mark == Mark::none);

    {
        lib::Inputs named = base_inputs();
        named.played = {"ridge", "28", "Ridge"};
        lib::Listing listed = listing(core, Kind::brain, "steady-commander", "Steady Commander");
        lib::BrainFacts facts;
        facts.works_with.push_back({"*", "", ""});
        facts.verdict = compatible;
        lib::Installed held = installed(core, Kind::brain, "steady-commander", "Steady Commander");
        held.brain = facts;
        listed.brain = facts;
        named.listings.push_back(std::move(listed));
        named.installed.push_back(std::move(held));
        const lib::Library library = built(std::move(named));
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            const std::vector<lib::Fact> checked_facts = lib::facts(library, *entry);
            const lib::Fact* checked = fact(checked_facts, "Checked");
            OA_CHECK(
                checked != nullptr && checked->long_value == "With Ridge: it fits this game" &&
                checked->mark == Mark::good
            );
        }
    }

    {
        const lib::Library library = brain_with(std::nullopt, false, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            OA_CHECK(fact(lib::facts(library, *entry), "Checked") == nullptr);
            OA_CHECK(
                actions_of(library, *entry) == Buttons(
                                                   {{Action::use_by_default, true},
                                                    {Action::uninstall, true},
                                                    {Action::open_folder, true},
                                                    {Action::homepage, true}}
                                               )
            );
            OA_CHECK_TEXT(lib::status_line(library, *entry), "Installed");
            const lib::Entry* player = lib::find_entry(library, lib::built_in_brain_id());
            OA_CHECK(player != nullptr && player->is_default);
            if (player != nullptr)
                OA_CHECK_TEXT(lib::status_line(library, *player), "Built in · Default");
        }
    }
    {
        const lib::Library library = brain_with(compatible, false, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr && !entry->is_default);
        if (entry != nullptr)
            OA_CHECK(
                actions_of(library, *entry) == Buttons(
                                                   {{Action::use_by_default, true},
                                                    {Action::check_with_mod, true},
                                                    {Action::uninstall, true},
                                                    {Action::open_folder, true},
                                                    {Action::homepage, true}}
                                               )
            );
    }
    {
        const lib::Library library = brain_with(compatible, true, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr && entry->is_default);
        if (entry != nullptr) {
            OA_CHECK(
                actions_of(library, *entry) == Buttons(
                                                   {{Action::check_with_mod, true},
                                                    {Action::uninstall, true},
                                                    {Action::open_folder, true},
                                                    {Action::homepage, true}}
                                               )
            );
            OA_CHECK_TEXT(lib::status_line(library, *entry), "Installed · Default");
        }
        const lib::Entry* player = lib::find_entry(library, lib::built_in_brain_id());
        OA_CHECK(player != nullptr && !player->is_default);
        if (player != nullptr) {
            OA_CHECK(actions_of(library, *player) == Buttons({{Action::use_by_default, true}}));
            OA_CHECK_TEXT(lib::status_line(library, *player), "Built in");
        }
    }
    {
        const lib::Library library = brain_with(compatible, false, true);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            bool saw = false;
            for (const auto& [action, enabled] : actions_of(library, *entry)) {
                if (action == Action::check_with_mod) {
                    saw = true;
                    OA_CHECK(!enabled);
                }
            }
            OA_CHECK(saw);
        }
    }
    {
        const lib::Library library = brain_with(running, false, false);
        const lib::Entry* entry = steady_of(library);
        OA_CHECK(entry != nullptr);
        if (entry != nullptr) {
            bool saw = false;
            for (const auto& [action, enabled] : actions_of(library, *entry)) {
                if (action == Action::check_with_mod) {
                    saw = true;
                    OA_CHECK(!enabled);
                }
            }
            OA_CHECK(saw);
        }
    }

    {
        lib::Inputs inputs = base_inputs();
        lib::Listing listed = listing(core, Kind::brain, "tide-keeper", "Tide Keeper");
        listed.homepage = "https://example.test/tide";
        lib::BrainFacts facts;
        facts.works_with.push_back({"*", "", ""});
        facts.verdict = compatible;
        listed.brain = facts;
        lib::Installed held = installed(core, Kind::brain, "tide-keeper", "Tide Keeper");
        held.brain = facts;
        inputs.listings.push_back(std::move(listed));
        inputs.installed.push_back(std::move(held));
        inputs.updates.push_back(update(core, Kind::brain, "tide-keeper"));
        inputs.default_brain = "tide-keeper";
        const lib::Library library = built(std::move(inputs));
        const lib::Entry* entry = at(library, core, Kind::brain, "tide-keeper");
        OA_CHECK(entry != nullptr && entry->state == State::update && entry->is_default);
        if (entry != nullptr) {
            OA_CHECK(
                actions_of(library, *entry) == Buttons(
                                                   {{Action::update, true},
                                                    {Action::check_with_mod, true},
                                                    {Action::uninstall, true},
                                                    {Action::open_folder, true},
                                                    {Action::homepage, true}}
                                               )
            );
            OA_CHECK_TEXT(
                lib::status_line(library, *entry),
                "Version 4.8 available · " + lib::size_text(3355443) + " to download"
            );
            const std::vector<lib::Fact> update_facts = lib::facts(library, *entry);
            const lib::Fact* size = fact(update_facts, "Size");
            OA_CHECK(size != nullptr);
            if (size != nullptr) {
                OA_CHECK(
                    size->long_value ==
                    lib::size_text(64 * mib) + " · " + lib::size_text(3355443) + " to download"
                );
                OA_CHECK(size->short_value == lib::size_text(3355443) + " to download");
            }
        }
    }
    {
        lib::Inputs inputs = base_inputs();
        lib::Listing listed = listing(core, Kind::brain, "ridge-raider", "Ridge Raider");
        listed.homepage = "https://example.test/ridge";
        lib::BrainFacts facts;
        facts.works_with.push_back({"ridge", "", "Ridge"});
        listed.brain = facts;
        inputs.listings.push_back(std::move(listed));
        lib::QueueItem waiting_item;
        waiting_item.registry = std::string(core);
        waiting_item.kind = Kind::brain;
        waiting_item.key = "ridge-raider";
        waiting_item.name = "Ridge Raider";
        waiting_item.phase = Phase::downloading;
        inputs.queue.push_back(waiting_item);
        const lib::Library queued_library = built(inputs);
        const lib::Entry* queued_row = at(queued_library, core, Kind::brain, "ridge-raider");
        OA_CHECK(queued_row != nullptr && queued_row->not_compatible && queued_row->can_act);
        if (queued_row != nullptr)
            OA_CHECK(
                actions_of(queued_library, *queued_row) ==
                Buttons({{Action::cancel, true}, {Action::homepage, true}})
            );
        inputs.queue.clear();
        lib::QueueItem failed_item = waiting_item;
        failed_item.phase = Phase::failed;
        failed_item.retryable = true;
        inputs.queue.push_back(failed_item);
        const lib::Library failed_library = built(std::move(inputs));
        const lib::Entry* failed_row = at(failed_library, core, Kind::brain, "ridge-raider");
        OA_CHECK(failed_row != nullptr && failed_row->can_act && failed_row->blocked.empty());
        if (failed_row != nullptr) {
            OA_CHECK(
                actions_of(failed_library, *failed_row) ==
                Buttons({{Action::retry, true}, {Action::homepage, true}})
            );
            OA_CHECK(actions_of(failed_library, *failed_row).front().second);
        }
    }
    {
        lib::Inputs inputs = base_inputs();
        lib::Listing listed = listing(core, Kind::brain, "ridge-raider", "Ridge Raider");
        lib::BrainFacts facts;
        facts.works_with.push_back({"ridge", "", "Ridge"});
        listed.brain = facts;
        inputs.listings.push_back(std::move(listed));
        const lib::Library library = built(std::move(inputs));
        const lib::Entry* row = at(library, core, Kind::brain, "ridge-raider");
        OA_CHECK(row != nullptr && row->state == State::get && row->not_compatible);
        OA_CHECK(row != nullptr && row->can_act && row->blocked.empty());
        if (row != nullptr)
            OA_CHECK(actions_of(library, *row).front() == std::make_pair(Action::get, true));
    }
}

} // namespace

int main() {
    test_state_matrix();
    test_queue_states();
    test_blocked_reasons();
    test_installed_never_blocked();
    test_registries();
    test_tabs_filters_tags();
    test_opens_on_active();
    test_new_filter();
    test_active_setup();
    test_order_without_search();
    test_selection();
    test_entry_actions();
    test_switch_off();
    test_base_game();
    test_expansion_pills();
    test_expansion_download();
    test_expansion_names_translated();
    test_base_chooser();
    test_base_chooser_through_refresh();
    test_expansion_off_question_through_refresh();
    test_base_game_after_required_off();
    test_base_game_round_trip();
    test_tile_applies_after_any_choices();
    test_command_line_lock();
    test_check_now();
    test_built_in_languages();
    test_tab_counts();
    test_added_package_count();
    test_tabs_list_what_their_counts_hold();
    test_rolled_back_update_returns();
    test_own_mod_description();
    test_facts();
    test_status_lines_and_bylines();
    test_playing_note();
    test_queue_lines();
    test_change_texts();
    test_texts();
    test_registry_texts();
    test_package_needs();
    test_mutators();
    test_brains();
    test_keys();
    test_brain_details();
    test_checked_words();
    test_mutator_details();
    test_lookup();
    return oa::test::check_exit_status();
}
