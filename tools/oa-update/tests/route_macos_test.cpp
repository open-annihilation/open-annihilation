// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS swap, decided from signatures a test fills in, and the
// operations that follow; when the moves need an administrator's rights,
// how macOS is asked for them, and whose a moved bundle becomes; and how
// the game is started again. Nothing here signs a bundle, asks for a
// password or starts a program.

#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/run.hpp"

#include "oa/app/update/plan.hpp"
#include "oa/test/check.hpp"
#include "oa/update_helper/paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
namespace helper = oa::update_helper;
namespace update = oa::app::update;

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v08-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

helper::BundleSignature team_of(std::string_view team, bool valid, bool notarized) {
    helper::BundleSignature signature;
    signature.team = std::string(team);
    signature.valid = valid;
    signature.notarized = notarized;
    return signature;
}

void the_same_team_may_swap() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", true, true), false
    );
    OA_CHECK(decision.go);
    OA_CHECK(decision.why.empty());
}

void another_team_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("OTHERTEAM1", true, true), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle is signed by another team");
}

void an_invalid_signature_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", false, true), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle's signature is not valid");
}

void a_bundle_that_is_not_notarized_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", true, false), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle is not notarized");
}

void an_ad_hoc_release_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", true, false), false);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "an ad hoc signed build does not update in place");
}

void an_ad_hoc_self_check_may_swap() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", true, false), true);
    OA_CHECK(decision.go);
    OA_CHECK(decision.why.empty());
}

void a_mixed_ad_hoc_signature_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("TEAMID1234", true, true), team_of("", true, false), true);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "an ad hoc signed build does not update in place");
}

void an_invalid_ad_hoc_self_check_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", false, false), true);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle's signature is not valid");
}

fs::path make_app(const fs::path& path) {
    fs::create_directories(path / "Contents" / "MacOS");
    std::ofstream game(path / "Contents" / "MacOS" / "game");
    game << "game";
    return path;
}

struct LaidOut {
    fs::path install;
    fs::path staged;
    fs::path application;
    update::Plan plan;
};

LaidOut lay_out(const Scratch& scratch) {
    LaidOut laid;
    laid.install = make_app(scratch.path / "Applications" / "Game.app");
    laid.staged = scratch.path / "stage";
    laid.application = make_app(laid.staged / "Open-Annihilation" / "Open Annihilation.app");
    fs::create_directories(scratch.path / "updates");
    laid.plan.route = update::Route::macos_bundle;
    laid.plan.install = helper::utf8_of(laid.install);
    laid.plan.updates = helper::utf8_of(scratch.path / "updates");
    laid.plan.file = helper::utf8_of(scratch.path / "Open-Annihilation.pkg");
    return laid;
}

void the_same_volume_renames() {
    const Scratch scratch("same");
    const LaidOut laid = lay_out(scratch);
    std::string why = "uncleared";
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 2);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::rename);
    OA_CHECK(operations[0].from == laid.install);
    OA_CHECK(operations[0].to == previous);
    OA_CHECK(operations[1].kind == helper::Operation::Kind::rename);
    OA_CHECK(operations[1].from == laid.application);
    OA_CHECK(operations[1].to == laid.install);
}

void a_cross_volume_move_copies_both_ways() {
    const Scratch scratch("exdev");
    const LaidOut laid = lay_out(scratch);
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::exdev, &why);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 3);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::copy_tree);
    OA_CHECK(operations[0].from == laid.install);
    OA_CHECK(operations[0].to == previous);
    OA_CHECK(operations[1].kind == helper::Operation::Kind::remove_tree);
    OA_CHECK(operations[1].path == laid.install);
    OA_CHECK(operations[2].kind == helper::Operation::Kind::copy_tree);
    OA_CHECK(operations[2].from == laid.application);
    OA_CHECK(operations[2].to == laid.install);
}

void an_older_kept_bundle_is_removed_first() {
    const Scratch scratch("kept");
    const LaidOut laid = lay_out(scratch);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    fs::create_directories(previous);
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 3);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::remove_tree);
    OA_CHECK(operations[0].path == previous);
}

void the_installer_is_opened() {
    const Scratch scratch("pkg");
    LaidOut laid = lay_out(scratch);
    laid.plan.installer = true;
    std::string why = "uncleared";
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, {}, helper::MacosVolume::same, &why);
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 1);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::run_and_wait);
    OA_CHECK(operations[0].program == "/usr/bin/open");
    OA_CHECK(operations[0].arguments.size() == 1);
    OA_CHECK(operations[0].arguments[0] == laid.plan.file);
    OA_CHECK(operations[0].inverse_program.empty());
}

void a_missing_installer_is_refused() {
    update::Plan plan;
    plan.installer = true;
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(plan, {}, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the installer is missing");
}

void two_applications_are_refused() {
    const Scratch scratch("two");
    LaidOut laid = lay_out(scratch);
    make_app(laid.staged / "Open-Annihilation" / "Other.app");
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the staged package is not one application");
}

void restoring_moves_the_kept_bundle_back() {
    const Scratch scratch("restore-bundle");
    const fs::path install = scratch.path / "Open Annihilation.app";
    const fs::path kept = scratch.path / "previous" / "Open Annihilation.app";
    fs::create_directories(install / "Contents");
    fs::create_directories(kept / "Contents");
    std::ofstream(install / "Contents" / "Info.plist") << "new";
    std::ofstream(kept / "Contents" / "Info.plist") << "old";
    std::ofstream(scratch.path / "notes.txt") << "player";
    update::State state;
    state.installed_version = "0.8.7";
    state.kept_version = "0.8.6";
    state.kept_path = helper::utf8_of(scratch.path / "previous");
    state.kept_route = update::Route::macos_bundle;
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::restore_operations(state, install, "0.8.7", &why);
    OA_CHECK(why.empty());
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, operations, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    std::ifstream restored(install / "Contents" / "Info.plist");
    std::string body;
    std::getline(restored, body);
    OA_CHECK(body == "old");
    OA_CHECK(!fs::exists(kept));
    std::ifstream notes(scratch.path / "notes.txt");
    std::string note;
    std::getline(notes, note);
    OA_CHECK(note == "player");
    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    std::ifstream put_back(install / "Contents" / "Info.plist");
    std::getline(put_back, body);
    OA_CHECK(body == "new");
    std::ifstream kept_back(kept / "Contents" / "Info.plist");
    std::getline(kept_back, body);
    OA_CHECK(body == "old");
}

void a_callback_that_names_a_process_has_started() {
    helper::MacosLaunchSeen seen;
    seen.callback_finished = true;
    seen.callback_started = true;
    seen.callback_pid = 42;
    seen.application_listed = true;
    seen.listed_pid = 7;
    seen.version_confirmed = true;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::started);
    OA_CHECK(decision.pid == 42);
}

void a_listed_process_counts_before_the_callback() {
    helper::MacosLaunchSeen seen;
    seen.application_listed = true;
    seen.listed_pid = 77;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::started);
    OA_CHECK(decision.pid == 77);
}

void a_listed_process_wins_over_a_failed_callback() {
    helper::MacosLaunchSeen seen;
    seen.callback_finished = true;
    seen.callback_started = false;
    seen.application_listed = true;
    seen.listed_pid = 88;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::started);
    OA_CHECK(decision.pid == 88);
}

void a_confirmed_version_starts_with_no_process_id() {
    helper::MacosLaunchSeen seen;
    seen.version_confirmed = true;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::started);
    OA_CHECK(decision.pid == 0);
}

void a_confirmed_version_wins_over_a_failed_callback() {
    helper::MacosLaunchSeen seen;
    seen.callback_finished = true;
    seen.version_confirmed = true;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::started);
    OA_CHECK(decision.pid == 0);
}

void a_failed_callback_is_a_failed_launch() {
    helper::MacosLaunchSeen seen;
    seen.callback_finished = true;
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision(seen);
    OA_CHECK(decision.launch == helper::MacosLaunch::failed);
    OA_CHECK(decision.pid == 0);
}

void a_launch_with_nothing_seen_is_still_waiting() {
    const helper::MacosLaunchDecision decision = helper::macos_launch_decision({});
    OA_CHECK(decision.launch == helper::MacosLaunch::waiting);
    OA_CHECK(decision.pid == 0);
}

void a_callback_that_names_no_process_falls_through() {
    helper::MacosLaunchSeen seen;
    seen.callback_finished = true;
    seen.callback_started = true;
    seen.callback_pid = 0;
    seen.application_listed = true;
    seen.listed_pid = 15;
    const helper::MacosLaunchDecision listed = helper::macos_launch_decision(seen);
    OA_CHECK(listed.launch == helper::MacosLaunch::started);
    OA_CHECK(listed.pid == 15);

    seen.application_listed = false;
    seen.listed_pid = 0;
    seen.version_confirmed = true;
    const helper::MacosLaunchDecision confirmed = helper::macos_launch_decision(seen);
    OA_CHECK(confirmed.launch == helper::MacosLaunch::started);
    OA_CHECK(confirmed.pid == 0);

    seen.version_confirmed = false;
    const helper::MacosLaunchDecision waiting = helper::macos_launch_decision(seen);
    OA_CHECK(waiting.launch == helper::MacosLaunch::waiting);
    OA_CHECK(waiting.pid == 0);
}

void a_missing_bundle_is_refused() {
    const Scratch scratch("missing");
    LaidOut laid = lay_out(scratch);
    laid.plan.install = helper::utf8_of(scratch.path / "absent.app");
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the installed bundle is missing");
}

void an_install_the_player_can_move_needs_no_administrator() {
    helper::MacosSwapAccess access;
    access.parent_writable = true;
    access.install_movable = true;
    OA_CHECK(!helper::macos_swap_needs_elevation(access));
}

void each_move_the_player_cannot_make_asks() {
    helper::MacosSwapAccess folder;
    folder.parent_writable = false;
    folder.install_movable = true;
    OA_CHECK(helper::macos_swap_needs_elevation(folder));

    // The installer package's bundle: Applications takes the admin group's
    // writes, but the bundle is the administrator's.
    helper::MacosSwapAccess installed;
    installed.parent_writable = true;
    installed.install_movable = false;
    OA_CHECK(helper::macos_swap_needs_elevation(installed));

    helper::MacosSwapAccess kept;
    kept.parent_writable = true;
    kept.install_movable = true;
    kept.kept_movable = false;
    OA_CHECK(helper::macos_swap_needs_elevation(kept));
}

#if !defined(_WIN32)
/// A bundle whose own folder cannot be written cannot move into another
/// folder, even from a folder the player can write, as the installer
/// package's bundle cannot. The access read says so before anything is
/// tried, and the rename itself fails the same way.
void a_bundle_the_player_cannot_write_asks_for_an_administrator() {
    // The administrator may write every folder, so the refusal cannot be seen.
    if (::geteuid() == 0)
        return;
    const Scratch scratch("locked-bundle");
    const fs::path install = make_app(scratch.path / "Applications" / "Open Annihilation.app");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates / "previous");
    const fs::path kept = helper::macos_kept_bundle(updates, install);
    OA_CHECK(kept == updates / "previous" / "Open Annihilation.app");

    const helper::MacosSwapAccess open = helper::macos_swap_access(install, kept);
    OA_CHECK(open.parent_writable);
    OA_CHECK(open.install_movable);
    OA_CHECK(open.kept_movable);
    OA_CHECK(!helper::macos_swap_needs_elevation(open));

    std::error_code io;
    fs::permissions(
        install,
        fs::perms::owner_write | fs::perms::group_write | fs::perms::others_write,
        fs::perm_options::remove,
        io
    );
    OA_CHECK(!io);
    const helper::MacosSwapAccess locked = helper::macos_swap_access(install, kept);
    OA_CHECK(locked.parent_writable);
    OA_CHECK(!locked.install_movable);
    OA_CHECK(helper::macos_swap_needs_elevation(locked));
    fs::rename(install, kept, io);
    OA_CHECK(io);
    OA_CHECK(fs::is_directory(install));
    OA_CHECK(!fs::exists(kept));
    fs::permissions(install, fs::perms::owner_write, fs::perm_options::add, io);

    // A kept bundle the player cannot write asks too.
    make_app(kept);
    fs::permissions(kept, fs::perms::owner_write, fs::perm_options::remove, io);
    const helper::MacosSwapAccess old_kept = helper::macos_swap_access(install, kept);
    OA_CHECK(old_kept.install_movable);
    OA_CHECK(!old_kept.kept_movable);
    OA_CHECK(helper::macos_swap_needs_elevation(old_kept));
    fs::permissions(kept, fs::perms::owner_write, fs::perm_options::add, io);

    // A folder the player cannot write asks.
    fs::permissions(install.parent_path(), fs::perms::owner_write, fs::perm_options::remove, io);
    const helper::MacosSwapAccess folder = helper::macos_swap_access(install, kept);
    OA_CHECK(!folder.parent_writable);
    OA_CHECK(helper::macos_swap_needs_elevation(folder));
    fs::permissions(install.parent_path(), fs::perms::owner_write, fs::perm_options::add, io);
}
#endif

void the_prompt_runs_one_quoted_command() {
    const std::vector<std::string> command = helper::macos_elevation_command(
        "Open Annihilation wants to put 0.8.9 in place of the copy in Applications.",
        "/Applications/Open Annihilation.app/Contents/MacOS/oa-update",
        {"--plan", "/Users/player/it's here/plan.json", "--elevated-swap"}
    );
    OA_CHECK(command.size() >= 6);
    OA_CHECK(command.front() == "/usr/bin/osascript");
    std::string script;
    std::size_t at = 1;
    while (at + 1 < command.size() && command[at] == "-e") {
        script += command[at + 1];
        script += '\n';
        at += 2;
    }
    // The script quotes every value itself and runs one command with
    // administrator privileges; the values follow, the prompt first.
    OA_CHECK(script.find("quoted form of") != std::string::npos);
    OA_CHECK(script.find("with administrator privileges") != std::string::npos);
    OA_CHECK(script.find("with prompt (item 1 of argv)") != std::string::npos);
    OA_CHECK(script.find("echo $?") != std::string::npos);
    OA_CHECK(command.size() == at + 5);
    OA_CHECK(command[at].starts_with("Open Annihilation wants to put 0.8.9"));
    OA_CHECK(command[at + 1] == "/Applications/Open Annihilation.app/Contents/MacOS/oa-update");
    OA_CHECK(command[at + 2] == "--plan");
    OA_CHECK(command[at + 3] == "/Users/player/it's here/plan.json");
    OA_CHECK(command[at + 4] == "--elevated-swap");
}

void the_prompts_ending_is_read() {
    const helper::ElevatedRelaunch swapped = helper::macos_elevation_outcome(true, 0, "0\n", "");
    OA_CHECK(swapped.ran);
    OA_CHECK(!swapped.cancelled);
    OA_CHECK(swapped.code == helper::exit_swapped);

    const helper::ElevatedRelaunch undone = helper::macos_elevation_outcome(true, 0, "3", "");
    OA_CHECK(undone.ran);
    OA_CHECK(undone.code == helper::exit_undone);

    // Cancel, in whatever language the system names it.
    const helper::ElevatedRelaunch cancelled = helper::macos_elevation_outcome(
        true, 1, "", "0:120: execution error: Abgebrochen. (-128)\n"
    );
    OA_CHECK(cancelled.cancelled);
    OA_CHECK(!cancelled.ran);

    const helper::ElevatedRelaunch no_session = helper::macos_elevation_outcome(
        true, 1, "", "0:120: execution error: No user interaction allowed. (-60007)\n"
    );
    OA_CHECK(!no_session.ran);
    OA_CHECK(!no_session.cancelled);
    OA_CHECK(no_session.error.starts_with("the administrator's permission could not be asked"));
    OA_CHECK(no_session.error.find("(-60007)") != std::string::npos);

    const helper::ElevatedRelaunch not_started = helper::macos_elevation_outcome(false, 1, "", "");
    OA_CHECK(!not_started.ran);
    OA_CHECK(!not_started.cancelled);
    OA_CHECK(!not_started.error.empty());

    const helper::ElevatedRelaunch garbled = helper::macos_elevation_outcome(true, 0, "done", "");
    OA_CHECK(!garbled.ran);
    OA_CHECK(!garbled.error.empty());
}

void a_moved_bundle_belongs_where_it_lands() {
    const fs::path updates = "/Users/player/Library/Application Support/oa/updates";
    const fs::path install = "/Applications/Open Annihilation.app";
    OA_CHECK(
        helper::moved_tree_owner(
            updates, install, updates / "previous" / "Open Annihilation.app"
        ) == helper::MovedOwner::player
    );
    OA_CHECK(
        helper::moved_tree_owner(updates, install, updates / "journal-held" / "3") ==
        helper::MovedOwner::player
    );
    OA_CHECK(helper::moved_tree_owner(updates, install, install) == helper::MovedOwner::install);
    OA_CHECK(
        helper::moved_tree_owner(updates, install, "/Applications/Other.app") ==
        helper::MovedOwner::unchanged
    );
    OA_CHECK(
        helper::moved_tree_owner(updates, install, updates / ".." / "elsewhere") ==
        helper::MovedOwner::unchanged
    );
    OA_CHECK(helper::moved_tree_owner(updates, install, updates) == helper::MovedOwner::unchanged);
}

struct CountedRenames {
    int calls{0};
};

bool counted_rename(void* context, const fs::path& from, const fs::path& to) {
    ++static_cast<CountedRenames*>(context)->calls;
    std::error_code io;
    fs::rename(from, to, io);
    return !io;
}

/// The administrator's copy continues the journal the player's run began:
/// its moves go through the rename hook, and a failure undoes every step,
/// the player's too.
void the_administrators_moves_continue_the_players_journal() {
    const Scratch scratch("continued");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    const fs::path record = updates / "state.json";
    std::ofstream(record) << "before";
    helper::Operation write;
    write.kind = helper::Operation::Kind::write_file;
    write.path = record;
    write.bytes = "after";
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, {write}, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );

    const fs::path install = make_app(scratch.path / "Applications" / "Open Annihilation.app");
    const fs::path kept = updates / "previous" / "Open Annihilation.app";
    helper::Operation aside;
    aside.kind = helper::Operation::Kind::rename;
    aside.from = install;
    aside.to = kept;
    helper::Operation missing;
    missing.kind = helper::Operation::Kind::rename;
    missing.from = scratch.path / "stage" / "absent.app";
    missing.to = install;
    CountedRenames counted;
    helper::JournalHooks hooks;
    hooks.context = &counted;
    hooks.rename = counted_rename;
    OA_CHECK(
        helper::continue_journal(updates, {aside, missing}, hooks, &error) ==
        helper::JournalResult::failed
    );
    // The bundle moved aside, the missing one failed, the bundle moved back.
    OA_CHECK(counted.calls == 3);
    OA_CHECK(fs::is_directory(install));
    OA_CHECK(!fs::exists(kept));
    std::string text;
    {
        // Closed before the next journal writes the record, which Windows
        // refuses while the file is open.
        std::ifstream back(record);
        std::getline(back, text);
    }
    OA_CHECK(text == "before");
    OA_CHECK(!fs::exists(updates / "journal.json"));

    // A journal that finishes holds both runs' steps, numbered on.
    OA_CHECK(
        helper::apply_journal(updates, {write}, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(
        helper::continue_journal(updates, {aside}, hooks, &error) == helper::JournalResult::finished
    );
    OA_CHECK(fs::is_directory(kept));
    OA_CHECK(helper::undo_journal(updates, hooks, &error));
    OA_CHECK(fs::is_directory(install));
    std::ifstream undone(record);
    std::getline(undone, text);
    OA_CHECK(text == "before");
}

void the_game_starts_as_the_finder_starts_it() {
    update::Plan plan;
    plan.route = update::Route::macos_bundle;
    OA_CHECK(helper::start_way(plan, false) == helper::StartWay::open_bundle);
    // The game never starts with an administrator's rights.
    OA_CHECK(helper::start_way(plan, true) == helper::StartWay::refused);
    plan.route = update::Route::windows_folder;
    OA_CHECK(helper::start_way(plan, false) == helper::StartWay::program);
    plan.route = update::Route::linux_versions;
    OA_CHECK(helper::start_way(plan, false) == helper::StartWay::program);
    plan.route = update::Route::check_folder;
    OA_CHECK(helper::start_way(plan, false) == helper::StartWay::program);
}

} // namespace

void test_macos_route() {
    the_same_team_may_swap();
    another_team_is_refused();
    an_invalid_signature_is_refused();
    a_bundle_that_is_not_notarized_is_refused();
    an_ad_hoc_release_is_refused();
    an_ad_hoc_self_check_may_swap();
    a_mixed_ad_hoc_signature_is_refused();
    an_invalid_ad_hoc_self_check_is_refused();
    the_same_volume_renames();
    a_cross_volume_move_copies_both_ways();
    an_older_kept_bundle_is_removed_first();
    the_installer_is_opened();
    a_missing_installer_is_refused();
    two_applications_are_refused();
    a_missing_bundle_is_refused();
    restoring_moves_the_kept_bundle_back();
    a_callback_that_names_a_process_has_started();
    a_listed_process_counts_before_the_callback();
    a_listed_process_wins_over_a_failed_callback();
    a_confirmed_version_starts_with_no_process_id();
    a_confirmed_version_wins_over_a_failed_callback();
    a_failed_callback_is_a_failed_launch();
    a_launch_with_nothing_seen_is_still_waiting();
    a_callback_that_names_no_process_falls_through();
    an_install_the_player_can_move_needs_no_administrator();
    each_move_the_player_cannot_make_asks();
#if !defined(_WIN32)
    a_bundle_the_player_cannot_write_asks_for_an_administrator();
#endif
    the_prompt_runs_one_quoted_command();
    the_prompts_ending_is_read();
    a_moved_bundle_belongs_where_it_lands();
    the_administrators_moves_continue_the_players_journal();
    the_game_starts_as_the_finder_starts_it();
}
