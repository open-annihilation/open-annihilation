// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/run.hpp"

#include "oa/app/update/plan.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/lock.hpp"
#include "oa/update_helper/log.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/start.hpp"
#include "oa/update_helper/unpack.hpp"
#include "oa/update_helper/wait.hpp"
#include "oa/update_helper/window.hpp"

#include <fstream>
#include <iostream>
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

int start_old(const update::Plan& plan, Log& log, ProgressWindow& window) {
    std::string error;
    if (!start_detached(path_from(plan.program), plan.arguments, false, {}, &error))
        log.line(std::string("could not start the game: ") + error);
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

int carry_out(const update::Plan& plan, const std::vector<FeedKey>& keys) {
    const fs::path updates = path_from(plan.updates);
    std::error_code io;
    fs::create_directories(updates, io);
    if (io) {
        std::cerr << "oa-update: the updates folder could not be made\n";
        std::string error;
        static_cast<void>(
            start_detached(path_from(plan.program), plan.arguments, false, {}, &error)
        );
        return exit_usage;
    }

    UpdateLock held;
    std::string error;
    if (!held.try_lock(updates, &error)) {
        std::cerr << "oa-update: " << error << '\n';
        return exit_usage;
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
        start_old(plan, log, window);
        return exit_undone;
    }

    if (plan.action != update::Action::install) {
        log.line("roll-back is not carried out");
        start_old(plan, log, window);
        return exit_refused;
    }
#if !OA_SELF_CHECKS_BUILT
    if (plan.route == update::Route::check_folder || !keys.empty()) {
        log.line("this build does not carry out that route");
        start_old(plan, log, window);
        return exit_usage;
    }
#endif
    if (plan.route != update::Route::check_folder) {
        log.line("this route is not carried out");
        start_old(plan, log, window);
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
        start_old(plan, log, window);
        return exit_refused;
    }
    log.line("checked the package");
    window.show(plan.title, plan.line, 350, false);

    const std::string top = top_folder_name(verified.version, verified.package);
    const fs::path stage = updates / std::to_string(verified.release) / "stage";
    if (!unpack_package(request.file, stage, top, &error)) {
        log.line(error);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window);
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
        start_old(plan, log, window);
        return exit_refused;
    }

    fs::create_directories(updates / "previous", io);
    if (io) {
        log.line("the kept folder could not be made");
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window);
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
        start_old(plan, log, window);
        return exit_undone;
    }
    window.show(plan.title, plan.line, oa::ui::kit::question_progress_whole, false);

    if (!start_detached(path_from(plan.program), plan.arguments, true, plan.from_version, &error)) {
        log.line(error.empty() ? "the game could not be started" : error);
        std::string undo_error;
        if (!undo_journal(updates, hooks, &undo_error) && !undo_error.empty())
            log.line(undo_error);
        remove_stage(stage);
        window.show(plan.title, could_not_check, 0, true);
        start_old(plan, log, window);
        return exit_undone;
    }

    drop_journal(updates);
    remove_stage(stage);
    log.line("started the new version");
    window.close();
    return exit_swapped;
}

} // namespace

int run_plan(const fs::path& plan_path, const std::vector<FeedKey>& keys) {
#if !OA_SELF_CHECKS_BUILT
    if (!keys.empty()) {
        std::cerr << "oa-update: --feed-key is not in this build\n";
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
    return carry_out(*plan, keys);
#else
    return carry_out(*plan, {});
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
