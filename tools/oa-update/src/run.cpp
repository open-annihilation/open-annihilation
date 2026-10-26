// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/run.hpp"

#include "oa/app/update/plan.hpp"
#include "oa/app/update/route.hpp"
#include "oa/platform/machine.hpp"
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
#include "oa/update_helper/window.hpp"

#include <fstream>
#include <iostream>
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

int start_old(const update::Plan& plan, Log& log, ProgressWindow& window, bool start) {
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

int carry_out(
    const update::Plan& plan,
    const fs::path& plan_path,
    const std::vector<FeedKey>& keys,
    bool elevated_swap,
    bool stage_only
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

    UpdateLock held;
    std::string error;
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

    const UpdatesGateResult pending = take_pending(updates);
    if (pending.gate == UpdatesGate::restart) {
        const std::string version = pending.version.empty() ? plan.from_version : pending.version;
        if (!elevated_swap)
            window.show(plan.title, restart_line(version), 0, true);
        window.close();
        return exit_swapped;
    }
    if (pending.gate == UpdatesGate::restore_failed) {
        log.line(pending.detail.empty() ? "the kept copy could not be put back" : pending.detail);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    const bool undone = undo_journal(updates, hooks, &error);
    const WaitOutcome waited = wait_for_process(plan.wait_for, nullptr, &error);
    if (waited == WaitOutcome::still_running) {
        log.line(still_running_text);
        window.show(plan.title, still_running_text, 0, true);
        window.close();
        return exit_refused;
    }
    if (!undone) {
        log.line(error.empty() ? "a previous update could not be undone" : error);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    if (plan.action != update::Action::install) {
        log.line("roll-back is not carried out");
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
#if !OA_SELF_CHECKS_BUILT
    if (plan.route == update::Route::check_folder || !keys.empty()) {
        log.line("this build does not carry out that route");
        start_old(plan, log, window, !elevated_swap);
        return exit_usage;
    }
#endif
    const bool windows_route = plan.route == update::Route::windows_installer ||
                               plan.route == update::Route::windows_folder;
    if (elevated_swap && plan.route != update::Route::windows_folder) {
        log.line("this route is not carried out");
        window.close();
        return exit_usage;
    }
    const bool carried = windows_route || plan.route == update::Route::check_folder ||
                         plan.route == update::Route::linux_versions;
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
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    log.line("checked the package");
    window.show(plan.title, plan.line, 350, false);

    if (windows_route) {
        fs::path stage;
        if (plan.route == update::Route::windows_folder) {
            const std::string top = top_folder_name(verified.version, verified.package);
            stage = updates / std::to_string(verified.release) / "stage";
            if (!unpack_package(request.file, stage, top, &error)) {
                log.line(error);
                window.show(plan.title, could_not_check, 0, true);
                start_old(plan, log, window, !elevated_swap);
                return exit_refused;
            }
            log.line("unpacked the package");
            window.show(plan.title, plan.line, 700, false);
        }
        return finish_windows_route(
            plan, verified, plan_path, stage, elevated_swap, log, window, hooks
        );
    }

    const std::string top = top_folder_name(verified.version, verified.package);
    const fs::path stage = updates / std::to_string(verified.release) / "stage";
    if (!unpack_package(request.file, stage, top, &error)) {
        log.line(error);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_refused;
    }
    log.line("unpacked the package");
    window.show(plan.title, plan.line, 700, false);

    std::string why;
    std::vector<Operation> operations = plan_operations(plan, stage, &why);
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

    Operation preferences;
    preferences.kind = Operation::Kind::copy_tree;
    preferences.from = path_from(plan.preferences);
    preferences.to = updates / "previous" / "preferences.conf";
    operations.push_back(std::move(preferences));

    update::State state;
    state.installed_version = verified.version;
    state.installed_release = verified.release;
    state.kept_version = plan.from_version;
    if (plan.route == update::Route::linux_versions)
        state.kept_path =
            utf8_of(path_from(plan.install) / "versions" / path_from(plan.from_version));
    else
        state.kept_path = utf8_of(updates / "previous");
    state.kept_route = plan.route;
    state.preferences_copy = utf8_of(updates / "previous" / "preferences.conf");
    state.confirmed = false;
    state.failed_starts = 0;
    Operation record;
    record.kind = Operation::Kind::write_file;
    record.path = updates / "state.json";
    record.bytes = update::state_text(state);
    operations.push_back(std::move(record));

    const JournalResult applied = apply_journal(updates, operations, hooks, &error);
    if (applied != JournalResult::finished) {
        log.line(error.empty() ? "a step failed" : error);
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }
    window.show(plan.title, plan.line, oa::ui::kit::question_progress_whole, false);

    const bool linux = plan.route == update::Route::linux_versions;
    // The launcher adds --updated-from itself. On a Deck, and with
    // --stage-only, the new version waits until the next start.
    if (linux && (stage_only || deck_session())) {
        const std::string line = next_start_line(verified.version);
        log.line(line);
        window.show(plan.title, line, oa::ui::kit::question_progress_whole, true);
        drop_journal(updates);
        remove_stage(stage);
        window.close();
        return exit_swapped;
    }
    const fs::path program =
        linux ? path_from(plan.install) / "open-annihilation" : path_from(plan.program);
    if (!start_detached(program, plan.arguments, !linux, plan.from_version, &error)) {
        log.line(error.empty() ? "the game could not be started" : error);
        std::string undo_error;
        if (!undo_journal(updates, hooks, &undo_error) && !undo_error.empty())
            log.line(undo_error);
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window, !elevated_swap);
        return exit_undone;
    }

    drop_journal(updates);
    remove_stage(stage);
    log.line("started the new version");
    window.close();
    return exit_swapped;
}

} // namespace

int run_plan(
    const fs::path& plan_path, const std::vector<FeedKey>& keys, bool elevated_swap, bool stage_only
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
    return carry_out(*plan, plan_path, keys, elevated_swap, stage_only);
#else
    return carry_out(*plan, plan_path, {}, elevated_swap, false);
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
    if (!undo_journal(updates, hooks, &error)) {
        log.line(error.empty() ? "a previous update could not be undone" : error);
        return exit_undone;
    }
    log.line("undid a previous update");
    return exit_swapped;
}

} // namespace oa::update_helper
