// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ENTER, the B key and "-y" ask the runtime's entry, then open the Boneyards
// when it is ready: the online features are on, the latest Open
// Annihilation is running, the mod is off, and the latest Boneyards package
// is installed and in use. Only then does the update screen show what was
// checked and the Boneyards open. A package the entry installs is put in use
// by starting the run once more, which then enters.
// The two entry checks start that once the main menu's strip is up, and
// check that the expansion's art and sounds are what the Boneyards read.
// ENTER waits until the menu has settled and the update check has answered.
// "-y", a headless or unattended run and a check are quiet: they are asked
// nothing about the online features or the update, and change no setting.
// The strip's pump asks again when the one pass ran before the strip could
// take it. A menu picture is written once, while the strip is showing and
// the Boneyards are not open.
#include "boneyards_launch.hpp"
#include "boneyards_mod_gate.hpp"
#include "boneyards_options.hpp"
#include "console_state.hpp"
#include "expansion_folder.hpp"

#include "oa/app/app.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/expansion_entry.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/boneyards/console/status_widget.hpp"
#include "oa/boneyards/rover_session.hpp"
#include "oa/boneyards/shown_text.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <vector>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

namespace console = oa::boneyards::console;

/// The update question, in English; "{version}" is the offered release.
constexpr std::string_view k_question =
    "Open Annihilation {version} is out. The Boneyards needs the latest version.";

/// The question about the online features, in English.
constexpr std::string_view k_services_question =
    "The Boneyards needs Open Annihilation's online features: update checks and the content "
    "catalogue. Turn them on and continue?";

/// The failed update check's sentence, in English.
constexpr std::string_view k_retry_question = "Open Annihilation could not check for updates.";

/// The line a first install that is not in use shows when it cannot start
/// the run again.
constexpr std::string_view k_not_in_use =
    "The Boneyards is installed. Start Open Annihilation again to enter the Boneyards.";

/// The sound name of the Boneyards' message chime, a WAV the expansion's archive holds.
constexpr const char* k_chime = "MESSAGE";

/// The console's art, which only the expansion's archive holds.
constexpr const char* k_console_art = "anims/Main_Console.gaf";

/// The longest path of a sound's file the entry check reads back.
constexpr std::size_t k_sound_path_capacity = 256;

/// Pumps the entry check stays in the Boneyards after checking what they
/// read, so the Main Frame's sounds and pages are asked for, before it ends.
constexpr int k_check_linger_pumps = 240;

/// The fetch line, in English. "{percent}" is filled in as the download moves.
constexpr std::string_view k_progress = "Fetching the Boneyards… {percent}%";

/// The package the entry asks for.
constexpr std::string_view k_package = "boneyards";

/// The runtime the entry was installed on. Startup sets it before any click.
Runtime* g_runtime = nullptr;

/// One picture has been written for this run.
bool g_snapshot_written = false;

/// Pumps since the run started, counting the one now running.
int g_pump = 0;

/// The entry check has started its one request.
bool g_started = false;

/// This entry drives the real update screen. A check or a headless run does not.
bool g_real = false;

/// "-y" or a click has begun an entry. The pump asks once until this is set,
/// so a pass that ran before the strip could take it is tried again.
bool g_entry_begun = false;

/// Each line is printed once for a request.
bool g_printed_ask = false;
bool g_printed_open = false;
bool g_printed_stayed = false;
bool g_printed_failed = false;
bool g_printed_mod = false;
bool g_printed_services = false;
bool g_printed_retry = false;
bool g_printed_updating = false;
bool g_printed_restart = false;

/// The entry check's pumps since the Boneyards opened; -1 before they did.
int g_open_pumps = -1;

/// The next run was asked to enter once. Remembered mods stay until answered.
/// Cleared once that entry has begun, so a later ENTER in the same run asks.
bool g_enter_once = false;

/// The entry now running came from the one-shot a restart carried. It does
/// not last past that entry.
bool g_carried = false;

/// This run has already asked to switch a remembered mod back on.
bool g_leave_asked = false;

/// The previous pump was inside the Boneyards, with the strip showing.
bool g_was_inside = false;

/// The mod-entry check has acted for this run.
bool g_mod_check_ran = false;

/// Restart 0 of that check has cleared a leftover entry request.
bool g_mod_check_cleared = false;

/// The Boneyards catalogue beside the program, or an empty one.
///
/// A missing file leaves every line in English. The path is not logged.
///
/// @return the catalogue; it lives for the process
const oa::data::languages::InterfaceText& entry_words() {
    static const oa::data::languages::InterfaceText catalogue = [] {
        oa::data::languages::InterfaceText loaded;
        const std::filesystem::path file =
            oa::platform::path_from_utf8(oa::platform::program_directory()) / "languages" /
            "boneyards.tdf";
        std::ifstream input(file, std::ios::binary);
        if (!input)
            return loaded;
        const std::string bytes(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
        );
        loaded.add(bytes, nullptr);
        return loaded;
    }();
    return catalogue;
}

/// The sentences and button captions for one request.
///
/// @return the ask; the package id is the Boneyards expansion
ExpansionEntryAsk make_ask() {
    namespace languages = oa::data::languages;
    // Each shown word is copied before the next call: the pointer is reused.
    const char* ok_word = oa::boneyards::shown_text("OK");
    const std::string ok_label(ok_word != nullptr ? ok_word : "");
    const oa::data::languages::InterfaceText& words = entry_words();
    const oa::data::languages::Language& language = expansion_entry_language(*g_runtime);
    ExpansionEntryAsk ask;
    ask.key = std::string(k_package);
    ask.question = std::string(languages::interface_text(k_question));
    ask.progress = std::string(words.text(k_progress, language));
    ask.update_label = std::string(languages::interface_text("Update"));
    ask.cancel_label = std::string(languages::interface_text("Cancel"));
    ask.ok_label = ok_label;
    ask.services_question = std::string(languages::interface_text(k_services_question));
    ask.turn_on_label = std::string(languages::interface_text("Turn on"));
    ask.retry_question = std::string(languages::interface_text(k_retry_question));
    ask.retry_label = std::string(languages::interface_text("Try again"));
    // "-y" opens once a result already in hand has been taken; ENTER, and the
    // entry a restart carried, wait for the update check's answer.
    ask.skip_unanswered_check = boneyards_options().setup_requested;
    ask.carried = g_carried;
    return ask;
}

/// Replaces `{mod}` once.
///
/// @param pattern the sentence, already translated
/// @param name the mod's name
/// @return the sentence to show
std::string fill_mod(std::string_view pattern, std::string_view name) {
    std::string text(pattern);
    constexpr std::string_view token = "{mod}";
    const std::size_t at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), name);
    return text;
}

/// The name a question shows for the mod that is playing.
///
/// @param playing the mod
/// @return the profile's name, else its id, else the folder's name
std::string mod_display_name(const ActiveMod& playing) {
    if (playing.has_profile) {
        if (!playing.profile_name.empty())
            return playing.profile_name;
        if (!playing.profile_id.empty())
            return playing.profile_id;
    }
    return oa::platform::path_to_utf8(oa::platform::path_from_utf8(playing.folder).filename());
}

/// What is playing, for the entry's decision.
///
/// @param playing the mod
/// @return none, the Boneyards' own profile, or another mod
PlayedMod played_kind(const ActiveMod& playing) {
    if (!playing.active)
        return PlayedMod::none;
    if (playing.has_profile && boneyards_own_profile(playing.profile_id))
        return PlayedMod::own;
    return PlayedMod::foreign;
}

/// A headless run, an unattended run, or a Boneyards check keeps today's entry.
///
/// @return true when the real update screen stays off
bool quiet_run();

/// Who can answer. "-y" and a run with nobody to ask switch a mod off quietly.
///
/// @param runtime the running game
/// @return quiet when the player cannot answer
EntryWay entry_way(const Runtime& runtime) {
    const Options& options = runtime_options(runtime);
    if (quiet_run() || boneyards_options().setup_requested || options.headless_check ||
        options.unattended)
        return EntryWay::quiet;
    return EntryWay::player;
}

/// The sentence a command-line mod is told, in English. The catalogue has it.
constexpr std::string_view k_command_mod =
    "Start Open Annihilation without the mod on its command line to enter the Boneyards.";

/// Fills the mod question when another mod is playing.
///
/// The Boneyards' own profile, and a run with no mod, leave the fields empty.
/// A mod the command line named is not switched off.
///
/// @param runtime the running game
/// @param[in,out] ask the entry's sentences
void fill_mod_ask(Runtime& runtime, ExpansionEntryAsk& ask) {
    const ActiveMod playing = active_mod(runtime);
    const Options& options = runtime_options(runtime);
    const bool command_line = !options.mod_dir.empty() || !options.mod_file.empty();
    const EntryDecision decision =
        decide_mod_entry(played_kind(playing), entry_way(runtime), command_line);
    if (decision == EntryDecision::proceed)
        return;
    if (decision == EntryDecision::command_notice || decision == EntryDecision::command_stay) {
        // The folder stays empty: the mod is not switched off, whatever a
        // quiet run would do with a folder.
        ask.mod_command_line = true;
        ask.mod_switch_quietly = decision == EntryDecision::command_stay;
        ask.mod_question = std::string(oa::data::languages::interface_text(k_command_mod));
        return;
    }
    const std::string name = mod_display_name(playing);
    ask.mod_name = name;
    ask.mod_folder = playing.folder;
    ask.mod_switch_quietly = decision == EntryDecision::switch_quietly;
    ask.mod_switch_label = std::string(oa::data::languages::interface_text("Switch off and enter"));
    ask.mod_cancel_label = std::string(oa::data::languages::interface_text("Cancel"));
    ask.mod_question = fill_mod(
        oa::data::languages::interface_text(
            "The Boneyards plays Total Annihilation without mods. Switch {mod} off and enter the "
            "Boneyards?"
        ),
        name
    );
}

/// A headless run, an unattended run, or a Boneyards check keeps today's entry.
///
/// "-y" is a player's entry: it runs the checks and is not one of these.
///
/// @return true when the real update screen stays off
bool quiet_run() {
    const Options& options = runtime_options(*g_runtime);
    if (options.headless_check || options.unattended)
        return true;
    const BoneyardsOptions& taken = boneyards_options();
    return taken.boneyards_entry != BoneyardsEntryCheck::none || taken.check_boneyards_login ||
           taken.enter_boneyards_ticks || taken.boneyards_launch || taken.boneyards_features ||
           taken.check_boneyards_unit_limit || taken.boneyards_overlay ||
           taken.check_boneyards_shortcut || taken.check_menu_modes;
}

/// Reports whether the run is one of the game's own checks: a
/// `--headless-check` run, any of the module's check options, the mod-entry
/// and strip-button checks, and a run that replays recorded Rover replies.
///
/// Such a run never asks a built-in registry or takes the package from one
/// (registry_reached). An unattended or `-y` run that plays, as the test
/// fleet's does, is not one.
///
/// @return true for one of the game's own checks
bool own_check_run() {
    if (runtime_options(*g_runtime).headless_check)
        return true;
    const BoneyardsOptions& taken = boneyards_options();
    return taken.boneyards_entry != BoneyardsEntryCheck::none || taken.check_boneyards_login ||
           taken.enter_boneyards_ticks || taken.boneyards_launch || taken.boneyards_features ||
           taken.check_boneyards_unit_limit || taken.boneyards_overlay ||
           taken.check_boneyards_shortcut || taken.check_menu_modes ||
           taken.check_boneyards_mod_entry || taken.check_strip_buttons ||
           !taken.boneyards_replay.empty();
}

/// The screen's facts, copied from the checks the entry says it ran.
///
/// @param status where the entry stands
/// @return the facts
console::UpdateCheckFacts facts_of(const ExpansionEntryStatus& status) {
    console::UpdateCheckFacts facts;
    facts.services_checked = status.services_known;
    facts.services_on = status.services_on;
    facts.files_checked = status.files_known;
    facts.mismatched_file = status.mismatched_file;
    facts.matched_files = status.matched_files;
    facts.feed_checked = status.feed_known;
    facts.feed_failed = status.feed_failed;
    facts.update_offered = status.update_offered;
    facts.offered_version = status.offered_version;
    facts.update_declined = status.update_declined;
    facts.extension_checked = status.extension_known;
    facts.extension_percent = status.extension_percent;
    facts.extension_installed_now = status.extension_installed;
    facts.extension_current = status.extension_current;
    facts.extension_version = status.extension_version;
    facts.entry_failed = status.step == ExpansionEntryStep::failed && !status.stopped;
    if (facts.entry_failed)
        facts.failure = status.notice;
    facts.entry_ready = status.step == ExpansionEntryStep::ready;
    return facts;
}

/// Copies what the checks found onto the update screen.
///
/// The screen opens only once every check has passed: the questions, the
/// fetch and a failure stay on the menu.
///
/// @param widget the main-menu strip; null does nothing
/// @param status where the entry stands
void show_real(console::StatusWidget* widget, const ExpansionEntryStatus& status) {
    if (!g_real || widget == nullptr)
        return;
    const console::UpdateCheckPicture picture = console::boneyards_update_picture(facts_of(status));
    console::boneyards_status_widget_set_real_update(widget, picture);
}

/// Checks that the Boneyards read the expansion's files once they opened:
/// an image of its Boneyards folder from the expansion folder, the console's
/// art from the archive in that folder, and the message chime as that
/// archive holds it.
///
/// @param runtime the running game
/// @param[out] what the file that is not read so, when one is not
/// @return true when both are read from the expansion
bool expansion_in_use(Runtime& runtime, std::string& what) {
    namespace bundles = oa::data::content_bundles;
    const bundles::Bundle* bundle = bundles::find_bundle(k_package);
    if (bundle == nullptr || bundle->extends.empty()) {
        what = "the expansion is not in the bundle table";
        return false;
    }
    const std::filesystem::path folder = player_folder(runtime) /
                                         std::string(bundles::managed_expansions_folder) /
                                         std::string(bundle->extends[0]) / std::string(bundle->id);
    // Paths are compared in one spelling, so a folder reached through a
    // link still counts as the expansion folder.
    const auto spelled = [](const std::filesystem::path& path) {
        std::error_code ignored;
        return oa::platform::path_to_utf8(std::filesystem::weakly_canonical(path, ignored));
    };
    const std::string folder_text = spelled(folder);
    std::string image;
    for (const bundles::ContentFile& file : bundle->files) {
        if (file.copy_name.starts_with("Boneyards/images/")) {
            image = std::string(file.copy_name);
            break;
        }
    }
    const std::string placed = oa::boneyards::rover_session_placed_path(image.c_str());
    std::vector<uint8_t> image_bytes;
    if (image.empty() || spelled(oa::platform::path_from_utf8(placed)).rfind(folder_text, 0) != 0 ||
        !oa::boneyards::rover_session_read_file(image.c_str(), &image_bytes) ||
        image_bytes.empty()) {
        what = image.empty() ? std::string("an image of the Boneyards folder") : image;
        return false;
    }
    // The console's art is read from the expansion's archive, which the
    // entry mounted. The base game's add-ons may hold the chime too, so the
    // sound the name plays is compared with that archive's own copy.
    std::string archive_name;
    for (const bundles::ContentFile& file : bundle->files) {
        if (file.copy_name.find('/') == std::string_view::npos &&
            file.copy_name.ends_with(".ccx")) {
            archive_name = std::string(file.copy_name);
            break;
        }
    }
    const CheckHost host = check_host(runtime);
    const oa::AssetStore* assets = host.assets != nullptr ? host.assets(host.context) : nullptr;
    const std::optional<std::filesystem::path> art =
        assets != nullptr ? assets->providing_archive(k_console_art) : std::nullopt;
    if (archive_name.empty() || !art || spelled(*art).rfind(folder_text, 0) != 0) {
        what = k_console_art;
        return false;
    }
    char sound[k_sound_path_capacity] = {};
    if (host.sound_resource == nullptr ||
        !host.sound_resource(host.context, k_chime, sound, sizeof sound)) {
        what = k_chime;
        return false;
    }
    std::ifstream input(folder / archive_name, std::ios::binary);
    std::vector<uint8_t> archive_bytes(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    auto opened = oa::HpiArchive::open(std::move(archive_bytes));
    std::vector<uint8_t> own;
    if (opened.ok()) {
        auto read = opened.value->read(sound);
        if (read.ok())
            own = std::move(*read.value);
    }
    std::vector<uint8_t> played;
    try {
        played = assets->read(sound).bytes;
    } catch (const std::exception&) {
        played.clear();
    }
    if (own.empty() || played != own) {
        what = sound;
        return false;
    }
    std::cout << "entry check: " << image << " and " << k_console_art << " are read from the "
              << "installed expansion, and " << sound << " plays its archive's sound\n";
    std::cout.flush();
    return true;
}

/// Prints where the entry stands, and opens the Boneyards when it is ready.
///
/// @param runtime the running game
/// @param widget the main-menu strip; null does not open
void drive(Runtime& runtime, console::StatusWidget* widget) {
    const ExpansionEntryStatus status = advance_expansion_entry(runtime);
    show_real(widget, status);
    if (status.step == ExpansionEntryStep::services_asking && !g_printed_services) {
        g_printed_services = true;
        std::cout << "entry flow: asking to turn the online features on\n" << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::retry_asking && !g_printed_retry) {
        g_printed_retry = true;
        std::cout << "entry flow: asking to check for updates again\n" << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::updating && !g_printed_updating) {
        g_printed_updating = true;
        std::cout << "entry flow: updating Open Annihilation\n";
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::restarting && !g_printed_restart) {
        g_printed_restart = true;
        std::cout << "entry flow: starting again to enter\n";
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::mod_asking && !g_printed_mod) {
        g_printed_mod = true;
        std::cout << "entry flow: asking to switch a mod off\n" << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::asking && !g_printed_ask) {
        g_printed_ask = true;
        std::cout << "entry flow: asking to update\n" << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::stayed && !g_printed_stayed) {
        g_printed_stayed = true;
        std::cout << "entry flow: stayed\n";
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::failed && !g_printed_failed) {
        g_printed_failed = true;
        std::cout << "entry flow: failed: " << status.notice << '\n';
        std::cout.flush();
        return;
    }
    if (status.step == ExpansionEntryStep::ready && !g_printed_open) {
        // The Boneyards' screens are built with the main menu, and the
        // strip's layouts, images and fonts stay as they were read then. A
        // package this entry installed was not there yet, so the run starts
        // again once and the next run, which mounts it from its start,
        // enters on its own. The folder the Boneyards read their files
        // through is looked up again first, since the next run keeps it.
        if (status.extension_new) {
            (void)boneyards_expansion_folder_refreshed();
            restart_expansion_entry(
                runtime, std::string(oa::data::languages::interface_text(k_not_in_use))
            );
            const ExpansionEntryStatus after = advance_expansion_entry(runtime);
            if (after.step != ExpansionEntryStep::ready) {
                drive(runtime, widget);
                return;
            }
        }
        g_printed_open = true;
        if (widget != nullptr)
            console::boneyards_status_widget_enter(widget);
        std::cout << "entry flow: boneyards open\n";
        std::cout.flush();
        if (boneyards_options().boneyards_entry != BoneyardsEntryCheck::none)
            g_open_pumps = 0;
    }
}

/// ENTER, the B key and "-y". A click during a check or a download is ignored,
/// and so is one after the Boneyards are already open.
///
/// @param widget the strip that was clicked
void on_request(console::StatusWidget* widget) {
    if (widget == nullptr || g_runtime == nullptr)
        return;
    if (console::boneyards_status_widget_in_boneyards(widget))
        return;
    if (expansion_entry_busy(*g_runtime))
        return;
    // Every entry takes the one-shot, including one "-y" starts itself. A
    // later ENTER in this run does not inherit it.
    EntryCarry once;
    once.pending =
        g_enter_once || expansion_preference(*g_runtime, kExpansionEnterOnce).has_value();
    g_carried = once.pending;
    if (take_entry_request(once)) {
        keep_expansion_preference(*g_runtime, kExpansionEnterOnce, {});
        save_expansion_preferences(*g_runtime);
    }
    g_enter_once = false;
    g_printed_ask = false;
    g_printed_open = false;
    g_printed_stayed = false;
    g_printed_failed = false;
    g_printed_mod = false;
    g_printed_services = false;
    g_printed_retry = false;
    g_printed_updating = false;
    g_printed_restart = false;
    g_entry_begun = true;
    g_real = !quiet_run();
    ExpansionEntryAsk ask = make_ask();
    fill_mod_ask(*g_runtime, ask);
    ask.quiet = entry_way(*g_runtime) == EntryWay::quiet;
    ask.own_check = own_check_run();
    // The update screen opens only once every check has passed.
    ask.report_on_screen = false;
    if (g_real)
        console::boneyards_status_widget_begin_real_update(widget);
    begin_expansion_entry(*g_runtime, ask);
    // A mod named on the command line is not switched off. A run that cannot
    // ask logs the notice and stays on the menu, with no restart.
    if (ask.mod_command_line && ask.mod_switch_quietly) {
        std::cout << ask.mod_question << '\n';
        std::cout.flush();
        return;
    }
    drive(*g_runtime, widget);
    // A run that cannot ask switches a stored mod off once the update check
    // has passed. The sentence names it.
    if (ask.mod_switch_quietly && expansion_will_restart(*g_runtime)) {
        std::cout << "The Boneyards switched " << ask.mod_name << " off to enter.\n";
        std::cout.flush();
    }
}

/// Forgets a one-shot entry and a remembered mod.
///
/// @param runtime the running game
void clear_expansion_keys(Runtime& runtime) {
    keep_expansion_preference(runtime, kExpansionEnterOnce, {});
    keep_expansion_preference(runtime, kExpansionSwitchedFolder, {});
    keep_expansion_preference(runtime, kExpansionSwitchedName, {});
    save_expansion_preferences(runtime);
}

/// Asks once to switch a remembered mod back on.
///
/// The question is skipped while a run cannot ask, while "-y" is ending, on
/// the run that is about to enter from the one-shot, and during the mod-entry
/// check. Leaving the Boneyards asks. A later main menu asks when a remembered
/// mod was left behind.
///
/// @param runtime the running game
void consider_leave(Runtime& runtime) {
    console::StatusWidget* widget = boneyards_console_status_widget();
    const bool showing = widget != nullptr && console::boneyards_status_widget_visible(widget);
    const bool inside = showing && console::boneyards_status_widget_in_boneyards(widget);
    const bool left = g_was_inside && showing && !inside;
    g_was_inside = inside;
    if (!showing || g_leave_asked)
        return;
    if (quiet_run() || boneyards_options().setup_requested ||
        boneyards_options().check_boneyards_mod_entry)
        return;
    if (expansion_entry_busy(runtime))
        return;
    const bool remembered = expansion_preference(runtime, kExpansionSwitchedFolder).has_value();
    if (decide_mod_leave(remembered, g_leave_asked) != LeaveDecision::ask)
        return;
    const bool leftover = !g_enter_once && !g_entry_begun && !inside;
    if (!left && !leftover)
        return;
    std::string name;
    if (const std::optional<std::string> stored =
            expansion_preference(runtime, kExpansionSwitchedName))
        name = *stored;
    else if (
        const std::optional<std::string> folder =
            expansion_preference(runtime, kExpansionSwitchedFolder)
    )
        name = oa::platform::path_to_utf8(oa::platform::path_from_utf8(*folder).filename());
    const std::string question =
        fill_mod(oa::data::languages::interface_text("Switch {mod} back on?"), name);
    const std::string on_label(oa::data::languages::interface_text("Switch on"));
    const std::string later_label(oa::data::languages::interface_text("Not now"));
    ask_remembered_mod(runtime, question, on_label, later_label);
    if (remembered_mod_question_open(runtime))
        g_leave_asked = true;
}

/// Throws when a step of the mod-entry check fails.
///
/// @param ok the step passed
/// @param what what failed
void require_mod_entry(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mod entry check: " + std::string(what));
}

/// Proves a mod is switched off before the entry, across the soft restarts.
///
/// Restart 0 writes made-up mod Ridge and switches it on. Restart 1 answers
/// Switch off. Restart 2 finds no mod, the one-shot already taken, and the
/// entry under way, then forgets the remembered mod. A release build does nothing.
///
/// @param runtime the running game
void run_mod_entry_check(Runtime& runtime) {
    if constexpr (!self_checks_built)
        return;
    if (g_mod_check_ran)
        return;
    console::StatusWidget* widget = boneyards_console_status_widget();
    const bool visible = widget != nullptr && console::boneyards_status_widget_visible(widget) &&
                         !console::boneyards_status_widget_in_boneyards(widget);
    const uint32_t restarts = runtime_options(runtime).restarts;
    // The one-shot opens the Boneyards on the same pump, before this check.
    // Restart 2 still has to read the result after that.
    if (restarts >= 2) {
        if (!g_entry_begun || player_folder(runtime).empty())
            return;
        g_mod_check_ran = true;
        require_mod_entry(!active_mod(runtime).active, "a mod is still on");
        require_mod_entry(
            !expansion_preference(runtime, kExpansionEnterOnce).has_value(),
            "the entry request is still pending"
        );
        require_mod_entry(g_entry_begun, "the entry has not begun");
        require_mod_entry(
            expansion_preference(runtime, kExpansionSwitchedFolder).has_value(),
            "the mod was not remembered"
        );
        keep_expansion_preference(runtime, kExpansionSwitchedFolder, {});
        keep_expansion_preference(runtime, kExpansionSwitchedName, {});
        save_expansion_preferences(runtime);
        std::cout << "mod entry check: the mod was switched off and the entry is under way\n";
        std::cout.flush();
        end_expansion_run(runtime);
        return;
    }
    if (!visible || player_folder(runtime).empty())
        return;
    if (restarts == 0) {
        g_mod_check_ran = true;
        const std::filesystem::path folder =
            player_folder(runtime) / std::string(user_mods_folder_name) / "ridge";
        std::filesystem::create_directories(folder);
        std::ofstream profile(folder / "oamod.yaml", std::ios::binary | std::ios::trunc);
        profile << "oamod: 1\n"
                   "id: ridge\n"
                   "name: Ridge\n"
                   "version: \"1.0\"\n"
                   "description: A made-up mod the entry check plays.\n"
                   "requires: {base: ta-3.1c, catalogue: 1}\n"
                   "author: {name: unknown}\n"
                   "packaging: {revision: 1, date: 2026-11-05, packager: Open Annihilation}\n";
        profile.flush();
        const bool wrote = static_cast<bool>(profile);
        profile.close();
        require_mod_entry(wrote && !profile.fail(), "the profile could not be written");
        std::string refusal;
        require_mod_entry(
            switch_active_mod(runtime, oa::platform::path_to_utf8(folder), refusal),
            refusal.empty() ? "Ridge could not be switched on" : refusal
        );
        require_mod_entry(expansion_will_restart(runtime), "the run did not restart");
        std::cout << "mod entry check: switching Ridge on\n";
        std::cout.flush();
        return;
    }
    if (restarts == 1) {
        g_mod_check_ran = true;
        const ActiveMod playing = active_mod(runtime);
        require_mod_entry(
            playing.active && playing.has_profile && playing.profile_id == "ridge",
            "Ridge is not on"
        );
        on_request(widget);
        require_mod_entry(expansion_mod_question_open(runtime), "the question is not open");
        answer_expansion_mod_question(runtime, true);
        require_mod_entry(expansion_will_restart(runtime), "the mod was not switched off");
        std::cout << "mod entry check: switching Ridge off\n";
        std::cout.flush();
    }
}

/// The entry check once the Boneyards opened: what they read is the
/// expansion's, checked on the first pump inside, then the run ends after
/// the Main Frame has had time to ask for its sounds and pages.
///
/// @param runtime the running game
void run_entry_check_inside(Runtime& runtime) {
    if (g_open_pumps == 0) {
        std::string what;
        if (!expansion_in_use(runtime, what)) {
            std::cout << "entry check: " << what << " is not read from the installed expansion\n";
            std::cout.flush();
            g_open_pumps = -1;
            end_expansion_run(runtime);
            return;
        }
    }
    if (++g_open_pumps < k_check_linger_pumps)
        return;
    g_open_pumps = -1;
    std::cout << "entry check: the Boneyards opened with the expansion in use\n";
    std::cout.flush();
    end_expansion_run(runtime);
}

/// The entry check's part of one pump: start once, and keep a check or a
/// download moving.
///
/// @param runtime the running game
void on_pump(Runtime& runtime) {
    ++g_pump;
    console::StatusWidget* widget = boneyards_console_status_widget();
    const bool visible = widget != nullptr && console::boneyards_status_widget_visible(widget) &&
                         !console::boneyards_status_widget_in_boneyards(widget);
    // A leftover one-shot from an earlier run of the check must not enter
    // before Ridge is switched on.
    if (boneyards_options().check_boneyards_mod_entry && runtime_options(runtime).restarts == 0 &&
        !g_mod_check_cleared && visible) {
        g_mod_check_cleared = true;
        clear_expansion_keys(runtime);
    }
    if (visible && expansion_preference(runtime, kExpansionEnterOnce).has_value()) {
        // The entry itself clears the key. The flag only starts that entry
        // on this pump, before the check reads the result.
        g_enter_once = true;
    }
    const BoneyardsEntryCheck mode = boneyards_options().boneyards_entry;
    if (mode != BoneyardsEntryCheck::none && !g_started && visible) {
        g_started = true;
        on_request(widget);
    } else if (!g_entry_begun && visible && (boneyards_options().setup_requested || g_enter_once)) {
        // "-y", and the one-shot a restart carried, ask once the strip is up.
        // ENTER still waits for a click.
        on_request(widget);
    } else if (expansion_entry_busy(runtime) && !remembered_mod_question_open(runtime)) {
        drive(runtime, widget);
    }
    if (g_open_pumps >= 0)
        run_entry_check_inside(runtime);
    consider_leave(runtime);
    if (boneyards_options().check_boneyards_mod_entry)
        run_mod_entry_check(runtime);
}

/// Writes the main menu once, while the strip is up and the Boneyards are not.
///
/// Checks that enter, and a headless run, keep the picture they write themselves.
/// A path is not printed.
///
/// @param runtime the running game
void write_menu_snapshot(Runtime& runtime) {
    if (g_snapshot_written)
        return;
    const Options& options = runtime_options(runtime);
    if (options.snapshot.empty() || options.headless_check)
        return;
    const BoneyardsOptions& taken = boneyards_options();
    if (taken.boneyards_entry != BoneyardsEntryCheck::none || taken.check_boneyards_login ||
        taken.enter_boneyards_ticks || taken.boneyards_launch || taken.boneyards_features ||
        taken.check_boneyards_unit_limit || taken.boneyards_overlay || taken.setup_requested ||
        taken.check_boneyards_shortcut || taken.check_menu_modes || taken.check_boneyards_mod_entry)
        return;
    console::StatusWidget* widget = boneyards_console_status_widget();
    if (!console::boneyards_status_widget_visible(widget) ||
        console::boneyards_status_widget_in_boneyards(widget))
        return;
    const CheckHost host = check_host(runtime);
    if (host.surface == nullptr)
        return;
    const auto* surface = host.surface(host.context);
    if (surface == nullptr)
        return;
    try {
        write_ppm(options.snapshot, *surface);
        g_snapshot_written = true;
    } catch (const std::exception&) {
        std::cerr << "open-annihilation: the menu picture could not be written\n";
        g_snapshot_written = true;
    }
}

} // namespace

/// The flow ENTER copies onto a strip created after this (console_screens.cpp).
void boneyards_console_set_entry_flow(void (*flow)(oa::boneyards::console::StatusWidget*));

void boneyards_entry_install(Runtime& runtime) {
    g_runtime = &runtime;
    g_snapshot_written = false;
    g_pump = 0;
    g_started = false;
    g_real = false;
    g_entry_begun = false;
    g_printed_ask = false;
    g_printed_open = false;
    g_printed_stayed = false;
    g_printed_failed = false;
    g_printed_mod = false;
    g_printed_services = false;
    g_printed_retry = false;
    g_printed_updating = false;
    g_printed_restart = false;
    g_open_pumps = -1;
    g_enter_once = false;
    g_carried = false;
    g_leave_asked = false;
    g_was_inside = false;
    g_mod_check_ran = false;
    g_mod_check_cleared = false;
    boneyards_console_set_entry_flow(&on_request);
}

void boneyards_entry_on_frame(Runtime& runtime, FrameStage stage) {
    if (stage == FrameStage::presented)
        write_menu_snapshot(runtime);
    else if (stage == FrameStage::pump)
        on_pump(runtime);
}

} // namespace oa::app
