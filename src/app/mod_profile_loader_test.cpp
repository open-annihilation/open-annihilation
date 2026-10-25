// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mod profile's files: a folder's oamod.yaml found whatever the case of
// its name, two that differ only in case refused, --print-profile's output
// and exit status, a --mod profile's limits block filling the Limits record,
// a --mod profile the engine cannot use stopping the run with every error, and
// the player's settings in the mod's INI file reaching the parameters the
// profile binds; the file each of the game's movies plays from; and a folder
// the player picks checked as a mod folder.

#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <array>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>

namespace {

namespace fs = std::filesystem;
using namespace oa::app;

/// A profile that changes nothing.
constexpr std::string_view base_profile = "oamod: 1\n"
                                          "id: example\n"
                                          "name: Example mod\n"
                                          "version: \"1.0\"\n"
                                          "requires: {base: ta-3.1c, catalogue: 1}\n"
                                          "author:\n"
                                          "  name: unknown\n"
                                          "packaging:\n"
                                          "  revision: 1\n"
                                          "  date: 2026-10-04\n"
                                          "  packager: Open Annihilation\n";

/// Writes a file.
///
/// @param path the file
/// @param text its contents
void write(const fs::path& path, std::string_view text) {
    std::ofstream out{path, std::ios::binary};
    out << text;
}

void test_find(const fs::path& scratch) {
    const fs::path game = scratch / "game";
    fs::create_directories(game);
    std::string error;
    OA_CHECK(!find_mod_profile(game, error) && error.empty());
    write(game / "OAMOD.YAML", base_profile);
    const auto found = find_mod_profile(game, error);
    OA_CHECK(found && found->filename() == "OAMOD.YAML" && error.empty());

    // A file system that tells case apart may hold both spellings.
    write(game / "oamod.yaml", base_profile);
    std::string both;
    const bool case_sensitive = fs::exists(game / "OAMOD.YAML") &&
                                fs::exists(game / "oamod.yaml") &&
                                !fs::equivalent(game / "OAMOD.YAML", game / "oamod.yaml");
    if (case_sensitive) {
        OA_CHECK(!find_mod_profile(game, both));
        OA_CHECK(both.find("differ only in case") != std::string::npos);
    }
    OA_CHECK(!find_mod_profile(scratch / "missing", error) && !error.empty());
}

void test_print(const fs::path& scratch) {
    const fs::path game = scratch / "plain";
    fs::create_directories(game);
    std::ostringstream out;
    std::ostringstream err;
    OA_CHECK(print_mod_profile({}, game, false, out, err) == 0);
    OA_CHECK(out.str().find("no oamod.yaml; the folder plays base 3.1c") != std::string::npos);

    const fs::path file = scratch / "example.oamod";
    write(file, base_profile);
    std::ostringstream printed;
    std::ostringstream warnings;
    OA_CHECK(print_mod_profile(file, {}, false, printed, warnings) == 0);
    OA_CHECK(printed.str().find("\"id\": \"example\"") != std::string::npos);
    OA_CHECK(printed.str().find("sha256-sim  ") != std::string::npos);
    OA_CHECK(printed.str().find("sha256-full ") != std::string::npos);
    OA_CHECK(warnings.str().empty());

    // A hack this build does not implement yet refuses the profile, unless
    // a development run accepts it.
    std::string hack;
    for (const auto& entry : oa::data::mod_profile::registry::table().entries)
        if (hack.empty() && entry.kind == oa::data::mod_profile::registry::EntryKind::hack &&
            !entry.implemented && !entry.visual)
            hack = entry.id;
    if (!hack.empty()) {
        const fs::path hacked = scratch / "hacked.oamod";
        write(hacked, std::string{base_profile} + "hacks:\n  " + hack + ": true\n");
        std::ostringstream refused_out;
        std::ostringstream refused_err;
        OA_CHECK(print_mod_profile(hacked, {}, false, refused_out, refused_err) == 1);
        OA_CHECK(refused_err.str().find("error: ") != std::string::npos);
        OA_CHECK(
            refused_err.str().find(
                ":7:3: hacks." + hack + ": this engine does not implement the hack yet"
            ) != std::string::npos
        );
        OA_CHECK(refused_out.str().empty());
        std::ostringstream accepted_out;
        std::ostringstream accepted_err;
        OA_CHECK(print_mod_profile(hacked, {}, true, accepted_out, accepted_err) == 0);
        OA_CHECK(accepted_err.str().find("warning: ") != std::string::npos);
        OA_CHECK(accepted_out.str().find("\"" + hack + "\"") != std::string::npos);
    }

    std::ostringstream missing_out;
    std::ostringstream missing_err;
    OA_CHECK(print_mod_profile(scratch / "none.oamod", {}, false, missing_out, missing_err) == 1);
    OA_CHECK(missing_err.str().find("cannot be read") != std::string::npos);
}

void test_check(const fs::path& scratch) {
    const fs::path file = scratch / "check.oamod";
    write(file, base_profile);
    std::ostringstream out;
    const auto profile = check_mod_profile(file, false, out);
    OA_CHECK(profile.id == "example");
    OA_CHECK(out.str().find("mod profile example 1.0 resolved") != std::string::npos);
    // Without a limits block the game keeps 3.1c's capacities.
    OA_CHECK(profile.limits == oa::data::limits::Limits{});

    // A limits block fills the one Limits record the game sizes its tables from.
    const fs::path raised = scratch / "raised.oamod";
    write(
        raised,
        std::string{base_profile} + "limits:\n"
                                    "  units-per-player: {default: 1000, max: 1500}\n"
                                    "  path-search-budget: true\n"
                                    "  build-list-entries: {copy: 36, overflow: dynamic}\n"
    );
    std::ostringstream raised_out;
    const auto raised_profile = check_mod_profile(raised, false, raised_out);
    OA_CHECK(raised_profile.limits.units_per_player.default_limit == 1000);
    OA_CHECK(raised_profile.limits.units_per_player.minimum == 20);
    OA_CHECK(raised_profile.limits.units_per_player.maximum == 1500);
    OA_CHECK(raised_profile.limits.path_search.nodes == 66650);
    OA_CHECK(raised_profile.limits.build_lists.copy == 36);
    OA_CHECK(
        raised_profile.limits.build_lists.overflow == oa::data::limits::BuildListOverflow::dynamic
    );
    OA_CHECK(raised_profile.limits.effects == oa::data::limits::Effects{});
    OA_CHECK(
        oa::data::limits::check_limits(raised_profile.limits) == oa::data::limits::LimitsError::none
    );

    const fs::path broken = scratch / "broken.oamod";
    write(broken, std::string{base_profile} + "colour: red\nhacks: {nope.nope: true}\n");
    std::string message;
    try {
        std::ostringstream ignored;
        std::ignore = check_mod_profile(broken, false, ignored);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    OA_CHECK(message.find("the mod profile cannot be used") != std::string::npos);
    OA_CHECK(message.find("unknown top-level key 'colour'") != std::string::npos);
    OA_CHECK(message.find("hacks.nope.nope: unknown hack") != std::string::npos);
}

void test_settings(const fs::path& scratch) {
    const auto ini = read_ini_settings(
        "Loose = 1\n; a comment\n[Preferences] ; custom settings\n"
        "UnitLimit = 1000;\r\n"
        "SfxLimit=20480 ; experimental\n"
        "  MultiGameWeapon = FALSE;\n"
        "no value here\n"
        "[Other]\nKey = text with spaces\n"
    );
    OA_CHECK(ini.size() == 4);
    OA_CHECK(ini[0].name == "Preferences/UnitLimit" && ini[0].value == "1000");
    OA_CHECK(ini[1].name == "Preferences/SfxLimit" && ini[1].value == "20480");
    OA_CHECK(ini[2].name == "Preferences/MultiGameWeapon" && ini[2].value == "FALSE");
    OA_CHECK(ini[3].name == "Other/Key" && ini[3].value == "text with spaces");

    // A profile that binds settings, keeps its registry under a root of its
    // own and seeds two registry values.
    const fs::path game = scratch / "settings";
    fs::create_directories(game);
    write(
        game / "oamod.yaml",
        std::string{base_profile} +
            "identity: {settings-file: Mod.ini, registry-root: 'Software\\Example'}\n"
            "limits:\n"
            "  units-per-player: true\n"
            "  effects: true\n"
            "settings:\n"
            "  limits.units-per-player.default: {registry: UnitLimit}\n"
            "  limits.effects.queue: {ini: Preferences/SfxLimit}\n"
            "  registry-seeds: {UnitLimit: 900, GameSpeed: 10}\n"
    );
    write(game / "MOD.INI", "[Preferences]\nSfxLimit = 1000;\n");
    const auto seeded = resolve_folder_profile({game}, {});
    OA_CHECK(seeded.errors.empty() && seeded.profile);
    OA_CHECK(seeded.profile->limits.effects.queue == 1000);
    OA_CHECK(seeded.profile->limits.units_per_player.default_limit == 900);
    // --print-profile reads the same INI file through the same reader.
    const auto printed = mod_settings_of(*seeded.profile, {game}, nullptr);
    OA_CHECK(printed.ini.size() == 1 && printed.registry.empty());

    const auto prefix = registry_key_prefix(*seeded.profile);
    OA_CHECK(prefix == "registry:Software\\Example\\");
    oa::platform::preferences::Values values{{prefix + "Total Annihilation|gamespeed", "5"}};
    OA_CHECK(seed_registry(*seeded.profile, values));
    OA_CHECK(values.at(prefix + "Total Annihilation|UnitLimit") == "900");
    OA_CHECK(!values.contains(prefix + "Total Annihilation|GameSpeed"));
    OA_CHECK(!seed_registry(*seeded.profile, values));
    values[prefix + "Total Annihilation|UnitLimit"] = "700";
    ModChoice choice{};
    choice.preferences = &values;
    const auto chosen = resolve_folder_profile({game}, choice);
    OA_CHECK(chosen.profile && chosen.profile->limits.units_per_player.default_limit == 700);

    // The profile's source, read again for Developer Mode, resolves to the
    // same profile, and with an override laid over it to that profile with
    // the override.
    const auto source = folder_profile_source({game}, choice);
    OA_CHECK(source.has_value());
    if (source) {
        OA_CHECK(source->name.ends_with("oamod.yaml") && !source->text.empty());
        OA_CHECK(source->options.overrides.empty() && !source->options.settings.ini.empty());
        const auto again =
            oa::data::mod_profile::resolve_profile(source->text, source->name, source->options);
        OA_CHECK(
            again.resolution && again.resolution->profile.full_hash == chosen.profile->full_hash
        );
        auto options = source->options;
        options.overrides.push_back({"units.id-reuse-delay", true, {}});
        const auto laid =
            oa::data::mod_profile::resolve_profile(source->text, source->name, options);
        OA_CHECK(laid.resolution && laid.resolution->profile.rules.units.id_reuse_delay.enabled);
        OA_CHECK(laid.resolution && laid.resolution->profile.sim_hash != chosen.profile->sim_hash);
    }
    OA_CHECK(!folder_profile_source({scratch / "nothing"}, {}).has_value());
    OA_CHECK(!folder_profile_source({}, {}).has_value());

    // The base game's registry root keeps the base game's own keys.
    OA_CHECK(registry_key_prefix(oa::data::mod_profile::ModProfile{}).empty());
}

void test_layout(const fs::path& scratch) {
    OA_CHECK(data_layout_of(nullptr) == oa::data::defs::DataLayout{});
    const auto base_plan = discovery_plan_of(nullptr);
    OA_CHECK(base_plan.revision_archive == "rev31.GP3" && base_plan.folders_as_disc);
    const fs::path file = scratch / "layout.oamod";
    write(
        file,
        std::string{base_profile} +
            "identity: {network-version: [10, 2], side-names: [Red, Blue, Green],\n"
            "           watcher-side-frame: 4, skirmish-hidden-sides: [3, 1],\n"
            "           battle-room-hidden-sides: [2]}\n"
            "layout:\n"
            "  revision-archive: modrev.gp4\n"
            "  archive-patterns: {ufo: \"*.SWX\"}\n"
            "  directories: {units: unitsM, weapons: weaponM}\n"
            "  file-extensions: {unit-definition: UNM}\n"
            "  map-units-section: unitsM\n"
    );
    const auto result = load_mod_profile(file, false);
    OA_CHECK(result.resolution.has_value());
    const auto& profile = result.resolution->profile;
    const auto layout = data_layout_of(&profile);
    using oa::data::defs::DataDirectory;
    OA_CHECK(layout.directories[static_cast<std::size_t>(DataDirectory::units)] == "unitsM");
    OA_CHECK(layout.directories[static_cast<std::size_t>(DataDirectory::weapons)] == "weaponM");
    OA_CHECK(layout.directories[static_cast<std::size_t>(DataDirectory::guis)] == "guis");
    OA_CHECK(layout.unit_extension == "UNM" && layout.map_units_section == "unitsM");
    OA_CHECK(layout.build_version[0] == 10 && layout.build_version[1] == 2);
    OA_CHECK(layout.side_names[0] == "Red" && layout.side_names[1] == "Blue");
    OA_CHECK(layout.side_names.size() == 3 && layout.side_names[2] == "Green");
    OA_CHECK(layout.watcher_side_frame == 4);
    OA_CHECK(layout.skirmish_hidden_sides == ((1U << 3) | (1U << 1)));
    OA_CHECK(layout.battle_room_hidden_sides == (1U << 2));
    const auto plan = discovery_plan_of(&profile);
    OA_CHECK(plan.revision_archive == "modrev.gp4" && plan.ufo_pattern == "*.SWX");
    OA_CHECK(plan.ccx_pattern == "*.CCX" && plan.hpi_pattern == "*.HPI");

    // Thirty-two sides: frame 63, and hidden sides 30, 31 and 5.
    const fs::path wide = scratch / "wide-sides.oamod";
    write(
        wide,
        std::string{base_profile} +
            "limits: {sides: 32}\n"
            "identity: {watcher-side-frame: 63, skirmish-hidden-sides: [31, 30],\n"
            "           battle-room-hidden-sides: [5]}\n"
    );
    const auto wide_result = load_mod_profile(wide, false);
    OA_CHECK(wide_result.resolution.has_value());
    if (wide_result.resolution) {
        const auto wide_layout = data_layout_of(&wide_result.resolution->profile);
        OA_CHECK(wide_layout.watcher_side_frame == 63);
        OA_CHECK(wide_layout.skirmish_hidden_sides == ((1U << 31) | (1U << 30)));
        OA_CHECK(wide_layout.battle_room_hidden_sides == (1U << 5));
    }
}

/// Checks the file each of the game's movies plays from: 3.1c's without a
/// profile or with one at its baselines, else the profile's, empty for none.
///
/// @param scratch a folder for the profile
void test_movies(const fs::path& scratch) {
    constexpr std::array<std::string_view, 5> base_files{
        "1.zrb", "2.zrb", "3.zrb", "4.zrb", "5.zrb"
    };
    const oa::data::mod_profile::ModProfile unchanged{};
    for (const std::string_view base_file : base_files) {
        OA_CHECK(movie_file_of(nullptr, base_file) == base_file);
        OA_CHECK(movie_file_of(&unchanged, base_file) == base_file);
    }
    const fs::path file = scratch / "movies.oamod";
    write(
        file,
        std::string{base_profile} +
            "media:\n"
            "  movies: {logo: \"\", intro: open.zrb, ending-a: won-a.smk, ending-b: won-b.smk,\n"
            "           credits: \"\"}\n"
    );
    const auto result = load_mod_profile(file, false);
    OA_CHECK(result.resolution.has_value());
    const auto& profile = result.resolution->profile;
    OA_CHECK(movie_file_of(&profile, "1.zrb").empty());
    OA_CHECK(movie_file_of(&profile, "2.zrb") == "open.zrb");
    OA_CHECK(movie_file_of(&profile, "3.zrb") == "won-a.smk");
    OA_CHECK(movie_file_of(&profile, "4.zrb") == "won-b.smk");
    OA_CHECK(movie_file_of(&profile, "5.zrb").empty());
    OA_CHECK(movie_file_of(&profile, "6.zrb") == "6.zrb");
}

void test_mod_folders(const fs::path& scratch) {
    const fs::path game = scratch / "offered";
    fs::create_directories(game / "MODS" / "beta");
    fs::create_directories(game / "MODS" / "Alpha");
    fs::create_directories(game / "MODS" / "empty");
    write(game / "MODS" / "beta" / "oamod.yaml", base_profile);
    write(game / "MODS" / "Alpha" / "OAMOD.YAML", base_profile);
    // Every folder is a mod, a folder without an oamod.yaml among them.
    const auto offered = list_mod_folders(game);
    OA_CHECK(offered.size() == 3);
    OA_CHECK(offered.size() == 3 && offered[0].filename() == "Alpha");
    OA_CHECK(offered.size() == 3 && offered[1].filename() == "beta");
    OA_CHECK(offered.size() == 3 && offered[2].filename() == "empty");
    OA_CHECK(list_mod_folders(scratch / "nothing").empty());
    // A folder of mods of its own, as the player's Mods folder, lists the
    // same way; a missing one lists none.
    const auto own = list_mods_in(game / "MODS");
    OA_CHECK(own == offered);
    OA_CHECK(list_mods_in(scratch / "nothing" / "Mods").empty());
    // A folder whose name starts with a dot is hidden: an install's own
    // folders in Mods, and any other.
    const fs::path hidden = scratch / "hidden-mods";
    fs::create_directories(hidden / ".oamod-staging-example-mod");
    fs::create_directories(hidden / ".hidden");
    fs::create_directories(hidden / "example-mod");
    const auto listed = list_mods_in(hidden);
    OA_CHECK(listed.size() == 1 && listed[0].filename() == "example-mod");

    // A mod folder without a profile is no error: it plays with none, by
    // 3.1c's own rules; a --mod file replaces its missing profile.
    const auto lacking = resolve_folder_profile(
        {game / "MODS" / "empty", game}, {game / "MODS" / "empty", {}, false, nullptr}
    );
    OA_CHECK(lacking.errors.empty() && lacking.warnings.empty() && !lacking.profile);
    const auto named = resolve_folder_profile(
        {game / "MODS" / "empty", game},
        {game / "MODS" / "empty", game / "MODS" / "beta" / "oamod.yaml", false, nullptr}
    );
    OA_CHECK(named.errors.empty() && named.profile && named.profile->id == "example");
}

void test_picked_folder(const fs::path& scratch) {
    const fs::path game = scratch / "picked-game";
    fs::create_directories(game);
    const fs::path mod = scratch / "anywhere" / "my mod";
    fs::create_directories(mod);
    write(mod / "OaMod.yaml", base_profile);

    // A folder that holds a profile resolving over the game folder is
    // played, under its own profile's id.
    const auto played = check_picked_mod_folder(mod, game, {});
    OA_CHECK(played.refusal.empty() && played.errors.empty());
    OA_CHECK(played.profile_id == "example");

    // Each refusal says why in a few words, and the log gets the errors.
    // A folder without a profile is not refused: it waits for the player to
    // agree to play it by 3.1c's own rules, its overrides under its own id.
    const fs::path empty = scratch / "anywhere" / "empty";
    fs::create_directories(empty);
    const auto none = check_picked_mod_folder(empty, game, {});
    OA_CHECK(none.refusal.empty() && none.errors.empty() && none.without_profile);
    OA_CHECK(none.profile_id == folder_overrides_id(empty));
    OA_CHECK(!played.without_profile);
    const auto missing = check_picked_mod_folder(scratch / "nowhere", game, {});
    OA_CHECK(missing.refusal == "That folder cannot be found.");
    const fs::path broken = scratch / "anywhere" / "broken";
    fs::create_directories(broken);
    write(broken / "oamod.yaml", "oamod: 1\nid: [\n");
    const auto unusable = check_picked_mod_folder(broken, game, {});
    OA_CHECK(unusable.refusal == "Its oamod.yaml cannot be played; the log says why.");
    OA_CHECK(!unusable.errors.empty() && unusable.profile_id.empty());
    // No profile takes the plain game's id, under which the game without a
    // mod keeps its overrides: the resolver refuses it.
    const fs::path base_id = scratch / "anywhere" / "base-id";
    fs::create_directories(base_id);
    write(
        base_id / "oamod.yaml",
        "oamod: 1\nid: ta-3.1c\nname: Same id\nversion: \"1.0\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"
    );
    const auto same_id = check_picked_mod_folder(base_id, game, {});
    OA_CHECK(same_id.refusal == "Its oamod.yaml cannot be played; the log says why.");
    OA_CHECK(same_id.profile_id.empty());
    // A game folder that is a mod's own copy carries no mod folder.
    const fs::path copied = scratch / "copied-game";
    fs::create_directories(copied);
    write(copied / "oamod.yaml", base_profile);
    const auto over_copy = check_picked_mod_folder(mod, copied, {});
    OA_CHECK(!over_copy.refusal.empty() && !over_copy.errors.empty());
    const auto empty_over_copy = check_picked_mod_folder(empty, copied, {});
    OA_CHECK(empty_over_copy.refusal == "That folder cannot be played; the log says why.");
    OA_CHECK(!empty_over_copy.errors.empty() && !empty_over_copy.without_profile);
    OA_CHECK(empty_over_copy.profile_id.empty());
}

void test_folder_overrides_id(const fs::path& scratch) {
    // The folder's absolute path after the prefix, which no profile's
    // kebab-case id, nor the plain game's, can be.
    const fs::path folder = (scratch / "ids" / "plain").lexically_normal();
    const std::string id = folder_overrides_id(folder);
    const auto absolute = fs::absolute(folder).u8string();
    OA_CHECK(
        id == std::string(folder_overrides_prefix) + std::string(absolute.begin(), absolute.end())
    );
    OA_CHECK(id != oa::data::mod_profile::base_game_id);
    // '%' and '|' are written out, so that no key holds a '|' and no two
    // folders share an id.
    const std::string odd = folder_overrides_id(scratch / "ids" / "a|b%7C");
    OA_CHECK(odd.find('|') == std::string::npos);
    OA_CHECK(odd.ends_with("a%7Cb%257C"));
    OA_CHECK(odd != folder_overrides_id(scratch / "ids" / "a%7Cb|"));
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("oa-app-mod-profile");
    test_find(scratch);
    test_print(scratch);
    test_check(scratch);
    test_settings(scratch);
    test_layout(scratch);
    test_movies(scratch);
    test_mod_folders(scratch);
    test_picked_folder(scratch);
    test_folder_overrides_id(scratch);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return oa::test::check_exit_status();
}
