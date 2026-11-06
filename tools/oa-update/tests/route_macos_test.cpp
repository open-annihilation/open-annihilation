// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS swap, decided from signatures a test fills in, and the
// operations that follow; when the moves need an administrator's rights,
// how macOS is asked for them, and whose a moved bundle becomes; that the
// bundle put in place is opened to every account and that the player must
// be able to start it, or the swap is undone; and how the game is started
// again. Nothing here signs a bundle, asks for a password or starts a
// program. The administrator cannot be the owner in a test, so another
// account is the player instead.

#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/run.hpp"
#include "oa/update_helper/unpack.hpp"

#include "oa/app/update/plan.hpp"
#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/test/raw_zip.hpp"
#include "oa/update_helper/paths.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
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

void a_tree_opened_to_every_account_is_read_and_run_by_all() {
    // Readable by everyone; runnable by everyone when it is a folder or some
    // account could already run it.
    OA_CHECK(helper::opened_mode(0600, false) == 0644);
    OA_CHECK(helper::opened_mode(0644, false) == 0644);
    OA_CHECK(helper::opened_mode(0700, false) == 0755);
    OA_CHECK(helper::opened_mode(0744, false) == 0755);
    OA_CHECK(helper::opened_mode(0710, false) == 0755);
    OA_CHECK(helper::opened_mode(0700, true) == 0755);
    OA_CHECK(helper::opened_mode(0500, true) == 0555);
    // No set-user, set-group or sticky bit, and nothing the group or others may write.
    OA_CHECK(helper::opened_mode(04755, false) == 0755);
    OA_CHECK(helper::opened_mode(02777, false) == 0755);
    OA_CHECK(helper::opened_mode(0666, false) == 0644);
    OA_CHECK(helper::opened_mode(01777, true) == 0755);
}

void the_first_class_that_names_the_account_decides() {
    helper::PlayerAccount player;
    player.user = 501;
    player.group = 20;
    player.groups = {12, 80};
    const uint32_t read_and_run = helper::access_read | helper::access_execute;
    // What 0.8.10 left: the administrator's program, run by its owner alone.
    OA_CHECK(!helper::mode_allows(0744, 0, 0, player, helper::access_execute));
    OA_CHECK(helper::mode_allows(0744, 0, 0, player, helper::access_read));
    OA_CHECK(helper::mode_allows(0755, 0, 0, player, read_and_run));
    // The player's own file follows the owner's bits.
    OA_CHECK(helper::mode_allows(0744, 501, 0, player, read_and_run));
    OA_CHECK(!helper::mode_allows(0055, 501, 20, player, helper::access_read));
    // A group the player is in follows the group's bits, even where others may.
    OA_CHECK(!helper::mode_allows(0705, 0, 80, player, helper::access_read));
    OA_CHECK(helper::mode_allows(0750, 0, 20, player, read_and_run));
    OA_CHECK(helper::mode_allows(0750, 0, 12, player, read_and_run));
    OA_CHECK(!helper::mode_allows(0750, 0, 0, player, helper::access_read));
}

#if !defined(_WIN32)
/// Gives `path` exactly the permission bits `mode`.
void set_mode(const fs::path& path, uint32_t mode) {
    std::error_code io;
    fs::permissions(path, static_cast<fs::perms>(mode), fs::perm_options::replace, io);
    OA_CHECK(!io);
}

/// The permission bits of a file or folder, without its type. Only the macOS
/// checks read it.
[[maybe_unused]] uint32_t mode_of(const fs::path& path) {
    struct stat info{};
    OA_CHECK(::lstat(path.c_str(), &info) == 0);
    return static_cast<uint32_t>(info.st_mode & 07777);
}

/// An account that neither owns `path` nor is in its group: the player of
/// a copy the administrator owns.
helper::PlayerAccount another_account(const fs::path& path) {
    struct stat info{};
    OA_CHECK(::lstat(path.c_str(), &info) == 0);
    helper::PlayerAccount account;
    account.user = static_cast<uint32_t>(info.st_uid) + 1;
    account.group = static_cast<uint32_t>(info.st_gid) + 1;
    return account;
}

/// A bundle laid out as the release's: Info.plist, the game and the helper,
/// folders 0755, the programs 0755 and Info.plist 0644.
fs::path make_release_app(const fs::path& path, std::string_view plist) {
    const fs::path macos = path / "Contents" / "MacOS";
    fs::create_directories(macos);
    std::ofstream(path / "Contents" / "Info.plist") << plist;
    std::ofstream(macos / "open-annihilation") << "game";
    std::ofstream(macos / "oa-update") << "helper";
    set_mode(path, 0755);
    set_mode(path / "Contents", 0755);
    set_mode(macos, 0755);
    set_mode(path / "Contents" / "Info.plist", 0644);
    set_mode(macos / "open-annihilation", 0755);
    set_mode(macos / "oa-update", 0755);
    return path;
}

std::string first_line(const fs::path& path) {
    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    return line;
}

void the_player_must_be_able_to_start_the_bundle() {
    const Scratch scratch("can-run");
    const fs::path app = make_release_app(scratch.path / "Open Annihilation.app", "new");
    const fs::path game = app / "Contents" / "MacOS" / "open-annihilation";
    const helper::PlayerAccount self = helper::effective_account();
    const helper::PlayerAccount other = another_account(app);
    std::string why = "uncleared";
    OA_CHECK(helper::macos_player_can_run(app, self, &why));
    OA_CHECK(helper::macos_player_can_run(app, other, &why));

    // Run by its owner alone: its owner may start it, another account not.
    set_mode(game, 0744);
    OA_CHECK(helper::macos_player_can_run(app, self, &why));
    OA_CHECK(!helper::macos_player_can_run(app, other, &why));
    OA_CHECK(why == "Contents/MacOS/open-annihilation cannot be run by the player");
    set_mode(game, 0700);
    OA_CHECK(!helper::macos_player_can_run(app, other, &why));
    OA_CHECK(why == "Contents/MacOS/open-annihilation cannot be read by the player");
    set_mode(game, 0755);

    const fs::path plist = app / "Contents" / "Info.plist";
    set_mode(plist, 0600);
    OA_CHECK(!helper::macos_player_can_run(app, other, &why));
    OA_CHECK(why == "Contents/Info.plist cannot be read by the player");
    set_mode(plist, 0644);

    set_mode(app / "Contents" / "MacOS", 0700);
    OA_CHECK(!helper::macos_player_can_run(app, other, &why));
    OA_CHECK(why == "Contents/MacOS cannot be opened by the player");
    set_mode(app / "Contents" / "MacOS", 0755);

    // A missing program, or something else in its place, refuses for every account.
    fs::remove(app / "Contents" / "MacOS" / "oa-update");
    OA_CHECK(!helper::macos_player_can_run(app, self, &why));
    OA_CHECK(why == "Contents/MacOS/oa-update is missing");
    fs::create_directories(app / "Contents" / "MacOS" / "oa-update");
    OA_CHECK(!helper::macos_player_can_run(app, self, &why));
    OA_CHECK(why == "Contents/MacOS/oa-update is not a file");
    fs::remove(app / "Contents" / "MacOS" / "oa-update");
    fs::remove(game);
    OA_CHECK(!helper::macos_player_can_run(app, self, &why));
    OA_CHECK(why == "Contents/MacOS/open-annihilation is missing");
    OA_CHECK(!helper::macos_player_can_run(scratch.path / "absent.app", self, &why));
    OA_CHECK(why == "absent.app is missing");
}

struct SwapLaidOut {
    fs::path install;
    fs::path updates;
    fs::path application;
    update::Plan plan;
};

/// An installed bundle and a staged one under the updates folder, as an
/// install finds them.
SwapLaidOut lay_out_swap(const Scratch& scratch) {
    SwapLaidOut laid;
    laid.install = make_release_app(scratch.path / "Applications" / "Open Annihilation.app", "old");
    laid.updates = scratch.path / "updates";
    const fs::path stage = laid.updates / "2" / "stage";
    laid.application = make_release_app(
        stage / "open-annihilation-v0.8.12-macos-universal" / "Open Annihilation.app", "new"
    );
    fs::create_directories(laid.updates / "previous");
    laid.plan.route = update::Route::macos_bundle;
    laid.plan.install = helper::utf8_of(laid.install);
    laid.plan.updates = helper::utf8_of(laid.updates);
    return laid;
}

/// Swaps the staged bundle in under a journal, as an install does.
void swap_in(const SwapLaidOut& laid) {
    std::string why;
    const std::vector<helper::Operation> operations = helper::macos_bundle_operations(
        laid.plan, laid.application.parent_path().parent_path(), helper::MacosVolume::same, &why
    );
    OA_CHECK(why.empty());
    std::string error;
    OA_CHECK(
        helper::apply_journal(laid.updates, operations, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(first_line(laid.install / "Contents" / "Info.plist") == "new");
}

/// The check before an install counts as done: a game the player cannot
/// run is not kept, and the version that was there comes back.
void a_bundle_the_player_cannot_start_is_put_back() {
    const Scratch scratch("put-back");
    const SwapLaidOut laid = lay_out_swap(scratch);
    // Run by its owner alone, and the owner is not the player.
    set_mode(laid.application / "Contents" / "MacOS" / "open-annihilation", 0744);
    const helper::PlayerAccount player = another_account(laid.application);
    swap_in(laid);
    std::string why;
    OA_CHECK(!helper::keep_swap_the_player_can_run(
        laid.updates, laid.install, player, helper::JournalHooks{}, &why
    ));
    OA_CHECK(why.starts_with(helper::player_cannot_run));
    OA_CHECK(
        why.find("Contents/MacOS/open-annihilation cannot be run by the player") !=
        std::string::npos
    );
    OA_CHECK(first_line(laid.install / "Contents" / "Info.plist") == "old");
    OA_CHECK(first_line(laid.application / "Contents" / "Info.plist") == "new");
    OA_CHECK(!fs::exists(helper::macos_kept_bundle(laid.updates, laid.install)));
    OA_CHECK(!fs::exists(laid.updates / "journal.json"));
    OA_CHECK(helper::line_after_failed_change(laid.updates) == helper::update_failed_put_back);
}

void a_bundle_the_player_can_start_is_kept() {
    const Scratch scratch("kept-swap");
    const SwapLaidOut laid = lay_out_swap(scratch);
    const helper::PlayerAccount player = another_account(laid.application);
    swap_in(laid);
    std::string why = "uncleared";
    OA_CHECK(
        helper::keep_swap_the_player_can_run(
            laid.updates, laid.install, player, helper::JournalHooks{}, &why
        )
    );
    OA_CHECK(why == "uncleared");
    OA_CHECK(first_line(laid.install / "Contents" / "Info.plist") == "new");
    // The journal is left for the run, which drops it once the game has started.
    OA_CHECK(fs::exists(laid.updates / "journal.json"));
}

/// An entry made on Unix that records `mode`.
oa::test::raw_zip::RawFile unix_entry(std::string name, std::string_view text, uint32_t mode) {
    oa::test::raw_zip::RawFile file = oa::test::raw_zip::stored_text(std::move(name), text);
    file.host = 3;
    file.external_attributes = mode << 16U;
    return file;
}

void write_zip(const fs::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    OA_CHECK(static_cast<bool>(out));
}

/// Hands the tree to an owner as the copy with an administrator's rights
/// does once it is in the install's place. The test keeps the tree's own
/// owner: it cannot give it to the administrator.
void hand_to_install_owner(const fs::path& app) {
#if defined(__APPLE__)
    struct stat info{};
    OA_CHECK(::lstat(app.c_str(), &info) == 0);
    OA_CHECK(
        helper::hand_over_tree(
            app, static_cast<uint32_t>(info.st_uid), static_cast<uint32_t>(info.st_gid), true
        )
    );
#else
    (void)app;
#endif
}

/// The update that left the player unable to start the game: the release
/// zip unpacked, the bundle handed to the install's owner, who is not the
/// player, and the player then had to start it.
void a_release_installed_for_another_owner_starts_for_the_player() {
    const Scratch scratch("another-owner");
    constexpr std::string_view top = "open-annihilation-v0.8.12-macos-universal";
    const std::vector<std::string> tops = helper::top_folder_names("0.8.12", "macos-universal");
    const std::string app_name = std::string(top) + "/Open Annihilation.app/";
    const std::vector<std::string> names = {
        app_name + "Contents/Info.plist",
        app_name + "Contents/MacOS/open-annihilation",
        app_name + "Contents/MacOS/oa-update",
    };
    const fs::path stage = scratch.path / "stage";
    const fs::path app = stage / helper::path_from(app_name);

    // As the release zips record them: folders and programs 0755, the rest 0644.
    const fs::path recorded = scratch.path / "recorded.zip";
    write_zip(
        recorded,
        oa::test::raw_zip::build_raw_archive(
            {
                unix_entry(std::string(top) + "/", "", 040755),
                unix_entry(app_name, "", 040755),
                unix_entry(app_name + "Contents/", "", 040755),
                unix_entry(names[0], "plist", 0100644),
                unix_entry(app_name + "Contents/MacOS/", "", 040755),
                unix_entry(names[1], "game", 0100755),
                unix_entry(names[2], "helper", 0100755),
            }
        )
            .bytes
    );
    std::string error;
    OA_CHECK(helper::unpack_package(recorded, stage, tops, &error));
    const helper::PlayerAccount player = another_account(app);
    std::string why;
    OA_CHECK(helper::macos_player_can_run(app, player, &why));
    hand_to_install_owner(app);
    OA_CHECK(helper::macos_player_can_run(app, player, &why));
    std::error_code removed;
    fs::remove_all(stage, removed);

    // A zip that records no modes.
    std::vector<uint8_t> plain_bytes;
    std::vector<oa::formats::zip::NewEntry> entries;
    const std::vector<uint8_t> body = {'x'};
    for (const std::string& name : names)
        entries.push_back({name, body});
    oa::formats::zip::ZipError zip_error;
    OA_CHECK(oa::formats::zip::write_archive(entries, plain_bytes, zip_error));
    const fs::path plain = scratch.path / "plain.zip";
    write_zip(plain, plain_bytes);
    OA_CHECK(helper::unpack_package(plain, stage, tops, &error));
    OA_CHECK(helper::macos_player_can_run(app, player, &why));
    hand_to_install_owner(app);
    OA_CHECK(helper::macos_player_can_run(app, player, &why));

    // The programs as 0.8.10 left them, run by their owner alone: refused,
    // until the copy in the install's place is opened to every account.
    set_mode(app / "Contents" / "MacOS" / "open-annihilation", 0744);
    set_mode(app / "Contents" / "MacOS" / "oa-update", 0744);
    OA_CHECK(!helper::macos_player_can_run(app, player, &why));
#if defined(__APPLE__)
    hand_to_install_owner(app);
    OA_CHECK(helper::macos_player_can_run(app, player, &why));
#endif
}
#endif

#if defined(__APPLE__)
/// The bundle put in place is handed to its owner and opened to every
/// account, whatever modes it arrived with.
void the_bundle_put_in_place_is_opened_to_every_account() {
    const Scratch scratch("opened");
    const fs::path app = scratch.path / "Open Annihilation.app";
    const fs::path contents = app / "Contents";
    const fs::path macos = contents / "MacOS";
    fs::create_directories(macos);
    fs::create_directories(contents / "Resources");
    std::ofstream(contents / "Info.plist") << "plist";
    std::ofstream(macos / "open-annihilation") << "game";
    std::ofstream(macos / "oa-update") << "helper";
    std::ofstream(contents / "Resources" / "raised") << "raised";
    std::ofstream(contents / "Resources" / "shared") << "shared";
    std::ofstream(scratch.path / "elsewhere") << "elsewhere";
    std::error_code io;
    fs::create_hard_link(scratch.path / "elsewhere", contents / "Resources" / "linked", io);
    OA_CHECK(!io);
    set_mode(contents / "Info.plist", 0600);
    set_mode(macos / "open-annihilation", 0744);
    set_mode(macos / "oa-update", 0700);
    set_mode(contents / "Resources" / "raised", 04755);
    set_mode(contents / "Resources" / "shared", 0666);
    set_mode(scratch.path / "elsewhere", 0600);
    set_mode(contents / "Resources", 0700);
    set_mode(macos, 0700);
    set_mode(contents, 0700);
    set_mode(app, 0700);
    struct stat info{};
    OA_CHECK(::lstat(app.c_str(), &info) == 0);
    const auto user = static_cast<uint32_t>(info.st_uid);
    const auto group = static_cast<uint32_t>(info.st_gid);

    // Handed to the player, a tree keeps its modes.
    OA_CHECK(helper::hand_over_tree(app, user, group, false));
    OA_CHECK(mode_of(app) == 0700);
    OA_CHECK(mode_of(macos / "open-annihilation") == 0744);

    OA_CHECK(helper::hand_over_tree(app, user, group, true));
    OA_CHECK(mode_of(app) == 0755);
    OA_CHECK(mode_of(contents) == 0755);
    OA_CHECK(mode_of(macos) == 0755);
    OA_CHECK(mode_of(contents / "Resources") == 0755);
    OA_CHECK(mode_of(contents / "Info.plist") == 0644);
    OA_CHECK(mode_of(macos / "open-annihilation") == 0755);
    OA_CHECK(mode_of(macos / "oa-update") == 0755);
    OA_CHECK(mode_of(contents / "Resources" / "raised") == 0755);
    OA_CHECK(mode_of(contents / "Resources" / "shared") == 0644);
    // A file with another name outside the tree is left as it is.
    OA_CHECK(mode_of(contents / "Resources" / "linked") == 0600);
    OA_CHECK(helper::macos_player_can_run(app, another_account(app), nullptr));
}
#endif

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
    a_tree_opened_to_every_account_is_read_and_run_by_all();
    the_first_class_that_names_the_account_decides();
#if !defined(_WIN32)
    the_player_must_be_able_to_start_the_bundle();
    a_bundle_the_player_cannot_start_is_put_back();
    a_bundle_the_player_can_start_is_kept();
    a_release_installed_for_another_owner_starts_for_the_player();
#endif
#if defined(__APPLE__)
    the_bundle_put_in_place_is_opened_to_every_account();
#endif
    the_game_starts_as_the_finder_starts_it();
}
