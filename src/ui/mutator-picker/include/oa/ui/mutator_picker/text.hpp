// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every word the mutator picker shows, in English. A pattern's places are
// written {name}; a translation keeps them and may move them. The texts are
// looked up whole through the Library's TextHooks before their places are
// filled, so a translator finds every one of them here.
#pragma once

#include "oa/ui/library/text.hpp"

#include <string_view>

namespace oa::ui::mutator_picker {

namespace words {

/// The amber line on a mutator an On mutator needs.
inline constexpr std::string_view line_needed_by = "Needed by {name}";
/// An Off mutator that is not installed. Switching it on downloads it.
inline constexpr std::string_view line_not_installed =
    "Not installed: switching it on downloads it";
/// Why a mutator cannot be switched on here, or an incompatible verdict.
inline constexpr std::string_view line_not_compatible = "Not compatible with this mod: {reason}";
/// A verdict whose check did not finish.
inline constexpr std::string_view line_failed = "Stopped with this mod: {reason}";
/// A verdict that applies and changes nothing.
inline constexpr std::string_view line_no_effect = "No effect with this mod";
/// A mutator that cannot be on beside one that already is. `{name}` is the other.
inline constexpr std::string_view line_conflicts = "Cannot run beside {name}";
/// Switching on would pass most_on, counting the prerequisites it brings.
inline constexpr std::string_view refused_cap = "At most {count} mutators can be on";
/// Why a row is locked: a prerequisite that names no version range and is
/// not among the mutators the picker can get. GET WITH uses the same words.
/// With a range, the Library's `blocked_prerequisite` says it; a loop is
/// the Library's `blocked_cycle`.
inline constexpr std::string_view blocked_prerequisite_any =
    oa::ui::library::words::blocked_prerequisite_any;
/// One option on a row, the Library's Options fact form.
inline constexpr std::string_view option_line = "{label}: {value}";
/// The clash choice on a row that brings units or weapons.
inline constexpr std::string_view clash_label = "Units and weapons";
/// The clash choice's Add value.
inline constexpr std::string_view clash_add = "Add";
/// The clash choice's Replace value.
inline constexpr std::string_view clash_replace = "Replace";
/// The footer's count of mutators that are on.
inline constexpr std::string_view summary_on = "{count} on";
/// The footer's summary when none are on.
inline constexpr std::string_view summary_none = "None on";
/// The first prerequisite still on that was brought with a dependant.
inline constexpr std::string_view summary_came_with = "{name} came with {dependant}";
/// The footer's chip when every On mutator was checked and changes the mod.
inline constexpr std::string_view verdict_checked = "Checked with {mod} ✓";
/// The footer's chip while any On mutator is being checked.
inline constexpr std::string_view verdict_checking = "Checking with {mod}…";
/// The footer's chip for the first On mutator that changes nothing.
inline constexpr std::string_view verdict_no_effect = "No effect with {mod}: {name}";
/// The footer's chip for the first On mutator that cannot run with the mod.
inline constexpr std::string_view verdict_not_compatible = "Not compatible with {mod}: {name}";
/// The footer's chip for the first On mutator whose check did not finish.
inline constexpr std::string_view verdict_failed = "{name} stopped with {mod}";
/// The same five chips when no mod is played. A translator writes the base
/// game in the sentence, so it is not "with {mod}" filled with "No Mod".
inline constexpr std::string_view verdict_checked_base = "Checked with No Mod ✓";
inline constexpr std::string_view verdict_checking_base = "Checking with No Mod…";
inline constexpr std::string_view verdict_no_effect_base = "No effect with No Mod: {name}";
inline constexpr std::string_view verdict_not_compatible_base =
    "Not compatible with No Mod: {name}";
inline constexpr std::string_view verdict_failed_base = "{name} stopped with No Mod";
/// The hint when the picker opened on the remembered skirmish set.
inline constexpr std::string_view hint_last_skirmish =
    "Your last skirmish set, ready to use. Cancel plays without mutators.";
/// The hint when the picker opened on the remembered multiplayer set.
inline constexpr std::string_view hint_last_multiplayer =
    "Your last multiplayer set, ready to use. Cancel plays without mutators.";
/// The hint when the picker opened on the remembered campaign set.
inline constexpr std::string_view hint_last_campaign =
    "Your last campaign set, ready to use. Cancel plays without mutators.";
/// The campaign's second sentence, after the last-set hint when that shows.
inline constexpr std::string_view hint_campaign =
    "The set holds for every mission of this campaign, and is saved with it.";
/// Mutators the last set named that were not switched on.
inline constexpr std::string_view hint_left_out = "Left out of your last set: {names}";
/// The hint while a guest looks at the host's set.
inline constexpr std::string_view hint_view = "The host's mutators. Only the host can change them.";
/// The question asked before a prerequisite is switched off.
inline constexpr std::string_view turn_off_title = "Turn off {name}?";
/// The question's first paragraph when one On mutator needs it.
inline constexpr std::string_view turn_off_one = "{names} turns off too: it needs {name}.";
/// The question's first paragraph when more than one On mutator needs it.
inline constexpr std::string_view turn_off_many = "{names} turn off too: they need {name}.";
/// The question's second paragraph for one dependant.
inline constexpr std::string_view turn_off_kept_one =
    "Both stay installed. Turn {dependant} on again and {name} comes back with it.";
/// The question's second paragraph for more than one dependant.
inline constexpr std::string_view turn_off_kept_many =
    "They all stay installed. Turn one on again and {name} comes back with it.";
/// The question's button that leaves the set as it is.
inline constexpr std::string_view cancel = "Cancel";
/// The question's button that switches the prerequisite and its dependants off.
inline constexpr std::string_view turn_off = "Turn off";

/// The window's title.
inline constexpr std::string_view title_mutators = "Mutators";
/// The skirmish setup, in the header's context.
inline constexpr std::string_view setup_skirmish = "Skirmish";
/// The battle room setup, in the header's context.
inline constexpr std::string_view setup_battle = "Battle room";
/// The campaign setup, in the header's context.
inline constexpr std::string_view setup_campaign = "Campaign";
/// The header's context when no mod is being played.
inline constexpr std::string_view no_mod = "No Mod";
/// The header's context: the setup and the mod, or No Mod.
inline constexpr std::string_view context_setup = "{setup} · {mod}";
/// The filter that keeps every mutator.
inline constexpr std::string_view filter_all = "All";
/// The filter that keeps installed mutators.
inline constexpr std::string_view filter_installed = "Installed";
/// The filter that keeps mutators that are not installed.
inline constexpr std::string_view filter_downloads = "Downloads";
/// A filter pill or the compact drop-down: the filter and how many it keeps.
inline constexpr std::string_view filter_with_count = "{filter} ({count})";
/// The button that holds the tags that do not fit as pills.
inline constexpr std::string_view tags_menu = "Tags";
/// One tag in the compact drop-down or the tags menu, with how many it keeps.
inline constexpr std::string_view tag_with_count = "Tag: {tag} ({count})";
/// The search field's placeholder.
inline constexpr std::string_view search_mutators = "Search mutators";
/// The heading over the On rows.
inline constexpr std::string_view heading_on = "ON · APPLIED TOP TO BOTTOM";
/// The heading over the Off rows. Not a switch's Off half.
inline constexpr std::string_view heading_off = "OFF · NOT APPLIED";
/// A switch's Off half. The catalogue's shared Off, which every switch uses.
inline constexpr std::string_view switch_off = "Off";
/// A switch's On half. The catalogue's shared On.
inline constexpr std::string_view switch_on = "On";
/// The clash strip's Add caption, on a mutator that brings units or weapons.
inline constexpr std::string_view clash_mode_add = "Add";
/// The clash strip's Replace caption.
inline constexpr std::string_view clash_mode_replace = "Replace";
/// The details' order label.
inline constexpr std::string_view order_label = "Order";
/// The button that moves the selected mutator up.
inline constexpr std::string_view order_up = "▲ Up";
/// The button that moves the selected mutator down.
inline constexpr std::string_view order_down = "▼ Down";
/// The details' version and author under the name.
inline constexpr std::string_view version_author = "{version} · {author}";
/// Compact's button that opens the details page.
inline constexpr std::string_view details_button = "Details";
/// The details page's button that returns to the list.
inline constexpr std::string_view back_button = "Back";
/// The button that keeps the set.
inline constexpr std::string_view use_these = "Use these";
/// Use these's summary while the first On mutator still downloading is fetched.
/// `{name}` is that mutator. `{percent}` is how much of it has arrived, 0 when
/// its size is not known yet.
inline constexpr std::string_view waiting_getting = "Getting {name} · {percent}%";
/// Use these's summary when that download stopped. `{problem}` is why.
inline constexpr std::string_view waiting_failed = "{name}: {problem}";
/// Why a mutator does not work with the mod being played. The row wraps it in
/// `line_not_compatible`. `{names}` is the list it works with.
inline constexpr std::string_view works_with_mods = "it works with {names}";
/// The read-only footer's button, and the header's close mark.
inline constexpr std::string_view close_button = "Close";
/// The list when the host gave no mutators.
inline constexpr std::string_view empty_none =
    "No mutators here yet. The Library on the main menu gets them.";
/// The list when the filters keep nothing.
inline constexpr std::string_view empty_filtered = "No mutator matches these filters";

/// "Start without X?" when one mutator stopped the game from starting.
inline constexpr std::string_view start_without_one = "Start without {name}?";
/// "Start without X?" when more than one did.
inline constexpr std::string_view start_without_many = "Start without them?";
/// What one mutator did, and that nothing it changed is kept.
inline constexpr std::string_view stopped_one =
    "{name} stopped before the game could start. Nothing it changed is kept, and the other "
    "mutators are unaffected.";
/// What several mutators did, and that nothing they changed is kept.
inline constexpr std::string_view stopped_many =
    "{names} stopped before the game could start. Nothing they changed is kept, and the other "
    "mutators are unaffected.";
/// One mutator's amber line: its name and why it stopped.
inline constexpr std::string_view failure_line = "{name}: {reason}";
/// The button that starts without the one mutator that stopped.
inline constexpr std::string_view start_without_it = "Start without it";
/// The button that starts without every mutator that stopped.
inline constexpr std::string_view start_without_them = "Start without them";
/// The question after a crash that left a set on.
inline constexpr std::string_view forget_title = "Forget the last set?";
/// What forgetting does, and that nothing is uninstalled.
inline constexpr std::string_view forget_body =
    "The last game ended in a crash with {names} on. Forget the set and the Mutators button "
    "opens empty next time; nothing is uninstalled.";
/// Where the set's names are written, so their authors can be told.
inline constexpr std::string_view forget_hint =
    "The game's log names them, so their authors can be told.";
/// The button that keeps the remembered set.
inline constexpr std::string_view keep_it = "Keep it";
/// The button that forgets the remembered set.
inline constexpr std::string_view forget_it = "Forget it";
/// The question while On mutators are downloaded.
inline constexpr std::string_view get_mutators_title = "Get mutators";
/// Which download this is, among the set's.
inline constexpr std::string_view downloading_of = "Downloading {name} · {index} of {total}";
/// The amber line once a download has stopped. `{problem}` is why.
inline constexpr std::string_view problem_retry = "{problem} Retry tries again.";
/// The question when another machine's mutators would play differently.
inline constexpr std::string_view did_not_start_title = "The game did not start";
/// Why the game was not started.
inline constexpr std::string_view different_result =
    "{player}'s mutators gave a different result from yours, so the game would play differently "
    "on each machine.";
/// A mutator this machine and that machine hold at different releases.
inline constexpr std::string_view release_here_there =
    "{name}: release {here} here, release {there} on {player}'s machine.";
/// A mutator that machine does not have.
inline constexpr std::string_view missing_there = "{name}: {player} does not have it.";
/// The release that machine's GET MUTATORS can fetch.
inline constexpr std::string_view get_fetches =
    "{player}'s GET MUTATORS button fetches release {release}.";

} // namespace words

} // namespace oa::ui::mutator_picker
