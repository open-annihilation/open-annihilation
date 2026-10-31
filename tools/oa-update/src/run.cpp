// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/run.hpp"

#include "oa/app/update/offer.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/app/update/route.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/lock.hpp"
#include "oa/update_helper/log.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/route_windows.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/start.hpp"
#include "oa/update_helper/unpack.hpp"
#include "oa/update_helper/wait.hpp"
#include "oa/update_helper/watch.hpp"
#include "oa/update_helper/window.hpp"

#include <SDL3/SDL.h>

#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

struct Note {
    Log* log{};
};

void note_line(void* context, std::string_view line) {
    if (context != nullptr)
        static_cast<Note*>(context)->log->line(line);
}

bool read_capped(
    const fs::path& path, std::size_t most, std::vector<uint8_t>& out, std::string& error
) {
    std::error_code io;
    if (!fs::is_regular_file(path, io) || io) {
        error = "the plan could not be read";
        return false;
    }
    const auto size = fs::file_size(path, io);
    if (io || size > most) {
        error = size > most ? "the plan is too large" : "the plan could not be read";
        return false;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "the plan could not be read";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    if (!out.empty())
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    if (!in) {
        error = "the plan could not be read";
        return false;
    }
    return true;
}

/// True while a journal is left under `updates`: an undo that did not finish.
bool journal_left(const fs::path& updates) {
    std::error_code io;
    return fs::is_regular_file(updates / "journal.json", io) && !io;
}

/// Records a failed step that changed files, then how its undo went, and the outcome.
///
/// @param log the log and its receipt
/// @param updates the data folder's updates directory
/// @param failed the step that failed
/// @param detail why it failed
void record_failed_change(
    Log& log, const fs::path& updates, update::Step failed, std::string_view detail
) {
    log.step(failed, update::StepResult::failed, detail);
    const bool undone = !journal_left(updates);
    log.step(update::Step::undo, undone ? update::StepResult::done : update::StepResult::failed);
    log.finish(update::failure_outcome(log.receipt_action(), true, undone));
}

/// Starts the version that was running, after a refusal or a failure.
///
/// A receipt that has no outcome yet stopped before any file moved, and says
/// so before the game starts.
int start_old(const update::Plan& plan, Log& log, ProgressWindow& window, bool start) {
    if (log.receipt_running())
        log.finish(update::failure_outcome(log.receipt_action(), false, true));
    if (start) {
        std::string error;
        if (!start_detached(path_from(plan.program), plan.arguments, false, {}, &error))
            log.line(std::string("could not start the game: ") + error);
    }
    window.close();
    return 0;
}

void remove_stage(const fs::path& stage) {
    std::error_code io;
    fs::remove_all(stage, io);
}

void drop_journal(const fs::path& updates) {
    std::error_code io;
    fs::remove(updates / "journal.json", io);
    fs::remove_all(updates / "journal-held", io);
}

/// The Deck sentence, with {version} filled. A line past the window's limit is cut.
///
/// @param version the release being installed
/// @return the line
std::string next_start_line(std::string_view version) {
    std::string line(update::installs_next_start);
    const std::string token = "{version}";
    const std::string::size_type at = line.find(token);
    if (at != std::string::npos)
        line.replace(at, token.size(), version.data(), version.size());
    if (line.size() > update::max_window_line_bytes)
        line.resize(update::max_window_line_bytes);
    return line;
}

/// True on a Steam Deck, or while Steam is in Game Mode.
///
/// @return true when the helper must not start the game
bool deck_session() {
    return oa::platform::running_steam_deck_model() != oa::platform::SteamDeckModel::none ||
           oa::platform::running_in_steam_game_mode();
}

bool carry_run(
    void*, const fs::path& program, const std::vector<std::string>& arguments, std::string* error
) {
    const std::string name = utf8_of(program.filename());
    const std::string whole = utf8_of(program);
    if (name == "oa-display-version" || whole == "oa-display-version") {
        if (arguments.empty()) {
            if (error != nullptr)
                *error = "the kept version is missing";
            return false;
        }
        return apply_display_version(program, arguments, error);
    }
    return run_and_wait(program, arguments, error);
}

bool file_there(const fs::path& path) {
    std::error_code io;
    return fs::exists(fs::symlink_status(path, io)) && !io;
}

void write_update_starts(const fs::path& install) {
    std::ofstream out(install / "update-starts", std::ios::binary | std::ios::trunc);
    if (out)
        out << "0\n";
}

void remove_update_starts(const fs::path& install) {
    std::error_code io;
    fs::remove(install / "update-starts", io);
}

std::string read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

/// The preferences bytes a roll back writes: the copy, plus the bad release
/// skipped, when the copy is a preferences file. A copy that cannot be read
/// is put back unchanged.
std::string preferences_after_roll_back(const fs::path& copy, int64_t bad_release, Log& log) {
    const std::string raw = read_bytes(copy);
    try {
        oa::platform::preferences::Values values = oa::platform::preferences::load(copy);
        update::write_skipped(values, bad_release);
        const fs::path temporary = copy.parent_path() / "preferences-skipped.tmp";
        oa::platform::preferences::save(
            temporary, values, oa::platform::preferences::SyncFolder::no
        );
        const std::string skipped = read_bytes(temporary);
        std::error_code io;
        fs::remove(temporary, io);
        if (skipped.empty()) {
            log.line("the preferences copy could not be read");
            return raw;
        }
        return skipped;
    } catch (const std::exception& failure) {
        log.line(std::string("the preferences copy could not be read: ") + failure.what());
        return raw;
    }
}

std::string going_back_title(std::string_view version) {
    std::string title = "Going back to ";
    title.append(version);
    if (title.size() > update::max_window_line_bytes)
        title.resize(update::max_window_line_bytes);
    return title;
}

std::optional<update::State> load_state(const fs::path& updates, std::string& error) {
    std::vector<uint8_t> bytes;
    if (!read_capped(updates / "state.json", update::max_plan_bytes, bytes, error))
        return std::nullopt;
    return update::read_state(bytes, &error);
}

struct Started {
    SDL_Process* process{};
    int pid{};
    bool ok{false};
};

Started start_version(
    const update::Plan& plan,
    const std::vector<std::string>& arguments,
    bool add_updated_from,
    std::string_view from_version,
    std::string_view expect_version,
    std::string& error
) {
    Started started;
    const bool bundle = plan.route == update::Route::macos_bundle;
    const bool linux = plan.route == update::Route::linux_versions;
    if (bundle) {
#if defined(__APPLE__)
        started.ok = start_macos_bundle(
            path_from(plan.install),
            arguments,
            add_updated_from ? from_version : std::string_view{},
            path_from(plan.updates) / "state.json",
            expect_version,
            &error,
            &started.pid
        );
#else
        (void)expect_version;
        error = "this route is not carried out";
#endif
        return started;
    }
    (void)expect_version;
    const fs::path program =
        linux ? path_from(plan.install) / "open-annihilation" : path_from(plan.program);
    started.process =
        start_process(program, arguments, add_updated_from && !linux, from_version, &error);
    started.ok = started.process != nullptr;
    return started;
}

void forget_started(Started& started) {
    if (started.process != nullptr) {
        SDL_DestroyProcess(started.process);
        started.process = nullptr;
    }
    started.pid = 0;
}

} // namespace

std::vector<Operation> kept_record_operations(
    const update::Plan& plan, std::string_view installed_version, int64_t installed_release
) {
    const fs::path updates = path_from(plan.updates);
    const fs::path preferences_copy = updates / "previous" / "preferences.conf";
    std::vector<Operation> operations;
    std::error_code io;
    const fs::file_status existing = fs::symlink_status(preferences_copy, io);
    if (!io && fs::exists(existing)) {
        Operation remove;
        remove.kind = Operation::Kind::remove_tree;
        remove.path = preferences_copy;
        operations.push_back(std::move(remove));
    }

    Operation preferences;
    preferences.kind = Operation::Kind::copy_tree;
    preferences.from = path_from(plan.preferences);
    preferences.to = preferences_copy;
    operations.push_back(std::move(preferences));

    update::State state;
    state.installed_version = std::string(installed_version);
    state.installed_release = installed_release;
    state.kept_version = plan.from_version;
    if (plan.route == update::Route::linux_versions) {
        state.kept_path =
            utf8_of(path_from(plan.install) / "versions" / path_from(plan.from_version));
    } else {
        state.kept_path = utf8_of(updates / "previous");
    }
    state.kept_route = plan.route;
    state.preferences_copy = utf8_of(preferences_copy);
    state.confirmed = false;
    state.failed_starts = 0;

    Operation record;
    record.kind = Operation::Kind::write_file;
    record.path = updates / "state.json";
    record.bytes = update::state_text(state);
    operations.push_back(std::move(record));
    return operations;
}

int restore_kept(
    const update::Plan& plan,
    const fs::path& updates,
    const fs::path* plan_path,
    Log& log,
    ProgressWindow& window,
    JournalHooks& hooks,
    update::RollBackReason reason,
    bool may_elevate,
    bool start_game
) {
    // After an install's failed starts, a roll back that does not finish
    // leaves the new version in place: its recovery did not finish.
    const bool after_install = plan.action == update::Action::install;
    const auto restore_failed = [&](std::string_view detail) {
        log.step(update::Step::restore, update::StepResult::failed, detail);
        log.finish(
            after_install ? update::Outcome::recovery_incomplete : update::Outcome::roll_back_failed
        );
    };
    std::string error;
    const std::optional<update::State> state = load_state(updates, error);
    if (!state) {
        log.line(error.empty() ? "the kept version could not be read" : error);
        restore_failed(error.empty() ? "the kept version could not be read" : error);
        start_old(plan, log, window, start_game);
        return exit_refused;
    }
    if (!after_install)
        log.set_to_version(state->kept_version);
    const std::string bad_version = state->installed_version;
    const int64_t bad_release = state->installed_release;
    const std::string title = plan.action == update::Action::roll_back && !plan.title.empty()
                                  ? plan.title
                                  : going_back_title(state->kept_version);
    const std::string line = plan.action == update::Action::roll_back ? plan.line : std::string{};
    if (start_game)
        window.show(title, line, 0, false);

    const bool windows_route = state->kept_route == update::Route::windows_installer ||
                               state->kept_route == update::Route::windows_folder;
    if (may_elevate && windows_route && windows_roll_back_elevate(path_from(plan.install))) {
        if (plan_path == nullptr) {
            log.line("the install folder could not be written");
            restore_failed("the install folder could not be written");
            start_old(plan, log, window, start_game);
            return exit_refused;
        }
        update::Plan again = plan;
        again.action = update::Action::roll_back;
        again.roll_back_reason = reason;
        if (again.title.empty())
            again.title = title;
        const fs::path written = updates / "roll-back.json";
        {
            std::ofstream out(written, std::ios::binary | std::ios::trunc);
            if (!out) {
                log.line("the roll-back plan could not be written");
                restore_failed("the roll-back plan could not be written");
                start_old(plan, log, window, start_game);
                return exit_refused;
            }
            out << update::plan_text(again);
        }
        const ElevatedRelaunch child = relaunch_elevated_plan(written);
        std::error_code io;
        fs::remove(written, io);
        if (child.cancelled) {
            log.line("the roll back was cancelled");
            restore_failed("the roll back was cancelled");
            start_old(plan, log, window, start_game);
            return exit_refused;
        }
        if (!child.ran || child.code != exit_swapped) {
            log.line(child.error.empty() ? "the elevated roll back did not finish" : child.error);
            restore_failed(
                child.error.empty() ? "the elevated roll back did not finish" : child.error
            );
            start_old(plan, log, window, start_game);
            return exit_undone;
        }
        log.step(update::Step::restore, update::StepResult::done);
        log.finish(update::Outcome::rolled_back);
        if (!start_game) {
            window.close();
            return exit_swapped;
        }
        const char* const reason_name = update::roll_back_reason_name(reason);
        std::vector<std::string> arguments = plan.arguments;
        arguments.emplace_back("--rolled-back-from");
        arguments.emplace_back(bad_version);
        arguments.emplace_back("--roll-back-reason");
        arguments.emplace_back(reason_name == nullptr ? "" : reason_name);
        Started started = start_version(plan, arguments, false, {}, {}, error);
        forget_started(started);
        if (!started.ok) {
            log.line(error.empty() ? "the kept version could not be started" : error);
            log.step(update::Step::start, update::StepResult::failed, error);
            window.close();
            return exit_refused;
        }
        log.line("started the kept version");
        log.step(update::Step::start, update::StepResult::done, state->kept_version);
        window.close();
        return exit_swapped;
    }

    std::string why;
    std::vector<Operation> operations =
        restore_operations(*state, path_from(plan.install), bad_version, &why);
    if (!why.empty()) {
        log.line(why);
        restore_failed(why);
        start_old(plan, log, window, start_game);
        return exit_refused;
    }

    const fs::path preferences = path_from(plan.preferences);
    const fs::path copy = path_from(state->preferences_copy);
    if (file_there(copy)) {
        const std::string bytes = preferences_after_roll_back(copy, bad_release, log);
        if (file_there(preferences)) {
            Operation remove;
            remove.kind = Operation::Kind::remove_tree;
            remove.path = preferences;
            operations.push_back(std::move(remove));
        }
        Operation write;
        write.kind = Operation::Kind::write_file;
        write.path = preferences;
        write.bytes = bytes;
        operations.push_back(std::move(write));
    } else {
        log.line("the preferences copy is missing");
    }

    update::State restored;
    restored.installed_version = state->kept_version;
    restored.installed_release = 0;
    restored.kept_route = state->kept_route;
    restored.confirmed = true;
    restored.failed_starts = 0;
    Operation record;
    record.kind = Operation::Kind::write_file;
    record.path = updates / "state.json";
    record.bytes = update::state_text(restored);
    operations.push_back(std::move(record));

    const JournalResult applied = apply_journal(updates, operations, hooks, &error);
    if (applied != JournalResult::finished) {
        log.line(error.empty() ? "a step failed" : error);
        if (after_install) {
            restore_failed(error.empty() ? "a step failed" : error);
        } else {
            log.step(
                update::Step::restore,
                update::StepResult::failed,
                error.empty() ? "a step failed" : error
            );
            const bool undone = !journal_left(updates);
            log.step(
                update::Step::undo, undone ? update::StepResult::done : update::StepResult::failed
            );
            log.finish(update::failure_outcome(update::Action::roll_back, true, undone));
        }
        window.show(title, line_after_failed_roll_back(updates), 0, true);
        start_old(plan, log, window, start_game);
        return exit_undone;
    }
    drop_journal(updates);
    log.step(update::Step::restore, update::StepResult::done);
    log.finish(update::Outcome::rolled_back);
    if (plan.route == update::Route::linux_versions)
        remove_update_starts(path_from(plan.install));

    if (!start_game) {
        log.line("put the kept version back");
        window.close();
        return exit_swapped;
    }

    const char* const reason_name = update::roll_back_reason_name(reason);
    std::vector<std::string> arguments = plan.arguments;
    arguments.emplace_back("--rolled-back-from");
    arguments.emplace_back(bad_version);
    arguments.emplace_back("--roll-back-reason");
    arguments.emplace_back(reason_name == nullptr ? "" : reason_name);
    Started started = start_version(plan, arguments, false, {}, {}, error);
    forget_started(started);
    if (!started.ok) {
        log.line(error.empty() ? "the kept version could not be started" : error);
        log.step(update::Step::start, update::StepResult::failed, error);
        window.close();
        return exit_refused;
    }
    log.line("started the kept version");
    log.step(update::Step::start, update::StepResult::done, state->kept_version);
    window.close();
    return exit_swapped;
}

namespace {

/// When this process is the install's own helper, a folder swap cannot rename
/// that file on Windows XP. Copy it under the updates folder, start the copy,
/// and leave. The copy waits until this process has left, which frees the
/// lock and the install's helper file, and only then renames anything.
///
/// @return an exit status when this process should stop; nothing when it should carry on
std::optional<int> step_aside_from_install(
    const update::Plan& plan,
    const fs::path& plan_path,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only,
    int64_t helper_parent,
    Log& log,
    ProgressWindow& window
) {
    if (plan.route != update::Route::windows_folder)
        return std::nullopt;
    const fs::path running = running_helper_program();
    if (!folder_swap_must_step_aside(running, path_from(plan.install), helper_parent))
        return std::nullopt;
    const fs::path updates = path_from(plan.updates);
    const fs::path aside = helper_aside_path(updates);
    std::string error;
    if (!copy_helper_aside(running, aside, &error)) {
        log.line(error.empty() ? "the helper could not be copied" : error);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    const std::vector<std::string> arguments =
        helper_aside_arguments(plan_path, keys, elevated_swap, stage_only, helper_process_id());
    log.line("continuing the update outside the install folder");
    window.close();
    if (!start_detached(aside, arguments, false, {}, &error)) {
        log.line(error.empty() ? "the helper could not be copied" : error);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    return exit_swapped;
}

int carry_out(
    const update::Plan& plan,
    const fs::path& plan_path,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only,
    int64_t helper_parent
) {
    const fs::path updates = path_from(plan.updates);
    std::error_code io;
    fs::create_directories(updates, io);
    if (io) {
        std::cerr << "oa-update: the updates folder could not be made\n";
        if (!elevated_swap) {
            std::string error;
            static_cast<void>(
                start_detached(path_from(plan.program), plan.arguments, false, {}, &error)
            );
        }
        return exit_usage;
    }

    // The copy outside the install waits until the process that spawned it
    // has left. That process holds the lock until it exits, and its helper
    // file cannot be renamed until then. The wait is before this copy takes
    // the lock.
    std::string error;
    if (helper_parent > 0) {
        const WaitOutcome parent_wait = wait_for_process(helper_parent, nullptr, &error);
        if (parent_wait != WaitOutcome::finished) {
            Log early;
            static_cast<void>(early.open(updates, nullptr));
            early.line("the helper could not leave the install folder");
            ProgressWindow early_window;
            start_old(plan, early, early_window, !elevated_swap);
            return exit_refused;
        }
    }

    UpdateLock held;
    if (!held.try_lock(updates, &error)) {
        // The parent still holds the lock. The elevated copy continues, and
        // any other failure still stops.
        if (!(elevated_swap && error == "another update is running")) {
            std::cerr << "oa-update: " << error << '\n';
            return exit_usage;
        }
    }

    Log log;
    if (!log.open(updates, &error))
        std::cerr << "oa-update: " << error << '\n';
    ProgressWindow window;
    window.set_log(&log);
    Note note{&log};
    JournalHooks hooks;
    hooks.context = &note;
    hooks.note = note_line;
    hooks.run = carry_run;
    // The receipt the game reads on its next start. An elevated copy only
    // swaps a folder for the run that started it, which keeps the receipt.
    if (!elevated_swap) {
        update::Receipt receipt;
        receipt.action = plan.action;
        receipt.reason = plan.roll_back_reason;
        receipt.from_version = plan.from_version;
        receipt.release = plan.release;
        log.begin_receipt(updates, std::move(receipt));
    }

    const UpdatesGateResult pending = take_pending(updates);
    if (pending.gate == UpdatesGate::restart) {
        const std::string version = pending.version.empty() ? plan.from_version : pending.version;
        log.set_to_version(version);
        log.finish(update::Outcome::restart_windows);
        if (!elevated_swap)
            window.show(plan.title, restart_line(version), 0, true);
        window.close();
        return exit_swapped;
    }
    if (pending.gate == UpdatesGate::restore_failed) {
        log.line(pending.detail.empty() ? "the kept copy could not be put back" : pending.detail);
        log.step(update::Step::undo, update::StepResult::failed, pending.detail);
        log.finish(update::failure_outcome(plan.action, true, false));
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    const bool undone = undo_journal(updates, hooks, &error);
    const WaitOutcome waited = wait_for_process(plan.wait_for, nullptr, &error);
    if (waited == WaitOutcome::still_running) {
        log.line(still_running_text);
        log.step(update::Step::wait, update::StepResult::failed, still_running_text);
        log.finish(update::failure_outcome(plan.action, false, true));
        window.show(plan.title, still_running_text, 0, true);
        window.close();
        return exit_refused;
    }
    log.step(update::Step::wait, update::StepResult::done);
    if (!undone) {
        log.line(error.empty() ? "a previous update could not be undone" : error);
        log.step(update::Step::undo, update::StepResult::failed, error);
        log.finish(update::failure_outcome(plan.action, true, false));
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    if (const std::optional<int> handed = step_aside_from_install(
            plan, plan_path, keys, elevated_swap, stage_only, helper_parent, log, window
        ))
        return *handed;

    if (plan.action == update::Action::roll_back) {
        const bool windows_route = plan.route == update::Route::windows_installer ||
                                   plan.route == update::Route::windows_folder;
        if (elevated_swap && !windows_route) {
            log.line("this route is not carried out");
            window.close();
            return exit_usage;
        }
        return restore_kept(
            plan,
            updates,
            &plan_path,
            log,
            window,
            hooks,
            plan.roll_back_reason,
            !elevated_swap,
            !elevated_swap
        );
    }
#if !OA_SELF_CHECKS_BUILT
    if (plan.route == update::Route::check_folder || !keys.empty()) {
        log.line("this build does not carry out that route");
        start_old(plan, log, window, !elevated_swap);
        return exit_usage;
    }
#endif
    const bool bundle = plan.route == update::Route::macos_bundle;
    const bool linux = plan.route == update::Route::linux_versions;
    const bool windows_route = plan.route == update::Route::windows_installer ||
                               plan.route == update::Route::windows_folder;
    if (elevated_swap && plan.route != update::Route::windows_folder) {
        log.line("this route is not carried out");
        window.close();
        return exit_usage;
    }
    const bool carried =
        windows_route || bundle || linux || plan.route == update::Route::check_folder;
    if (!carried) {
        log.line("this route is not carried out");
        start_old(plan, log, window, !elevated_swap);
        return exit_usage;
    }

    window.show(plan.title, plan.line, 0, false);

    VerifyRequest request;
    request.feed = path_from(plan.feed);
    request.signature = path_from(plan.signature);
    request.file = path_from(plan.file);
    request.release = plan.release;
    request.package = plan.package;
    request.installer = plan.installer;
    request.keys = keys;
    Verified verified;
    if (!verify_package(request, verified, &error)) {
        log.line(error);
        log.step(update::Step::verify, update::StepResult::failed, error);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    log.line("checked the package");
    log.step(update::Step::verify, update::StepResult::done);
    log.set_to_version(verified.version);
    window.show(plan.title, plan.line, 350, false);

    // A folder the player cannot write is opened in Installer. The bundle
    // stays where it is, and Installer relaunches nothing.
    if (bundle && plan.installer) {
        std::string why;
        const std::vector<Operation> operations =
            macos_checked_operations(plan, {}, !keys.empty(), &why);
        if (!why.empty()) {
            log.line(why);
            window.show(plan.title, could_not_check, 0, true);
            start_old(plan, log, window, !elevated_swap);
            return exit_refused;
        }
        const JournalResult opened = apply_journal(updates, operations, hooks, &error);
        if (opened != JournalResult::finished) {
            log.line(error.empty() ? "a step failed" : error);
            record_failed_change(
                log, updates, update::Step::installer, error.empty() ? "a step failed" : error
            );
            window.show(plan.title, line_after_failed_change(updates), 0, true);
            start_old(plan, log, window, !elevated_swap);
            return exit_undone;
        }
        drop_journal(updates);
        log.line("opened the installer");
        log.step(update::Step::installer, update::StepResult::done);
        log.finish(update::Outcome::opened_installer);
        window.close();
        return exit_swapped;
    }

    if (windows_route) {
        fs::path stage;
        if (plan.route == update::Route::windows_folder) {
            const std::vector<std::string> tops =
                top_folder_names(verified.version, verified.package);
            stage = updates / std::to_string(verified.release) / "stage";
            if (!unpack_package(request.file, stage, tops, &error)) {
                log.line(error);
                log.step(update::Step::unpack, update::StepResult::failed, error);
                window.show(plan.title, could_not_check, 0, true);
                start_old(plan, log, window, !elevated_swap);
                return exit_refused;
            }
            log.line("unpacked the package");
            log.step(update::Step::unpack, update::StepResult::done);
            window.show(plan.title, plan.line, 700, false);
        }
        return finish_windows_route(
            plan, verified, plan_path, stage, elevated_swap, log, window, hooks
        );
    }

    const std::vector<std::string> tops = top_folder_names(verified.version, verified.package);
    const fs::path stage = updates / std::to_string(verified.release) / "stage";
    if (!unpack_package(request.file, stage, tops, &error)) {
        log.line(error);
        log.step(update::Step::unpack, update::StepResult::failed, error);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    log.line("unpacked the package");
    log.step(update::Step::unpack, update::StepResult::done);
    window.show(plan.title, plan.line, 700, false);

    std::string why;
    std::vector<Operation> operations =
        bundle ? macos_checked_operations(plan, stage, !keys.empty(), &why)
               : plan_operations(plan, stage, &why);
    if (!why.empty()) {
        log.line(why);
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }

    fs::create_directories(updates / "previous", io);
    if (io) {
        log.line("the kept folder could not be made");
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }

    std::vector<Operation> record =
        kept_record_operations(plan, verified.version, verified.release);
    operations.insert(
        operations.end(),
        std::make_move_iterator(record.begin()),
        std::make_move_iterator(record.end())
    );

    const JournalResult applied = apply_journal(updates, operations, hooks, &error);
    if (applied != JournalResult::finished) {
        log.line(error.empty() ? "a step failed" : error);
        record_failed_change(
            log, updates, update::Step::swap, error.empty() ? "a step failed" : error
        );
        remove_stage(stage);
        window.show(plan.title, line_after_failed_change(updates), 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }
    log.step(update::Step::swap, update::StepResult::done);
    window.show(plan.title, plan.line, oa::ui::kit::question_progress_whole, false);

    // The launcher counts an unconfirmed start. The file is written before the
    // start, including a Deck or a staged update that this process does not run.
    if (linux)
        write_update_starts(path_from(plan.install));
    if (linux && (stage_only || deck_session())) {
        const std::string line = next_start_line(verified.version);
        log.line(line);
        log.finish(update::Outcome::installs_next_start);
        window.show(plan.title, line, oa::ui::kit::question_progress_whole, true);
        drop_journal(updates);
        remove_stage(stage);
        window.close();
        return exit_swapped;
    }

    // The new version is in place. The receipt says so before it starts,
    // so the game finds the outcome on its first menu.
    log.finish(update::Outcome::installed);
    Started started =
        start_version(plan, plan.arguments, true, plan.from_version, verified.version, error);
    if (!started.ok) {
        if (linux)
            remove_update_starts(path_from(plan.install));
        if (bundle) {
            // The swap has already happened. Undoing it would put the old
            // app back after the new one is already in its place.
            log.line("the new version could not be started; the kept bundle was left in place");
            if (!error.empty())
                log.line(error);
            log.step(update::Step::start, update::StepResult::failed, error);
            drop_journal(updates);
            remove_stage(stage);
            window.close();
            return exit_refused;
        }
        log.line(error.empty() ? "the game could not be started" : error);
        log.step(
            update::Step::start,
            update::StepResult::failed,
            error.empty() ? "the game could not be started" : error
        );
        std::string undo_error;
        if (!undo_journal(updates, hooks, &undo_error) && !undo_error.empty())
            log.line(undo_error);
        const bool put_back = !journal_left(updates);
        log.step(
            update::Step::undo, put_back ? update::StepResult::done : update::StepResult::failed
        );
        log.finish(update::failure_outcome(update::Action::install, true, put_back));
        remove_stage(stage);
        window.show(plan.title, line_after_failed_change(updates), 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    drop_journal(updates);
    remove_stage(stage);
    log.line("started the new version");
    log.step(update::Step::start, update::StepResult::done, verified.version);
    window.close();

    struct Again {
        const update::Plan* plan{};
        std::string* error{};
        std::string expect_version;
    };

    Again again{&plan, &error, std::string(verified.version)};
    const auto restart = [](void* context, SDL_Process** process, int* pid, std::string* failure) {
        auto* held = static_cast<Again*>(context);
        Started next = start_version(
            *held->plan,
            held->plan->arguments,
            true,
            held->plan->from_version,
            held->expect_version,
            *failure
        );
        if (!next.ok)
            return false;
        *process = next.process;
        *pid = next.pid;
        return true;
    };
    const WatchTwo watched = watch_two_starts(
        started.process, started.pid, updates / "state.json", restart, &again, &error
    );
    if (watched == WatchTwo::confirmed) {
        log.step(update::Step::watch, update::StepResult::done);
        return exit_swapped;
    }
    // A quit before the main menu is the player's, not a failed start: the
    // new version stays, and its next start counts again.
    if (watched == WatchTwo::quit) {
        log.line("the new version was closed before the main menu");
        return exit_swapped;
    }
    // A start the watch cannot see is not taken for a failed one.
    if (watched == WatchTwo::unobserved) {
        log.line("the new version's start could not be watched");
        return exit_swapped;
    }
    if (watched == WatchTwo::restart_failed && !error.empty())
        log.line(error);
    log.step(update::Step::watch, update::StepResult::failed, error);
    return restore_kept(
        plan,
        updates,
        &plan_path,
        log,
        window,
        hooks,
        update::RollBackReason::failed_starts,
        true,
        true
    );
}

} // namespace

int run_plan(
    const fs::path& plan_path,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only,
    int64_t helper_parent
) {
#if !OA_SELF_CHECKS_BUILT
    if (!keys.empty()) {
        std::cerr << "oa-update: --feed-key is not in this build\n";
        return exit_usage;
    }
    if (stage_only) {
        std::cerr << "oa-update: --stage-only is not in this build\n";
        return exit_usage;
    }
#else
    (void)keys;
#endif
    std::string error;
    std::vector<uint8_t> bytes;
    if (!read_capped(plan_path, update::max_plan_bytes, bytes, error)) {
        std::cerr << "oa-update: " << error << '\n';
        return exit_usage;
    }
    const std::optional<update::Plan> plan = update::read_plan(bytes, &error);
    if (!plan) {
        std::cerr << "oa-update: " << error << '\n';
        return exit_usage;
    }
#if OA_SELF_CHECKS_BUILT
    return carry_out(*plan, plan_path, keys, elevated_swap, stage_only, helper_parent);
#else
    return carry_out(*plan, plan_path, {}, elevated_swap, false, helper_parent);
#endif
}

int recover_updates(const fs::path& updates) {
    std::error_code io;
    if (!fs::is_directory(updates, io) || io) {
        std::cerr << "oa-update: the updates folder could not be read\n";
        return exit_usage;
    }
    UpdateLock held;
    std::string error;
    if (!held.try_lock(updates, &error)) {
        std::cerr << "oa-update: " << error << '\n';
        return exit_usage;
    }
    Log log;
    static_cast<void>(log.open(updates, nullptr));
    Note note{&log};
    JournalHooks hooks;
    hooks.context = &note;
    hooks.note = note_line;
    hooks.run = carry_run;
    if (!undo_journal(updates, hooks, &error)) {
        log.line(error.empty() ? "a previous update could not be undone" : error);
        return exit_undone;
    }
    log.line("undid a previous update");
    return exit_swapped;
}

} // namespace oa::update_helper
