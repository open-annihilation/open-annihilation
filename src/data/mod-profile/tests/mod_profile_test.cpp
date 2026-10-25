// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The resolver: every way to write a limit or hack (true, false, a preset
// with overrides, a shorthand scalar), settings bindings with registry seeds
// and clamping, the host's match options, script-extension mounts, data keys,
// every refusal, the records it fills, the canonical form and the hashes.
// The hashes pinned here are the ones the reference resolver gives for the
// same profiles and registry.
//
// With --references, resolves every profile in the folders under the folder
// OA_MOD_PROFILES_DIR names and checks each against the pinned hashes of the
// reference profiles; without that folder it skips (exit 77).

#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/platform/system.hpp"
#include "oa/test/check.hpp"

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names this build's version, which an unmet requirement is checked against"
#endif

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <iterator>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace oa::data::mod_profile;
namespace fs = std::filesystem;

/// Returns a hack this engine does not carry out yet, which a profile turns
/// on with true.
///
/// @return its ID, or empty once every hack is implemented
std::string_view unimplemented_hack() {
    for (const registry::Entry& entry : registry::table().entries)
        if (entry.kind == registry::EntryKind::hack && !entry.implemented && !entry.visual)
            return entry.id;
    return {};
}

/// Counts the hacks among some IDs that this engine does not carry out yet.
///
/// @param ids hack IDs
/// @return how many the registry marks not implemented
size_t unimplemented_among(std::initializer_list<std::string_view> ids) {
    size_t count = 0;
    for (const auto id : ids)
        if (const registry::Entry* entry = registry::find_entry(id);
            entry != nullptr && !entry->implemented)
            ++count;
    return count;
}

/// The test profile's head, then its blocks.
constexpr std::string_view head = "oamod: 1\n"
                                  "id: {id}\n"
                                  "name: t\n"
                                  "version: \"1\"\n"
                                  "requires: {base: ta-3.1c, catalogue: 1}\n"
                                  "author: {name: unknown}\n"
                                  "packaging: {revision: 1, date: 2026-10-04, packager: t}\n";

/// A profile with every way of writing limits and hacks, and settings bindings.
constexpr std::string_view single_body =
    "limits:\n"
    "  units-per-player: {default: 1000, max: 1200, min: 30}\n"
    "  path-search-budget: 5000\n"
    "  effects: true\n"
    "  unit-types: true\n"
    "hacks:\n"
    "  units.id-reuse-delay: 90\n"
    "  orders.weapons-free-while-busy: {preset: baseline, states: "
    "[repair, nanolathe]}\n"
    "  teams.team-number-alliances: true\n"
    "  repair.rate: false\n"
    "  console.game-speed-range: true\n"
    "  recorder.ta-demo-recorder: true\n"
    "settings:\n"
    "  limits.units-per-player.default: {registry: UnitLimit}\n"
    "  limits.unit-types.bitset-bits: {ini: Preferences/UnitType}\n"
    "  limits.effects.queue: {ini: Preferences/SfxLimit}\n"
    "  registry-seeds: {UnitLimit: 1100}\n";

/// Makes a profile's text from the head and a body.
///
/// @param id the profile's id
/// @param body the blocks after the head
/// @return the text
std::string profile_text(std::string_view id, std::string_view body = {}) {
    std::string text{head};
    text.replace(text.find("{id}"), 4, id);
    return text + std::string{body};
}

/// Resolves a profile's text, accepting hacks that are not implemented yet.
///
/// @param text the profile
/// @param options further options
/// @return the result
ResolveResult resolve(std::string_view text, ResolveOptions options = {}) {
    options.accept_unimplemented_hacks = true;
    return resolve_profile(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
        "test.oamod",
        options
    );
}

/// Tells whether resolving a profile fails with an error containing a needle.
///
/// @param text the profile
/// @param needle text the error must contain
/// @param options further options
/// @return true when it does
bool refused_with(std::string_view text, std::string_view needle, ResolveOptions options = {}) {
    const ResolveResult result = resolve(text, std::move(options));
    for (const Diagnostic& diagnostic : result.errors) {
        if (format_diagnostic(diagnostic).find(needle) != std::string::npos)
            return !result.resolution.has_value();
    }
    std::fprintf(
        stderr, "no error containing '%.*s'; got:\n", static_cast<int>(needle.size()), needle.data()
    );
    for (const Diagnostic& diagnostic : result.errors)
        std::fprintf(stderr, "  %s\n", format_diagnostic(diagnostic).c_str());
    return false;
}

/// Follows a path of keys through the effective profile.
///
/// @param resolution the resolved profile
/// @param keys the keys
/// @return the value, or null
const Value* at(const Resolution& resolution, std::initializer_list<std::string_view> keys) {
    const Value* value = &resolution.effective;
    for (const std::string_view key : keys) {
        if (value == nullptr)
            return nullptr;
        value = find_member(*value, key);
    }
    return value;
}

/// Writes a value's canonical text, or "absent".
///
/// @param value the value, or null
/// @return its text
std::string text_of(const Value* value) {
    return value != nullptr ? canonical_json(*value) : "absent";
}

/// Returns the provenance of a path.
///
/// @param resolution the resolved profile
/// @param path the path
/// @return which step set it, or empty
std::string source_of(const Resolution& resolution, std::string_view path) {
    for (const Provenance& provenance : resolution.provenance) {
        if (provenance.path == path)
            return provenance.source;
    }
    return {};
}

void test_canonical_form() {
    Value value = make_map();
    Value list = make_list();
    list.items.push_back(make_integer(1));
    list.items.push_back(make_string("\xE2\x82\xAC\n"));
    set_member(value, "b", std::move(list));
    set_member(value, "a", Value{});
    set_member(value, "\xC3\xA9", make_boolean(true));
    OA_CHECK(
        canonical_json(value) == "{\"a\":null,\"b\":[1,\"\xE2\x82\xAC\\n\"],\"\xC3\xA9\":true}"
    );
    // Keys sort by UTF-16 code units: a supplementary character sorts before U+FF61.
    Value keys = make_map();
    set_member(keys, "\xEF\xBD\xA1", make_integer(1));
    set_member(keys, "\xF0\x9F\x98\x80", make_integer(2));
    set_member(keys, "\x7F", make_integer(3));
    OA_CHECK(canonical_json(keys) == "{\"\x7F\":3,\"\xF0\x9F\x98\x80\":2,\"\xEF\xBD\xA1\":1}");
    Value control = make_string(std::string{"\x01\"\\", 3});
    OA_CHECK(canonical_json(control) == "\"\\u0001\\\"\\\\\"");
}

void test_single_profile() {
    const std::string single = profile_text("single", single_body);
    const ResolveResult result = resolve(single);
    OA_CHECK(result.errors.empty());
    if (!result.resolution)
        return;
    const Resolution& r = *result.resolution;
    OA_CHECK(
        text_of(at(r, {"limits", "units-per-player"})) ==
        "{\"default\":1100,\"limits-screen-fallback\":1500,\"max\":1200,\"min\":30}"
    );
    OA_CHECK(text_of(at(r, {"limits", "path-search-budget"})) == "{\"nodes\":5000}");
    OA_CHECK(
        text_of(at(r, {"limits", "build-list-entries"})) ==
        "{\"copy\":30,\"overflow\":\"truncate\"}"
    );
    OA_CHECK(text_of(at(r, {"hacks", "units.id-reuse-delay"})) == "{\"ticks\":90}");
    OA_CHECK(
        text_of(at(r, {"hacks", "teams.team-number-alliances", "bit7-keeps-alliances"})) == "true"
    );
    OA_CHECK(at(r, {"hacks", "repair.rate"}) == nullptr);
    // An explicit parameter beats the preset; a set lists its values in the registry's order.
    OA_CHECK(
        text_of(at(r, {"hacks", "orders.weapons-free-while-busy", "states"})) ==
        "[\"nanolathe\",\"repair\"]"
    );
    OA_CHECK(source_of(r, "hacks.units.id-reuse-delay.ticks") == "single");
    OA_CHECK(source_of(r, "limits.units-per-player.default") == "settings registry:UnitLimit");
    OA_CHECK(source_of(r, "limits.build-list-entries.copy") == "baseline");
    OA_CHECK(source_of(r, "identity.network-version") == "baseline");
    OA_CHECK(
        digest_text(r.profile.sim_hash) ==
        "cdee39782c4639170ac6b57ab6f3813bd1bfee630c2c62c197763952d1c37274"
    );
    OA_CHECK(
        digest_text(r.profile.full_hash) ==
        "6cf56cae2f2be7c4039bad5f5eb8d40d9781eec07bc317ec3c6c59207d27763b"
    );
    // Every hack turned on here that waits for its implementation says so.
    OA_CHECK(
        result.warnings.size() >= unimplemented_among(
                                      {"units.id-reuse-delay",
                                       "orders.weapons-free-while-busy",
                                       "teams.team-number-alliances",
                                       "console.game-speed-range",
                                       "recorder.ta-demo-recorder"}
                                  )
    );

    // The records.
    const ModProfile& profile = r.profile;
    OA_CHECK(profile.id == "single" && profile.name == "t" && profile.version == "1");
    OA_CHECK(profile.limits.units_per_player.default_limit == 1100);
    OA_CHECK(profile.limits.units_per_player.minimum == 30);
    OA_CHECK(profile.limits.path_search.nodes == 5000);
    OA_CHECK(profile.limits.effects.reserve == 204800);
    OA_CHECK(profile.rules.units.id_reuse_delay.enabled);
    OA_CHECK(profile.rules.units.id_reuse_delay.ticks == 90);
    OA_CHECK(!profile.rules.repair.rate.enabled);
    OA_CHECK(profile.rules.orders.weapons_free_while_busy.states.contains(
        oa::data::match_rules::OrdersWeaponsFreeWhileBusyStates::repair
    ));
    OA_CHECK(profile.rules.console.game_speed_range.minimum == 0);
    OA_CHECK(profile.recorder.ta_demo_recorder.enabled);
    OA_CHECK(profile.recorder.ta_demo_recorder.auto_extension == ".tad");
    OA_CHECK(profile.registry_seeds.size() == 1 && profile.registry_seeds[0].value == "1100");
    OA_CHECK(profile.registry_seeds[0].integer);

    // The player's settings: the unit-type bits step, a limit above the
    // maximum clamped, and the reserve following the queue.
    ResolveOptions settings{};
    settings.settings.ini = {{"preferences/unittype", "2000"}, {"Preferences/SfxLimit", "16000"}};
    settings.settings.registry = {{"UnitLimit", "3000"}};
    const ResolveResult r2 = resolve(single, settings);
    OA_CHECK(r2.resolution.has_value());
    if (r2.resolution) {
        OA_CHECK(text_of(at(*r2.resolution, {"limits", "unit-types", "bitset-bits"})) == "2048");
        OA_CHECK(text_of(at(*r2.resolution, {"limits", "units-per-player", "default"})) == "1200");
        OA_CHECK(
            text_of(at(*r2.resolution, {"limits", "effects"})) ==
            "{\"queue\":16000,\"reserve\":160000}"
        );
        OA_CHECK(r2.resolution->profile.sim_hash != r.profile.sim_hash);
        OA_CHECK(
            digest_text(r2.resolution->profile.sim_hash) ==
            "a9857b147eb8e5a18205568cda9ea0053e0bb65b41b2f433d66fcf59001868aa"
        );
        OA_CHECK(
            digest_text(r2.resolution->profile.full_hash) ==
            "3ede0bbfdd007205a6ce4ff463547877de2e586199f4aaac8ac25ce33e6ad30b"
        );
        bool clamped = false;
        for (const Diagnostic& warning : r2.warnings)
            clamped = clamped || format_diagnostic(warning).find("clamped to [min, max] = 1200") !=
                                     std::string::npos;
        OA_CHECK(clamped);
    }
    // A view-scope setting changes only the full hash.
    ResolveOptions view{};
    view.settings.ini = {{"Preferences/SfxLimit", "300"}};
    const ResolveResult r3 = resolve(single, view);
    OA_CHECK(r3.resolution && r3.resolution->profile.sim_hash == r.profile.sim_hash);
    OA_CHECK(r3.resolution && r3.resolution->profile.full_hash != r.profile.full_hash);
    OA_CHECK(
        r3.resolution && digest_text(r3.resolution->profile.full_hash) ==
                             "fc9f3d3e760e885d2e86947dbe3932523f4d1673509965203ca1fb2fed82cce8"
    );
    // A setting that does not read as its type is ignored with a warning.
    ResolveOptions junk{};
    junk.settings.ini = {{"Preferences/SfxLimit", "lots"}};
    const ResolveResult r_junk = resolve(single, junk);
    OA_CHECK(r_junk.resolution && r_junk.resolution->profile.full_hash == r.profile.full_hash);
    OA_CHECK(!r_junk.warnings.empty());

    // The host's options for one game.
    ResolveOptions match{};
    match.match = {
        *parse_match_option("limits.units-per-player.default=400"),
        *parse_match_option("console.game-speed-range.min=5")
    };
    const ResolveResult r4 = resolve(single, match);
    OA_CHECK(r4.resolution.has_value());
    if (r4.resolution) {
        OA_CHECK(text_of(at(*r4.resolution, {"limits", "units-per-player", "default"})) == "400");
        OA_CHECK(text_of(at(*r4.resolution, {"hacks", "console.game-speed-range", "min"})) == "5");
        OA_CHECK(
            digest_text(r4.resolution->profile.sim_hash) ==
            "fc779dd9c7b923719351fdf77c7e0fbffe5e32dfad2f9e93458dd279279571df"
        );
        OA_CHECK(r4.resolution->profile.limits.units_per_player.default_limit == 400);
        OA_CHECK(source_of(*r4.resolution, "limits.units-per-player.default") == "match");
    }
    ResolveOptions fixed{};
    fixed.match = {*parse_match_option("limits.path-search-budget.nodes=10")};
    OA_CHECK(refused_with(single, "not match", fixed));
    ResolveOptions bounds{};
    bounds.match = {*parse_match_option("console.game-speed-range.min=30")};
    OA_CHECK(refused_with(single, "outside", bounds));
    ResolveOptions absent{};
    absent.match = {*parse_match_option("repair.rate.mode=exact-remainder")};
    OA_CHECK(refused_with(single, "the parameter is fixed, not match", absent));
    OA_CHECK(!parse_match_option("no-equals").has_value());
}

void test_mutator_match() {
    const std::string single = profile_text("single", single_body);
    // A match option wins over an override of the same parameter.
    ResolveOptions both{};
    both.overrides = {HackOverride{"console.game-speed-range", true, {{"min", make_integer(3)}}}};
    both.match = {*parse_match_option("console.game-speed-range.min=7")};
    const ResolveResult won = resolve(single, both);
    OA_CHECK(won.resolution.has_value());
    if (won.resolution) {
        OA_CHECK(text_of(at(*won.resolution, {"hacks", "console.game-speed-range", "min"})) == "7");
        OA_CHECK(source_of(*won.resolution, "hacks.console.game-speed-range.min") == "match");
    }
    // An override that turns the hack on makes a match option valid.
    const std::string off = profile_text("off");
    ResolveOptions opened{};
    opened.overrides = {HackOverride{"console.game-speed-range", true, {}}};
    opened.match = {*parse_match_option("console.game-speed-range.min=4")};
    const ResolveResult opened_result = resolve(off, opened);
    OA_CHECK(opened_result.resolution.has_value());
    if (opened_result.resolution) {
        OA_CHECK(
            text_of(at(*opened_result.resolution, {"hacks", "console.game-speed-range", "min"})) ==
            "4"
        );
        OA_CHECK(
            source_of(*opened_result.resolution, "hacks.console.game-speed-range.min") == "match"
        );
    }
    // turn_on turns the hack on when the profile leaves it off. The other
    // parameters take the registry's defaults.
    MatchOption turned;
    turned.path = "console.game-speed-range.min";
    turned.value = "5";
    turned.source = "host";
    turned.turn_on = true;
    ResolveOptions host{};
    host.match = {turned};
    const ResolveResult turned_on = resolve(off, host);
    OA_CHECK(turned_on.resolution.has_value());
    if (turned_on.resolution) {
        OA_CHECK(
            text_of(at(*turned_on.resolution, {"hacks", "console.game-speed-range", "min"})) == "5"
        );
        OA_CHECK(
            text_of(at(*turned_on.resolution, {"hacks", "console.game-speed-range", "max"})) == "20"
        );
        OA_CHECK(
            text_of(at(*turned_on.resolution, {"hacks", "console.game-speed-range", "syncon"})) ==
            "true"
        );
        OA_CHECK(
            source_of(*turned_on.resolution, "hacks.console.game-speed-range.min") ==
            "match host (turned on)"
        );
        OA_CHECK(
            source_of(*turned_on.resolution, "hacks.console.game-speed-range.max") == "default"
        );
        OA_CHECK(
            source_of(*turned_on.resolution, "hacks.console.game-speed-range.syncon") == "default"
        );
    }
    // And when an override turns it off, after the profile had set it.
    const std::string narrowed = profile_text(
        "narrowed", "hacks:\n  console.game-speed-range: {min: 1, max: 12, syncon: false}\n"
    );
    ResolveOptions closed{};
    closed.overrides = {HackOverride{"console.game-speed-range", false, {}}};
    closed.match = {turned};
    const ResolveResult reopened = resolve(narrowed, closed);
    OA_CHECK(reopened.resolution.has_value());
    if (reopened.resolution) {
        OA_CHECK(
            text_of(at(*reopened.resolution, {"hacks", "console.game-speed-range", "min"})) == "5"
        );
        OA_CHECK(
            text_of(at(*reopened.resolution, {"hacks", "console.game-speed-range", "max"})) == "20"
        );
        OA_CHECK(
            text_of(at(*reopened.resolution, {"hacks", "console.game-speed-range", "syncon"})) ==
            "true"
        );
        OA_CHECK(
            source_of(*reopened.resolution, "hacks.console.game-speed-range.min") ==
            "match host (turned on)"
        );
    }
    // A limit the profile leaves off is updated in place to the defaults.
    MatchOption cap;
    cap.path = "limits.units-per-player.default";
    cap.value = "400";
    cap.source = "ridge";
    cap.turn_on = true;
    ResolveOptions limited{};
    limited.match = {cap};
    const ResolveResult capped = resolve(off, limited);
    OA_CHECK(capped.resolution.has_value());
    if (capped.resolution) {
        OA_CHECK(
            text_of(at(*capped.resolution, {"limits", "units-per-player", "default"})) == "400"
        );
        OA_CHECK(text_of(at(*capped.resolution, {"limits", "units-per-player", "max"})) == "1500");
        OA_CHECK(
            source_of(*capped.resolution, "limits.units-per-player.default") ==
            "match ridge (turned on)"
        );
        OA_CHECK(source_of(*capped.resolution, "limits.units-per-player.max") == "default");
    }
    // source shows in provenance when the entry is already on.
    MatchOption sourced;
    sourced.path = "console.game-speed-range.min";
    sourced.value = "6";
    sourced.source = "host";
    ResolveOptions named{};
    named.match = {sourced};
    const ResolveResult sourced_result = resolve(single, named);
    OA_CHECK(sourced_result.resolution.has_value());
    if (sourced_result.resolution) {
        OA_CHECK(
            source_of(*sourced_result.resolution, "hacks.console.game-speed-range.min") ==
            "match host"
        );
    }
    // turn_on on an entry the registry does not mark is an error naming it.
    MatchOption warp;
    warp.path = "setup.commander-warp.available";
    warp.value = "true";
    warp.turn_on = true;
    ResolveOptions unmarked{};
    unmarked.match = {warp};
    OA_CHECK(refused_with(off, "setup.commander-warp is not marked mutator: true", unmarked));

    MatchOption fixed_option;
    fixed_option.path = "console.game-speed-range.syncon";
    fixed_option.value = "true";
    const std::optional<std::string> fixed_problem = match_option_problem(fixed_option);
    OA_CHECK(fixed_problem.has_value() && fixed_problem->find("fixed") != std::string::npos);
    MatchOption unknown;
    unknown.path = "nope.nope";
    unknown.value = "1";
    const std::optional<std::string> unknown_problem = match_option_problem(unknown);
    OA_CHECK(
        unknown_problem.has_value() &&
        unknown_problem->find("unknown parameter") != std::string::npos
    );
    MatchOption range;
    range.path = "console.game-speed-range.min";
    range.value = "30";
    const std::optional<std::string> range_problem = match_option_problem(range);
    OA_CHECK(range_problem.has_value() && range_problem->find("outside") != std::string::npos);
    MatchOption good;
    good.path = "console.game-speed-range.min";
    good.value = "5";
    OA_CHECK(!match_option_problem(good).has_value());

    std::set<std::string> marked;
    for (const registry::Entry& entry : registry::table().entries) {
        if (!entry.mutator)
            continue;
        marked.insert(std::string{entry.id});
        OA_CHECK(!entry.visual);
        OA_CHECK(entry.scope != registry::Scope::view);
        OA_CHECK(!std::string{entry.id}.starts_with("ui."));
        bool match = false;
        for (const registry::Parameter& parameter : registry::parameters_of(entry)) {
            if (parameter.adjustable != registry::Adjustable::match)
                continue;
            match = true;
            OA_CHECK(parameter.value.scope != registry::Scope::view);
        }
        OA_CHECK(match);
    }
    OA_CHECK(marked.size() == 2);
    OA_CHECK(marked.count("limits.units-per-player") == 1);
    OA_CHECK(marked.count("console.game-speed-range") == 1);
}

void test_base_profile() {
    // A profile that changes nothing resolves to 3.1c's records.
    const ResolveResult result = resolve(profile_text("minimal"));
    OA_CHECK(result.resolution.has_value() && result.warnings.empty());
    if (!result.resolution)
        return;
    const ModProfile& profile = result.resolution->profile;
    OA_CHECK(profile.rules == oa::data::match_rules::MatchRules{});
    OA_CHECK(profile.limits == oa::data::limits::Limits{});
    OA_CHECK(profile.identity == Identity{} && profile.layout == Layout{});
    OA_CHECK(profile.strings == Strings{} && profile.media == Media{});
    OA_CHECK(profile.network == NetworkRules{} && profile.recorder == RecorderRules{});
    OA_CHECK(profile.ui == UiRules{} && profile.data_keys == DataKeyBindings{});
    OA_CHECK(
        digest_text(profile.sim_hash) ==
        "1502111e3b1f69bd93be99405e3c61e0c6337698a7c8502f9f6320e80600094d"
    );
    OA_CHECK(
        digest_text(profile.full_hash) ==
        "93e104addbcc13711a9c169f9296e63b4bb4b52fe0562e7e23bde7c916d37f02"
    );
    OA_CHECK(
        describe_resolution(*result.resolution).find("sha256-sim  1502111e") != std::string::npos
    );
}

void test_blocks() {
    const std::string text = profile_text(
        "blocks",
        "identity:\n"
        "  network-version: [10, 2]\n"
        "  side-names: [North, South]\n"
        "layout:\n"
        "  revision-archive: MOD.gp3\n"
        "  directories: {units: unitsX, weapons: weaponX}\n"
        "  archive-patterns: {ufo: \"*.XYZ\"}\n"
        "  cd-check: false\n"
        "strings:\n"
        "  status: {nanolathing: Building}\n"
        "media:\n"
        "  movies: {intro: \"\", credits: intro.smk}\n"
        "limits:\n"
        "  units-per-player: {limits-screen-fallback: 1000}\n"
        "script-extensions:\n"
        "  get: [unit.kills-x100, unit.my-id]\n"
        "  set: {}\n"
        "data-keys:\n"
        "  unit:\n"
        "    Thresholds: veterancy.thresholds\n"
        "  weapon:\n"
        "    nottoair: weapons.not-to-air\n"
        "    unused: false\n"
        "hacks:\n"
        "  veterancy.model: {preset: baseline, damage-dealt-per-level: 10}\n"
        "  ai.difficulty-names: true\n"
        "  network.vercheck: periodic-challenge\n"
    );
    const ResolveResult result = resolve(text);
    OA_CHECK(result.errors.empty());
    if (!result.resolution)
        return;
    const Resolution& r = *result.resolution;
    const ModProfile& profile = r.profile;
    OA_CHECK(text_of(at(r, {"identity", "network-version"})) == "[10,2]");
    OA_CHECK(text_of(at(r, {"layout", "directories", "units"})) == "\"unitsX\"");
    OA_CHECK(text_of(at(r, {"layout", "directories", "guis"})) == "\"guis\"");
    OA_CHECK(
        text_of(at(r, {"script-extensions", "get"})) ==
        "{\"32\":\"unit.kills-x100\",\"71\":\"unit.my-id\"}"
    );
    OA_CHECK(text_of(at(r, {"script-extensions", "fidelity"})) == "\"exact\"");
    OA_CHECK(text_of(at(r, {"data-keys", "weapon"})) == "{\"nottoair\":\"weapons.not-to-air\"}");
    OA_CHECK(
        source_of(r, "hacks.veterancy.model.default-thresholds").find("per-unit key Thresholds") !=
        std::string::npos
    );
    OA_CHECK(source_of(r, "hacks.veterancy.model.damage-dealt-cap") == "preset baseline (blocks)");
    OA_CHECK(source_of(r, "layout.directories.units") == "profile");

    OA_CHECK(profile.identity.network_version[0] == 10 && profile.identity.network_version[1] == 2);
    OA_CHECK(profile.identity.side_names[1] == "South");
    OA_CHECK(profile.identity.settings_file == "totala.ini");
    OA_CHECK(profile.layout.revision_archive == "MOD.gp3");
    OA_CHECK(profile.layout.installation_archives.empty());
    OA_CHECK(profile.layout.directories.units == "unitsX");
    OA_CHECK(profile.layout.directories.weapons == "weaponX");
    OA_CHECK(profile.layout.directories.gamedata == "gamedata");
    OA_CHECK(profile.layout.archive_patterns.ufo == "*.XYZ");
    OA_CHECK(!profile.layout.cd_check);
    OA_CHECK(profile.strings.status.nanolathing == "Building");
    OA_CHECK(profile.strings.status.paralyzed == "Paralyzed");
    OA_CHECK(profile.media.movies.intro.empty() && profile.media.movies.logo == "1.zrb");
    OA_CHECK(profile.media.movies.credits == "intro.smk");
    OA_CHECK(profile.limits.units_per_player.limits_screen_fallback == 1000);
    OA_CHECK(profile.limits.units_per_player.default_limit == 1500);
    OA_CHECK(profile.data_keys.veterancy_thresholds == "Thresholds");
    OA_CHECK(profile.data_keys.weapons_not_to_air == "nottoair");
    OA_CHECK(profile.data_keys.weapons_surface_fire.empty());
    OA_CHECK(profile.rules.veterancy.model.enabled);
    OA_CHECK(profile.rules.veterancy.model.damage_dealt_per_level == 10);
    OA_CHECK(profile.rules.veterancy.model.damage_dealt_cap == 5);
    OA_CHECK(
        profile.rules.ai.difficulty_names.names[0] ==
        oa::data::match_rules::AiDifficultyNamesNames::hard
    );
    OA_CHECK(profile.network.vercheck.enabled);
    OA_CHECK(profile.network.vercheck.revision == NetworkVercheckRevision::periodic_challenge);
    using oa::data::match_rules::ScriptExtension;
    OA_CHECK(profile.rules.script_get.count == 2);
    OA_CHECK(profile.rules.script_get.find(32) == ScriptExtension::unit_kills_x100);
    OA_CHECK(profile.rules.script_get.find(71) == ScriptExtension::unit_my_id);
    OA_CHECK(profile.rules.script_get.find(69) == ScriptExtension::none);
    OA_CHECK(profile.rules.script_set.count == 0);

    // Installation archives: the list is kept in the order written, an empty
    // list is the baseline, and a path, a repeat or two casings are refused.
    const ResolveResult archives = resolve(
        profile_text("archives", "layout:\n  installation-archives: [zeta.ccx, addon.ccx]\n")
    );
    OA_CHECK(archives.errors.empty() && archives.resolution.has_value());
    const ResolveResult left_out = resolve(profile_text("minimal"));
    const ResolveResult none =
        resolve(profile_text("minimal", "layout:\n  installation-archives: []\n"));
    OA_CHECK(left_out.resolution && none.resolution);
    if (left_out.resolution && none.resolution) {
        // An empty list hashes as a profile that leaves the key out, which is
        // the hash that profile had before the key existed.
        OA_CHECK(none.resolution->profile.sim_hash == left_out.resolution->profile.sim_hash);
        OA_CHECK(none.resolution->profile.full_hash == left_out.resolution->profile.full_hash);
        OA_CHECK(
            digest_text(none.resolution->profile.sim_hash) ==
            "1502111e3b1f69bd93be99405e3c61e0c6337698a7c8502f9f6320e80600094d"
        );
        OA_CHECK(
            digest_text(none.resolution->profile.full_hash) ==
            "93e104addbcc13711a9c169f9296e63b4bb4b52fe0562e7e23bde7c916d37f02"
        );
        OA_CHECK(none.resolution->canonical.find("installation-archives") == std::string::npos);
        OA_CHECK(text_of(at(*none.resolution, {"layout", "installation-archives"})) == "[]");
        OA_CHECK(
            describe_resolution(*none.resolution).find("\"installation-archives\": []") !=
            std::string::npos
        );
    }
    OA_CHECK(
        none.resolution && none.resolution->profile.layout.installation_archives.empty() &&
        none.resolution->profile.layout == Layout{}
    );
    if (archives.resolution && left_out.resolution) {
        OA_CHECK(
            archives.resolution->profile.layout.installation_archives ==
            (std::vector<std::string>{"zeta.ccx", "addon.ccx"})
        );
        OA_CHECK(
            text_of(at(*archives.resolution, {"layout", "installation-archives"})) ==
            "[\"zeta.ccx\",\"addon.ccx\"]"
        );
        OA_CHECK(
            archives.resolution->canonical.find(
                "\"installation-archives\":[\"zeta.ccx\",\"addon.ccx\"]"
            ) != std::string::npos
        );
        // Naming an archive changes the sim hash. The pins are this profile's.
        OA_CHECK(archives.resolution->profile.sim_hash != left_out.resolution->profile.sim_hash);
        OA_CHECK(
            digest_text(archives.resolution->profile.sim_hash) ==
            "8b68a6d256edf64bd485c0d6c9487888f46b1b41ed8b7efdb6273a9d46a4fdae"
        );
        OA_CHECK(
            digest_text(archives.resolution->profile.full_hash) ==
            "9feedf99c534fd7b70012f8b8d559f852ef11ddb90335e822eba2f3b4d43362f"
        );
    }
    // The watcher's side frame: 2, 3.1c's, hashes as a profile without the
    // key, which is the hash that profile had before the key existed; another
    // frame is view scope, so it changes the full hash and not the sim hash.
    const ResolveResult watcher_baseline =
        resolve(profile_text("minimal", "identity:\n  watcher-side-frame: 2\n"));
    const ResolveResult watcher_moved =
        resolve(profile_text("minimal", "identity:\n  watcher-side-frame: 4\n"));
    OA_CHECK(watcher_baseline.resolution && watcher_moved.resolution);
    if (watcher_baseline.resolution && watcher_moved.resolution && left_out.resolution) {
        const auto& base = left_out.resolution->profile;
        OA_CHECK(watcher_baseline.resolution->profile.full_hash == base.full_hash);
        OA_CHECK(watcher_baseline.resolution->profile.sim_hash == base.sim_hash);
        OA_CHECK(
            watcher_baseline.resolution->canonical.find("watcher-side-frame") == std::string::npos
        );
        OA_CHECK(watcher_moved.resolution->profile.identity.watcher_side_frame == 4);
        OA_CHECK(watcher_moved.resolution->profile.sim_hash == base.sim_hash);
        OA_CHECK(watcher_moved.resolution->profile.full_hash != base.full_hash);
        OA_CHECK(
            watcher_moved.resolution->canonical.find("\"watcher-side-frame\":4") !=
            std::string::npos
        );
    }
    // The skirmish's hidden sides: an empty list hashes as a profile without
    // the key; a side named changes the full hash and not the sim hash.
    const ResolveResult hidden_none =
        resolve(profile_text("minimal", "identity:\n  skirmish-hidden-sides: []\n"));
    const ResolveResult hidden_watch =
        resolve(profile_text("minimal", "identity:\n  skirmish-hidden-sides: [3]\n"));
    OA_CHECK(hidden_none.resolution && hidden_watch.resolution);
    if (hidden_none.resolution && hidden_watch.resolution && left_out.resolution) {
        const auto& base = left_out.resolution->profile;
        OA_CHECK(hidden_none.resolution->profile.full_hash == base.full_hash);
        OA_CHECK(hidden_watch.resolution->profile.sim_hash == base.sim_hash);
        OA_CHECK(hidden_watch.resolution->profile.full_hash != base.full_hash);
        const auto& hidden = hidden_watch.resolution->profile.identity.skirmish_hidden_sides;
        OA_CHECK(hidden.count == 1 && hidden.items[0] == 3);
    }
    // The battle room's hidden sides follow the same rule.
    const ResolveResult room_none =
        resolve(profile_text("minimal", "identity:\n  battle-room-hidden-sides: []\n"));
    const ResolveResult room_other =
        resolve(profile_text("minimal", "identity:\n  battle-room-hidden-sides: [3]\n"));
    OA_CHECK(room_none.resolution && room_other.resolution);
    if (room_none.resolution && room_other.resolution && left_out.resolution) {
        const auto& base = left_out.resolution->profile;
        OA_CHECK(room_none.resolution->profile.full_hash == base.full_hash);
        OA_CHECK(room_other.resolution->profile.sim_hash == base.sim_hash);
        OA_CHECK(room_other.resolution->profile.full_hash != base.full_hash);
        const auto& hidden = room_other.resolution->profile.identity.battle_room_hidden_sides;
        OA_CHECK(hidden.count == 1 && hidden.items[0] == 3);
    }
    OA_CHECK(refused_with(
        profile_text("hidden-side-past", "identity:\n  skirmish-hidden-sides: [5]\n"),
        "is outside [0, 4]"
    ));
    OA_CHECK(refused_with(
        profile_text("hidden-side-twice", "identity:\n  skirmish-hidden-sides: [2, 2]\n"),
        "repeated items"
    ));
    OA_CHECK(refused_with(
        profile_text("watcher-frame-past", "identity:\n  watcher-side-frame: 16\n"),
        "is outside [0, 15]"
    ));
    OA_CHECK(refused_with(
        profile_text("archives-slash", "layout:\n  installation-archives: [\"dir/addon.ccx\"]\n"),
        "does not match"
    ));
    OA_CHECK(refused_with(
        profile_text("archives-dot", "layout:\n  installation-archives: [\".\"]\n"),
        "does not match"
    ));
    OA_CHECK(refused_with(
        profile_text("archives-empty", "layout:\n  installation-archives: [\"\"]\n"),
        "does not match"
    ));
    OA_CHECK(refused_with(
        profile_text(
            "archives-repeat", "layout:\n  installation-archives: [addon.ccx, addon.ccx]\n"
        ),
        "repeated items"
    ));
    OA_CHECK(refused_with(
        profile_text("archives-case", "layout:\n  installation-archives: [addon.ccx, Addon.ccx]\n"),
        "names that differ only in case name one archive"
    ));

    // The index-map form mounts at the indices given; false leaves one empty.
    const ResolveResult indexed = resolve(profile_text(
        "indexed", "script-extensions:\n  fidelity: safe\n  get: {33: unit.my-id, 40: false}\n"
    ));
    OA_CHECK(indexed.resolution.has_value());
    if (indexed.resolution) {
        OA_CHECK(
            text_of(at(*indexed.resolution, {"script-extensions", "get"})) ==
            "{\"33\":\"unit.my-id\"}"
        );
        OA_CHECK(
            indexed.resolution->profile.rules.script_get.find(33) == ScriptExtension::unit_my_id
        );
        OA_CHECK(
            indexed.resolution->profile.rules.script_fidelity ==
            oa::data::match_rules::ScriptFidelity::safe
        );
    }
    // A string the simulation reads, kept in place.
    const ResolveResult named =
        resolve(profile_text("named", "hacks:\n  setup.ai-player-name-format: \"AI:%s %d\"\n"));
    OA_CHECK(
        named.resolution &&
        named.resolution->profile.rules.setup.ai_player_name_format.format.view() == "AI:%s %d"
    );
}

/// A profile names one side a name, up to five sides; three names give three.
void test_side_names() {
    const ResolveResult result =
        resolve(profile_text("sides", "identity:\n  side-names: [North, South, East]\n"));
    OA_CHECK(result.errors.empty());
    if (!result.resolution)
        return;
    const auto& names = result.resolution->profile.identity.side_names;
    OA_CHECK(names.size() == 3 && names[0] == "North" && names[2] == "East");
    OA_CHECK(
        text_of(at(*result.resolution, {"identity", "side-names"})) ==
        "[\"North\",\"South\",\"East\"]"
    );
}

void test_comments_and_true() {
    const std::string text = "# a comment line\n" + profile_text("c") +
                             "hacks:\n  units.id-reuse-delay: true   # trailing comment\n";
    const ResolveResult result = resolve(text);
    OA_CHECK(
        result.resolution &&
        text_of(at(*result.resolution, {"hacks", "units.id-reuse-delay"})) == "{\"ticks\":150}"
    );
}

void test_unimplemented() {
    const std::string hack{unimplemented_hack()};
    if (hack.empty())
        return;
    const std::string text = profile_text("dev", "hacks:\n  " + hack + ": true\n");
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    const ResolveResult refused = resolve_profile(bytes, "dev.oamod");
    OA_CHECK(!refused.resolution.has_value() && refused.errors.size() == 1);
    OA_CHECK(
        !refused.errors.empty() &&
        format_diagnostic(refused.errors[0]) ==
            "dev.oamod:7:3: hacks." + hack + ": this engine does not implement the hack yet"
    );
    ResolveOptions options{};
    options.accept_unimplemented_hacks = true;
    const ResolveResult accepted = resolve_profile(bytes, "dev.oamod", options);
    OA_CHECK(accepted.resolution.has_value() && accepted.warnings.size() == 1);
    // A hack written false is off, implemented or not.
    const std::string off = profile_text("off", "hacks:\n  " + hack + ": false\n");
    OA_CHECK(resolve_profile(
                 std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(off.data()), off.size()},
                 "off.oamod"
    )
                 .resolution.has_value());
}

void test_baseline_profile() {
    // Every limit and hack that can play as 3.1c while on, at its baseline
    // preset: each value resolves from the preset, the limits are 3.1c's and
    // the hacks are on.
    const std::string text = baseline_profile_text();
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    ResolveOptions options{};
    options.accept_unimplemented_hacks = true;
    const ResolveResult result = resolve_profile(bytes, "baseline.oamod", options);
    for (const Diagnostic& error : result.errors)
        std::cerr << format_diagnostic(error) << '\n';
    OA_CHECK(result.resolution.has_value());
    if (!result.resolution)
        return;
    const ModProfile& profile = result.resolution->profile;
    OA_CHECK(profile.id == "baseline-rules");
    OA_CHECK(profile.limits == oa::data::limits::Limits{});
    OA_CHECK(profile.rules.repair.rate.enabled);
    OA_CHECK(profile.rules.repair.rate.mode == oa::data::match_rules::RepairRateMode::clamp_max_1);
    OA_CHECK(profile.rules.units.id_reuse_delay.enabled);
    OA_CHECK(profile.rules.units.id_reuse_delay.ticks == 0);
    size_t from_preset = 0;
    for (const Provenance& value : result.resolution->provenance) {
        if (!value.path.starts_with("hacks.") && !value.path.starts_with("limits."))
            continue;
        const bool baseline =
            value.source == "baseline" || value.source.starts_with("preset baseline ");
        if (!baseline)
            std::cerr << value.path << " resolves from " << value.source << '\n';
        OA_CHECK(baseline);
        from_preset += value.source.starts_with("preset baseline ") ? 1 : 0;
    }
    OA_CHECK(from_preset > 0);
}

void test_refusals() {
    struct Case {
        std::string_view body;
        std::string_view needle;
    };

    const Case cases[] = {
        {"hacks: {nope.nope: true}", "unknown hack"},
        {"hacks: {units.id-reuse-delay: {tick: 5}}", "unknown parameter"},
        {"hacks: {units.id-reuse-delay: {ticks: -1}}", "outside"},
        {"hacks: {units.id-reuse-delay: {ticks: 1.5}}", "integer"},
        {"hacks: {repair.rate: {mode: fast}}", "not one of"},
        {"hacks: {repair.rate: {preset: unknown-preset}}", "unknown preset"},
        {"hacks: {ai.squad5-factory-tick: 3}", "expected true, false"},
        {"hacks: {orders.weapons-free-while-busy: {states: [repair, repair]}}", "repeated"},
        {"hacks: {veterancy.model: {default-thresholds: [5, 5]}}", "ascend"},
        {"hacks: {ai.income-multipliers: {production: [1, 2]}}", "3 items"},
        {"limits: {units-per-player: {default: 100, min: 200}}", "break"},
        {"limits: {unit-types: {bitset-bits: 1000}}", "multiple of"},
        {"script-extensions: {get: {32: unit.min-id, '32': unit.max-id}}", "twice"},
        {"script-extensions: {get: {5: unit.min-id}}", "3.1c range"},
        {"script-extensions: {get: {40: unit.min-id, 41: unit.min-id}}", "mounted at"},
        {"script-extensions: {set: {40: unit.min-id}}", "get extension"},
        {"script-extensions: {get: [unit.nothing]}", "unknown script extension"},
        {"script-extensions: {fidelity: loose}", "exact or safe"},
        {"script-extensions: {get: {'x': unit.min-id}}", "not a decimal integer"},
        {"data-keys: {unit: {Foo: weapons.not-to-air}}", "not a unit"},
        {"data-keys: {unit: {VeterancyThresholds: veterancy.thresholds}}", "which is off"},
        {"data-keys: {feature: {}}", "unit or weapon"},
        {"settings: {limits.build-list-entries.copy: {ini: A/B}}", "fixed"},
        {"settings: {limits.effects.queue: {ini: NoSection}}", "Section/Key"},
        {"settings: {limits.effects.queue: {file: x}}", "expected {ini"},
        {"settings: {limits.effects.nothing: {ini: A/B}}", "unknown parameter"},
        {"settings: {registry-seeds: {UnitLimit: true}}", "integers or strings"},
        {"identity: {network-version: [3, 1, 0]}", "2 items"},
        {"identity: {side-names: [Arm]}", "items"},
        {"identity: {side-names: [A, B, C, D, E, F]}", "items"},
        {"identity: {display-version: 3.1}", "expected a string"},
        {"layout: {directories: {maps: maps}}", "unknown key"},
        {"layout: {archive-patterns: {ufo: SWX}}", "does not match"},
        {"layout: {directories: {units: \"..\"}}", "does not match"},
        {"layout: {directories: {units: \"units/x\"}}", "does not match"},
        {"media: {movies: {intro: \"../x.zrb\"}}", "does not match"},
        {"media: {movies: {logo: \"C:x.zrb\"}}", "does not match"},
        {"media: {movies: {credits: \"a\\\\b.zrb\"}}", "does not match"},
        {"layout: [a]", "layout must be an object"},
        {"colour: red", "unknown top-level"},
        {"extends: [base]", "unknown top-level"},
        {"detect: {all: [{file: a.gp3}]}", "unknown top-level"},
        {"notes: text", "unknown top-level"},
        {"5: five", "unknown top-level"},
        {"limits: {weapon-types: 255}", "outside"},
        {"limits: {weapon-types: 65537}", "outside"},
        {"limits: {sides: 4}", "outside"},
        {"limits: {sides: 33}", "outside"},
        {"limits: {weapon-types: 1.5}", "integer"},
    };
    for (const Case& refusal : cases) {
        const bool refused =
            refused_with(profile_text("bad", std::string{refusal.body} + "\n"), refusal.needle);
        if (!refused)
            std::fprintf(
                stderr, "  case: %.*s\n", static_cast<int>(refusal.body.size()), refusal.body.data()
            );
        OA_CHECK(refused);
    }
    // The head.
    std::string unquoted = profile_text("vers");
    unquoted.replace(unquoted.find("\"1\""), 3, "1");
    OA_CHECK(refused_with(unquoted, "version must be a string"));
    std::string grammar = profile_text("grammar");
    grammar.replace(0, 8, "oamod: 2");
    OA_CHECK(refused_with(grammar, "oamod must be 1"));
    OA_CHECK(refused_with(profile_text("Not_Kebab"), "id must be kebab-case"));
    std::string base = profile_text("base");
    base.replace(base.find("ta-3.1c"), 7, "ta-3.0");
    OA_CHECK(refused_with(base, "does not match base"));
    // The reader's refusals name the rule and position.
    const ResolveResult anchored = resolve("oamod: 1\nid: &a x\n");
    OA_CHECK(
        !anchored.errors.empty() &&
        format_diagnostic(anchored.errors[0]) == "test.oamod:2:5: anchors (&) are not allowed"
    );
    // A validation error names the offending path and where it is written.
    const ResolveResult bounded =
        resolve(profile_text("pos", "hacks:\n  units.id-reuse-delay:\n    ticks: 40000\n"));
    OA_CHECK(
        !bounded.errors.empty() &&
        format_diagnostic(bounded.errors[0]) ==
            "test.oamod:10:12: hacks.units.id-reuse-delay.ticks: 40000 is outside [0, 30000]"
    );
    const ResolveResult listed =
        resolve(profile_text("pos", "hacks:\n  veterancy.model: {default-thresholds: [5, x]}\n"));
    OA_CHECK(
        !listed.errors.empty() &&
        format_diagnostic(listed.errors[0])
                .find("hacks.veterancy.model.default-thresholds[1]: expected an integer") !=
            std::string::npos
    );
}

/// Replaces a profile's author and packaging lines.
///
/// @param id the profile's id
/// @param author the author line, empty to leave it out
/// @param packaging the packaging line, empty to leave it out
/// @return the profile's text
std::string with_meta(std::string_view id, std::string_view author, std::string_view packaging) {
    std::string text = profile_text(id);
    const size_t author_line = text.find("author:");
    text.erase(author_line);
    text += author.empty() ? "" : std::string{author} + "\n";
    text += packaging.empty() ? "" : std::string{packaging} + "\n";
    return text;
}

void test_author_and_packaging() {
    // Both blocks are carried as written and fill the profile's records.
    const ResolveResult full = resolve(with_meta(
        "meta",
        "author: {name: A. Modder, email: a.modder@example.com}",
        "packaging: {revision: 3, date: \"2024-02-29\", packager: P. Packer}"
    ));
    OA_CHECK(full.resolution.has_value());
    if (full.resolution) {
        const ModProfile& profile = full.resolution->profile;
        OA_CHECK(profile.author.name == "A. Modder");
        OA_CHECK(profile.author.email == "a.modder@example.com");
        OA_CHECK(profile.packaging.revision == 3);
        OA_CHECK(profile.packaging.date == "2024-02-29");
        OA_CHECK(profile.packaging.packager == "P. Packer");
        OA_CHECK(text_of(at(*full.resolution, {"author", "email"})) == "\"a.modder@example.com\"");
        OA_CHECK(text_of(at(*full.resolution, {"packaging", "revision"})) == "3");
    }
    // The e-mail address may be left out; a plain date reads as text.
    const ResolveResult plain = resolve(profile_text("plain"));
    OA_CHECK(plain.resolution.has_value());
    if (plain.resolution && full.resolution) {
        OA_CHECK(plain.resolution->profile.author.name == "unknown");
        OA_CHECK(plain.resolution->profile.author.email.empty());
        OA_CHECK(plain.resolution->profile.packaging.date == "2026-10-04");
        // They are not rules: the sim hash ignores them and the full hash does not.
        OA_CHECK(plain.resolution->profile.sim_hash == full.resolution->profile.sim_hash);
        OA_CHECK(plain.resolution->profile.full_hash != full.resolution->profile.full_hash);
    }

    struct Case {
        std::string_view author;
        std::string_view packaging;
        std::string_view needle;
    };

    constexpr std::string_view author_ok = "author: {name: unknown}";
    constexpr std::string_view packaging_ok =
        "packaging: {revision: 1, date: 2026-10-04, packager: p}";
    const Case cases[] = {
        {"", packaging_ok, "author must be an object"},
        {"author: Somebody", packaging_ok, "author must be an object"},
        {"author: {email: a@example.com}", packaging_ok, "author.name must be a string"},
        {"author: {name: \"\"}", packaging_ok, "author.name must be a string of 1 to 128"},
        {"author: {name: 5}", packaging_ok, "author.name must be a string"},
        {"author: {name: a, mail: a@example.com}", packaging_ok, "unknown author key 'mail'"},
        {"author: {name: a, email: nobody}", packaging_ok, "author.email must be an e-mail"},
        {"author: {name: a, email: a@b}", packaging_ok, "author.email must be an e-mail"},
        {"author: {name: a, email: \"a b@example.com\"}", packaging_ok, "author.email must be"},
        {"author: {name: a, email: a@@example.com}", packaging_ok, "author.email must be"},
        {author_ok, "", "packaging must be an object"},
        {author_ok, "packaging: {date: 2026-10-04, packager: p}", "packaging.revision must be"},
        {author_ok, "packaging: {revision: 0, date: 2026-10-04, packager: p}", "from 1 to 65535"},
        {author_ok, "packaging: {revision: 1.5, date: 2026-10-04, packager: p}", "revision must"},
        {author_ok, "packaging: {revision: \"1\", date: 2026-10-04, packager: p}", "revision must"},
        {author_ok, "packaging: {revision: 1, packager: p}", "packaging.date must be an ISO 8601"},
        {author_ok, "packaging: {revision: 1, date: 2026-02-29, packager: p}", "packaging.date"},
        {author_ok, "packaging: {revision: 1, date: 2026-13-01, packager: p}", "packaging.date"},
        {author_ok, "packaging: {revision: 1, date: 2026-1-04, packager: p}", "packaging.date"},
        {author_ok, "packaging: {revision: 1, date: 20261004, packager: p}", "packaging.date"},
        {author_ok, "packaging: {revision: 1, date: 2026-10-04}", "packaging.packager must be"},
        {author_ok,
         "packaging: {revision: 1, date: 2026-10-04, packager: p, notes: x}",
         "unknown packaging key 'notes' (have [revision, date, packager])"},
    };
    for (const Case& refusal : cases) {
        const bool refused =
            refused_with(with_meta("bad", refusal.author, refusal.packaging), refusal.needle);
        if (!refused)
            std::fprintf(
                stderr,
                "  case: %.*s / %.*s\n",
                static_cast<int>(refusal.author.size()),
                refusal.author.data(),
                static_cast<int>(refusal.packaging.size()),
                refusal.packaging.data()
            );
        OA_CHECK(refused);
    }
    // A leap day of a leap year is a date; 1900 was not a leap year, 2000 was.
    OA_CHECK(
        resolve(
            with_meta("leap", author_ok, "packaging: {revision: 1, date: 2000-02-29, packager: p}")
        )
            .resolution.has_value()
    );
    OA_CHECK(refused_with(
        with_meta("century", author_ok, "packaging: {revision: 1, date: 1900-02-29, packager: p}"),
        "packaging.date"
    ));
}

void test_description() {
    // A description is kept as written, in the records and the effective
    // profile; it changes the full hash and never the sim hash.
    const ResolveResult with =
        resolve(profile_text("described", "description: A made-up mod for the tests.\n"));
    const ResolveResult without = resolve(profile_text("described"));
    OA_CHECK(with.resolution.has_value() && without.resolution.has_value());
    if (!with.resolution || !without.resolution)
        return;
    OA_CHECK(with.resolution->profile.description == "A made-up mod for the tests.");
    OA_CHECK(text_of(at(*with.resolution, {"description"})) == "\"A made-up mod for the tests.\"");
    OA_CHECK(without.resolution->profile.description.empty());
    OA_CHECK(text_of(at(*without.resolution, {"description"})) == "absent");
    OA_CHECK(with.resolution->profile.sim_hash == without.resolution->profile.sim_hash);
    OA_CHECK(with.resolution->profile.full_hash != without.resolution->profile.full_hash);

    // The limit counts characters, not bytes: 120 two-byte characters fit, 121 do not.
    std::string longest;
    for (size_t index = 0; index < max_description_characters; ++index)
        longest += "\xC3\xA9";
    const ResolveResult fits = resolve(profile_text("described", "description: " + longest + "\n"));
    OA_CHECK(fits.resolution.has_value() && fits.resolution->profile.description == longest);
    OA_CHECK(refused_with(
        profile_text("described", "description: " + longest + "x\n"),
        "description: description must be at most 120 characters (it has 121)"
    ));
    OA_CHECK(resolve(profile_text("described", "description: \"\"\n")).resolution.has_value());

    // One line of plain text: no line break, tab, other control character
    // or line separator, however it is escaped.
    for (const std::string_view escaped :
         {"one\\ntwo",
          "one\\rtwo",
          "one\\ttwo",
          "bell\\a",
          "next\\x85line",
          "line\\u2028separator",
          "para\\u2029graph",
          "delete\\x7f"})
        OA_CHECK(refused_with(
            profile_text("described", "description: \"" + std::string{escaped} + "\"\n"),
            "description: description must be one line, without line breaks or control "
            "characters"
        ));

    // Not a string.
    for (const std::string_view value : {"12", "true", "~", "[a, b]", "{a: b}"})
        OA_CHECK(refused_with(
            profile_text("described", "description: " + std::string{value} + "\n"),
            "description: description must be a string"
        ));
}

/// The hashes the reference resolver gives each reference profile: sim, then full.
constexpr std::pair<std::string_view, std::string_view> reference_hashes[] = {
    {"f7a558e891e845b270f26fc5faf59cf39b5911733d5f3e51778ffc435749449f",
     "d941f67a3b4ac21752fbba47dce35d4f6cd989f36a6dd6311ebf723a5c7e533e"},
    {"237a8fa4ba09d0951d7bfcd1b2a5aafb84bba9861758024eeab443b5ba6296d7",
     "7fbd7aba1e6ce27a9818f42762b5f917e24029bdef263c1aebebdd5f241a88a4"},
    {"7bd577f653cf4ca28223b9e9631346374446278b9bca92d4d06c87d494c2ec02",
     "f9a43dfae42cfab8ecbf405145b895d02f86c44ee80766dae90db10c897e9d33"},
    {"43f81e8688ecf1cf02a4fb33b149626ca332e50f3fe2501ba2e1cc6afad883c8",
     "b99f8aa74b18026f24d644abd959a0f3a9b00a0035d097644723b581a0134e02"},
};

/// Resolves every reference profile and checks it against its pinned hashes.
///
/// @return the program's exit status: 77 when no reference folder is named
int test_references() {
    const auto named = oa::platform::environment_value("OA_MOD_PROFILES_DIR");
    const std::string folder = named.value_or("");
    if (folder.empty() || !fs::is_directory(folder)) {
        std::printf("data-mod-profile-references: skipped; OA_MOD_PROFILES_DIR names no folder\n");
        return 77;
    }
    std::set<std::pair<std::string, std::string>> found;
    size_t profiles = 0;
    for (const auto& directory : fs::directory_iterator{folder}) {
        if (!directory.is_directory())
            continue;
        for (const auto& file : fs::directory_iterator{directory.path()}) {
            std::string name = file.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (name != "oamod.yaml")
                continue;
            std::ifstream in{file.path(), std::ios::binary};
            const std::string text{std::istreambuf_iterator<char>{in}, {}};
            const ResolveResult result = resolve(text);
            ++profiles;
            for (const Diagnostic& diagnostic : result.errors)
                std::fprintf(
                    stderr,
                    "%s: %s\n",
                    file.path().string().c_str(),
                    format_diagnostic(diagnostic).c_str()
                );
            OA_CHECK(result.resolution.has_value());
            if (!result.resolution)
                continue;
            const auto pair = std::pair{
                digest_text(result.resolution->profile.sim_hash),
                digest_text(result.resolution->profile.full_hash)
            };
            std::printf(
                "%s\n  sha256-sim  %s\n  sha256-full %s\n",
                file.path().string().c_str(),
                pair.first.c_str(),
                pair.second.c_str()
            );
            found.insert(pair);
            // The profile with the most rules: values a reader of the profile can check by eye.
            if (pair.first == reference_hashes[0].first) {
                const Resolution& r = *result.resolution;
                OA_CHECK(
                    text_of(at(r, {"hacks", "veterancy.model", "damage-dealt-cap"})) == "\"none\""
                );
                OA_CHECK(!r.profile.rules.veterancy.model.damage_dealt_cap.has_value());
                OA_CHECK(
                    text_of(at(r, {"hacks", "orders.weapons-free-while-busy", "states"})) ==
                    "[\"nanolathe\"]"
                );
                OA_CHECK(r.profile.limits.units_per_player.limits_screen_fallback == 1000);
                OA_CHECK(
                    text_of(at(r, {"script-extensions", "get", "32"})) == "\"unit.kills-x100\""
                );
                OA_CHECK(
                    source_of(r, "hacks.veterancy.model.default-thresholds")
                        .find("per-unit key VeterancyThresholds") != std::string::npos
                );
            }
        }
    }
    OA_CHECK(profiles == std::size(reference_hashes));
    for (const auto& [sim, full] : reference_hashes)
        OA_CHECK(found.contains({std::string{sim}, std::string{full}}));
    return oa::test::check_exit_status();
}

} // namespace

/// Makes a list of numbers.
///
/// @param numbers the numbers' texts, as a profile writes them
/// @return the list
Value number_list(std::initializer_list<std::string_view> numbers) {
    Value list = make_list();
    for (const std::string_view text : numbers) {
        oa::formats::oamod::Number number{};
        OA_CHECK(
            oa::formats::oamod::parse_number(text, number) == oa::formats::oamod::NumberStatus::ok
        );
        list.items.push_back(make_number(number));
    }
    return list;
}

/// Counts the warnings that say an override was left out, with a text.
///
/// @param result the result
/// @param needle text the warning must contain
/// @return how many do
size_t left_out_with(const ResolveResult& result, std::string_view needle) {
    size_t count = 0;
    for (const Diagnostic& warning : result.warnings) {
        const std::string line = format_diagnostic(warning);
        if (line.find("the override is left out") != std::string::npos &&
            line.find(needle) != std::string::npos)
            ++count;
    }
    return count;
}

void test_overrides() {
    // A profile with two hacks on, and every way an override changes one.
    const std::string text =
        profile_text("overridden", "hacks:\n  units.id-reuse-delay: 90\n  repair.rate: true\n");
    const ResolveResult plain = resolve(text);
    OA_CHECK(plain.resolution.has_value());
    if (!plain.resolution)
        return;
    ResolveOptions options{};
    // On over the profile's own values: the parameter it names, the rest kept.
    options.overrides.push_back(
        HackOverride{"units.id-reuse-delay", true, {{"ticks", make_integer(120)}}}
    );
    // Off, as false in the profile.
    options.overrides.push_back(HackOverride{"repair.rate", false, {}});
    // On where the profile has it off: its defaults, the parameter it names.
    options.overrides.push_back(
        HackOverride{"ai.attack-wave-size", true, {{"units", make_integer(40)}}}
    );
    const ResolveResult laid = resolve(text, options);
    OA_CHECK(laid.errors.empty() && laid.warnings.empty());
    if (!laid.resolution)
        return;
    const Resolution& r = *laid.resolution;
    OA_CHECK(text_of(at(r, {"hacks", "units.id-reuse-delay", "ticks"})) == "120");
    OA_CHECK(at(r, {"hacks", "repair.rate"}) == nullptr);
    OA_CHECK(text_of(at(r, {"hacks", "ai.attack-wave-size", "units"})) == "40");
    OA_CHECK(source_of(r, "hacks.units.id-reuse-delay.ticks") == "override");
    OA_CHECK(source_of(r, "hacks.ai.attack-wave-size.units") == "override");
    OA_CHECK(r.profile.rules.units.id_reuse_delay.enabled);
    OA_CHECK(r.profile.rules.units.id_reuse_delay.ticks == 120);
    OA_CHECK(!r.profile.rules.repair.rate.enabled);
    OA_CHECK(r.profile.rules.ai.attack_wave_size.enabled);
    OA_CHECK(r.profile.rules.ai.attack_wave_size.units == 40);
    OA_CHECK(r.profile.sim_hash != plain.resolution->profile.sim_hash);
    OA_CHECK(r.profile.full_hash != plain.resolution->profile.full_hash);
    // The same values written in the profile give the same hashes.
    const ResolveResult written = resolve(profile_text(
        "overridden", "hacks:\n  units.id-reuse-delay: 120\n  ai.attack-wave-size: 40\n"
    ));
    OA_CHECK(written.resolution.has_value());
    if (written.resolution) {
        OA_CHECK(written.resolution->profile.sim_hash == r.profile.sim_hash);
        OA_CHECK(written.resolution->profile.full_hash == r.profile.full_hash);
        OA_CHECK(written.resolution->canonical == r.canonical);
    }

    // A display (view-scope) hack changes the full hash and keeps the sim hash.
    ResolveOptions view{};
    view.overrides.push_back(HackOverride{"ui.whiteboard", true, {}});
    const ResolveResult shown = resolve(text, view);
    OA_CHECK(shown.resolution.has_value());
    if (shown.resolution) {
        OA_CHECK(shown.resolution->profile.ui.whiteboard.enabled);
        OA_CHECK(shown.resolution->profile.sim_hash == plain.resolution->profile.sim_hash);
        OA_CHECK(shown.resolution->profile.full_hash != plain.resolution->profile.full_hash);
    }

    // Each override that does not fit is left out whole, with a warning,
    // and the others apply.
    ResolveOptions bad{};
    bad.overrides = {
        HackOverride{"units.no-such-hack", true, {}},
        HackOverride{"units.id-reuse-delay", true, {{"ticks", make_integer(-1)}}},
        HackOverride{"units.id-reuse-delay", true, {{"nothing", make_integer(1)}}},
        HackOverride{
            "console.game-speed-range", true, {{"min", make_integer(15)}, {"max", make_integer(5)}}
        },
        HackOverride{"ai.attack-wave-size", true, {{"units", make_string("many")}}},
        HackOverride{"ai.patrol-group-size", true, {{"units", make_integer(9)}}},
    };
    const ResolveResult refused = resolve(text, bad);
    OA_CHECK(refused.errors.empty() && refused.resolution.has_value());
    OA_CHECK(left_out_with(refused, "hacks.units.no-such-hack: unknown hack") == 1);
    OA_CHECK(left_out_with(refused, "is outside [0, 30000]") == 1);
    OA_CHECK(left_out_with(refused, "hacks.units.id-reuse-delay.nothing: unknown parameter") == 1);
    OA_CHECK(left_out_with(refused, "break min <= max") == 1);
    OA_CHECK(left_out_with(refused, "expected an integer") == 1);
    OA_CHECK(refused.warnings.size() == 5);
    if (refused.resolution) {
        const ModProfile& profile = refused.resolution->profile;
        OA_CHECK(profile.rules.units.id_reuse_delay.ticks == 90);
        OA_CHECK(!profile.rules.ai.attack_wave_size.enabled);
        OA_CHECK(!profile.rules.console.game_speed_range.enabled);
        OA_CHECK(profile.rules.ai.patrol_group_size.enabled);
        OA_CHECK(profile.rules.ai.patrol_group_size.units == 9);
    }

    // A hack a data key the profile maps needs stays on.
    const std::string keyed = profile_text(
        "keyed",
        "data-keys:\n  unit:\n    Thresholds: veterancy.thresholds\n"
        "hacks:\n  veterancy.model: true\n"
    );
    ResolveOptions off{};
    off.overrides.push_back(HackOverride{"veterancy.model", false, {}});
    const ResolveResult kept = resolve(keyed, off);
    OA_CHECK(kept.errors.empty() && kept.resolution.has_value());
    OA_CHECK(left_out_with(kept, "data-keys.unit.Thresholds needs the hack on") == 1);
    if (kept.resolution)
        OA_CHECK(at(*kept.resolution, {"hacks", "veterancy.model"}) != nullptr);

    // A hack this engine does not implement is turned on only while such
    // hacks are accepted.
    if (const std::string_view hack = unimplemented_hack(); !hack.empty()) {
        ResolveOptions strict{};
        strict.overrides.push_back(HackOverride{std::string{hack}, true, {}});
        const ResolveResult result = resolve_profile(
            std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
            "test.oamod",
            strict
        );
        OA_CHECK(left_out_with(result, "does not implement") == 1);
    }

    // The plain 3.1c baseline: 3.1c's records and the sim hash of a profile
    // that changes nothing; an override turns a hack on over it.
    const std::string base = base_game_profile_text();
    const ResolveResult baseline = resolve(base);
    OA_CHECK(baseline.resolution.has_value() && baseline.warnings.empty());
    if (baseline.resolution) {
        OA_CHECK(baseline.resolution->profile.id == "base-game");
        OA_CHECK(baseline.resolution->profile.rules == oa::data::match_rules::MatchRules{});
        OA_CHECK(
            digest_text(baseline.resolution->profile.sim_hash) ==
            "1502111e3b1f69bd93be99405e3c61e0c6337698a7c8502f9f6320e80600094d"
        );
    }
    ResolveOptions over_base{};
    over_base.overrides.push_back(
        HackOverride{
            "ai.income-multipliers", true, {{"production", number_list({"1", "2", "3.5"})}}
        }
    );
    const ResolveResult on_base = resolve(base, over_base);
    OA_CHECK(on_base.resolution.has_value() && on_base.warnings.empty());
    if (on_base.resolution) {
        OA_CHECK(
            text_of(at(*on_base.resolution, {"hacks", "ai.income-multipliers", "production"})) ==
            "[1,2,3.5]"
        );
        OA_CHECK(
            text_of(at(*on_base.resolution, {"hacks", "ai.income-multipliers", "reclaim"})) ==
            "[0.5,1,4]"
        );
    }
}

void test_override_text_and_states() {
    // The text the settings keep: one object, its members sorted, read back.
    const std::vector<HackOverride> overrides{
        HackOverride{"ui.whiteboard", true, {}},
        HackOverride{"repair.rate", false, {}},
        HackOverride{
            "ai.income-multipliers", true, {{"production", number_list({"0.5", "1", "2"})}}
        },
    };
    const std::string text = overrides_text(overrides);
    OA_CHECK(
        text == "{\"ai.income-multipliers\":{\"production\":[0.5,1,2]},\"repair.rate\":false,"
                "\"ui.whiteboard\":true}"
    );
    const std::vector<HackOverride> read = read_overrides(text);
    OA_CHECK(read.size() == overrides.size());
    for (const HackOverride& override : overrides) {
        const HackOverride* found = find_override(read, override.hack);
        OA_CHECK(found != nullptr && *found == override);
    }
    OA_CHECK(overrides_text({}) == "{}");
    OA_CHECK(read_overrides("{}").empty());
    OA_CHECK(read_overrides("not a mapping").empty());
    OA_CHECK(read_overrides("[1, 2]").empty());
    OA_CHECK(read_overrides("{\"a\": true, \"a\": false}").empty());
    const std::vector<HackOverride> partly = read_overrides("{\"a\": 3, \"b\": true}");
    OA_CHECK(partly.size() == 1 && partly[0].hack == "b" && partly[0].on);

    // Every hack of the registry, in its order; 3.1c's states are all off
    // at their baselines.
    const auto hacks = standard_hacks();
    size_t registry_hacks = 0;
    for (const registry::Entry& entry : registry::table().entries)
        if (entry.kind == registry::EntryKind::hack)
            ++registry_hacks;
    OA_CHECK(hacks.size() == registry_hacks);
    for (size_t index = 0; index < hacks.size(); ++index) {
        OA_CHECK(hacks[index]->kind == registry::EntryKind::hack);
        OA_CHECK(standard_hack_index(hacks[index]->id) == index);
        OA_CHECK(hacks[index]->id.starts_with(std::string{hacks[index]->area} + "."));
    }
    OA_CHECK(!standard_hack_index("units.no-such-hack").has_value());
    const std::vector<HackState> base = base_hack_states();
    OA_CHECK(base.size() == hacks.size());
    for (size_t index = 0; index < base.size(); ++index) {
        OA_CHECK(!base[index].on);
        const auto parameters = registry::parameters_of(*hacks[index]);
        OA_CHECK(base[index].values.size() == parameters.size());
        for (size_t at = 0; at < parameters.size() && at < base[index].values.size(); ++at)
            OA_CHECK(values_equal(
                base[index].values[at], registry::literal_value(parameters[at].value.baseline)
            ));
    }

    // A profile's states, and an override laid over one as the resolver lays it.
    const ResolveResult result =
        resolve(profile_text("states", "hacks:\n  units.id-reuse-delay: 90\n"));
    OA_CHECK(result.resolution.has_value());
    if (!result.resolution)
        return;
    const std::vector<HackState> states = hack_states(result.resolution->effective);
    const auto delay = standard_hack_index("units.id-reuse-delay");
    const auto wave = standard_hack_index("ai.attack-wave-size");
    OA_CHECK(delay.has_value() && wave.has_value());
    if (!delay || !wave)
        return;
    OA_CHECK(states[*delay].on && text_of(&states[*delay].values[0]) == "90");
    OA_CHECK(!states[*wave].on && text_of(&states[*wave].values[0]) == "6");
    const registry::Entry& wave_hack = *hacks[*wave];
    OA_CHECK(text_of(&on_values(wave_hack, states[*wave])[0]) == "10");
    const HackOverride turned_on{"ai.attack-wave-size", true, {}};
    const HackState on = overridden_state(wave_hack, states[*wave], &turned_on);
    OA_CHECK(on.on && text_of(&on.values[0]) == "10");
    const HackOverride set{"ai.attack-wave-size", true, {{"units", make_integer(33)}}};
    OA_CHECK(text_of(&overridden_state(wave_hack, states[*wave], &set).values[0]) == "33");
    const HackOverride turned_off{"units.id-reuse-delay", false, {}};
    const HackState off = overridden_state(*hacks[*delay], states[*delay], &turned_off);
    OA_CHECK(!off.on && text_of(&off.values[0]) == "0");
    OA_CHECK(overridden_state(*hacks[*delay], states[*delay], nullptr).on);
}

/// Replaces the head's requires line, so a profile does not name requires twice.
///
/// @param id the profile's id
/// @param requires_line the whole requires line, including its newline
/// @return the profile
std::string with_requires(std::string_view id, std::string_view requires_line) {
    std::string text = profile_text(id);
    constexpr std::string_view line = "requires: {base: ta-3.1c, catalogue: 1}\n";
    const size_t at = text.find(line);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos)
        text.replace(at, line.size(), requires_line);
    return text;
}

/// homepage, tags and requires.engine are kept, change the full hash only,
/// and an unmet or ill-formed one is refused.
void test_package_keys() {
    const std::string body = "homepage: \"https://example.org/mod\"\n"
                             "tags: [balance, ai]\n";
    std::string held_text = profile_text("keys", body);
    constexpr std::string_view line = "requires: {base: ta-3.1c, catalogue: 1}\n";
    const size_t at = held_text.find(line);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos)
        held_text.replace(
            at, line.size(), "requires: {base: ta-3.1c, catalogue: 1, engine: \">= 0.0.1\"}\n"
        );
    const ResolveResult held = resolve(held_text);
    const ResolveResult plain = resolve(profile_text("keys"));
    OA_CHECK(held.resolution.has_value() && plain.resolution.has_value());
    if (held.resolution && plain.resolution) {
        const ModProfile& profile = held.resolution->profile;
        OA_CHECK(profile.homepage == "https://example.org/mod");
        OA_CHECK(profile.tags == std::vector<std::string>({"balance", "ai"}));
        OA_CHECK(profile.requires_engine == ">= 0.0.1");
        OA_CHECK(profile.sim_hash == plain.resolution->profile.sim_hash);
        OA_CHECK(profile.full_hash != plain.resolution->profile.full_hash);
    }

    OA_CHECK(refused_with(profile_text("home", "homepage: \"javascript:no\"\n"), "homepage:"));
    OA_CHECK(refused_with(profile_text("tag", "tags: [Not-Kebab]\n"), "tags[0]:"));
    OA_CHECK(
        refused_with(profile_text("many", "tags: [a, b, c, d, e, f, g, h, i]\n"), "tags lists 9")
    );
    OA_CHECK(refused_with(profile_text("dup", "tags: [balance, balance]\n"), "repeats"));
    OA_CHECK(refused_with(profile_text("empty-tags", "tags: []\n"), "leave the key out"));
    OA_CHECK(refused_with(
        with_requires("syntax", "requires: {base: ta-3.1c, catalogue: 1, engine: \">= 1.2\"}\n"),
        "requires.engine:"
    ));
    OA_CHECK(refused_with(
        with_requires("foo", "requires: {foo: 1}\n"), "requires takes base, catalogue and engine"
    ));
    const std::string unmet = with_requires("unmet", "requires: {engine: \">= 9999.0.0\"}\n");
    OA_CHECK(refused_with(unmet, "9999.0.0 or later"));
    OA_CHECK(refused_with(unmet, std::string{"this is Open Annihilation "} + OA_ENGINE_VERSION));
    const ResolveResult met = resolve(with_requires("met", "requires: {engine: \">= 0.0.1\"}\n"));
    OA_CHECK(met.resolution.has_value());
    OA_CHECK(met.resolution && met.resolution->profile.requires_engine == ">= 0.0.1");
}

/// supportedGames and requiredExpansions are read, and they change the full hash only.
void test_content_needs() {
    const ResolveResult omitted = resolve(with_requires("omit", ""));
    OA_CHECK(omitted.resolution.has_value() && omitted.warnings.empty());
    if (omitted.resolution) {
        OA_CHECK(omitted.resolution->profile.supported_games == std::vector<std::string>{"totala"});
        OA_CHECK(omitted.resolution->profile.required_expansions.empty());
        OA_CHECK(omitted.resolution->canonical.find("supportedGames") == std::string::npos);
        OA_CHECK(omitted.resolution->canonical.find("requiredExpansions") == std::string::npos);
    }

    const ResolveResult legacy = resolve(profile_text("legacy"));
    OA_CHECK(legacy.resolution.has_value() && legacy.warnings.empty());
    if (legacy.resolution) {
        OA_CHECK(legacy.resolution->profile.supported_games == std::vector<std::string>{"totala"});
        OA_CHECK(legacy.resolution->profile.required_expansions.empty());
    }

    const ResolveResult named = resolve(profile_text(
        "named",
        "supportedGames: [\"totala-demo\", \"totala\"]\n"
        "requiredExpansions: [\"music\"]\n"
    ));
    const ResolveResult plain = resolve(profile_text("named"));
    OA_CHECK(named.resolution.has_value() && named.warnings.empty());
    if (named.resolution && plain.resolution) {
        OA_CHECK(
            named.resolution->profile.supported_games ==
            (std::vector<std::string>{"totala-demo", "totala"})
        );
        OA_CHECK(
            named.resolution->profile.required_expansions == std::vector<std::string>{"music"}
        );
        OA_CHECK(named.resolution->profile.sim_hash == plain.resolution->profile.sim_hash);
        OA_CHECK(named.resolution->profile.full_hash != plain.resolution->profile.full_hash);
        OA_CHECK(named.resolution->canonical.find("\"supportedGames\"") != std::string::npos);
        OA_CHECK(named.resolution->canonical.find("\"requiredExpansions\"") != std::string::npos);
    }

    const ResolveResult none = resolve(profile_text("none", "requiredExpansions: []\n"));
    OA_CHECK(none.resolution.has_value() && none.warnings.empty());
    if (none.resolution)
        OA_CHECK(none.resolution->profile.required_expansions.empty());

    OA_CHECK(refused_with(profile_text("unknown", "supportedGames: [\"no-such\"]\n"), "'no-such'"));
    OA_CHECK(refused_with(
        profile_text("unknown", "supportedGames: [\"no-such\"]\n"), "is not a base game"
    ));
    OA_CHECK(refused_with(
        profile_text("kind", "supportedGames: [\"core-contingency\"]\n"), "is not a base game"
    ));
    OA_CHECK(refused_with(
        profile_text("expansion", "requiredExpansions: [\"totala\"]\n"), "is not an expansion"
    ));
    OA_CHECK(refused_with(
        profile_text(
            "extends",
            "supportedGames: [\"totala-demo\"]\n"
            "requiredExpansions: [\"core-contingency\"]\n"
        ),
        "extends none of the supported games"
    ));
    OA_CHECK(refused_with(
        profile_text("empty", "supportedGames: []\n"),
        "supportedGames must list at least one base game"
    ));
    OA_CHECK(refused_with(
        profile_text("repeat", "supportedGames: [\"totala\", \"totala\"]\n"), "'totala' is repeated"
    ));
    OA_CHECK(
        refused_with(profile_text("scalar", "supportedGames: totala\n"), "must be a list of ids")
    );
    OA_CHECK(refused_with(profile_text("number", "supportedGames: [1]\n"), "must be an id"));
    std::string many = "supportedGames: [";
    for (int index = 0; index < 17; ++index) {
        if (index != 0)
            many += ", ";
        many += "\"g" + std::to_string(index) + "\"";
    }
    many += "]\n";
    OA_CHECK(refused_with(profile_text("many-games", many), "at most 16"));
}

void test_weapon_and_side_limits() {
    const ResolveResult minimal = resolve(profile_text("minimal"));
    OA_CHECK(minimal.resolution.has_value());
    if (!minimal.resolution)
        return;
    const Resolution& base = *minimal.resolution;
    OA_CHECK(base.profile.limits.weapon_types.types == 256);
    OA_CHECK(base.profile.limits.sides.count == 5);
    OA_CHECK(text_of(at(base, {"limits", "weapon-types"})) == "{\"types\":256}");
    OA_CHECK(text_of(at(base, {"limits", "sides"})) == "{\"count\":5}");
    OA_CHECK(base.canonical.find("\"weapon-types\"") == std::string::npos);
    OA_CHECK(base.canonical.find("\"sides\"") == std::string::npos);

    const ResolveResult turned_on =
        resolve(profile_text("on", "limits: {weapon-types: true, sides: true}\n"));
    OA_CHECK(turned_on.resolution.has_value());
    if (turned_on.resolution) {
        OA_CHECK(turned_on.resolution->profile.limits.weapon_types.types == 4096);
        OA_CHECK(turned_on.resolution->profile.limits.sides.count == 10);
    }

    const ResolveResult shorthand =
        resolve(profile_text("shorthand", "limits: {weapon-types: 1024, sides: 7}\n"));
    OA_CHECK(shorthand.resolution.has_value());
    if (shorthand.resolution) {
        OA_CHECK(shorthand.resolution->profile.limits.weapon_types.types == 1024);
        OA_CHECK(shorthand.resolution->profile.limits.sides.count == 7);
        OA_CHECK(
            shorthand.resolution->canonical.find("\"weapon-types\":{\"types\":1024}") !=
            std::string::npos
        );
        OA_CHECK(
            shorthand.resolution->canonical.find("\"sides\":{\"count\":7}") != std::string::npos
        );
    }

    const char* written_at_baseline[] = {
        "limits: {weapon-types: false, sides: false}\n",
        "limits: {weapon-types: 256, sides: 5}\n",
        "limits: {weapon-types: {preset: baseline}, sides: {preset: baseline}}\n",
    };
    for (const char* body : written_at_baseline) {
        const ResolveResult written = resolve(profile_text("minimal", body));
        OA_CHECK(written.resolution.has_value());
        if (!written.resolution)
            continue;
        OA_CHECK(written.resolution->profile.sim_hash == base.profile.sim_hash);
        OA_CHECK(written.resolution->profile.full_hash == base.profile.full_hash);
    }

    OA_CHECK(turned_on.resolution && shorthand.resolution);
    if (turned_on.resolution && shorthand.resolution) {
        OA_CHECK(turned_on.resolution->profile.sim_hash != base.profile.sim_hash);
        OA_CHECK(turned_on.resolution->profile.full_hash != base.profile.full_hash);
        OA_CHECK(shorthand.resolution->profile.sim_hash != base.profile.sim_hash);
        OA_CHECK(shorthand.resolution->profile.full_hash != base.profile.full_hash);
        OA_CHECK(turned_on.resolution->profile.sim_hash != shorthand.resolution->profile.sim_hash);
        OA_CHECK(
            turned_on.resolution->profile.full_hash != shorthand.resolution->profile.full_hash
        );
    }
}

int main(int argc, char** argv) {
    if (argc > 1 && std::string_view{argv[1]} == "--references")
        return test_references();
    test_canonical_form();
    test_baseline_profile();
    test_single_profile();
    test_mutator_match();
    test_base_profile();
    test_blocks();
    test_side_names();
    test_comments_and_true();
    test_unimplemented();
    test_refusals();
    test_weapon_and_side_limits();
    test_author_and_packaging();
    test_overrides();
    test_override_text_and_states();
    test_description();
    test_package_keys();
    test_content_needs();
    return oa::test::check_exit_status();
}
