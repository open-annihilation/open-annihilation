// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The receipt the helper writes beside its log, a roll back's outcome in
// it, and how the watch reads the end of a start: reached the main menu,
// quit by the player, failed, or not seen. No program is started.

#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/log.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/run.hpp"
#include "oa/update_helper/watch.hpp"
#include "oa/update_helper/window.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace helper = oa::update_helper;
namespace update = oa::app::update;

update::Receipt read_back(const fs::path& updates) {
    const std::optional<update::Receipt> loaded =
        update::load_receipt(updates / std::string(update::receipt_file_name));
    OA_CHECK(loaded.has_value());
    return loaded.value_or(update::Receipt{});
}

void the_log_writes_each_step() {
    const fs::path updates = oa::test::make_scratch_directory("helper-receipt");
    helper::Log log;
    std::string error;
    OA_CHECK(log.open(updates, &error));
    // Without a receipt, steps write nothing.
    log.step(update::Step::verify, update::StepResult::done);
    log.finish(update::Outcome::installed);
    OA_CHECK(!fs::exists(updates / std::string(update::receipt_file_name)));
    OA_CHECK(!log.receipt_running());

    update::Receipt begun;
    begun.action = update::Action::install;
    begun.from_version = "0.0.1";
    begun.release = 2;
    log.begin_receipt(updates, begun);
    OA_CHECK(log.receipt_running());
    update::Receipt read = read_back(updates);
    OA_CHECK(read.outcome == update::Outcome::running);
    OA_CHECK(read.started_at > 0);
    OA_CHECK(read.steps.empty());
    OA_CHECK(read.from_version == "0.0.1");

    log.step(update::Step::verify, update::StepResult::done);
    log.set_to_version("0.0.2");
    log.line("the install folder is busy");
    log.step(update::Step::swap, update::StepResult::failed, log.last_line());
    // Each step is on disk before the next: a run cut short keeps them.
    read = read_back(updates);
    OA_CHECK(read.outcome == update::Outcome::running);
    OA_CHECK(read.steps.size() == 2U);
    OA_CHECK(read.to_version == "0.0.2");
    OA_CHECK(!read.steps.empty() && read.steps.back().detail == "the install folder is busy");

    log.finish(update::Outcome::failed_restored);
    OA_CHECK(!log.receipt_running());
    read = read_back(updates);
    OA_CHECK(read.outcome == update::Outcome::failed_restored);
    OA_CHECK(read.finished_at >= read.started_at);
    OA_CHECK(!read.shown);
    std::error_code removed;
    fs::remove_all(updates, removed);
}

/// A plain folder with a kept version and a state that names it.
struct KeptFolder {
    fs::path root;
    fs::path install;
    fs::path updates;
    fs::path kept;
    update::Plan plan;
};

KeptFolder kept_folder(std::string_view name, update::Action action) {
    KeptFolder folder;
    folder.root = oa::test::make_scratch_directory(name);
    folder.install = folder.root / "install";
    folder.updates = folder.root / "updates";
    folder.kept = folder.updates / "previous";
    fs::create_directories(folder.install);
    fs::create_directories(folder.kept);
    std::ofstream(folder.install / "one.txt") << "new";
    std::ofstream(folder.kept / "one.txt") << "old";
    std::ofstream(folder.kept / "preferences.conf") << "open-annihilation-preferences 1\n";
    std::ofstream(folder.root / "preferences.conf") << "open-annihilation-preferences 1\n";
    update::State state;
    state.installed_version = "0.0.2";
    state.installed_release = 2;
    state.kept_version = "0.0.1";
    state.kept_path = helper::utf8_of(folder.kept);
    state.kept_route = update::Route::check_folder;
    state.preferences_copy = helper::utf8_of(folder.kept / "preferences.conf");
    std::string error;
    OA_CHECK(update::write_state(folder.updates / "state.json", state, &error));
    folder.plan.action = action;
    folder.plan.roll_back_reason = action == update::Action::roll_back
                                       ? update::RollBackReason::player
                                       : update::RollBackReason::none;
    folder.plan.route = update::Route::check_folder;
    folder.plan.install = helper::utf8_of(folder.install);
    folder.plan.program = helper::utf8_of(folder.install / "program");
    folder.plan.from_version = "0.0.2";
    folder.plan.feed = helper::utf8_of(folder.updates / "feed.json");
    folder.plan.signature = helper::utf8_of(folder.updates / "feed.json.sig");
    folder.plan.release = 2;
    folder.plan.package = "kept";
    folder.plan.file = helper::utf8_of(folder.updates / "kept.pkg");
    folder.plan.preferences = helper::utf8_of(folder.root / "preferences.conf");
    folder.plan.updates = helper::utf8_of(folder.updates);
    return folder;
}

void a_roll_back_records_its_outcome() {
    KeptFolder folder = kept_folder("helper-roll-back", update::Action::roll_back);
    helper::Log log;
    std::string error;
    OA_CHECK(log.open(folder.updates, &error));
    update::Receipt begun;
    begun.action = update::Action::roll_back;
    begun.reason = update::RollBackReason::player;
    begun.from_version = "0.0.2";
    begun.release = 2;
    log.begin_receipt(folder.updates, begun);
    helper::ProgressWindow window;
    helper::JournalHooks hooks;
    const int code = helper::restore_kept(
        folder.plan,
        folder.updates,
        nullptr,
        log,
        window,
        hooks,
        update::RollBackReason::player,
        false,
        false
    );
    OA_CHECK(code == helper::exit_swapped);
    const update::Receipt read = read_back(folder.updates);
    OA_CHECK(read.outcome == update::Outcome::rolled_back);
    OA_CHECK(read.to_version == "0.0.1");
    OA_CHECK(!read.steps.empty() && read.steps.back().step == update::Step::restore);
    OA_CHECK(!read.steps.empty() && read.steps.back().result == update::StepResult::done);
    std::error_code removed;
    fs::remove_all(folder.root, removed);
}

void a_roll_back_that_cannot_read_the_state_changed_nothing() {
    KeptFolder folder = kept_folder("helper-roll-back-missing", update::Action::roll_back);
    fs::remove(folder.updates / "state.json");
    helper::Log log;
    std::string error;
    OA_CHECK(log.open(folder.updates, &error));
    update::Receipt begun;
    begun.action = update::Action::roll_back;
    begun.reason = update::RollBackReason::player;
    begun.from_version = "0.0.2";
    log.begin_receipt(folder.updates, begun);
    helper::ProgressWindow window;
    helper::JournalHooks hooks;
    const int code = helper::restore_kept(
        folder.plan,
        folder.updates,
        nullptr,
        log,
        window,
        hooks,
        update::RollBackReason::player,
        false,
        false
    );
    OA_CHECK(code == helper::exit_refused);
    const update::Receipt read = read_back(folder.updates);
    OA_CHECK(read.outcome == update::Outcome::roll_back_failed);
    OA_CHECK(!read.steps.empty() && read.steps.back().result == update::StepResult::failed);
    std::error_code removed;
    fs::remove_all(folder.root, removed);
}

void failed_starts_that_cannot_roll_back_leave_recovery_unfinished() {
    KeptFolder folder = kept_folder("helper-after-install", update::Action::install);
    fs::remove(folder.updates / "state.json");
    helper::Log log;
    std::string error;
    OA_CHECK(log.open(folder.updates, &error));
    update::Receipt begun;
    begun.action = update::Action::install;
    begun.from_version = "0.0.1";
    log.begin_receipt(folder.updates, begun);
    log.finish(update::Outcome::installed);
    helper::ProgressWindow window;
    helper::JournalHooks hooks;
    const int code = helper::restore_kept(
        folder.plan,
        folder.updates,
        nullptr,
        log,
        window,
        hooks,
        update::RollBackReason::failed_starts,
        false,
        false
    );
    OA_CHECK(code == helper::exit_refused);
    // The new version was in place and could not be taken back.
    OA_CHECK(read_back(folder.updates).outcome == update::Outcome::recovery_incomplete);
    std::error_code removed;
    fs::remove_all(folder.root, removed);
}

void the_watch_reads_how_a_start_ended() {
    OA_CHECK(helper::ended_watch(std::nullopt) == helper::WatchEnd::ended);
    update::State state;
    state.installed_version = "0.0.2";
    state.failed_starts = 1;
    OA_CHECK(helper::ended_watch(state) == helper::WatchEnd::ended);
    update::uncount_quit_start(state);
    OA_CHECK(helper::ended_watch(state) == helper::WatchEnd::quit);
    update::confirm_state(state);
    OA_CHECK(helper::ended_watch(state) == helper::WatchEnd::confirmed);

    // A start with no process and no pid cannot be seen, and is not a failure.
    const fs::path folder = oa::test::make_scratch_directory("helper-watch");
    const fs::path state_path = folder / "state.json";
    OA_CHECK(helper::watch_until_settled(nullptr, 0, state_path) == helper::WatchEnd::unobserved);
    OA_CHECK(
        helper::watch_two_starts(nullptr, 0, state_path, nullptr, nullptr, nullptr) ==
        helper::WatchTwo::unobserved
    );
    std::error_code removed;
    fs::remove_all(folder, removed);
}

} // namespace

void test_receipts() {
    the_log_writes_each_step();
    a_roll_back_records_its_outcome();
    a_roll_back_that_cannot_read_the_state_changed_nothing();
    failed_starts_that_cannot_roll_back_leave_recovery_unfinished();
    the_watch_reads_how_a_start_ended();
}
