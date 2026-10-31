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

#if defined(__APPLE__)
#include <unistd.h>
#endif

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// The administrator prompt's sentence for an install. {version} and
/// {folder} are filled; macOS adds its own request for the password.
constexpr std::string_view install_prompt =
    "Open Annihilation wants to put {version} in place of the copy in {folder}.";

/// The administrator prompt's sentence for a roll back.
constexpr std::string_view roll_back_prompt =
    "Open Annihilation wants to put {version} back in place of the copy in {folder}.";

/// The window line when an update stopped before anything changed for a
/// reason the player can try again.
constexpr std::string_view update_not_installed =
    "The update was not installed. Nothing was changed.";

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

/// True when this process holds an administrator's rights on macOS, where
/// the game must never start with them.
///
/// @return true when the real or the effective user is the administrator
bool holds_administrator_rights() {
#if defined(__APPLE__)
    return ::getuid() == 0 || ::geteuid() == 0;
#else
    return false;
#endif
}

/// Starts the version that is in place, without the update flag.
///
/// A macOS bundle is opened through Launch Services, as the player opens
/// it from the Finder: a program started directly is refused the player's
/// Documents folder, and would take the player's copy of the game for
/// missing. The other routes start the program directly. A process with an
/// administrator's rights starts nothing on macOS.
///
/// @param plan the plan
/// @param[out] error why it did not start; may be null
/// @return true when the game started
bool start_again(const update::Plan& plan, std::string* error) {
    switch (start_way(plan, holds_administrator_rights())) {
    case StartWay::refused:
        if (error != nullptr)
            *error = "the game is not started with an administrator's rights";
        return false;
    case StartWay::open_bundle:
#if defined(__APPLE__)
        return start_macos_bundle(
            path_from(plan.install), plan.arguments, {}, {}, {}, error, nullptr
        );
#else
        if (error != nullptr)
            *error = "this route is not carried out";
        return false;
#endif
    case StartWay::program:
        break;
    }
    return start_detached(path_from(plan.program), plan.arguments, false, {}, error);
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
        if (!start_again(plan, &error))
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
    if (start_way(plan, holds_administrator_rights()) == StartWay::refused) {
        error = "the game is not started with an administrator's rights";
        return started;
    }
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

/// Fills {version} and {folder} in an administrator prompt's sentence.
///
/// @param sentence install_prompt or roll_back_prompt
/// @param version the version being put in place
/// @param install the bundle it replaces
/// @return the sentence
std::string administrator_prompt(
    std::string_view sentence, std::string_view version, std::string_view install
) {
    std::string text(sentence);
    const auto fill = [&text](std::string_view token, std::string_view value) {
        const std::string::size_type at = text.find(token);
        if (at != std::string::npos)
            text.replace(at, token.size(), value.data(), value.size());
    };
    const fs::path folder = path_from(install).parent_path();
    const std::string name = utf8_of(folder.filename());
    fill("{version}", version);
    fill("{folder}", name.empty() ? utf8_of(folder) : name);
    return text;
}

/// True when the bundle moves of this plan need an administrator's rights:
/// a macOS bundle the player cannot move, in a folder the player cannot
/// change, or beside a kept bundle the player cannot move.
///
/// @param plan the plan
/// @return true when macOS must ask for an administrator's password
bool swap_needs_administrator(const update::Plan& plan) {
#if defined(__APPLE__)
    if (plan.route != update::Route::macos_bundle || plan.installer)
        return false;
    const fs::path install = path_from(plan.install);
    return macos_swap_needs_elevation(
        macos_swap_access(install, macos_kept_bundle(path_from(plan.updates), install))
    );
#else
    (void)plan;
    return false;
#endif
}

/// How the run with an administrator's rights ended, for the run that asked.
struct AdministratorRun {
    /// The four endings.
    enum class Ending : uint8_t {
        moved,     ///< the bundles were moved
        cancelled, ///< the player closed the prompt; nothing ran
        refused,   ///< nothing was moved: the prompt could not be shown, or the copy refused
        failed,    ///< a move failed; the copy undid what it could
    };

    Ending ending{Ending::refused}; ///< how it ended
    std::string detail;             ///< why it did not move, for the receipt
};

#if defined(__APPLE__)
/// The window kept up while the prompt is shown.
struct PromptWindow {
    ProgressWindow* window{}; ///< the window
    std::string_view title;   ///< its title
};

/// Draws the window again while the prompt waits.
///
/// @param context the PromptWindow
void keep_prompt_window(void* context) {
    auto* const shown = static_cast<PromptWindow*>(context);
    shown->window->show(shown->title, macos_asks_permission, 700, true);
}
#endif

/// Asks macOS for an administrator's password and runs this helper again
/// with it, on `plan_file`, to move the bundles. The window says why it asks.
///
/// @param plan_file the plan the other copy reads
/// @param keys the feed keys this run was given, passed on
/// @param prompt the prompt's sentence
/// @param title the window's title
/// @param log the log
/// @param window the window
/// @return how it ended
AdministratorRun run_as_administrator(
    const fs::path& plan_file,
    const std::vector<FeedKey>& keys,
    std::string_view prompt,
    std::string_view title,
    Log& log,
    ProgressWindow& window
) {
    AdministratorRun run;
#if defined(__APPLE__)
    window.show(title, macos_asks_permission, 700, true);
    std::vector<std::string> arguments{"--plan", utf8_of(plan_file), "--elevated-swap"};
    for (const FeedKey& key : keys) {
        arguments.emplace_back("--feed-key");
        arguments.push_back(key.id + "=" + key.text);
    }
    log.line("asking for an administrator's password");
    PromptWindow shown{&window, title};
    const ElevatedRelaunch ran = relaunch_elevated_macos(
        prompt, macos_helper_program(), arguments, keep_prompt_window, &shown
    );
    if (ran.cancelled) {
        run.ending = AdministratorRun::Ending::cancelled;
        run.detail = std::string(permission_not_given);
    } else if (!ran.ran) {
        run.ending = AdministratorRun::Ending::refused;
        run.detail = ran.error;
    } else if (ran.code == exit_swapped) {
        run.ending = AdministratorRun::Ending::moved;
    } else if (ran.code == exit_refused || ran.code == exit_usage) {
        run.ending = AdministratorRun::Ending::refused;
        run.detail = "the copy with an administrator's rights refused the move (exit " +
                     std::to_string(ran.code) + ")";
    } else {
        run.ending = AdministratorRun::Ending::failed;
        run.detail = "the copy with an administrator's rights did not finish the move (exit " +
                     std::to_string(ran.code) + ")";
    }
    if (!run.detail.empty())
        log.line(run.detail);
#else
    (void)plan_file;
    (void)keys;
    (void)prompt;
    (void)title;
    (void)log;
    (void)window;
    run.detail = "this route is not carried out";
#endif
    return run;
}

} // namespace

StartWay start_way(const update::Plan& plan, bool administrator) noexcept {
    if (administrator)
        return StartWay::refused;
    if (plan.route == update::Route::macos_bundle)
        return StartWay::open_bundle;
    return StartWay::program;
}

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

    // The player's preferences and the record of what is installed. They are
    // the player's files, written as the player in both ways below.
    std::vector<Operation> record;
    const fs::path preferences = path_from(plan.preferences);
    const fs::path copy = path_from(state->preferences_copy);
    if (file_there(copy)) {
        const std::string bytes = preferences_after_roll_back(copy, bad_release, log);
        if (file_there(preferences)) {
            Operation remove;
            remove.kind = Operation::Kind::remove_tree;
            remove.path = preferences;
            record.push_back(std::move(remove));
        }
        Operation write;
        write.kind = Operation::Kind::write_file;
        write.path = preferences;
        write.bytes = bytes;
        record.push_back(std::move(write));
    } else {
        log.line("the preferences copy is missing");
    }

    update::State restored;
    restored.installed_version = state->kept_version;
    restored.installed_release = 0;
    restored.kept_route = state->kept_route;
    restored.confirmed = true;
    restored.failed_starts = 0;
    Operation recorded;
    recorded.kind = Operation::Kind::write_file;
    recorded.path = updates / "state.json";
    recorded.bytes = update::state_text(restored);
    record.push_back(std::move(recorded));

    if (may_elevate && state->kept_route == update::Route::macos_bundle &&
        swap_needs_administrator(plan)) {
        // The player cannot move the bundles. This run writes the record as
        // the player, and a copy with an administrator's rights moves the
        // bundles, continuing the same journal.
        if (plan_path == nullptr) {
            log.line("the install could not be changed");
            restore_failed("the install could not be changed");
            start_old(plan, log, window, start_game);
            return exit_refused;
        }
        if (apply_journal(updates, record, hooks, &error) != JournalResult::finished) {
            log.line(error.empty() ? "a step failed" : error);
            restore_failed(error.empty() ? "a step failed" : error);
            window.show(title, line_after_failed_roll_back(updates), 0, true);
            start_old(plan, log, window, start_game);
            return exit_undone;
        }
        update::Plan again = plan;
        again.action = update::Action::roll_back;
        again.roll_back_reason = reason;
        if (again.title.empty())
            again.title = title;
        const fs::path written = updates / "roll-back.json";
        bool plan_written = false;
        {
            std::ofstream out(written, std::ios::binary | std::ios::trunc);
            if (out)
                out << update::plan_text(again);
            plan_written = static_cast<bool>(out);
        }
        AdministratorRun asked;
        asked.detail = "the roll-back plan could not be written";
        if (plan_written) {
            asked = run_as_administrator(
                written,
                {},
                administrator_prompt(roll_back_prompt, state->kept_version, plan.install),
                title,
                log,
                window
            );
        }
        std::error_code removed;
        fs::remove(written, removed);
        if (asked.ending != AdministratorRun::Ending::moved) {
            std::string undo_error;
            if (!undo_journal(updates, hooks, &undo_error) && !undo_error.empty())
                log.line(undo_error);
            restore_failed(asked.detail);
            window.show(title, line_after_failed_roll_back(updates), 0, true);
            start_old(plan, log, window, start_game);
            return asked.ending == AdministratorRun::Ending::failed ? exit_undone : exit_refused;
        }
    } else {
        operations.insert(
            operations.end(),
            std::make_move_iterator(record.begin()),
            std::make_move_iterator(record.end())
        );
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
                    update::Step::undo,
                    undone ? update::StepResult::done : update::StepResult::failed
                );
                log.finish(update::failure_outcome(update::Action::roll_back, true, undone));
            }
            window.show(title, line_after_failed_roll_back(updates), 0, true);
            start_old(plan, log, window, start_game);
            return exit_undone;
        }
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

#if defined(__APPLE__)
/// Writes one line of the administrator's copy's journal to the log.
///
/// @param context the RaisedMoves
/// @param line the line
void raised_note(void* context, std::string_view line) {
    const auto* const moves = static_cast<const RaisedMoves*>(context);
    if (moves->log != nullptr)
        moves->log->line(line);
}
#endif

/// Moves the bundles for the run that asked for an administrator's rights.
///
/// This copy acts as the player, takes the administrator's rights only for
/// each rename, writes no receipt and starts nothing. It checks the bundles
/// itself, from the plan and their signatures: each must be signed by the
/// team that signed this helper and be this game. The moves continue the
/// journal the asking run began, and a failure undoes all of it.
///
/// @param plan the plan, an install or a roll back
/// @param player who this copy acts for
/// @param keys the feed keys this run was given
/// @param log the log
/// @param hooks the journal hooks; the note is replaced
/// @return exit_swapped, exit_refused when nothing moved, or exit_undone
int carry_out_as_administrator(
    const update::Plan& plan,
    const ActingPlayer& player,
    const std::vector<FeedKey>& keys,
    Log& log,
    JournalHooks hooks
) {
#if defined(__APPLE__)
    const fs::path updates = path_from(plan.updates);
    const fs::path install = path_from(plan.install);
    const bool test_key = !keys.empty();
    if (plan.installer) {
        log.line("this route is not carried out");
        return exit_usage;
    }
    std::string why;
    std::vector<Operation> operations;
    fs::path incoming;
    if (plan.action == update::Action::roll_back) {
        operations = macos_checked_restore(plan, test_key, &why);
        incoming = macos_kept_bundle(updates, install);
    } else {
        const fs::path stage = updates / std::to_string(plan.release) / "stage";
        operations = macos_checked_operations(plan, stage, test_key, &why);
        if (why.empty())
            static_cast<void>(macos_staged_application(stage, incoming, &why));
    }
    // A copy with an administrator's rights moves only this game's bundles,
    // signed by the team that signed it.
    if (why.empty() && player.can_raise && macos_bundle_is_ours(install, install, test_key, &why))
        static_cast<void>(macos_bundle_is_ours(incoming, install, test_key, &why));
    RaisedMoves moves;
    moves.player = player;
    moves.updates = updates;
    moves.install = install;
    moves.log = &log;
    if (why.empty() && !read_install_owner(moves))
        why = "the installed bundle is missing";
    if (!why.empty()) {
        log.line(why);
        return exit_refused;
    }
    hooks.context = &moves;
    hooks.note = raised_note;
    hooks.rename = raised_rename;
    std::string error;
    if (continue_journal(updates, operations, hooks, &error) != JournalResult::finished) {
        log.line(error.empty() ? "a step failed" : error);
        return exit_undone;
    }
    log.line("moved the bundles with an administrator's rights");
    return exit_swapped;
#else
    (void)plan;
    (void)player;
    (void)keys;
    (void)hooks;
    log.line("this route is not carried out");
    return exit_usage;
#endif
}

/// Puts the new bundle in place with an administrator's rights.
///
/// This run records the kept version as the player, then macOS asks for an
/// administrator's password and a second copy of the helper moves the
/// bundles, continuing this run's journal. A closed prompt, a refusal and a
/// failure undo the record and start the old version.
///
/// @param plan the plan
/// @param plan_path the plan file the second copy reads
/// @param keys the feed keys this run was given
/// @param version the version being installed
/// @param record the kept version's record, written as the player
/// @param stage the staging folder, removed when the swap does not happen
/// @param log the log
/// @param window the window
/// @param hooks the journal hooks
/// @return nothing when the bundles are in place; the exit status otherwise
std::optional<int> swap_with_administrator(
    const update::Plan& plan,
    const fs::path& plan_path,
    const std::vector<FeedKey>& keys,
    std::string_view version,
    const std::vector<Operation>& record,
    const fs::path& stage,
    Log& log,
    ProgressWindow& window,
    JournalHooks& hooks
) {
    const fs::path updates = path_from(plan.updates);
    std::string error;
    if (apply_journal(updates, record, hooks, &error) != JournalResult::finished) {
        const std::string detail = error.empty() ? "a step failed" : error;
        log.line(detail);
        record_failed_change(log, updates, update::Step::swap, detail);
        remove_stage(stage);
        window.show(plan.title, line_after_failed_change(updates), 0, true);
        start_old(plan, log, window, true);
        return exit_undone;
    }
    const AdministratorRun asked = run_as_administrator(
        plan_path,
        keys,
        administrator_prompt(install_prompt, version, plan.install),
        plan.title,
        log,
        window
    );
    if (asked.ending == AdministratorRun::Ending::moved)
        return std::nullopt;
    std::string undo_error;
    if (!undo_journal(updates, hooks, &undo_error) && !undo_error.empty())
        log.line(undo_error);
    remove_stage(stage);
    if (asked.ending == AdministratorRun::Ending::failed) {
        record_failed_change(log, updates, update::Step::swap, asked.detail);
        window.show(plan.title, line_after_failed_change(updates), 0, true);
        start_old(plan, log, window, true);
        return exit_undone;
    }
    // Nothing was moved, and this run's record was undone: the version that
    // was running is unchanged.
    log.step(update::Step::swap, update::StepResult::failed, asked.detail);
    if (journal_left(updates)) {
        log.step(update::Step::undo, update::StepResult::failed);
        log.finish(update::Outcome::recovery_incomplete);
        window.show(plan.title, line_after_failed_change(updates), 0, true);
    } else if (asked.ending == AdministratorRun::Ending::cancelled) {
        log.finish(update::Outcome::cancelled);
        window.show(plan.title, update_was_cancelled, 0, true);
    } else {
        log.finish(update::Outcome::not_installed);
        window.show(plan.title, update_not_installed, 0, true);
    }
    start_old(plan, log, window, true);
    return exit_refused;
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
    // A copy macOS runs with an administrator's rights acts as the player
    // from here, before it touches a file, so every file it writes under the
    // player's folder is the player's.
    ActingPlayer player;
#if defined(__APPLE__)
    if (elevated_swap && plan.route == update::Route::macos_bundle) {
        std::string refused;
        if (!act_as_player(updates, plan_path, player, &refused)) {
            std::cerr << "oa-update: " << refused << '\n';
            return exit_usage;
        }
    }
#endif
    std::error_code io;
    fs::create_directories(updates, io);
    if (io) {
        std::cerr << "oa-update: the updates folder could not be made\n";
        if (!elevated_swap) {
            std::string error;
            static_cast<void>(start_again(plan, &error));
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
    // The macOS copy with an administrator's rights moves the bundles and
    // nothing else; the run that asked for it keeps the journal's start, the
    // receipt and the game.
    if (elevated_swap && plan.route == update::Route::macos_bundle) {
        const int moved = carry_out_as_administrator(plan, player, keys, log, hooks);
        window.close();
        return moved;
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
    if (bundle && swap_needs_administrator(plan)) {
        // The player cannot move the bundles, as when the installer package
        // put the game in Applications for the administrator.
        if (const std::optional<int> stopped = swap_with_administrator(
                plan, plan_path, keys, verified.version, record, stage, log, window, hooks
            ))
            return *stopped;
    } else {
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
