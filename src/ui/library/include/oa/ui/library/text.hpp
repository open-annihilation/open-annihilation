// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every word the Library shows, in English, and the functions that write its
// sizes, ages, versions, rules hashes and engine requirements. A pattern's
// places are written {name}; a translation keeps them and may move them.
// The texts are looked up whole through TextHooks before their places are
// filled, so a translator finds every one of them here.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/replays/share.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/ui/library/library.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
struct VerdictRecord;
}

namespace oa::ui::library {

namespace words {

// Tabs and filters.
inline constexpr std::string_view tab_active = "Active";
inline constexpr std::string_view tab_mods = "Mods";
inline constexpr std::string_view tab_maps = "Maps";
inline constexpr std::string_view tab_mutators = "Mutators";
inline constexpr std::string_view tab_brains = "Brains";
inline constexpr std::string_view tab_keys = "Keys";
inline constexpr std::string_view tab_languages = "Languages";
inline constexpr std::string_view tab_languages_short = "Lang";
inline constexpr std::string_view tab_updates = "Updates";
inline constexpr std::string_view tab_updates_short = "Upd";
inline constexpr std::string_view tab_registries = "Registries";
/// Compact's short name, as Languages and Updates have.
inline constexpr std::string_view tab_registries_short = "Reg";
/// A tab and how many it lists under All, as the tab reads: "Mods (11)".
inline constexpr std::string_view tab_with_count = "{tab} ({count})";
inline constexpr std::string_view filter_all = "All";
inline constexpr std::string_view filter_new = "New";
inline constexpr std::string_view filter_installed = "Installed";
inline constexpr std::string_view filter_updates = "Updates";
/// A filter and how many entries it keeps on the tab shown, before the tag
/// and the search: "Installed (4)". The filter column, the chips and
/// Compact's drop-down all show it.
inline constexpr std::string_view filter_with_count = "{filter} ({count})";

// A row's state.
inline constexpr std::string_view state_get = "Get";
inline constexpr std::string_view state_installed = "Installed";
inline constexpr std::string_view state_update = "Update";
inline constexpr std::string_view state_playing = "Playing";
/// The chip on the language the game is showing, in place of Use now.
inline constexpr std::string_view chip_in_use = "In use";

// Buttons.
inline constexpr std::string_view action_get = "Get";
inline constexpr std::string_view action_update = "Update";
inline constexpr std::string_view action_update_all = "Update all";
inline constexpr std::string_view action_cancel = "Cancel";
inline constexpr std::string_view action_retry = "Retry";
inline constexpr std::string_view action_roll_back = "Roll back";
inline constexpr std::string_view action_play_now = "Play now";
/// On a built-in language, or an installed language pack, that is not the
/// one the game shows. It switches as Settings › Language does.
inline constexpr std::string_view action_use_now = "Use now";
inline constexpr std::string_view action_switch_off = "Switch off";
/// Unload mod on the Active tab. It asks what Switch off asks.
inline constexpr std::string_view action_unload_mod = "Unload mod";
inline constexpr std::string_view action_open_folder = "Open folder";
inline constexpr std::string_view action_homepage = "Homepage";
/// On an installed mutator whose verdict is set, or an installed brain whose
/// verdict is set. It checks that copy against the mod being played.
inline constexpr std::string_view action_check_with_mod = "Check with this mod";
/// On an installed brain that is not the default, and on the built-in
/// computer player while it is not. It makes that player the default.
inline constexpr std::string_view action_use_by_default = "Use by default";
/// Use on a key layout. The question's button uses the same word.
inline constexpr std::string_view action_use = "Use";
/// Edit keys… on the layout in use.
inline constexpr std::string_view action_edit_keys = "Edit keys…";
/// The chip on the layout in use.
inline constexpr std::string_view chip_using = "Using";
/// Get's place when one prerequisite is missing. `{name}` is that prerequisite.
inline constexpr std::string_view action_get_with = "Get with {name}";
/// Get's place when more than one package besides the mutator will download.
inline constexpr std::string_view action_get_with_more = "Get with {count} more";
/// The question Get with asks.
inline constexpr std::string_view get_with_title = "Get {name}?";
/// The question's paragraph when one prerequisite is missing. `{prerequisite}`
/// is its name and range; `{short}` is the name alone.
inline constexpr std::string_view get_with_body_one =
    "{name} needs {prerequisite}, which you don't have. Both download now, and {short} goes above "
    "{name} in every list.";
/// The question's paragraph when several prerequisites are missing.
/// `{prerequisites}` joins their names and ranges.
inline constexpr std::string_view get_with_body_many =
    "{name} needs {prerequisites}, which you don't have. They download first, and go above {name} "
    "in every list.";
/// After a package's name and version on the question: the registry it comes from.
/// The question writes a space in front. A catalogue key cannot begin with a
/// space: the reader trims it, and the four languages then have no translation.
inline constexpr std::string_view get_with_from = "· from {registry}";
/// The question's button when two packages download.
inline constexpr std::string_view action_get_both = "Get both";
/// The question's button when three or more packages download.
inline constexpr std::string_view action_get_all = "Get all";
/// The details note when one prerequisite downloads first. `{names}` is its name.
inline constexpr std::string_view note_downloads_first =
    "{names} downloads first, then sits above {name} in every list.";
/// The details note when several prerequisites download first. `{names}` joins them.
inline constexpr std::string_view note_download_first =
    "{names} download first, then sit above {name} in every list.";
/// Why Get with is off: no listing offers the prerequisite inside its range.
/// A prerequisite that names no range uses `blocked_prerequisite_any`, so
/// the sentence does not keep a blank before the comma.
inline constexpr std::string_view blocked_prerequisite =
    "Needs {name} {range}, which no registry offers";
/// Why Get with is off, and why a picker row is locked, when the
/// prerequisite names no version range.
inline constexpr std::string_view blocked_prerequisite_any =
    "Needs {name}, which no registry offers";
/// Why Get with is off: the mutators name each other. `{ids}` joins the ids in the loop.
inline constexpr std::string_view blocked_cycle = "These mutators need each other: {ids}";
/// The uninstall question, before its last paragraph, when one other installed
/// mutator needs this one.
inline constexpr std::string_view uninstall_needed_by_one =
    "{name} needs it: it stays installed, and cannot be switched on until this is back.";
/// The same paragraph when several other installed mutators need this one.
inline constexpr std::string_view uninstall_needed_by_many =
    "{names} need it: they stay installed, and cannot be switched on until this is back.";
inline constexpr std::string_view action_details = "Details";
/// Back on Compact's details page. It returns to the list, which keeps its place.
inline constexpr std::string_view action_back = "Back";
/// The header's refresh icon, and the same words while it is turning.
inline constexpr std::string_view action_refresh = "Refresh";
inline constexpr std::string_view action_refreshing = "Refreshing";
inline constexpr std::string_view action_settings = "Settings…";
inline constexpr std::string_view action_close = "Close";
/// The header's close mark. The footer's Close uses the same word.
inline constexpr std::string_view action_close_x = "Close";
inline constexpr std::string_view action_uninstall = "Uninstall";

// The Registries tab. The words are the ones Downloads' Registries part used.
inline constexpr std::string_view registry_built_in = "Built in";
/// The built-in computer player's name and the summary under it.
inline constexpr std::string_view built_in_brain_name = "Built-in computer player";
inline constexpr std::string_view built_in_brain_summary =
    "Open Annihilation's own computer player. It plays as Total Annihilation's does.";
inline constexpr std::string_view registry_add = "Add registry…";
inline constexpr std::string_view registry_add_mirror = "Add mirror…";
inline constexpr std::string_view registry_remove = "Remove";
inline constexpr std::string_view registry_game_lock = "Choose from the main menu.";
inline constexpr std::string_view registry_off = "Off";
inline constexpr std::string_view registry_on = "On";
inline constexpr std::string_view registry_unreachable = "couldn't reach";
inline constexpr std::string_view registry_not_reviewed = "not reviewed";
/// Refresh on a registry's card, the same word as the header's icon.
inline constexpr std::string_view registry_refresh = "Refresh";
/// When a refresh last read the registry, with the age in its place.
inline constexpr std::string_view registry_refreshed = "refreshed {age}";
/// A registry no refresh has read yet.
inline constexpr std::string_view registry_never = "never refreshed";
/// The key tooltip's title for one signing key, and for more than one.
/// These are not the key bindings' "Keys".
inline constexpr std::string_view registry_key_one = "Signing key";
inline constexpr std::string_view registry_key_many = "Signing keys";
/// The key tooltip's aside for a registry that ships with the game.
inline constexpr std::string_view registry_key_built_in = "built in";
/// The key tooltip's aside for a registry the player added.
inline constexpr std::string_view registry_key_added = "added by you";
/// The key tooltip when the registry names no signing key.
inline constexpr std::string_view registry_no_key = "No signing key";
inline constexpr std::string_view registry_one_day = "1 day ago";
inline constexpr std::string_view registry_package_one = "{n} package";
inline constexpr std::string_view registry_package_many = "{n} packages";
inline constexpr std::string_view registry_mirror_one = "{n} mirror";
inline constexpr std::string_view registry_mirror_many = "{n} mirrors";
inline constexpr std::string_view registry_note =
    "Content from registries other than {names} isn't reviewed by the OA team. OA still checks "
    "every package before using it.";
inline constexpr std::string_view registry_note_unnamed =
    "Content from these registries isn't reviewed by the OA team. OA still checks "
    "every package before using it.";

// Why Get or Update is off.
inline constexpr std::string_view blocked_in_match = "Not during a game";
inline constexpr std::string_view blocked_downloads_off =
    "Downloads from {registry} are off (Settings › Downloads)";
inline constexpr std::string_view blocked_engine = "Needs Open Annihilation {range}";
inline constexpr std::string_view blocked_base = "Needs the base game {base}";
inline constexpr std::string_view blocked_hacks = "Needs hacks this OA lacks: {hacks}";
/// The missing hacks named, then how many more are not.
inline constexpr std::string_view hacks_and_more = "{hacks} · {count} more";
/// The comma between two names of a list. Callers write the joining space
/// after the lookup, so the looked-up text can be a catalogue section.
inline constexpr std::string_view list_separator = ",";

// Uninstall (uninstall.hpp): why it is off, its question and its notices.
/// Why Uninstall is off for a map pack while a game is played.
inline constexpr std::string_view uninstall_off_in_game =
    "Map packs can't be uninstalled during a game.";
/// Why Uninstall is off while the package is in the download queue.
inline constexpr std::string_view uninstall_off_queued =
    "It is in the download queue: Cancel it before you uninstall it.";
/// The question's title, and the notices'.
inline constexpr std::string_view uninstall_title = "Uninstall {name}";
/// What the question asks, with the folder under the player's folder, as
/// "Mods/ridge"; the second for a package that keeps a copy to roll back to.
inline constexpr std::string_view uninstall_ask =
    "Uninstall {name}? Its folder, {folder}, is removed.";
inline constexpr std::string_view uninstall_ask_kept =
    "Uninstall {name}? Its folder, {folder}, is removed, and with it the copy Roll back keeps.";
/// After the question: for a package a registry lists, and for the player's own.
inline constexpr std::string_view uninstall_listed = "Get installs it again.";
inline constexpr std::string_view uninstall_own =
    "No registry lists it, so the Library cannot get it again.";
/// The notice for the mod being played.
inline constexpr std::string_view uninstall_playing =
    "You are playing {name}. Switch it off first with Switch off, then uninstall it.";
/// The notice for the language pack in use.
inline constexpr std::string_view uninstall_language =
    "The game is shown in {name}. Choose another language in Settings › Language first, then "
    "uninstall it.";
/// The notice when an uninstall removed nothing, then one of the reasons below.
inline constexpr std::string_view uninstall_failed = "{name} was not uninstalled. {reason}";
inline constexpr std::string_view uninstall_failed_busy =
    "Open Annihilation, or another copy of it, is still changing its folder. Try again in a "
    "moment.";
inline constexpr std::string_view uninstall_failed_changed =
    "Its folder changed since the Library listed it. Try again.";
inline constexpr std::string_view uninstall_failed_link =
    "Its folder is a link, and OA never removes what a link leads to. Open folder shows it.";
inline constexpr std::string_view uninstall_failed_outside =
    "Its folder is not in your own Open Annihilation folder, and OA removes nothing outside it.";
inline constexpr std::string_view uninstall_failed_not_moved =
    "Its folder could not be moved; another program may be using it. Close it, then try again.";
/// The notices' one button.
inline constexpr std::string_view uninstall_ok = "OK";

// Sizes, binary units.
/// A number with one decimal.
inline constexpr std::string_view one_decimal = "{whole}.{tenth}";
inline constexpr std::string_view size_kb = "{size} KB";
inline constexpr std::string_view size_mb = "{size} MB";
inline constexpr std::string_view size_gb = "{size} GB";
/// A download's progress: the bytes so far as a number, the whole size with its unit.
inline constexpr std::string_view progress = "{done} of {total}";

// Ages.
inline constexpr std::string_view age_just_now = "just now";
inline constexpr std::string_view age_minutes = "{count} min ago";
inline constexpr std::string_view age_hours = "{count} h ago";
inline constexpr std::string_view age_days = "{count} days ago";

// Versions.
inline constexpr std::string_view version_with_revision = "{version} · rev {revision}";
inline constexpr std::string_view version_with_maps = "{version} · {maps}";
inline constexpr std::string_view revision_long = "revision {revision}";
inline constexpr std::string_view revision_short = "rev {revision}";
/// A version and its revision in the details' Installed and Latest facts.
inline constexpr std::string_view version_revision_fact = "{version} (rev {revision})";
/// What an update changes from and to where no version or revision above 0
/// names a side: the copy installed now, and the release it is updated to.
inline constexpr std::string_view change_old_copy = "the old copy";
inline constexpr std::string_view change_new_copy = "the new copy";
inline constexpr std::string_view change_old_copy_short = "old";
inline constexpr std::string_view change_new_copy_short = "new";
inline constexpr std::string_view map_count_one = "{count} map";
inline constexpr std::string_view map_count = "{count} maps";
// What a compatible verdict changed, one collection at a time. A count of
// one uses the singular, as the other counted words do.
inline constexpr std::string_view changes_units_one = "changes {count} unit";
inline constexpr std::string_view changes_units = "changes {count} units";
inline constexpr std::string_view adds_units_one = "adds {count} unit";
inline constexpr std::string_view adds_units = "adds {count} units";
inline constexpr std::string_view removes_units_one = "removes {count} unit";
inline constexpr std::string_view removes_units = "removes {count} units";
inline constexpr std::string_view changes_weapons_one = "changes {count} weapon";
inline constexpr std::string_view changes_weapons = "changes {count} weapons";
inline constexpr std::string_view adds_weapons_one = "adds {count} weapon";
inline constexpr std::string_view adds_weapons = "adds {count} weapons";
inline constexpr std::string_view removes_weapons_one = "removes {count} weapon";
inline constexpr std::string_view removes_weapons = "removes {count} weapons";
inline constexpr std::string_view changes_features_one = "changes {count} feature";
inline constexpr std::string_view changes_features = "changes {count} features";
inline constexpr std::string_view adds_features_one = "adds {count} feature";
inline constexpr std::string_view adds_features = "adds {count} features";
inline constexpr std::string_view removes_features_one = "removes {count} feature";
inline constexpr std::string_view removes_features = "removes {count} features";
inline constexpr std::string_view changes_menus_one = "changes {count} menu entry";
inline constexpr std::string_view changes_menus = "changes {count} menu entries";
inline constexpr std::string_view adds_files_one = "adds {count} file";
inline constexpr std::string_view adds_files = "adds {count} files";
inline constexpr std::string_view hides_files_one = "hides {count} file";
inline constexpr std::string_view hides_files = "hides {count} files";
/// The nine layers, in the order a manifest names them. Units, Weapons,
/// Maps and Hacks reuse words the catalogue already has.
inline constexpr std::string_view layer_files = "Files";
inline constexpr std::string_view layer_units = "Units";
inline constexpr std::string_view layer_weapons = "Weapons";
inline constexpr std::string_view layer_features = "Features";
inline constexpr std::string_view layer_sides = "Sides";
inline constexpr std::string_view layer_tables = "Game tables";
inline constexpr std::string_view layer_menus = "Build menus";

// Engine requirements, one comparison each, joined with list_separator.
inline constexpr std::string_view engine_at_least = "{version} or later";
inline constexpr std::string_view engine_above = "later than {version}";
inline constexpr std::string_view engine_at_most = "{version} or earlier";
inline constexpr std::string_view engine_below = "before {version}";
inline constexpr std::string_view engine_exactly = "exactly {version}";
inline constexpr std::string_view engine_at_least_short = "{version}+";
inline constexpr std::string_view engine_above_short = "after {version}";
inline constexpr std::string_view engine_at_most_short = "up to {version}";
inline constexpr std::string_view engine_below_short = "before {version}";
inline constexpr std::string_view engine_exactly_short = "{version} only";

// The facts of an entry's details: labels, then values.
inline constexpr std::string_view fact_installed = "Installed";
inline constexpr std::string_view fact_latest = "Latest";
inline constexpr std::string_view fact_author = "Author";
inline constexpr std::string_view fact_size = "Size";
inline constexpr std::string_view fact_base = "Base";
inline constexpr std::string_view fact_needs = "Needs";
inline constexpr std::string_view fact_hacks = "Hacks";
inline constexpr std::string_view fact_rules = "Rules";
inline constexpr std::string_view fact_maps = "Maps";
inline constexpr std::string_view fact_coverage = "Coverage";
inline constexpr std::string_view fact_from = "From";
inline constexpr std::string_view fact_works_with = "Works with";
inline constexpr std::string_view fact_levels = "Levels";
/// The built-in computer player's three levels, joined with names_and.
inline constexpr std::string_view level_easy = "Easy";
inline constexpr std::string_view level_medium = "Medium";
inline constexpr std::string_view level_hard = "Hard";
inline constexpr std::string_view fact_checked = "Checked";
inline constexpr std::string_view fact_options = "Options";
inline constexpr std::string_view fact_changes = "Changes";
/// Works with, when the package names no mod or names any mod.
inline constexpr std::string_view works_with_any = "Any mod";
/// Full awareness, in a brain's facts. The chip uses chip_full_awareness.
inline constexpr std::string_view full_awareness_sees =
    "Sees every unit on the map, as Total Annihilation's own computer player does";
/// A brain's Network fact.
inline constexpr std::string_view fact_network = "Network";
/// A brain that carries a learned network. `{size}` is size_text.
inline constexpr std::string_view network_learned = "Uses a learned network ({size})";
/// The summary a brain's compatible check keeps. The Checked fact fills it in.
inline constexpr std::string_view fits_this_game = "it fits this game";
inline constexpr std::string_view size_with_download = "{size} · {download} to download";
inline constexpr std::string_view size_download_short = "{download} to download";
inline constexpr std::string_view base_supported = "Total Annihilation 3.1c";
inline constexpr std::string_view base_supported_short = "TA 3.1c";
inline constexpr std::string_view needs_engine = "Open Annihilation {range}";
inline constexpr std::string_view needs_engine_short = "OA {range}";
/// A prerequisite no installed mutator meets. `{range}` is the version range
/// in the engine's comparison words. A prerequisite that names no range uses
/// `needs_mutator_missing_name` instead, so the line does not keep a blank.
inline constexpr std::string_view needs_mutator_missing = "{name} {range}: not installed";
/// A prerequisite an installed mutator meets, when it names a version range.
inline constexpr std::string_view needs_mutator_met = "{name} {range} ✓";
/// A prerequisite with no version range that no installed mutator meets.
inline constexpr std::string_view needs_mutator_missing_name = "{name}: not installed";
inline constexpr std::string_view hacks_counts = "{game} change the game · {view} view only";
inline constexpr std::string_view hacks_counts_short = "{game} game · {view} view";
inline constexpr std::string_view hacks_none = "none";
/// The hack counts, then the missing hacks in blocked_hacks' words.
inline constexpr std::string_view hacks_with_missing = "{counts} · {missing}";
inline constexpr std::string_view hacks_with_missing_short = "{counts} · {count} missing";
inline constexpr std::string_view rules_same = "{from} → {to} same";
inline constexpr std::string_view rules_same_short = "unchanged";
inline constexpr std::string_view rules_changes =
    "{from} → {to} · changes the game's rules; saved games from {old} won't load under {new}";
inline constexpr std::string_view rules_changes_short = "changes · {old} saves won't load";
inline constexpr std::string_view rules_unknown =
    "unknown · saved games from {old} may not load under {new}";
inline constexpr std::string_view rules_unknown_short = "unknown · saves may not load";
inline constexpr std::string_view coverage_percent = "{percent}%";
inline constexpr std::string_view not_reviewed = "Not reviewed by the OA team";
inline constexpr std::string_view from_not_reviewed = "{registry} · Not reviewed by the OA team";
inline constexpr std::string_view from_own = "Your own copy";
/// A mutator's Rules fact, shown whole at every size class.
inline constexpr std::string_view rules_mutator = "Changes the game: unranked on Core Prime";
/// Checked, when the verdict is compatible. `{summary}` is the counts in words.
inline constexpr std::string_view checked_good = "With {mod}: {summary}";
/// Checked, when the mutator applies and changes nothing.
inline constexpr std::string_view checked_no_effect = "With {mod}: no effect";
/// Checked, when the mutator cannot run with this mod.
inline constexpr std::string_view checked_incompatible = "With {mod}: {reason}";
/// Checked, when the check itself did not finish.
inline constexpr std::string_view checked_failed = "With {mod}: it stopped: {reason}";
/// Checked, while a check is running.
inline constexpr std::string_view checked_running = "Checking with {mod}…";
/// Checked, when the host can check and has not yet.
inline constexpr std::string_view checked_not_yet = "Not checked with {mod}";
/// The same six facts when no mod is played. A translator writes the base
/// game in the sentence, so it is not "With {mod}" filled with "No Mod".
inline constexpr std::string_view checked_good_base = "With No Mod: {summary}";
inline constexpr std::string_view checked_no_effect_base = "With No Mod: no effect";
inline constexpr std::string_view checked_incompatible_base = "With No Mod: {reason}";
inline constexpr std::string_view checked_failed_base = "With No Mod: it stopped: {reason}";
inline constexpr std::string_view checked_running_base = "Checking with No Mod…";
inline constexpr std::string_view checked_not_yet_base = "Not checked with No Mod";
/// A compatible verdict whose counts are all zero: sides, tables, or anything
/// else the counts do not name.
inline constexpr std::string_view changes_other_data = "changes other game data";
/// A compatible verdict for a list whose last mutator declares maps and
/// changed nothing a dry run can see. The map hook runs when a match starts.
inline constexpr std::string_view changes_map_on_start = "changes the map when a match starts";
/// One option's label and its value captions.
inline constexpr std::string_view option_values = "{label}: {values}";

// The line under an entry's name.
/// An update to a revision above 0 of the version installed.
inline constexpr std::string_view status_revision_available =
    "Revision {revision} available · {size} to download";
inline constexpr std::string_view status_version_available =
    "Version {version} available · {size} to download";
/// An update that keeps the version and names no revision above 0.
inline constexpr std::string_view status_update_available = "Update available · {size} to download";
inline constexpr std::string_view status_installed = "Installed";
/// An installed brain that is the default computer player.
inline constexpr std::string_view status_installed_default = "Installed · Default";
/// The built-in computer player while it is the default. While it is not,
/// the status is registry_built_in.
inline constexpr std::string_view status_built_in_default = "Built in · Default";
inline constexpr std::string_view status_playing = "Playing";
/// The status of a mod folder with no oamod.yaml, installed or played.
inline constexpr std::string_view status_without_manifest = "No oamod.yaml present";
inline constexpr std::string_view status_not_installed = "Not installed · {size}";
/// A mutator that is not installed. `{version}` includes a revision of 1.
inline constexpr std::string_view status_not_installed_version = "Not installed · {version}";
inline constexpr std::string_view status_waiting = "Waiting to download";
inline constexpr std::string_view status_downloading = "Downloading · {progress}";
inline constexpr std::string_view status_downloading_unsized = "Downloading";
inline constexpr std::string_view status_verifying = "Verifying";
inline constexpr std::string_view status_checking = "Checking";
inline constexpr std::string_view status_installing = "Installing";
inline constexpr std::string_view status_failed = "Failed";
inline constexpr std::string_view byline = "by {author}";

// The note on updating while a mod is played.
inline constexpr std::string_view note_finishes = "Finishes when you leave {name}.";
inline constexpr std::string_view note_playing_other =
    "You are playing {playing}. Updating {name} doesn't restart the game.";
/// Why PLAY NOW and SWITCH OFF are off: --mod-dir or --base-game.
inline constexpr std::string_view note_command_line = "The command line chose this run's mod.";
/// A mutator that does not work with the mod being played. Information only:
/// GET and UPDATE stay on. `{names}` is the mods it works with.
inline constexpr std::string_view note_not_compatible =
    "Not compatible with this mod: it works with {names}";
/// A Large row's chip when a mutator still needs another. `{name}` is that
/// mutator's name. Drawn in the lock colour; GET stays on.
inline constexpr std::string_view chip_needs = "Needs {name}";
/// A Large row's chip when a mutator does not work with the mod being played.
/// Information only, in the lock colour. GET stays on, and the row is not dimmed.
inline constexpr std::string_view chip_not_compatible = "Not compatible with this mod";
/// After a Compact row's title when a mutator still needs another.
inline constexpr std::string_view row_needs = "· needs {name}";
/// After a Compact row's title when a mutator does not work with this mod.
inline constexpr std::string_view row_not_compatible = "· not compatible with this mod";
/// After a Compact row's title when a brain does not fit the game being
/// played. `{reason}` is brain_unfit_reason. Information only.
inline constexpr std::string_view row_brain_unfit = "· {reason}";
/// The chip on the default computer player.
inline constexpr std::string_view chip_default = "Default";
/// The chip on a brain that sees every unit.
inline constexpr std::string_view chip_full_awareness = "Full awareness";
/// Why a brain does not work with the game being played. `{names}` is the
/// mods it works with, joined with names_or.
inline constexpr std::string_view made_for = "Made for {names}";
/// UNINSTALL of the default brain, before the question's last paragraph.
inline constexpr std::string_view uninstall_default_brain =
    "It is your default computer player: new computer seats go back to the built-in player.";
/// OA default's name and the line under it.
inline constexpr std::string_view built_in_layout_name = "OA default";
inline constexpr std::string_view built_in_layout_summary =
    "The keys Open Annihilation ships with.";
/// A key layout's facts.
inline constexpr std::string_view fact_asks_for = "Asks for";
inline constexpr std::string_view fact_not_in_version = "Not in this version";
inline constexpr std::string_view fact_key_layouts = "Key layouts";
/// Actions this version does not know. `{count}` is the number.
inline constexpr std::string_view not_in_version_one = "Not in this version: 1";
inline constexpr std::string_view not_in_version_many = "Not in this version: {count}";
/// The player's own changes, on the layout they sit on. `{count}` is the number.
inline constexpr std::string_view own_changes_one = "· 1 change of your own";
inline constexpr std::string_view own_changes_many = "· {count} changes of your own";
/// The Active tile, when a layout is set for the mod being played.
inline constexpr std::string_view keys_with_mod = "With {mod}: {name}";
/// USE's question.
inline constexpr std::string_view use_title = "Use {name}";
inline constexpr std::string_view use_already_one =
    "{name} already has 1 change of your own; it stays.";
inline constexpr std::string_view use_already_many =
    "{name} already has {count} changes of your own; they stay.";
inline constexpr std::string_view use_every_game = "Every game";
inline constexpr std::string_view use_only_with = "Only with {mod}";
inline constexpr std::string_view use_latch = "Latch {action}";
inline constexpr std::string_view use_hold = "Hold to confirm {action}";
/// The master switch, when a layout asks for it without naming an action.
inline constexpr std::string_view use_hold_plain = "Hold to confirm";
inline constexpr std::string_view use_sticky = "Sticky modifiers";
inline constexpr std::string_view use_pointer_keys = "Pointer keys";
inline constexpr std::string_view use_wheel_right = "Order wheel on the held right button";
inline constexpr std::string_view use_wheel_middle = "Order wheel on the held middle button";
inline constexpr std::string_view use_edge_off = "Edge scrolling off";
/// `{layout}` and `{yours}` are "left-click" or "right-click".
inline constexpr std::string_view use_pointer_mismatch =
    "Made for the {layout} interface; yours is {yours}.";
/// A key layout's state on its row and in its details: one word, never an icon.
inline constexpr std::string_view layout_state_installed = "Installed";
inline constexpr std::string_view layout_state_update = "Update";
inline constexpr std::string_view layout_state_get = "Get";
/// GET's state with the download's size. `{size}` is size_text's.
inline constexpr std::string_view layout_state_get_size = "Get · {size}";
/// The status line of the layout in use: where it is used. `{mod}` is the
/// mod being played.
inline constexpr std::string_view status_in_use_every_game = "In use for every game";
inline constexpr std::string_view status_in_use_with = "In use with {mod}";
/// The own changes on a layout's row. `{count}` is the number.
inline constexpr std::string_view own_count_one = "1 change of your own";
inline constexpr std::string_view own_count_many = "{count} changes of your own";
/// A key layout's facts, said by what it does.
inline constexpr std::string_view fact_your_changes = "Your changes";
/// How many commands a layout moves. `{count}` is the number.
inline constexpr std::string_view commands_one = "1 command";
inline constexpr std::string_view commands_many = "{count} commands";
/// One own change in the Your changes fact. `{command}` is the command's
/// name, `{key}` its key.
inline constexpr std::string_view own_change_on = "{command} on {key}";
/// One own change that leaves a command with no key.
inline constexpr std::string_view own_change_off = "{command} with no key";
/// After the commands a list names, how many it leaves out.
inline constexpr std::string_view names_more = "and {count} more";
/// What a layout works with after its mods: the keyboard, then the interface.
inline constexpr std::string_view works_keyboard_any = "any keyboard";
inline constexpr std::string_view works_keyboard_ansi = "ANSI keyboards";
inline constexpr std::string_view works_keyboard_iso = "ISO keyboards";
inline constexpr std::string_view works_keyboard_jis = "JIS keyboards";
inline constexpr std::string_view works_pointer_right = "the right-click interface";
inline constexpr std::string_view works_pointer_left = "the left-click interface";
/// USE's question, scope first. A section's heading is numbered: `{number}`
/// counts the sections shown and `{heading}` is its words.
inline constexpr std::string_view use_section = "{number} · {heading}";
inline constexpr std::string_view use_where = "Where";
/// The own-changes section. `{name}` is the layout they sit on.
inline constexpr std::string_view use_yours_one = "Your 1 change to {name}";
inline constexpr std::string_view use_yours_many = "Your {count} changes to {name}";
inline constexpr std::string_view use_keep_it = "Keep it on top";
inline constexpr std::string_view use_keep_them = "Keep them on top";
inline constexpr std::string_view use_start_clean = "Start clean";
inline constexpr std::string_view use_saved_one =
    "Either way it stays saved with {name}, and comes back when you use {name} again.";
inline constexpr std::string_view use_saved_many =
    "Either way they stay saved with {name}, and come back when you use {name} again.";
/// The before-and-after list.
inline constexpr std::string_view use_what_moves = "What moves";
inline constexpr std::string_view use_nothing_moves = "No command changes its key.";
/// After a command whose key afterwards is the player's own.
inline constexpr std::string_view use_yours = "yours";
/// In place of a key, for a command with none.
inline constexpr std::string_view use_no_key = "No key";
/// The helpers' note: they are settings, not part of a layout.
inline constexpr std::string_view use_helpers_every_game = "Helpers act in every game.";
inline constexpr std::string_view use_helpers_not_only =
    "Helpers act in every game, not only with {mod}.";
/// USE's button names the answer.
inline constexpr std::string_view use_for_every_game = "Use for every game";
inline constexpr std::string_view use_for_mod = "Use for {mod}";
inline constexpr std::string_view use_cancel = "Cancel";
/// UNINSTALL of the layout in use.
inline constexpr std::string_view uninstall_layout_in_use =
    "You play with {name} now. Uninstall it and go back to OA default? Your own changes stay "
    "saved.";

// The download queue in the footer.
inline constexpr std::string_view queue_package = "{name} {version}";
inline constexpr std::string_view queue_downloading = "Downloading {package} · {progress}";
inline constexpr std::string_view queue_downloading_unsized = "Downloading {package}";
inline constexpr std::string_view queue_waiting = "Waiting to download {package}";
inline constexpr std::string_view queue_verifying = "Verifying {name}";
inline constexpr std::string_view queue_checking = "Checking {name}";
inline constexpr std::string_view queue_installing = "Installing {name}";
inline constexpr std::string_view queue_held = "Waiting · finishes when you leave {name}";
inline constexpr std::string_view queue_installed = "Installed {name}";
inline constexpr std::string_view queue_updated_changes =
    "Updated {name} · {new} changes the game's rules · Roll back restores {old}";
inline constexpr std::string_view queue_updated_unknown =
    "Updated {name} · {new} may change the game's rules · Roll back restores {old}";
inline constexpr std::string_view queue_failed_retry = "{problem} · Retry";
inline constexpr std::string_view queue_then = "Then {package} · {phase}";
inline constexpr std::string_view queue_more = "{line} · {count} more";
inline constexpr std::string_view queue_percent = "{name} {percent}%";
inline constexpr std::string_view queue_rules_changed = "{name} · rules changed";
inline constexpr std::string_view queue_phase = "{name} · {phase}";
inline constexpr std::string_view phase_waiting = "waiting";
inline constexpr std::string_view phase_downloading = "downloading";
inline constexpr std::string_view phase_verifying = "verifying";
inline constexpr std::string_view phase_checking = "checking";
inline constexpr std::string_view phase_installing = "installing";
inline constexpr std::string_view phase_held = "after you leave it";
inline constexpr std::string_view phase_failed = "failed";
inline constexpr std::string_view phase_installed = "installed";

// The screens (screen.hpp): the header.
/// The Library's name, as the main menu's Library button shows it.
inline constexpr std::string_view title_library = "Library";
/// The header's title at Compact, and its second word at Regular and Large,
/// in capitals as Settings' header has its own.
inline constexpr std::string_view title_library_header = "LIBRARY";
/// The header's first words at Regular and Large; title_library_header
/// follows them.
inline constexpr std::string_view title_open_annihilation = "OPEN ANNIHILATION";
/// Beside the header's refresh icon when the last refresh could not reach a registry.
inline constexpr std::string_view offline_mark = "offline";

// The search and the filters.
inline constexpr std::string_view search_mods = "Search mods, authors, tags";
inline constexpr std::string_view search_mutators = "Search mutators, authors, tags";
inline constexpr std::string_view search_brains = "Search brains, authors, tags";
inline constexpr std::string_view search_keys = "Search key layouts";
inline constexpr std::string_view search_maps = "Search maps, authors, tags";
inline constexpr std::string_view search_languages = "Search languages";
/// The Active tab's maps field, above the maps pane.
inline constexpr std::string_view search_active_maps = "Search maps";
inline constexpr std::string_view search_updates = "Search updates";
/// A tag in Compact's filter drop-down.
inline constexpr std::string_view filter_tag = "Tag: {tag} ({count})";
/// A tag in the filter pane, with how many entries have it.
inline constexpr std::string_view tag_with_count = "{tag} ({count})";
/// The drop-down holding the tags that do not fit, and the filter pane's heading over them.
inline constexpr std::string_view tags_menu = "TAGS";
/// The filter pane's heading over the world tags, apart from the other
/// tags, and the heading before them in a drop-down.
inline constexpr std::string_view world_heading = "WORLD";
/// A world tag in Compact's filter drop-down: its world's name.
inline constexpr std::string_view filter_world = "World: {tag} ({count})";
/// The eight catalogue tag ids, as a player reads them. Any other id is shown as written.
inline constexpr std::string_view tag_statistics = "Statistics";
inline constexpr std::string_view tag_economy = "Economy";
inline constexpr std::string_view tag_commanders = "Commanders";
inline constexpr std::string_view tag_new_units = "New Units";
inline constexpr std::string_view tag_disable = "Disable";
inline constexpr std::string_view tag_gameplay = "Gameplay";
inline constexpr std::string_view tag_ai = "AI";
/// The nine brain tag ids, as a player reads them. Economy reuses tag_economy.
/// Any other id is shown as written.
inline constexpr std::string_view tag_classic = "Classic";
inline constexpr std::string_view tag_rush = "Rush";
inline constexpr std::string_view tag_turtle = "Turtle";
inline constexpr std::string_view tag_air = "Air";
inline constexpr std::string_view tag_naval = "Naval";
inline constexpr std::string_view tag_learned = "Learned";
inline constexpr std::string_view tag_teaching = "Teaching";
inline constexpr std::string_view tag_experimental = "Experimental";
/// The ten key-layout tags of the standard, other than Classic, which uses tag_classic.
inline constexpr std::string_view tag_grid = "Grid";
inline constexpr std::string_view tag_left_hand = "Left hand";
inline constexpr std::string_view tag_right_hand = "Right hand";
inline constexpr std::string_view tag_one_hand = "One hand";
inline constexpr std::string_view tag_mouse_first = "Mouse first";
inline constexpr std::string_view tag_accessibility = "Accessibility";
inline constexpr std::string_view tag_tournament = "Tournament";
inline constexpr std::string_view tag_mod = "Mod";
inline constexpr std::string_view tag_legacy = "Legacy";
/// The Compact details page's drop-down holding the actions after the first three.
inline constexpr std::string_view more_menu = "More";
/// How many updates the Updates tab shows.
inline constexpr std::string_view update_count_one = "{count} update";
inline constexpr std::string_view update_count_many = "{count} updates";
/// How many packages the player has added, as the main menu's Library button says.
/// A package is a mod, a map pack, a language pack, a mutator or a downloaded
/// expansion. An installed brain is not counted, and neither is a key layout.
inline constexpr std::string_view package_count_one = "{count} package";
inline constexpr std::string_view package_count_many = "{count} packages";

// The Active tab: the base game, its expansions, the mod being played, the
// language in use and the installed maps, under one heading each.
inline constexpr std::string_view active_heading_base = "BASE GAME";
inline constexpr std::string_view active_heading_mod = "MOD";
inline constexpr std::string_view active_heading_language = "LANGUAGE";
inline constexpr std::string_view active_heading_expansions = "EXPANSIONS";
inline constexpr std::string_view active_expansions_count = "EXPANSIONS · {on} OF {count} ON";
inline constexpr std::string_view active_heading_maps = "MAPS";
inline constexpr std::string_view active_heading_keys = "KEYS";
inline constexpr std::string_view active_change = "Change…";
inline constexpr std::string_view active_add_from = "{name} · Add from…";
inline constexpr std::string_view active_files_match = "All files match";
inline constexpr std::string_view active_file_differs = "{count} file differs";
inline constexpr std::string_view active_files_differ = "{count} files differ";
/// Music the source did not finish. `{held}` and `{total}` are counts.
inline constexpr std::string_view music_tracks = "{held} of {total} tracks";
inline constexpr std::string_view active_version_source = "{version} · {source}";
inline constexpr std::string_view active_turns_off = "{name} turns off when the Library closes.";
inline constexpr std::string_view active_turns_on = "{name} turns on when the Library closes.";
inline constexpr std::string_view active_needs = "{name} needs {need}.";
inline constexpr std::string_view active_as_made = "{game} as it was made";
inline constexpr std::string_view active_no_expansions = "This game has no expansions.";
inline constexpr std::string_view active_game_name = "Total Annihilation";
inline constexpr std::string_view active_version = "3.1c";
inline constexpr std::string_view active_version_demo = "Demo";

// The base game chooser, over the Library. CANCEL and "In use" are the
// words the rest of the Library already uses.
/// The chooser's title.
inline constexpr std::string_view base_title = "Base game";
/// The line under the title.
inline constexpr std::string_view base_lead =
    "The game OA plays. Mods, maps and languages say which base games they work with.";
/// A base game that is not copied.
inline constexpr std::string_view base_not_added = "Not added";
/// A copied base game that is not the one in use.
inline constexpr std::string_view base_copied = "Copied";
/// The demo, as the chooser names it.
inline constexpr std::string_view base_demo_name = "Total Annihilation demo";
/// The demo's version on the chooser.
inline constexpr std::string_view base_demo_version = "1997";
/// The demo while it is not copied.
inline constexpr std::string_view base_demo_detail =
    "Choosing it asks for the demo installer, then copies it. It takes no expansions.";
/// A base game that is not the demo, while it is not copied.
inline constexpr std::string_view base_ask_game = "Choosing it asks for the game, then copies it.";
/// Where a copied base game came from.
inline constexpr std::string_view base_copied_from = "Copied from {source}";
/// Shown when another base game is marked and a mod is being played.
inline constexpr std::string_view base_note_mod =
    "Choosing another base game turns on all of its expansions and unloads the mod you are playing "
    "({mod}). The change applies when the Library closes.";
/// Shown when another base game is marked and no mod is being played.
inline constexpr std::string_view base_note_plain =
    "Choosing another base game turns on all of its expansions. The change applies when the "
    "Library closes.";
/// The chooser's accent button.
inline constexpr std::string_view base_use = "Use this game";
/// The Active tab, after another base game was chosen.
inline constexpr std::string_view base_applies = "This base game is used when the Library closes.";
/// A copy the host could not finish.
inline constexpr std::string_view base_copy_failed = "The game could not be copied.";
/// Beside MAPS while the maps search holds text: how many tiles show, of every installed map.
inline constexpr std::string_view active_maps_count = "{shown} of {all}";
/// The mod being played, or the language pack in use: its name, then its version.
inline constexpr std::string_view active_package = "{name} {version}";
/// A language no pack holds, which the game shows from its own words.
inline constexpr std::string_view active_language_built_in = "{language} · built in";
/// A language pack's coverage, as its tile states it.
inline constexpr std::string_view active_coverage = "Coverage {percent}%";
/// The language the game shows with no language chosen.
inline constexpr std::string_view language_english = "English";
/// A built-in language's details, under its name: it is not a package.
inline constexpr std::string_view status_built_in_language = "Built into the game";
/// How many players a map is for: one, and every other count.
inline constexpr std::string_view active_players_one = "{count} player";
inline constexpr std::string_view active_players_many = "{count} players";
/// One map of an installed map pack, as automation reads its line.
inline constexpr std::string_view active_map = "{map} · {players} · {size}";
/// A map's players and size under its title: "4 players · 16x16".
inline constexpr std::string_view map_players_size = "{players} · {size}";
/// The link on an Active map's tile that opens its pack on the Maps tab.
inline constexpr std::string_view active_from_pack = "From {pack}";
inline constexpr std::string_view active_no_maps = "No map packs are installed.";
/// The maps pane while a search matches none of the maps that are installed.
inline constexpr std::string_view active_no_map_match = "No installed map matches.";

// A row: the version and size under the name at Compact.
inline constexpr std::string_view row_version_size = "{version} · {size}";
/// The line above, from a registry the player added.
inline constexpr std::string_view row_from_registry = "{line} · {registry}";

// An empty list, and a list the last fetch could not bring up to date.
inline constexpr std::string_view empty_matches = "Nothing matches \"{query}\".";
inline constexpr std::string_view empty_updates = "No updates.";
inline constexpr std::string_view empty_no_registries =
    "Nothing here yet. Turn a registry on in Settings › Downloads.";
inline constexpr std::string_view empty_tab = "Nothing to show.";
inline constexpr std::string_view offline_list =
    "Showing the list from {age}. OA couldn't reach the registries.";

// The details.
/// After a fact's value that is met.
inline constexpr std::string_view fact_met = "✓";

// The question asked before an update that may stop saved games loading
// (update_question.hpp).
inline constexpr std::string_view question_title = "Update {name}";
inline constexpr std::string_view question_title_all = "Update all";
/// The heading of an update to a new revision of the same version.
inline constexpr std::string_view question_heading_revision =
    "{name} {version} · revision {from} → {to}";
/// The heading of an update to a new version.
inline constexpr std::string_view question_heading_version = "{name} {from} → {to}";
/// The heading of an update that keeps the version and names no revision
/// above 0 on one side.
inline constexpr std::string_view question_heading_same = "{name} {version}";
/// `{to}` and `{from}` are change_texts' ("revision 4", or the version
/// "4.9"); `{To}` and `{From}` are the same with a capital first letter.
inline constexpr std::string_view question_changes =
    "{To} changes the game's rules. Saved games from {from} won't load under {to}. {From} is "
    "kept, and Roll back brings it back.";
inline constexpr std::string_view question_unknown =
    "OA can't tell whether {to} changes the game's rules. If it does, saved games from {from} "
    "won't load under it. {From} is kept, and Roll back brings it back.";
/// After the body, for the mod being played.
inline constexpr std::string_view question_finishes = "It finishes when you leave {name}.";
/// UPDATE ALL's body for one update whose rules change, and for several.
inline constexpr std::string_view question_all_one =
    "1 update changes the game's rules: {names}. Saved games from its current version won't "
    "load under the new one. It is kept, and Roll back brings it back.";
inline constexpr std::string_view question_all_many =
    "{count} updates change the game's rules: {names}. Saved games from their current versions "
    "won't load under the new ones. Each is kept, and Roll back brings it back.";
/// The names UPDATE ALL's body lists, then how many more it does not.
inline constexpr std::string_view question_names_more = "{names} and {count} more";
/// The question's Rules fact: the rules hashes before and after.
inline constexpr std::string_view question_rules = "{from} → {to}";
inline constexpr std::string_view question_rules_unknown = "unknown";
inline constexpr std::string_view fact_download = "Download";
/// One fact of the question, as a line of its text.
inline constexpr std::string_view question_fact = "{label}: {value}";
inline constexpr std::string_view action_not_now = "Not now";

// The questions about the mod played (mod_questions.hpp): SWITCH OFF's
// Switch Mod question, the note that PLAY NOW could not switch, and ROLL
// BACK's Roll Back Mod question.
inline constexpr std::string_view switch_title = "SWITCH MOD";
/// What the Switch Mod question asks, with the mod's title in its place.
inline constexpr std::string_view switch_ask =
    "Switch to {title} now? The game reloads its data for the new mod and returns to the main "
    "menu. Your other settings are kept.";
/// The title the Switch Mod question gives the game played with no mod.
inline constexpr std::string_view no_mod = "No Mod";
inline constexpr std::string_view expansion_off_title = "Turn off {name}?";
inline constexpr std::string_view expansion_off_too = "{name} turns off too: it needs {need}.";
inline constexpr std::string_view expansion_off_mod =
    "{mod} needs {name}, so it is unloaded and the game plays with No Mod.";
inline constexpr std::string_view expansion_off_maps =
    "{name}'s maps and campaigns leave the lists until you turn it on again. Nothing is deleted.";
inline constexpr std::string_view action_turn_off = "Turn off";
inline constexpr std::string_view action_switch = "Switch";
inline constexpr std::string_view action_ok = "OK";
inline constexpr std::string_view roll_back_title = "ROLL BACK MOD";
/// What the Roll Back Mod question asks, with the mod's title and the versions.
inline constexpr std::string_view roll_back_ask =
    "Roll back {title} to {to}? {from} is kept in its place, and Roll back again brings it back.";
/// What the Roll Back Mod question adds for the mod the game plays.
inline constexpr std::string_view roll_back_reload =
    "The game reloads its data for the mod and returns to the main menu.";

// What a package needs, on its row and in its details, against the choice
// the Active tab is holding. Product names stay in the values filled in.
inline constexpr std::string_view fact_games = "Games";
inline constexpr std::string_view names_and = "{left} and {right}";
inline constexpr std::string_view names_comma = "{left}, {right}";
inline constexpr std::string_view names_or = "{left} or {right}";
inline constexpr std::string_view fact_join = "{left} · {right}";
inline constexpr std::string_view name_met = "{name} ✓";
inline constexpr std::string_view name_off = "{name} off";
inline constexpr std::string_view needs_which_off = "Needs {name}, which is off";
inline constexpr std::string_view needs_which_are_off = "Needs {names}, which are off";
inline constexpr std::string_view needs_which_on = "Needs {name} · on";
inline constexpr std::string_view needs_which_are_on = "Needs {names} · on";
inline constexpr std::string_view for_demo_only = "For the Total Annihilation demo only";
inline constexpr std::string_view for_game_only = "For {name} only";
inline constexpr std::string_view action_turn_on = "Turn on {name}";
inline constexpr std::string_view turn_on_then_play =
    "Turning it on applies when the Library closes, and then Play now works.";
inline constexpr std::string_view turn_on_then_use =
    "Turning it on applies when the Library closes, and then Use now works.";
inline constexpr std::string_view turn_on_several_play =
    "Turning them on applies when the Library closes, and then Play now works.";
inline constexpr std::string_view turn_on_several_use =
    "Turning them on applies when the Library closes, and then Use now works.";
inline constexpr std::string_view turn_on_applies =
    "Turning it on applies when the Library closes.";
inline constexpr std::string_view turn_on_several_applies =
    "Turning them on applies when the Library closes.";
inline constexpr std::string_view mod_unloaded_because =
    "{mod} was unloaded because {name} is off. The game plays with No Mod.";
inline constexpr std::string_view mod_unloaded_note =
    "The mod was unloaded because an expansion it needs is off. The game plays with No Mod.";
/// Under the expansion pills, after the padlock, while the command line chose
/// the mod being played: the expansions it needs, whose pills are locked.
inline constexpr std::string_view active_command_line_needs =
    "The command line chose this run's mod, which needs {names}.";

// The Replays section.
inline constexpr std::string_view tab_replays = "Replays";
/// The Replays section's filters.
inline constexpr std::string_view replay_filter_this_month = "This month";
inline constexpr std::string_view replay_filter_older = "Older";
inline constexpr std::string_view replay_filter_kept = "Kept";
inline constexpr std::string_view replay_filter_imported = "Imported";
/// The kind menu: every kind, and the engine's own three.
inline constexpr std::string_view replay_kind_all = "All kinds";
inline constexpr std::string_view replay_kind_multiplayer = "Multiplayer";
inline constexpr std::string_view replay_kind_skirmish = "Skirmish";
inline constexpr std::string_view replay_kind_campaign = "Campaign";
/// The sort menu.
inline constexpr std::string_view replay_sort_newest = "Newest first";
inline constexpr std::string_view replay_sort_longest = "Longest first";
inline constexpr std::string_view replay_sort_map = "By map";
inline constexpr std::string_view replay_sort_players = "Most players first";
inline constexpr std::string_view search_replays = "Search replays";
/// The days the list is grouped by. `{weekday}`, `{day}`, `{month}` and
/// `{year}` are the words and digits below.
inline constexpr std::string_view replay_today = "Today";
inline constexpr std::string_view replay_yesterday = "Yesterday";
inline constexpr std::string_view replay_day = "{weekday} {day} {month}";
inline constexpr std::string_view replay_day_year = "{weekday} {day} {month} {year}";
inline constexpr std::string_view weekday_monday = "Mon";
inline constexpr std::string_view weekday_tuesday = "Tue";
inline constexpr std::string_view weekday_wednesday = "Wed";
inline constexpr std::string_view weekday_thursday = "Thu";
inline constexpr std::string_view weekday_friday = "Fri";
inline constexpr std::string_view weekday_saturday = "Sat";
inline constexpr std::string_view weekday_sunday = "Sun";
inline constexpr std::string_view month_january = "Jan";
inline constexpr std::string_view month_february = "Feb";
inline constexpr std::string_view month_march = "Mar";
inline constexpr std::string_view month_april = "Apr";
inline constexpr std::string_view month_may = "May";
inline constexpr std::string_view month_june = "Jun";
inline constexpr std::string_view month_july = "Jul";
inline constexpr std::string_view month_august = "Aug";
inline constexpr std::string_view month_september = "Sep";
inline constexpr std::string_view month_october = "Oct";
inline constexpr std::string_view month_november = "Nov";
inline constexpr std::string_view month_december = "Dec";
/// Two or three players on a row: "Varn vs Kestrel".
inline constexpr std::string_view replay_versus = "{left} vs {right}";
/// More than three players on a row, as a map's players read.
inline constexpr std::string_view replay_players_many = active_players_many;
/// A kind and the planet its battle was on: "Outpost · Corvane".
inline constexpr std::string_view replay_kind_planet = "{kind} · {planet}";
/// The card's subtitle with the sector as well: "Outpost · Corvane, Low Reach".
inline constexpr std::string_view replay_kind_place = "{kind} · {planet}, {sector}";
/// A row's parts, joined in its reading and in the card's lines.
inline constexpr std::string_view replay_joined = "{left} · {right}";
/// The chip on a replay finished after the game stopped.
inline constexpr std::string_view replay_after_crash = "After a crash";
/// The grey chip, and the row's reading, while the index pass has not read it.
inline constexpr std::string_view replay_reading_now = "Reading…";
/// The recording machine's player won.
inline constexpr std::string_view replay_won = "Won";
/// The recording machine's player lost.
inline constexpr std::string_view replay_lost = "Lost";
/// One unit destroyed, on the card.
inline constexpr std::string_view replay_kill_one = "1 kill";
/// Units destroyed, on the card. `{kills}` is a whole number other than one.
inline constexpr std::string_view replay_kills_many = "{kills} kills";
/// One unit lost, on the card.
inline constexpr std::string_view replay_loss_one = "1 loss";
/// Units lost, on the card. `{losses}` is a whole number other than one.
inline constexpr std::string_view replay_losses_many = "{losses} losses";
/// Under the card's buttons, once a Watch was left part way. `{time}` is the
/// time into the replay, as the length is written.
inline constexpr std::string_view replay_watched_to = "Watched to {time}";
/// The chip on a replay whose file does not read.
inline constexpr std::string_view replay_damaged = "Damaged";
/// A kept replay's star, as its row reads.
inline constexpr std::string_view replay_kept_mark = "Kept";
/// The details card's facts.
inline constexpr std::string_view replay_fact_length = "Length";
inline constexpr std::string_view replay_fact_recorded = "Recorded";
inline constexpr std::string_view replay_fact_mod = "Mod";
inline constexpr std::string_view replay_fact_expansions = "Expansions";
inline constexpr std::string_view replay_fact_mutators = "Mutators";
inline constexpr std::string_view replay_fact_file = "File";
inline constexpr std::string_view replay_fact_kept_until = "Kept until";
inline constexpr std::string_view replay_fact_kept = "Kept";
/// Kept's value: the player keeps it.
inline constexpr std::string_view replay_kept_always = "Until you delete it";
/// Kept's value on a replay opened from outside.
inline constexpr std::string_view replay_kept_imported = "Imported replays are kept";
/// Kept's value on a replay downloaded from Core Prime.
inline constexpr std::string_view replay_kept_downloaded = "Downloaded replays are kept";
/// Kept's value on a replay the player exported.
inline constexpr std::string_view replay_kept_exported = "Exported replays are kept";
/// A fact with nothing to name: no mod, no expansion, no mutator.
inline constexpr std::string_view replay_none = "None";
/// Who recorded a replay, as the card names the program: "Open Annihilation
/// 0.8.14". A `.oademo` holds the program's identifier; the card does not show it.
inline constexpr std::string_view replay_program = "Open Annihilation {version}";
/// A mod with its revision: "Ridge 2.8 (rev 1)".
inline constexpr std::string_view replay_mod_revision = "{mod} (rev {revision})";
/// The players' sides on the card.
inline constexpr std::string_view replay_side_arm = "Arm";
inline constexpr std::string_view replay_side_core = "Core";
inline constexpr std::string_view replay_side_watcher = "Watcher";
/// The card's buttons.
inline constexpr std::string_view action_watch = "Watch";
/// The card's button that continues a Watch. `{time}` is where it was left.
inline constexpr std::string_view action_watch_from = "Watch from {time}";
inline constexpr std::string_view action_keep = "Keep";
inline constexpr std::string_view action_kept = "Kept";
inline constexpr std::string_view action_turn_on_recording = "Turn on recording";
/// The empty section's button, and the Imported filter's while it shows none.
inline constexpr std::string_view action_open_replay_file = "Open a replay file…";
/// Delete on the card. The word Delete is the Delete key, so this one is its own.
inline constexpr std::string_view action_delete_replay = "Delete replay";
/// The delete question's title for the one replay the card shows.
inline constexpr std::string_view replay_delete_title = "Delete this replay?";
/// The delete question's title when several replays are marked. `{count}` is how many.
inline constexpr std::string_view replay_delete_title_many = "Delete these {count} replays?";
/// The delete question's line where the system has a trash.
inline constexpr std::string_view replay_delete_to_trash = "It goes to the system's trash.";
inline constexpr std::string_view replay_delete_to_trash_many = "They go to the system's trash.";
/// The delete question's line where the system has no trash.
inline constexpr std::string_view replay_delete_for_good =
    "It is deleted and cannot be undone. This system has no trash.";
inline constexpr std::string_view replay_delete_for_good_many =
    "They are deleted and cannot be undone. This system has no trash.";
/// The delete question's button that deletes several replays; one replay's
/// is action_delete_replay.
inline constexpr std::string_view action_delete_replays = "Delete replays";
/// The card while several replays are marked: Keep acts on all of them.
inline constexpr std::string_view replays_marked = "{count} replays selected";
/// The card on a damaged replay, above the reason.
inline constexpr std::string_view replay_cannot_watch = "This replay cannot be watched.";
/// The card when Delete could not move the one replay it asked about.
inline constexpr std::string_view replay_trash_refused =
    "This replay could not be moved to the trash.";
/// The card when Delete could not move several marked replays. `{count}` is how many stayed.
inline constexpr std::string_view replay_trash_refused_many =
    "{count} replays could not be moved to the trash.";
/// The empty section.
inline constexpr std::string_view replays_none_yet = "No replays yet.";
inline constexpr std::string_view replays_kept_days = "Games you play are kept here for 30 days.";
inline constexpr std::string_view replays_kept_90_days =
    "Games you play are kept here for 90 days.";
inline constexpr std::string_view replays_kept_year = "Games you play are kept here for a year.";
inline constexpr std::string_view replays_kept_forever =
    "Games you play are kept here until you delete them.";
/// The question the first time old replays would go.
inline constexpr std::string_view replay_retention_title = "Old replays";
inline constexpr std::string_view replay_retention_keep_all = "Keep them all";
inline constexpr std::string_view replay_retention_let_go = "Let them go";
inline constexpr std::string_view replay_retention_one =
    "1 replay is older than you asked to keep. It goes to the system's trash.";
inline constexpr std::string_view replay_retention_many =
    "{count} replays are older than you asked to keep. They go to the system's trash.";
inline constexpr std::string_view replay_retention_one_removed =
    "1 replay is older than you asked to keep. It is deleted. This system has no trash.";
inline constexpr std::string_view replay_retention_many_removed =
    "{count} replays are older than you asked to keep. They are deleted. This system has no trash.";
inline constexpr std::string_view replays_recording_off = "Recording is off.";
inline constexpr std::string_view replays_none_this_month = "No replays from this month.";
inline constexpr std::string_view replays_none_older = "No older replays.";
inline constexpr std::string_view replays_none_kept = "No replays kept.";
inline constexpr std::string_view replays_none_imported = "No replays imported.";
inline constexpr std::string_view replays_none_kind = "No replays of this kind.";
/// The section and the footer while the folder is read and its files are not
/// yet counted: the section shows it in place of "No replays yet.".
inline constexpr std::string_view replays_reading_folder = "Reading the replays…";
/// The footer while the folder is read, and again while the index pass reads
/// the replays that folder listed. `{read}` is how many are done, `{total}`
/// how many there are.
inline constexpr std::string_view replays_reading = "Reading {read} of {total} replays";
/// The footer otherwise: how many the list shows, and their size.
inline constexpr std::string_view replays_count_one = "1 replay · {size}";
inline constexpr std::string_view replays_count_many = "{count} replays · {size}";
/// The amber chip on a replay that needs a mod.
inline constexpr std::string_view replay_needs_release = "Needs {name} {version}";
/// The amber chip on a replay that needs a map pack.
inline constexpr std::string_view replay_needs_map = "Needs map";
/// The amber chip on a replay that needs a mutator.
inline constexpr std::string_view replay_needs_mutator = "Needs mutator";
/// The amber chip on a replay that needs an expansion. The same words name
/// an expansion elsewhere. The chip has no full stop.
inline constexpr std::string_view replay_needs_name = "Needs {name}";
/// The card's sentence for an expansion. The chip keeps the words without the stop.
inline constexpr std::string_view replay_needs_name_sentence = "Needs {name}.";
/// The amber chip on a replay this build is too old for.
inline constexpr std::string_view replay_needs_version = "Needs {version}";
/// The card when the mod is not installed and the note names no version.
inline constexpr std::string_view replay_recorded_with_name =
    "This replay was recorded with {name}.";
/// The card when the mod is not installed.
inline constexpr std::string_view replay_recorded_with =
    "This replay was recorded with {name} {version}.";
/// The card when that recording names a revision.
inline constexpr std::string_view replay_recorded_with_rev =
    "This replay was recorded with {name} {version} (rev {revision}).";
/// The card when another release is installed and the note names no version.
inline constexpr std::string_view replay_recorded_have_name =
    "This replay was recorded with {name}. You have {have}.";
/// The card when another release of that mod is installed.
inline constexpr std::string_view replay_recorded_have =
    "This replay was recorded with {name} {version}. You have {have}.";
/// The card when that recording names a revision and another release is installed.
inline constexpr std::string_view replay_recorded_have_rev =
    "This replay was recorded with {name} {version} (rev {revision}). You have {have}.";
/// The card when a map from a pack is not installed.
inline constexpr std::string_view replay_needs_the_map = "Needs the map {name}.";
/// The pack that map comes from.
inline constexpr std::string_view replay_from_pack = "From the map pack {pack}.";
/// The card when another release of that map pack is installed.
inline constexpr std::string_view replay_other_map_release =
    "This replay was recorded with another release of the map pack {name}.";
/// The card when a mutator is not installed.
inline constexpr std::string_view replay_needs_the_mutator = "Needs the mutator {name}.";
/// The card when this build is older than the replay needs.
inline constexpr std::string_view replay_needs_program = "Needs Open Annihilation {version}.";
/// The card while a package for this replay is being fetched.
inline constexpr std::string_view replay_getting = "Getting {name}… {percent}%";
/// Watch when one mod has to be fetched.
inline constexpr std::string_view replay_get_release = "Get {name} {version} and watch";
/// Watch when one map, mutator or expansion has to be fetched.
inline constexpr std::string_view replay_get_it = "Get it and watch";
/// Watch when more than one package has to be fetched.
inline constexpr std::string_view replay_get_them = "Get them and watch";
/// Watch when an expansion is installed and off.
inline constexpr std::string_view replay_turn_on = "Turn on and watch";
/// The same words as Settings. Listed here because the card's button says them.
inline constexpr std::string_view replay_check_updates = "Check for updates";
/// The card when the package is not in the catalogue that was read.
/// `{registry}` is Core Prime, or the registry's own name.
inline constexpr std::string_view replay_not_in_catalogue =
    "{name} {version} is not in {registry}'s catalogue.";
/// The same sentence when the note names no version.
inline constexpr std::string_view replay_not_in_catalogue_name =
    "{name} is not in {registry}'s catalogue.";
/// The switch above the Replays list.
inline constexpr std::string_view core_on_computer = "On this computer";
inline constexpr std::string_view core_on_prime = "On Core Prime";
/// Core Prime's filters. Latest is fact_latest.
inline constexpr std::string_view core_longest = "Longest";
inline constexpr std::string_view core_most_downloaded = "Popular";
inline constexpr std::string_view core_by_planet = "By planet";
/// The empty section, when Recordings has no replay.
inline constexpr std::string_view core_browse = "Browse Core Prime's replays";
/// Core Prime's list, while it is read and no row has come.
inline constexpr std::string_view core_reading = "Reading Core Prime's replays…";
/// The list when the answer could not be read.
inline constexpr std::string_view core_list_unreadable = "Core Prime's list cannot be read.";
/// The list when Core Prime says its replays are not available.
inline constexpr std::string_view core_replays_unavailable =
    "Core Prime's replays are not available just now.";
/// The list when Core Prime could not be reached.
inline constexpr std::string_view core_offline = "Core Prime cannot be reached.";
/// The list when the filter matches nothing.
inline constexpr std::string_view core_none = "Core Prime has no replays to show.";
inline constexpr std::string_view core_none_planet = "No replays for this planet.";
/// The footer.
inline constexpr std::string_view core_count_one = "1 replay";
inline constexpr std::string_view core_count_many = "{count} replays";
/// DOWNLOAD while the file is coming.
inline constexpr std::string_view core_getting_replay = "Getting the replay…";
/// The card when the file's SHA-256 is not the one the list named.
inline constexpr std::string_view core_file_differs = "The file does not match the replay.";
/// DOWNLOAD once the file is in Recordings. The button is disabled.
inline constexpr std::string_view core_downloaded = "Downloaded";
/// The card when the list names no `.oademo` to fetch.
inline constexpr std::string_view core_replay_no_file = "The replay names no file.";
/// The card when the file could not be written.
inline constexpr std::string_view core_replay_not_saved = "The replay could not be saved.";
/// The card while a 429 asks the player to wait. `{seconds}` is the wait.
inline constexpr std::string_view core_try_again = "Try again in {seconds} s.";
/// The card when a check expired before it was passed.
inline constexpr std::string_view core_check_expired =
    "The check ran out. Try again for a new code.";
/// The card while a check is waiting. `{address}` is the page, `{code}` the code.
inline constexpr std::string_view core_enter_code = "Go to {address} and enter {code}.";
/// The card when the registry could not be reached.
inline constexpr std::string_view replay_offline = "You're offline.";
/// The card's second offline sentence.
inline constexpr std::string_view replay_offline_get =
    "Get {name} {version} when you're back online.";
/// The same sentence when the note names no version.
inline constexpr std::string_view replay_offline_get_name = "Get {name} when you're back online.";
/// EXPORT… on the card.
inline constexpr std::string_view action_export = "Export…";
/// The dialog's confirm, and its title when nothing is selected.
inline constexpr std::string_view export_confirm = "Export";
/// The dialog's title when one replay is chosen. `{name}` is its title.
inline constexpr std::string_view export_title = "Export {name}";
/// The dialog's title when several replays are chosen.
inline constexpr std::string_view export_count = "Export {count} replays";
/// The `.oademo` choice. The extension is part of the name.
inline constexpr std::string_view export_oademo = "Open Annihilation replay (.oademo)";
/// The mark beside that choice.
inline constexpr std::string_view export_recommended = "Recommended";
/// What a `.oademo` holds.
inline constexpr std::string_view export_oademo_about =
    "Everything needed to watch it: the recording, and the game, mod and map it needs";
/// The `.tad` choice. The product name stays, and the extension is part of it.
inline constexpr std::string_view export_tad = "TA Demo Recorder (.tad)";
/// What a `.tad` is for.
inline constexpr std::string_view export_tad_about = "For TA Demo Recorder and other replayers";
/// The heading over the sectors box.
inline constexpr std::string_view export_include = "What to include";
/// The sectors box. On by default, and only for a `.tad`.
inline constexpr std::string_view export_oa_sectors = "What only Open Annihilation reads";
/// The heading over the warnings. Only for a `.tad`.
inline constexpr std::string_view export_compatibility = "Compatibility";
/// A named mutator.
inline constexpr std::string_view export_mutator =
    "It was played with the mutator {name}. Other replayers cannot apply it, so commanders may not "
    "last as they did.";
/// Mutators the recording does not name.
inline constexpr std::string_view export_mutators =
    "It was played with mutators. Other replayers cannot apply them, so commanders may not last as "
    "they did.";
/// A wide weapon record.
inline constexpr std::string_view export_wide =
    "Other replayers stop at the first of this mod's wide weapon records.";
/// A name cut to 63 bytes.
inline constexpr std::string_view export_names = "Names longer than 63 letters are cut.";
/// A status datagram cut to 512 bytes.
inline constexpr std::string_view export_status = "A player's status is cut to 512 bytes.";
/// While the files are written, when there are several.
inline constexpr std::string_view export_progress = "Exporting {done} of {total}";
/// While one recording is compressed. The size follows, as `progress_text` writes it.
inline constexpr std::string_view export_compressing = "Compressing the recording";
/// While one file is copied into place. `{name}` is the file, extension included.
/// The size follows, as `progress_text` writes it.
inline constexpr std::string_view export_writing = "Writing {name}";
/// A size that is not known yet.
inline constexpr std::string_view export_measuring = "Measuring…";
/// After they are written. `{folder}` is the folder.
inline constexpr std::string_view export_saved = "Saved to {folder}";
/// SHOW IN FOLDER, after the files are written.
inline constexpr std::string_view export_show = "Show in folder";
/// The dialog with nothing selected.
inline constexpr std::string_view export_none =
    "Nothing is selected. Choose a replay, then export it.";
/// A file that could not be written, when the reason is not one named below.
inline constexpr std::string_view export_failed = "The export could not be written.";
/// The folder has no room left. `{folder}` is the folder the dialog shows.
inline constexpr std::string_view export_no_room = "There is no room in {folder}.";
/// The folder refused the write. `{folder}` is the folder the dialog shows.
inline constexpr std::string_view export_cannot_write =
    "Open Annihilation cannot write to {folder}.";
/// Some files were written before one could not be. `{done}` and `{total}` are counts.
inline constexpr std::string_view export_saved_some = "Saved {done} of {total}.";
/// A replay that could not be read, so it is not offered as a write that failed.
inline constexpr std::string_view export_unreadable =
    "This replay cannot be read, so it cannot be exported.";
/// The title of the system's folder dialog.
inline constexpr std::string_view export_choose_folder = "Choose a folder";
/// The chooser's next page of folders.
inline constexpr std::string_view export_more_folders = "More folders";
/// The chooser's previous page of folders.
inline constexpr std::string_view export_previous_folders = "Previous folders";
/// The next page of compatibility warnings.
inline constexpr std::string_view export_more_warnings = "More warnings";
/// The previous page of compatibility warnings.
inline constexpr std::string_view export_previous_warnings = "Previous warnings";
/// Words already in the catalogue, looked up here and not listed again.
inline constexpr std::string_view export_choose = "Choose…";
inline constexpr std::string_view export_downloads = "Downloads";
inline constexpr std::string_view export_up = "Up";
inline constexpr std::string_view export_use_folder = "Use this folder";

/// SIGN IN beside the source switch, and in the question.
inline constexpr std::string_view action_sign_in = "Sign in";
/// SIGN OUT beside the source switch.
inline constexpr std::string_view action_sign_out = "Sign out";
/// The sign-in question's title.
inline constexpr std::string_view sign_in_title = "Sign in to Core Prime";
/// The handle field.
inline constexpr std::string_view sign_in_handle = "Handle";
/// The password field.
inline constexpr std::string_view sign_in_password = "Password";
/// The line under the fields, until a sentence replaces it.
inline constexpr std::string_view sign_in_forgot = "Forgot it? Reset it at coreprime.net/forgot";
/// The line under the source switch when this system cannot send a password over TLS.
inline constexpr std::string_view sign_in_needs_secure =
    "Signing in needs a secure connection, which this system does not have.";
/// SHARE ON CORE PRIME… on the card.
inline constexpr std::string_view action_share = "Share on Core Prime…";
/// SHARE MY OWN RECORDING, once Core Prime already lists the battle.
inline constexpr std::string_view action_share_own = "Share my own recording";
/// OPEN ON CORE PRIME.
inline constexpr std::string_view action_open_shared = "Open on Core Prime";
/// SHARE in the question.
inline constexpr std::string_view share_confirm = "Share";
/// The share question's title.
inline constexpr std::string_view share_title = "Share on Core Prime";
/// The title field.
inline constexpr std::string_view share_title_label = "Title";
/// Who can see a share, offered first.
inline constexpr std::string_view share_public = "Public";
/// Who can see a share the player does not list.
inline constexpr std::string_view share_unlisted = "Unlisted";
/// My battles, Core Prime's filter for the signed-in player.
inline constexpr std::string_view core_my_battles = "My battles";
/// What the player sees when a share is accepted.
inline constexpr std::string_view share_shared = oa::data::replays::shared_on_core_prime;
/// What the player sees when that replay is already there.
inline constexpr std::string_view share_already = oa::data::replays::already_on_core_prime;
/// What a signed-out share is told.
inline constexpr std::string_view share_sign_in_again = oa::data::replays::sign_in_to_share;
/// What an account that cannot share is told.
inline constexpr std::string_view share_account = oa::data::replays::account_cannot_share;
/// What a player who was not in the battle is told.
inline constexpr std::string_view share_not_in_battle =
    oa::data::replays::only_a_player_can_replace;
/// What a file past the limit is told.
inline constexpr std::string_view share_too_large = oa::data::replays::replay_larger_than_the_limit;
/// What an account that has shared enough for one day is told.
inline constexpr std::string_view share_today = oa::data::replays::shared_for_today;
/// What the player sees when uploads are off.
inline constexpr std::string_view share_uploads_off = oa::data::replays::uploads_not_available;
/// What the player sees when a login is accepted and names no session.
inline constexpr std::string_view sign_in_no_session = oa::data::replays::session_not_started;
/// What the player sees when a share's answer is empty or not the error object.
inline constexpr std::string_view share_no_answer = oa::data::replays::share_not_answered;
/// The sign-in question while the login is in flight.
inline constexpr std::string_view sign_in_busy = "Signing in…";
/// The share question while the upload is in flight.
inline constexpr std::string_view share_busy = "Sharing…";

} // namespace words

/// One place of a pattern and its value.
using Place = std::pair<std::string_view, std::string_view>;

/// Looks a text up in the language shown.
///
/// @param text how texts are looked up
/// @param english the text, in English as text.hpp writes it
/// @return the text to show; the English when the hooks give no lookup
[[nodiscard]] std::string_view shown(const TextHooks& text, std::string_view english);

/// Returns the label a mutator's catalogue tag id shows.
///
/// The eight ids give their words, looked up. Any other id, including a
/// different case of one of those, is returned as written. The id `maps`
/// uses the Maps tab's word. The screen uses this for a mutator's chips
/// and detail pills, and for the Mutators tab's filter pills, pane and
/// menu. A mod's or a map pack's tag is shown as the id was written.
///
/// @param tag the catalogue id
/// @param text how texts are looked up; empty shows the English label
/// @return the label; a view of `tag` when the id is not one of the eight
[[nodiscard]] inline std::string_view tag_label(std::string_view tag, const TextHooks& text = {}) {
    std::string_view english;
    if (tag == "statistics")
        english = words::tag_statistics;
    else if (tag == "economy")
        english = words::tag_economy;
    else if (tag == "commanders")
        english = words::tag_commanders;
    else if (tag == "new-units")
        english = words::tag_new_units;
    else if (tag == "disable")
        english = words::tag_disable;
    else if (tag == "gameplay")
        english = words::tag_gameplay;
    else if (tag == "ai")
        english = words::tag_ai;
    else if (tag == "maps")
        english = words::tab_maps;
    else
        return tag;
    return shown(text, english);
}

/// Returns the label a brain's tag id shows.
///
/// The nine ids give their words, looked up. Any other id, including a
/// different case of one of those, is returned as written. Economy uses the
/// mutators' word. The Brains tab's chips, pills, pane and menu use this.
/// A brain's tag is not given a mutator's icon.
///
/// @param tag the catalogue id
/// @param text how texts are looked up; empty shows the English label
/// @return the label; a view of `tag` when the id is not one of the nine
[[nodiscard]] inline std::string_view
brain_tag_label(std::string_view tag, const TextHooks& text = {}) {
    if (tag == "classic")
        return shown(text, words::tag_classic);
    if (tag == "economy")
        return shown(text, words::tag_economy);
    if (tag == "rush")
        return shown(text, words::tag_rush);
    if (tag == "turtle")
        return shown(text, words::tag_turtle);
    if (tag == "air")
        return shown(text, words::tag_air);
    if (tag == "naval")
        return shown(text, words::tag_naval);
    if (tag == "learned")
        return shown(text, words::tag_learned);
    if (tag == "teaching")
        return shown(text, words::tag_teaching);
    if (tag == "experimental")
        return shown(text, words::tag_experimental);
    return tag;
}

/// The ten key-layout tags, in the standard's order. Any other tag is shown as written.
inline constexpr std::array<std::string_view, 10> key_tag_ids{
    "classic",
    "grid",
    "left-hand",
    "right-hand",
    "one-hand",
    "mouse-first",
    "accessibility",
    "tournament",
    "mod",
    "legacy",
};

/// Returns the label a key layout's tag id shows.
///
/// The ten ids give their words, looked up. Any other id, including a
/// different case of one of those, is returned as written. Classic uses the
/// brains' word. The Keys tab's chips, pills, pane and menu use this.
///
/// @param tag the catalogue id
/// @param text how texts are looked up; empty shows the English label
/// @return the label; a view of `tag` when the id is not one of the ten
[[nodiscard]] inline std::string_view
key_tag_label(std::string_view tag, const TextHooks& text = {}) {
    if (tag == "classic")
        return shown(text, words::tag_classic);
    if (tag == "grid")
        return shown(text, words::tag_grid);
    if (tag == "left-hand")
        return shown(text, words::tag_left_hand);
    if (tag == "right-hand")
        return shown(text, words::tag_right_hand);
    if (tag == "one-hand")
        return shown(text, words::tag_one_hand);
    if (tag == "mouse-first")
        return shown(text, words::tag_mouse_first);
    if (tag == "accessibility")
        return shown(text, words::tag_accessibility);
    if (tag == "tournament")
        return shown(text, words::tag_tournament);
    if (tag == "mod")
        return shown(text, words::tag_mod);
    if (tag == "legacy")
        return shown(text, words::tag_legacy);
    return tag;
}

/// Looks a pattern up whole and fills its places.
///
/// A place no value names stays as it is written. Values are not looked up.
///
/// @param text how texts are looked up
/// @param pattern the pattern, in English, with places written {name}
/// @param places each place's name and value
/// @return the text
[[nodiscard]] std::string
filled(const TextHooks& text, std::string_view pattern, std::initializer_list<Place> places);

/// Joins names the way a sentence lists them, through names_comma and names_and.
///
/// @param text how texts are looked up
/// @param names the names, in order
/// @return the list; empty when `names` is empty
[[nodiscard]] inline std::string
join_names(const TextHooks& text, const std::vector<std::string>& names) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names.front();
    std::string joined = names.front();
    for (std::size_t index = 1; index + 1 < names.size(); ++index) {
        joined = filled(text, words::names_comma, {{"left", joined}, {"right", names[index]}});
    }
    return filled(text, words::names_and, {{"left", joined}, {"right", names.back()}});
}

/// Joins names with "or", through names_comma and names_or.
///
/// @param text how texts are looked up
/// @param names the names, in order
/// @return the list; empty when `names` is empty
[[nodiscard]] inline std::string
join_or(const TextHooks& text, const std::vector<std::string>& names) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names.front();
    std::string joined = names.front();
    for (std::size_t index = 1; index + 1 < names.size(); ++index) {
        joined = filled(text, words::names_comma, {{"left", joined}, {"right", names[index]}});
    }
    return filled(text, words::names_or, {{"left", joined}, {"right", names.back()}});
}

/// Writes a size in binary units.
///
/// Below 1 MiB in whole KB, at least 1; below 100 MiB in MB to one decimal;
/// below 1 GiB in whole MB; above in GB to one decimal. Halves round up:
/// 43,011,223 bytes is "41.0 MB", and 197,132,288 is "188 MB".
///
/// @param bytes the size
/// @param text how texts are looked up
/// @return the size
[[nodiscard]] std::string size_text(uint64_t bytes, const TextHooks& text = {});

/// Writes a download's progress, both numbers in the unit and precision of the whole size.
///
/// @param done the bytes downloaded
/// @param total the whole size, in bytes
/// @param text how texts are looked up
/// @return the progress, such as "18.2 of 64.0 MB"
[[nodiscard]] std::string progress_text(uint64_t done, uint64_t total, const TextHooks& text = {});

/// Writes how long ago something happened.
///
/// Below a minute, or a negative age, "just now"; below an hour in whole
/// minutes; below 48 hours in whole hours; else in whole days.
///
/// @param seconds the age
/// @param text how texts are looked up
/// @return the age, such as "4 min ago"
[[nodiscard]] std::string age_text(int64_t seconds, const TextHooks& text = {});

/// Writes a rules hash's first four bytes as two groups of hexadecimal digits.
///
/// @param digest the hash
/// @return the text, such as "9c1f 4e0b"
[[nodiscard]] std::string rules_text(const base::sha256::Digest& digest);

/// Writes a listing's version, its revision above 1, and a map pack's number of maps.
///
/// @param listing the listing
/// @param text how texts are looked up
/// @return the text, such as "4.8 · rev 3" or "1.2 · 2 maps"
[[nodiscard]] std::string version_text(const Listing& listing, const TextHooks& text = {});

/// Writes an installed package's version and its revision above 1.
///
/// @param installed the package
/// @param text how texts are looked up
/// @return the text, such as "4.8 · rev 3"
[[nodiscard]] std::string version_text(const Installed& installed, const TextHooks& text = {});

/// Writes a version and its revision as the details' Installed and Latest
/// facts give them. A revision of 0, which a catalogue or manifest that
/// names none leaves, is left out.
///
/// @param version the version, as written; empty when it has none
/// @param revision the packaging revision; 0 when it names none
/// @param text how texts are looked up
/// @return "4.8 (rev 3)", "4.8" for revision 0, "rev 3" without a version;
///         empty with neither
[[nodiscard]] std::string
release_text(std::string_view version, int64_t revision, const TextHooks& text = {});

/// What an update changes from and to, as the Rules fact, the queue and the update question name them.
struct ChangeTexts {
    std::string from{};       ///< "revision 3", the version "4.8", or "the old copy"
    std::string to{};         ///< "revision 4", the version "4.9", or "the new copy"
    std::string from_short{}; ///< "rev 3", the version "4.8", or "old"
    std::string to_short{};   ///< "rev 4", the version "4.9", or "new"
};

/// Names both sides of an update: by revision when it keeps the version
/// text, else by version. A side whose revision is 0 where its revision
/// would name it, or whose version is empty where its version would, is
/// named by its revision above 0 if it has one, else as the old or the new
/// copy, so that no side reads "revision 0".
///
/// @param installed the installed package
/// @param update its update
/// @param text how texts are looked up
/// @return the names
[[nodiscard]] ChangeTexts
change_texts(const Installed& installed, const UpdateNote& update, const TextHooks& text = {});

/// Describes an engine requirement in words.
///
/// @param range the requirement
/// @param text how texts are looked up
/// @return the words, such as "0.8.0 or later"
[[nodiscard]] std::string
engine_text(const formats::oamod::EngineRange& range, const TextHooks& text = {});

/// Writes a mutator version range in the engine's comparison words.
///
/// An empty range is empty. A range that does not parse is returned as
/// written. Several comparisons are joined with the list separator. The
/// Library's Needs fact, GET WITH and the mutator picker all use this, so
/// one range reads the same in each place.
///
/// @param range_text the range as the manifest writes it
/// @param text how texts are looked up
/// @return the words, such as "4.0 or later"
[[nodiscard]] std::string range_words(std::string_view range_text, const TextHooks& text = {});

/// Describes an engine requirement in a few characters.
///
/// @param range the requirement
/// @param text how texts are looked up
/// @return the words, such as "0.8.0+"
[[nodiscard]] std::string
engine_short_text(const formats::oamod::EngineRange& range, const TextHooks& text = {});

/// Names a tab.
///
/// @param tab the tab
/// @param compact the short name, for a small screen
/// @param text how texts are looked up
/// @return the name
[[nodiscard]] std::string_view tab_text(Tab tab, bool compact, const TextHooks& text = {});

/// Names a filter.
///
/// @param filter the filter
/// @param text how texts are looked up
/// @return the name
[[nodiscard]] std::string_view filter_text(Filter filter, const TextHooks& text = {});

/// Names a row's state, as its chip shows it.
///
/// @param state the state
/// @param text how texts are looked up
/// @return the name
[[nodiscard]] std::string_view state_text(State state, const TextHooks& text = {});

/// Names a button.
///
/// @param action what the button asks for
/// @param text how texts are looked up
/// @return the button's words
[[nodiscard]] std::string_view action_text(Action action, const TextHooks& text = {});

/// Maps a dry-run verdict onto the Library's verdict.
///
/// The state follows the record. The reason is copied as given. A compatible
/// verdict's summary is its non-zero counts, in `ChangeCounts` order, joined
/// with `names_comma` and `names_and`. A count of one uses the singular.
/// Every count at zero reads `changes_other_data`. A caller that knows the
/// list's last mutator declares maps replaces that with `apply_map_fact`.
///
/// @param record the dry-run verdict
/// @param text how texts are looked up
/// @return the Library's verdict
[[nodiscard]] Verdict
verdict_of_record(const oa::data::mutators::VerdictRecord& record, const TextHooks& text = {});

/// Replaces an empty-count compatible summary when the list's last mutator
/// declares maps.
///
/// The stored verdict stays compatible with no counts. The fact says the map
/// changes when a match starts, because a dry run has no map and the maps
/// hook does not run. Any other verdict is left as it is.
///
/// @param[in,out] verdict the Library's verdict
/// @param record the dry-run verdict
/// @param declares_maps true when the list's last mutator declares maps
/// @param text how texts are looked up
void apply_map_fact(
    Verdict& verdict,
    const oa::data::mutators::VerdictRecord& record,
    bool declares_maps,
    const TextHooks& text
);

/// Returns a registry's avatar: the first letters of its first three words
/// when its name has two or more, or the first two letters of its one word,
/// in upper case. While the interface is Turkish or Azerbaijani, a small i
/// becomes İ and a small ı becomes I. Every other language capitalises a
/// to z only.
///
/// @param name the registry's name, in UTF-8
/// @return at most three characters
[[nodiscard]] std::string registry_initials(std::string_view name);

/// Returns how long ago a registry was checked, as its subtitle says it:
/// "just now" under a minute, then whole minutes, hours and days, and
/// "1 day ago" through the second day.
///
/// @param seconds how long ago, in seconds
/// @param text how texts are looked up
/// @return such as "4 min ago", "2 h ago" or "3 days ago"
[[nodiscard]] std::string registry_age_text(int64_t seconds, const TextHooks& text = {});

/// Returns when a registry was last read: "refreshed 5 min ago", "never
/// refreshed", or "couldn't reach · 2 h ago" when the last refresh found
/// nothing usable.
///
/// @param registry the registry
/// @param text how texts are looked up
/// @return the line
[[nodiscard]] std::string
registry_refresh_line(const Registry& registry, const TextHooks& text = {});

/// Returns a registry's subtitle: how many packages it lists, "not reviewed"
/// when the player added it, and how many mirrors the player added.
///
/// @param registry the registry
/// @param text how texts are looked up
/// @return the subtitle
[[nodiscard]] std::string registry_subtitle(const Registry& registry, const TextHooks& text = {});

/// Returns the note under the Registries list: content from registries other
/// than the built-in ones, named, is not reviewed by the OA team.
///
/// @param registries the registries, in the order they are listed
/// @param text how texts are looked up
/// @return the note
[[nodiscard]] std::string
registry_note(const std::vector<Registry>& registries, const TextHooks& text = {});

/// The word a layer id shows. An id that is not one of the nine is empty.
///
/// @param id the layer id
/// @return the English word; empty when the id is not known
[[nodiscard]] inline std::string_view layer_word(std::string_view id) noexcept {
    if (id == "files")
        return words::layer_files;
    if (id == "units")
        return words::layer_units;
    if (id == "weapons")
        return words::layer_weapons;
    if (id == "features")
        return words::layer_features;
    if (id == "sides")
        return words::layer_sides;
    if (id == "tables")
        return words::layer_tables;
    if (id == "menus")
        return words::layer_menus;
    if (id == "maps")
        return words::tab_maps;
    if (id == "hacks")
        return words::fact_hacks;
    return {};
}

/// The Checked fact's value and how it is marked.
struct CheckedWords {
    std::string value{};   ///< the fact's value, already looked up
    Mark mark{Mark::none}; ///< how the value is marked
};

/// The Checked fact for a verdict against the mod being played.
///
/// Compatible is good, no effect is a warning, incompatible and failed are
/// bad, and a check that is running or not started yet has no mark.
///
/// @param verdict the check
/// @param mod the mod's name, as a player reads it
/// @param text how texts are looked up
/// @return the value and its mark
[[nodiscard]] inline CheckedWords
checked_text(const Verdict& verdict, std::string_view mod, const TextHooks& text) {
    CheckedWords checked;
    const bool base_game = mod.empty() || mod == shown(text, words::no_mod);
    switch (verdict.state) {
    case Verdict::State::compatible:
        checked.value =
            base_game
                ? filled(text, words::checked_good_base, {{"summary", verdict.summary}})
                : filled(text, words::checked_good, {{"mod", mod}, {"summary", verdict.summary}});
        checked.mark = Mark::good;
        break;
    case Verdict::State::no_effect:
        checked.value = base_game ? std::string(shown(text, words::checked_no_effect_base))
                                  : filled(text, words::checked_no_effect, {{"mod", mod}});
        checked.mark = Mark::warn;
        break;
    case Verdict::State::incompatible:
        checked.value =
            base_game
                ? filled(text, words::checked_incompatible_base, {{"reason", verdict.reason}})
                : filled(
                      text, words::checked_incompatible, {{"mod", mod}, {"reason", verdict.reason}}
                  );
        checked.mark = Mark::bad;
        break;
    case Verdict::State::failed:
        checked.value =
            base_game
                ? filled(text, words::checked_failed_base, {{"reason", verdict.reason}})
                : filled(text, words::checked_failed, {{"mod", mod}, {"reason", verdict.reason}});
        checked.mark = Mark::bad;
        break;
    case Verdict::State::checking:
        checked.value = base_game ? std::string(shown(text, words::checked_running_base))
                                  : filled(text, words::checked_running, {{"mod", mod}});
        break;
    case Verdict::State::unchecked:
        checked.value = base_game ? std::string(shown(text, words::checked_not_yet_base))
                                  : filled(text, words::checked_not_yet, {{"mod", mod}});
        break;
    }
    return checked;
}

} // namespace oa::ui::library
