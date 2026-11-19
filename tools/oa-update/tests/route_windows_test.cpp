// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Windows routes' decisions. No test starts a program. Moves, pauses
// and the Windows version are injected.

#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/route_windows.hpp"
#include "oa/update_helper/routes.hpp"
#include "oa/update_helper/run.hpp"

#include "oa/test/check.hpp"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <share.h>
#else
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
               (std::string("oa-v09-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

void write_file(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << bytes;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct ScriptedMove {
    std::vector<int> codes;
    int calls{0};
};

int scripted_move(void* context, const fs::path&, const fs::path&) {
    auto* script = static_cast<ScriptedMove*>(context);
    const int index = script->calls;
    script->calls += 1;
    if (script->codes.empty())
        return 0;
    if (index < static_cast<int>(script->codes.size()))
        return script->codes[static_cast<std::size_t>(index)];
    return script->codes.back();
}

void version_and_elevation() {
    OA_CHECK(helper::windows_9x(0xC0000004u));
    OA_CHECK(!helper::windows_9x(0x00000005u));
    OA_CHECK(!helper::windows_9x(0u));
    OA_CHECK(helper::windows_vista_or_later(0x00000006u));
    OA_CHECK(!helper::windows_vista_or_later(0x00000005u));
    OA_CHECK(!helper::windows_vista_or_later(0xC0000004u));
    OA_CHECK(!helper::windows_vista_or_later(0u));
    OA_CHECK(!helper::folder_needs_elevation(false, 0xC0000004u));
    OA_CHECK(helper::folder_needs_elevation(false, 0x00000006u));
    OA_CHECK(!helper::folder_needs_elevation(true, 0x00000006u));
    OA_CHECK(!helper::folder_needs_elevation(false, 0x00000005u));
}

void retries() {
    const fs::path from = "held.exe";
    const fs::path to = "next.exe";
    ScriptedMove held;
    held.codes = {helper::file_held};
    const helper::MoveAttempt five =
        helper::move_with_retries(scripted_move, nullptr, &held, false, from, to);
    OA_CHECK(!five.moved);
    OA_CHECK(!five.postponed);
    OA_CHECK(five.tries == helper::move_tries);
    OA_CHECK(five.pauses_ms.size() == 4u);
    OA_CHECK(five.pauses_ms[0] == helper::move_pause_ms);
    OA_CHECK(five.error == helper::file_held);

    ScriptedMove then_free;
    then_free.codes = {helper::file_held, helper::file_held, 0};
    const helper::MoveAttempt moved =
        helper::move_with_retries(scripted_move, nullptr, &then_free, false, from, to);
    OA_CHECK(moved.moved);
    OA_CHECK(!moved.postponed);
    OA_CHECK(moved.tries == 3);
    OA_CHECK(moved.pauses_ms.size() == 2u);

    ScriptedMove nine;
    nine.codes = {helper::file_held};
    const helper::MoveAttempt postponed =
        helper::move_with_retries(scripted_move, nullptr, &nine, true, from, to);
    OA_CHECK(!postponed.moved);
    OA_CHECK(postponed.postponed);
    OA_CHECK(postponed.tries == helper::move_tries);

    ScriptedMove other;
    other.codes = {5};
    const helper::MoveAttempt stopped =
        helper::move_with_retries(scripted_move, nullptr, &other, true, from, to);
    OA_CHECK(!stopped.moved);
    OA_CHECK(!stopped.postponed);
    OA_CHECK(stopped.tries == 1);
    OA_CHECK(stopped.pauses_ms.empty());
    OA_CHECK(stopped.error == 5);

    helper::MoveAttempt exists;
    exists.error = helper::destination_exists;
    OA_CHECK(helper::classify_move(exists, true) == helper::MoveOutcome::postponed);
    OA_CHECK(helper::classify_move(exists, false) == helper::MoveOutcome::failed);
    OA_CHECK(helper::classify_move(stopped, true) == helper::MoveOutcome::failed);
    OA_CHECK(helper::classify_move(moved, false) == helper::MoveOutcome::moved);
}

void wininit_text_and_setup_arguments() {
    const helper::PostponedRename row{"C:\\SHORT\\NEW.EXE", "C:\\SHORT\\OLD.EXE"};
    const std::string text =
        helper::wininit_rename_text(std::span<const helper::PostponedRename>(&row, 1));
    OA_CHECK(text == "[rename]\r\nC:\\SHORT\\OLD.EXE=C:\\SHORT\\NEW.EXE\r\n");

    const std::vector<std::string> arguments =
        helper::setup_arguments("C:\\Games\\Open Annihilation");
    OA_CHECK(arguments.size() == 2u);
    OA_CHECK(arguments[0] == "/S");
    OA_CHECK(arguments[1] == "/D=C:\\Games\\Open Annihilation");
    OA_CHECK(arguments[1].find('"') == std::string::npos);
    OA_CHECK(
        helper::setup_arguments("C:/Games/Open Annihilation/")[1] ==
        "/D=C:\\Games\\Open Annihilation"
    );
    OA_CHECK(helper::setup_arguments("C:\\")[1] == "/D=C:\\");
    OA_CHECK(helper::restart_line("0.8.7") == "Restart Windows to finish installing 0.8.7.");
}

update::Plan folder_plan(const Scratch& scratch, const fs::path& install) {
    update::Plan plan;
    plan.route = update::Route::windows_folder;
    plan.install = helper::utf8_of(install);
    plan.updates = helper::utf8_of(scratch.path / "updates");
    plan.file = helper::utf8_of(scratch.path / "package.zip");
    plan.preferences = helper::utf8_of(scratch.path / "preferences.conf");
    plan.program = helper::utf8_of(install / "open-annihilation.exe");
    return plan;
}

void installer_operations() {
    Scratch scratch("installer");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path staged = scratch.path / "stage";
    const fs::path top = staged / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    write_file(install / "open-annihilation.exe", "old-game");
    write_file(install / "oa-tool.exe", "old-tool");
    write_file(install / "oa-update.exe", "helper");
    write_file(install / "notes.txt", "player");
    write_file(install / "Uninstall.exe", "uninst");
    write_file(top / "open-annihilation.exe", "new-game");
    write_file(top / "oa-tool.exe", "new-tool");
    write_file(top / "oa-update.exe", "new-helper");
    write_file(top / "Uninstall.exe", "new-uninst");
    write_file(top / "readme.txt", "not-installed");
    write_file(updates / "previous" / "stale.txt", "stale");
    write_file(updates / "previous" / "preferences.conf", "prefs");

    update::Plan plan = folder_plan(scratch, install);
    plan.route = update::Route::windows_installer;
    plan.file = helper::utf8_of(scratch.path / "setup.exe");
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::windows_installer_operations(plan, staged, &why);
    OA_CHECK(why.empty());
    OA_CHECK(!operations.empty());
    OA_CHECK(operations.back().kind == helper::Operation::Kind::run_and_wait);
    OA_CHECK(operations.back().arguments == helper::setup_arguments(plan.install));
    OA_CHECK(operations.back().arguments[0] == "/S");
    OA_CHECK(operations.back().arguments[1].find('"') == std::string::npos);
    OA_CHECK(operations.back().arguments[1].find(' ') != std::string::npos);

    int copies = 0;
    bool removed_stale = false;
    bool removed_preferences = false;
    for (const helper::Operation& operation : operations) {
        const std::string name = operation.kind == helper::Operation::Kind::remove_tree
                                     ? helper::utf8_of(operation.path.filename())
                                     : helper::utf8_of(operation.from.filename());
        OA_CHECK(name != "notes.txt");
        OA_CHECK(name != "Uninstall.exe");
        OA_CHECK(name != "readme.txt");
        OA_CHECK(name != "oa-update.exe");
        OA_CHECK(name != "oa-update");
        if (operation.kind == helper::Operation::Kind::copy_tree)
            copies += 1;
        if (operation.kind == helper::Operation::Kind::remove_tree && name == "stale.txt")
            removed_stale = true;
        if (operation.kind == helper::Operation::Kind::remove_tree && name == "preferences.conf")
            removed_preferences = true;
    }
    OA_CHECK(copies == 2);
    OA_CHECK(removed_stale);
    OA_CHECK(!removed_preferences);
}

void folder_operations() {
    Scratch scratch("folder");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path staged = scratch.path / "stage";
    const fs::path top = staged / "Game";
    write_file(install / "previous" / "stale.txt", "stale");
    write_file(install / "open-annihilation.exe", "old-game");
    write_file(install / "oa-update.exe", "old-helper");
    write_file(install / "notes.txt", "player");
    write_file(top / "open-annihilation.exe", "new-game");
    write_file(top / "oa-update.exe", "new-helper");
    write_file(top / "extra.dll", "extra");
    const update::Plan plan = folder_plan(scratch, install);
    const fs::path helper_path = scratch.path / "oa-update.exe";
    const fs::path plan_file = scratch.path / "plan.json";

    std::string why;
    const helper::FolderPlan writable =
        helper::plan_windows_folder(plan, staged, true, 6u, helper_path, plan_file, &why);
    OA_CHECK(why.empty());
    OA_CHECK(!writable.elevate);
    OA_CHECK(!writable.operations.empty());
    bool removed = false;
    bool moved_old = false;
    bool moved_new = false;
    bool moved_old_helper = false;
    bool moved_new_helper = false;
    bool moved_notes = false;
    for (const helper::Operation& operation : writable.operations) {
        if (operation.kind == helper::Operation::Kind::remove_tree &&
            helper::utf8_of(operation.path.filename()) == "stale.txt")
            removed = true;
        if (operation.kind == helper::Operation::Kind::rename &&
            helper::utf8_of(operation.from.filename()) == "open-annihilation.exe" &&
            helper::utf8_of(operation.to.parent_path().filename()) == "previous")
            moved_old = true;
        if (operation.kind == helper::Operation::Kind::rename &&
            helper::utf8_of(operation.from.filename()) == "extra.dll" &&
            helper::utf8_of(operation.to.parent_path()) == helper::utf8_of(install))
            moved_new = true;
        if (operation.kind == helper::Operation::Kind::rename &&
            helper::utf8_of(operation.from.filename()) == "oa-update.exe" &&
            helper::utf8_of(operation.to.parent_path().filename()) == "previous")
            moved_old_helper = true;
        if (operation.kind == helper::Operation::Kind::rename &&
            helper::utf8_of(operation.from.filename()) == "oa-update.exe" &&
            helper::utf8_of(operation.to.parent_path()) == helper::utf8_of(install))
            moved_new_helper = true;
        if (helper::utf8_of(operation.from.filename()) == "notes.txt" ||
            helper::utf8_of(operation.path.filename()) == "notes.txt")
            moved_notes = true;
    }
    OA_CHECK(removed);
    OA_CHECK(moved_old);
    OA_CHECK(moved_new);
    OA_CHECK(moved_old_helper);
    OA_CHECK(moved_new_helper);
    OA_CHECK(!moved_notes);

    const helper::FolderPlan elevated =
        helper::plan_windows_folder(plan, staged, false, 6u, helper_path, plan_file, &why);
    OA_CHECK(why.empty());
    OA_CHECK(elevated.elevate);
    OA_CHECK(elevated.verb == "runas");
    OA_CHECK(elevated.program == helper::utf8_of(helper_path));
    OA_CHECK(elevated.operations.empty());
    OA_CHECK(elevated.arguments.size() == 3u);
    OA_CHECK(elevated.arguments[0] == "--elevated-swap");
    OA_CHECK(elevated.arguments[1] == "--plan");
    OA_CHECK(elevated.arguments[2] == helper::utf8_of(plan_file));

    const helper::FolderPlan xp =
        helper::plan_windows_folder(plan, staged, false, 5u, helper_path, plan_file, &why);
    OA_CHECK(!xp.elevate);
    OA_CHECK(!xp.operations.empty());
    const helper::FolderPlan nine =
        helper::plan_windows_folder(plan, staged, false, 0xC0000004u, helper_path, plan_file, &why);
    OA_CHECK(!nine.elevate);
    OA_CHECK(!nine.operations.empty());
}

void restore_and_markers() {
    Scratch scratch("restore");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path previous = scratch.path / "updates" / "previous";
    write_file(previous / "open-annihilation.exe", "old");
    write_file(previous / "preferences.conf", "prefs");
    write_file(previous / "bits" / "one.txt", "one");
    write_file(install / "open-annihilation.exe", "new");
    write_file(install / "notes.txt", "player");
    std::string error;
    OA_CHECK(helper::restore_kept_copy(previous, install, &error));
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old");
    OA_CHECK(read_file(install / "notes.txt") == "player");
    OA_CHECK(read_file(install / "bits" / "one.txt") == "one");
    OA_CHECK(!fs::exists(install / "preferences.conf"));

    write_file(install / "open-annihilation.exe", "new");
    write_file(scratch.path / "updates" / "installer-pending", helper::utf8_of(install));
    write_file(scratch.path / "updates" / "journal.json", "left");
    const helper::UpdatesGateResult restored = helper::take_pending(scratch.path / "updates");
    OA_CHECK(restored.gate == helper::UpdatesGate::ready);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old");
    OA_CHECK(!fs::exists(scratch.path / "updates" / "installer-pending"));
    OA_CHECK(fs::exists(scratch.path / "updates" / "journal.json"));

    write_file(
        scratch.path / "updates" / "installer-pending", helper::utf8_of(scratch.path / "missing")
    );
    const helper::UpdatesGateResult failed = helper::take_pending(scratch.path / "updates");
    OA_CHECK(failed.gate == helper::UpdatesGate::restore_failed);
    OA_CHECK(fs::exists(scratch.path / "updates" / "installer-pending"));

    fs::remove(scratch.path / "updates" / "installer-pending");
    write_file(scratch.path / "updates" / "reboot-pending", "0.8.7");
    const helper::UpdatesGateResult restart = helper::take_pending(scratch.path / "updates");
    OA_CHECK(restart.gate == helper::UpdatesGate::restart);
    OA_CHECK(restart.version == "0.8.7");
    OA_CHECK(!fs::exists(scratch.path / "updates" / "reboot-pending"));
    OA_CHECK(!fs::exists(scratch.path / "updates" / "journal.json"));
}

struct RecordedVersion {
    std::vector<std::string> versions;
};

bool record_version(
    void* context, const fs::path&, const std::vector<std::string>& arguments, std::string*
) {
    auto* recorded = static_cast<RecordedVersion*>(context);
    if (!arguments.empty())
        recorded->versions.push_back(arguments.front());
    return true;
}

void the_failure_line_follows_the_journal() {
    Scratch scratch("failure-line");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    OA_CHECK(helper::line_after_failed_change(updates) == helper::update_failed_put_back);
    OA_CHECK(
        std::string(helper::update_failed_put_back) == "The update failed. The game was put back."
    );
    write_file(updates / "journal.json", "{}");
    OA_CHECK(helper::line_after_failed_change(updates) == helper::update_failed_not_put_back);
    OA_CHECK(
        std::string(helper::update_failed_not_put_back) ==
        "The update failed. The game could not be put back."
    );
    OA_CHECK(
        std::string(helper::could_not_check) ==
        "The update could not be checked. Nothing was changed."
    );
}

void the_roll_back_failure_line_follows_the_journal() {
    Scratch scratch("roll-back-failure-line");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    OA_CHECK(
        std::string(helper::line_after_failed_roll_back(updates)) ==
        "The roll back failed. Nothing was changed."
    );
    write_file(updates / "journal.json", "{}");
    OA_CHECK(
        std::string(helper::line_after_failed_roll_back(updates)) ==
        "The roll back failed. The game could not be put back."
    );
}

void a_failed_setup_leaves_the_helper_alone() {
    Scratch scratch("failed-setup");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    const fs::path previous = updates / "previous";
    write_file(install / "open-annihilation.exe", "old-game");
    write_file(install / "oa-tool.exe", "old-tool");
    write_file(install / "oa-update.exe", "helper");
    write_file(install / "notes.txt", "player");
    write_file(install / "Uninstall.exe", "uninst");
    write_file(scratch.path / "preferences.conf", "prefs");

    update::Plan plan = folder_plan(scratch, install);
    plan.route = update::Route::windows_installer;
    plan.file = helper::utf8_of(scratch.path / "setup.exe");
    std::string why;
    std::vector<helper::Operation> operations =
        helper::windows_installer_operations(plan, {}, &why);
    OA_CHECK(why.empty());
    OA_CHECK(!operations.empty());
    OA_CHECK(operations.back().kind == helper::Operation::Kind::run_and_wait);
    operations.pop_back();
    for (const helper::Operation& operation : operations) {
        const std::string touched = operation.kind == helper::Operation::Kind::remove_tree
                                        ? helper::utf8_of(operation.path.filename())
                                        : helper::utf8_of(operation.from.filename());
        OA_CHECK(touched != "oa-update.exe");
        OA_CHECK(touched != "oa-update");
    }

    fs::create_directories(updates);
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, operations, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(read_file(previous / "open-annihilation.exe") == "old-game");
    OA_CHECK(read_file(previous / "oa-tool.exe") == "old-tool");
    OA_CHECK(!fs::exists(previous / "oa-update.exe"));
    OA_CHECK(read_file(install / "oa-update.exe") == "helper");

    // The setup changes other files and leaves the running helper where it is.
    // An older kept copy of that helper must not be written back over it.
    write_file(install / "open-annihilation.exe", "setup-game");
    write_file(install / "oa-tool.exe", "setup-tool");
    write_file(install / "oa-update.exe", "helper-still");
    write_file(previous / "oa-update.exe", "old-helper");
    OA_CHECK(helper::restore_kept_copy(previous, install, &error));
    OA_CHECK(error.empty());
    OA_CHECK(read_file(install / "oa-update.exe") == "helper-still");
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(read_file(install / "oa-tool.exe") == "old-tool");
    OA_CHECK(read_file(install / "notes.txt") == "player");
    OA_CHECK(read_file(install / "Uninstall.exe") == "uninst");
    OA_CHECK(!fs::exists(install / "preferences.conf"));

    write_file(install / "open-annihilation.exe", "setup-game");
    write_file(install / "oa-tool.exe", "setup-tool");
    write_file(install / "oa-update.exe", "helper-still");
    update::State state;
    state.installed_version = "0.8.7";
    state.kept_version = "0.8.6";
    state.kept_path = helper::utf8_of(previous);
    state.kept_route = update::Route::windows_installer;
    std::vector<helper::Operation> restore =
        helper::restore_operations(state, install, "0.8.7", &why);
    OA_CHECK(why.empty());
    for (const helper::Operation& operation : restore) {
        const std::string touched = operation.kind == helper::Operation::Kind::remove_tree
                                        ? helper::utf8_of(operation.path.filename())
                                        : helper::utf8_of(operation.from.filename());
        OA_CHECK(touched != "oa-update.exe");
        OA_CHECK(touched != "oa-update");
    }
    // The version step names a program. This test checks the files, not that step.
    if (!restore.empty() && restore.back().kind == helper::Operation::Kind::run_and_wait)
        restore.pop_back();
    OA_CHECK(
        helper::apply_journal(updates, restore, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(read_file(install / "oa-update.exe") == "helper-still");
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(read_file(install / "oa-tool.exe") == "old-tool");
    OA_CHECK(read_file(install / "notes.txt") == "player");
}

void journal_restores_the_folder_and_the_installer() {
    Scratch scratch("journal-restore");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path previous = install / "previous";
    const fs::path updates = scratch.path / "updates";
    write_file(previous / "open-annihilation.exe", "old");
    write_file(previous / "preferences.conf", "prefs");
    write_file(install / "open-annihilation.exe", "new");
    write_file(install / "notes.txt", "player");
    write_file(install / "oa-write-check.tmp", "scratch");
    update::State state;
    state.installed_version = "0.8.7";
    state.kept_version = "0.8.6";
    state.kept_path = helper::utf8_of(previous);
    state.kept_route = update::Route::windows_folder;
    std::string why;
    const std::vector<helper::Operation> folder =
        helper::restore_operations(state, install, "0.8.7", &why);
    OA_CHECK(why.empty());
    fs::create_directories(updates);
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, folder, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old");
    OA_CHECK(read_file(install / "notes.txt") == "player");
    OA_CHECK(read_file(install / "oa-write-check.tmp") == "scratch");
    OA_CHECK(!fs::exists(install / "preferences.conf"));
    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    OA_CHECK(read_file(install / "open-annihilation.exe") == "new");
    OA_CHECK(read_file(previous / "open-annihilation.exe") == "old");

    const fs::path kept = scratch.path / "kept";
    write_file(kept / "open-annihilation.exe", "kept-game");
    write_file(kept / "preferences.conf", "kept-prefs");
    write_file(install / "open-annihilation.exe", "new");
    state.kept_path = helper::utf8_of(kept);
    state.kept_route = update::Route::windows_installer;
    state.kept_version = "0.8.6";
    const std::vector<helper::Operation> installer =
        helper::restore_operations(state, install, "0.8.7", &why);
    OA_CHECK(why.empty());
    OA_CHECK(!installer.empty());
    OA_CHECK(installer.back().program == "oa-display-version");
    OA_CHECK(installer.back().arguments.size() == 1u);
    OA_CHECK(installer.back().arguments[0] == "0.8.6");
    OA_CHECK(installer.back().inverse_arguments.size() == 1u);
    OA_CHECK(installer.back().inverse_arguments[0] == "0.8.7");
    RecordedVersion recorded;
    helper::JournalHooks hooks;
    hooks.context = &recorded;
    hooks.run = record_version;
    OA_CHECK(
        helper::apply_journal(updates, installer, hooks, &error) == helper::JournalResult::finished
    );
    OA_CHECK(read_file(install / "open-annihilation.exe") == "kept-game");
    OA_CHECK(read_file(install / "notes.txt") == "player");
    OA_CHECK(!fs::exists(install / "preferences.conf"));
    OA_CHECK(helper::undo_journal(updates, hooks, &error));
    OA_CHECK(recorded.versions.size() == 2u);
    OA_CHECK(recorded.versions[0] == "0.8.6");
    OA_CHECK(recorded.versions[1] == "0.8.7");
    OA_CHECK(read_file(install / "open-annihilation.exe") == "new");
}

helper::MoveOutcome stay_put(void*, const fs::path&, const fs::path&, std::string*) {
    return helper::MoveOutcome::postponed;
}

void postponed_rename_undoes_as_nothing() {
    Scratch scratch("postpone");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    const fs::path source = scratch.path / "held.exe";
    write_file(source, "bytes");
    helper::Operation operation;
    operation.kind = helper::Operation::Kind::rename;
    operation.from = source;
    operation.to = scratch.path / "away.exe";
    helper::JournalHooks hooks;
    hooks.move = stay_put;
    std::string error;
    const std::vector<helper::Operation> operations{operation};
    OA_CHECK(
        helper::apply_journal(updates, operations, hooks, &error) == helper::JournalResult::finished
    );
    OA_CHECK(read_file(source) == "bytes");
    OA_CHECK(!fs::exists(scratch.path / "away.exe"));
    OA_CHECK(helper::undo_journal(updates, hooks, &error));
    OA_CHECK(read_file(source) == "bytes");
    OA_CHECK(!fs::exists(updates / "journal.json"));

    write_file(source, "bytes");
    helper::JournalHooks plain;
    OA_CHECK(
        helper::apply_journal(updates, operations, plain, &error) == helper::JournalResult::finished
    );
    std::error_code io;
    fs::rename(scratch.path / "away.exe", source, io);
    OA_CHECK(!io);
    OA_CHECK(helper::undo_journal(updates, plain, &error));
    OA_CHECK(read_file(source) == "bytes");
    OA_CHECK(!fs::exists(updates / "journal.json"));
}

void a_closed_window_can_let_go_of_font_files() {
    helper::ProgressWindow window;
    // The copy outside the install draws with the install's fonts.
    const fs::path beside = window.font_directory();
    OA_CHECK(beside.empty() || beside.filename() == "fonts");
    const fs::path installed = fs::path("Open Annihilation") / "fonts";
    window.use_font_directory(installed);
    OA_CHECK(window.font_directory() == installed);
    window.release_installed_files();
    window.show("Open Annihilation", "Installing", 0, false);
    window.allow_installed_files();
    window.release_installed_files();
    const helper::PlacedUpdateWindow placed =
        helper::place_update_window("Open Annihilation", "Installing", 0);
    OA_CHECK(placed.width > 0);
    OA_CHECK(placed.height > 0);
    OA_CHECK(!placed.line_text.empty());
}

void a_folder_swap_keeps_the_game_and_replaces_the_helper() {
    Scratch scratch("folder-helper");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path staged = scratch.path / "stage";
    const fs::path top = staged / "Game";
    const fs::path updates = scratch.path / "updates";
    write_file(install / "open-annihilation.exe", "old-game");
    write_file(install / "oa-update.exe", "old-helper");
    write_file(top / "open-annihilation.exe", "new-game");
    write_file(top / "oa-update.exe", "new-helper");
    write_file(updates / "oa-update-running.exe", "running-copy");
    const update::Plan plan = folder_plan(scratch, install);
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::windows_folder_operations(plan, staged, &why);
    OA_CHECK(why.empty());
    for (const helper::Operation& operation : operations) {
        const std::string name = operation.kind == helper::Operation::Kind::remove_tree
                                     ? helper::utf8_of(operation.path.filename())
                                     : helper::utf8_of(operation.from.filename());
        OA_CHECK(name != "oa-update-running.exe");
    }
    fs::create_directories(updates);
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, operations, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(read_file(install / "previous" / "open-annihilation.exe") == "old-game");
    OA_CHECK(read_file(install / "previous" / "oa-update.exe") == "old-helper");
    OA_CHECK(read_file(install / "open-annihilation.exe") == "new-game");
    OA_CHECK(read_file(install / "oa-update.exe") == "new-helper");
    OA_CHECK(read_file(updates / "oa-update-running.exe") == "running-copy");
    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(read_file(install / "oa-update.exe") == "old-helper");
}

void the_running_helper_steps_aside_before_the_swap() {
    Scratch scratch("step-aside");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(install);
    fs::create_directories(updates);
    const fs::path running = install / "oa-update.exe";
    write_file(running, "helper-bytes");
    OA_CHECK(helper::helper_image_blocks_swap(running, install));
    OA_CHECK(helper::helper_image_blocks_swap(install / "oa-update", install));
    OA_CHECK(!helper::helper_image_blocks_swap(install / "open-annihilation.exe", install));
    OA_CHECK(!helper::helper_image_blocks_swap(updates / "oa-update.exe", install));
    // The folder swap renames the helper's file and the setup writes it, so
    // both Windows routes continue from a copy outside the install; the
    // silent setup would otherwise skip the running helper and keep the old one.
    const update::Route folder_route = update::Route::windows_folder;
    const update::Route installer_route = update::Route::windows_installer;
    OA_CHECK(helper::helper_must_step_aside(folder_route, running, install, 0));
    OA_CHECK(helper::helper_must_step_aside(installer_route, running, install, 0));
    OA_CHECK(!helper::helper_must_step_aside(folder_route, running, install, 4242));
    OA_CHECK(!helper::helper_must_step_aside(installer_route, running, install, 4242));
    for (const update::Route other : {
             update::Route::check_folder, update::Route::macos_bundle, update::Route::linux_versions
         }) {
        OA_CHECK(!helper::helper_must_step_aside(other, running, install, 0));
    }
    const fs::path aside = helper::helper_aside_path(updates);
    OA_CHECK(aside.filename() == "oa-update-running.exe");
    OA_CHECK(!helper::helper_image_blocks_swap(aside, install));
    OA_CHECK(!helper::helper_must_step_aside(folder_route, aside, install, 0));
    OA_CHECK(!helper::helper_must_step_aside(installer_route, aside, install, 0));

    const fs::path plan_file = scratch.path / "plan.json";
    const helper::FeedKey feed_key{"ridge", "ed25519:abc"};
    const std::vector<std::string> arguments =
        helper::helper_aside_arguments(plan_file, {feed_key}, false, false, 4242);
    OA_CHECK(arguments.size() == 6u);
    OA_CHECK(arguments[0] == "--plan");
    OA_CHECK(arguments[1] == helper::utf8_of(plan_file));
    OA_CHECK(arguments[2] == "--feed-key");
    OA_CHECK(arguments[3] == "ridge=ed25519:abc");
    OA_CHECK(arguments[4] == "--helper-parent");
    OA_CHECK(arguments[5] == "4242");
    const std::vector<std::string> elevated =
        helper::helper_aside_arguments(plan_file, {}, true, true, 7);
    OA_CHECK(elevated.front() == "--elevated-swap");
    OA_CHECK(elevated[1] == "--plan");
    bool saw_stage = false;
    bool saw_parent = false;
    for (const std::string& argument : elevated) {
        if (argument == "--stage-only")
            saw_stage = true;
        if (argument == "--helper-parent")
            saw_parent = true;
        OA_CHECK(argument != "--feed-key");
    }
    OA_CHECK(saw_stage);
    OA_CHECK(saw_parent);

    write_file(install / "SDL3.dll", "sdl-bytes");
    write_file(install / "LIBGCC.DLL", "runtime");
    write_file(install / "notes.txt", "notes");
    write_file(install / "extra" / "nested.dll", "nested");
    write_file(updates / "SDL3.dll", "stale");

    std::string error;
    OA_CHECK(helper::copy_helper_aside(running, aside, &error));
    OA_CHECK(read_file(aside) == "helper-bytes");
    OA_CHECK(read_file(running) == "helper-bytes");
    OA_CHECK(read_file(updates / "SDL3.dll") == "sdl-bytes");
    OA_CHECK(read_file(updates / "LIBGCC.DLL") == "runtime");
    OA_CHECK(!fs::exists(updates / "notes.txt"));
    OA_CHECK(!fs::exists(updates / "nested.dll"));
    write_file(running, "newer");
    write_file(install / "SDL3.dll", "sdl-newer");
    OA_CHECK(helper::copy_helper_aside(running, aside, &error));
    OA_CHECK(read_file(aside) == "newer");
    OA_CHECK(read_file(updates / "SDL3.dll") == "sdl-newer");
}

/// A process list in which one other copy of the game runs from the install
/// until `closed` is set.
struct OtherCopy {
    std::string program;
    bool closed{false};
    int64_t now{0};
    int lines{0};
};

int64_t other_copy_now(void* context) {
    return static_cast<OtherCopy*>(context)->now;
}

void other_copy_sleep(void* context, int64_t milliseconds) {
    static_cast<OtherCopy*>(context)->now += milliseconds;
}

std::vector<helper::RunningProgram> other_copy_running(void* context) {
    const auto* copy = static_cast<const OtherCopy*>(context);
    if (copy->closed)
        return {};
    return {{4242, copy->program}};
}

void other_copy_line(void* context) {
    static_cast<OtherCopy*>(context)->lines += 1;
}

void nothing_moves_while_another_copy_runs() {
    Scratch scratch("other-copy");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    // A setup that did not finish left its kept files and its marker.
    write_file(updates / "previous" / "open-annihilation.exe", "old-game");
    write_file(updates / "installer-pending", helper::utf8_of(install));
    write_file(install / "open-annihilation.exe", "half-new");

    update::Plan plan = folder_plan(scratch, install);
    plan.route = update::Route::windows_installer;
    plan.wait_for = 0;
    OtherCopy copy;
    copy.program = helper::utf8_of(install / "open-annihilation.exe");
    helper::CopiesHooks copies;
    copies.context = &copy;
    copies.now_ms = other_copy_now;
    copies.sleep_ms = other_copy_sleep;
    copies.running = other_copy_running;
    copies.waiting = other_copy_line;
    helper::OpeningHooks hooks;
    hooks.copies = &copies;

    // The other copy stays open: the run stops with nothing moved, and the
    // window asked for the other windows to close.
    const helper::OpeningResult refused =
        helper::open_run(plan, false, 7, hooks, helper::JournalHooks{});
    OA_CHECK(refused.opening == helper::Opening::copies_running);
    OA_CHECK(refused.detail == helper::other_windows_open_text);
    OA_CHECK(copy.lines > 0);
    OA_CHECK(copy.now >= helper::wait_limit_ms);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "half-new");
    OA_CHECK(fs::exists(updates / "installer-pending"));

    // The copy with raised rights does not wait: the run that started it has.
    // A plan whose install holds no marker shows it carries on at once.
    update::Plan elevated = plan;
    elevated.updates = helper::utf8_of(scratch.path / "elsewhere");
    fs::create_directories(scratch.path / "elsewhere");
    copy.now = 0;
    copy.lines = 0;
    const helper::OpeningResult raised =
        helper::open_run(elevated, true, 7, hooks, helper::JournalHooks{});
    OA_CHECK(raised.opening == helper::Opening::carry_on);
    OA_CHECK(copy.lines == 0);

    // Once the other copy has closed, the kept files go back first.
    copy.closed = true;
    copy.now = 0;
    copy.lines = 0;
    const helper::OpeningResult opened =
        helper::open_run(plan, false, 7, hooks, helper::JournalHooks{});
    OA_CHECK(opened.opening == helper::Opening::carry_on);
    OA_CHECK(copy.lines == 0);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(!fs::exists(updates / "installer-pending"));
}

/// A Windows version, a write check and a copy with raised rights that
/// stands in for the one `runas` would start: it puts the files back as that
/// copy does, or ends as the test asks.
struct RaisedPutBack {
    uint32_t version{0x00000006u}; ///< Windows Vista
    bool writable{false};          ///< the player cannot write the install folder
    bool cancel{false};            ///< the player declines the prompt
    int code{0};                   ///< the copy's exit status
    int raised{0};                 ///< how many copies were started
    fs::path updates;              ///< the folder the last copy was given
};

uint32_t raised_version(void* context) {
    return static_cast<RaisedPutBack*>(context)->version;
}

bool raised_writable(void* context, const fs::path&) {
    return static_cast<RaisedPutBack*>(context)->writable;
}

helper::ElevatedRelaunch raised_copy(void* context, const fs::path& updates) {
    auto* const asked = static_cast<RaisedPutBack*>(context);
    asked->raised += 1;
    asked->updates = updates;
    helper::ElevatedRelaunch result;
    if (asked->cancel) {
        result.cancelled = true;
        return result;
    }
    result.ran = true;
    result.code = asked->code;
    if (asked->code == helper::exit_swapped) {
        std::string error;
        OA_CHECK(helper::put_back_marked(updates, &error));
    }
    return result;
}

helper::PutBackHooks raised_hooks(RaisedPutBack& asked) {
    helper::PutBackHooks hooks;
    hooks.context = &asked;
    hooks.windows_version = raised_version;
    hooks.writable = raised_writable;
    hooks.raise = raised_copy;
    return hooks;
}

void putting_kept_files_back_asks_for_rights_where_needed() {
    Scratch scratch("put-back");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    const fs::path marker = updates / "installer-pending";
    const auto half_installed = [&]() {
        write_file(updates / "previous" / "open-annihilation.exe", "old-game");
        write_file(install / "open-annihilation.exe", "half-new");
    };
    half_installed();
    RaisedPutBack asked;
    const helper::PutBackHooks hooks = raised_hooks(asked);
    std::string error;

    // A folder the player cannot write, on Vista or later: the copy with
    // raised rights puts the files back, never this process.
    OA_CHECK(helper::put_back_needs_elevation(install, &hooks));
    OA_CHECK(helper::put_back_kept(updates, install, &hooks, &error) == helper::PutBack::done);
    OA_CHECK(asked.raised == 1);
    OA_CHECK(asked.updates == updates);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(!fs::exists(marker));

    // The player declines: nothing goes back, and the marker stays for the
    // next run.
    half_installed();
    asked.cancel = true;
    OA_CHECK(helper::put_back_kept(updates, install, &hooks, &error) == helper::PutBack::cancelled);
    OA_CHECK(error == helper::permission_not_given);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "half-new");
    OA_CHECK(read_file(marker) == helper::utf8_of(install));

    // A copy that ends with a failure leaves the marker too.
    asked.cancel = false;
    asked.code = helper::exit_undone;
    OA_CHECK(helper::put_back_kept(updates, install, &hooks, &error) == helper::PutBack::failed);
    OA_CHECK(fs::exists(marker));

    // A folder the player can write, or Windows 9x, needs no prompt.
    asked.raised = 0;
    asked.code = helper::exit_swapped;
    asked.writable = true;
    OA_CHECK(!helper::put_back_needs_elevation(install, &hooks));
    OA_CHECK(helper::put_back_kept(updates, install, &hooks, &error) == helper::PutBack::done);
    OA_CHECK(asked.raised == 0);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    half_installed();
    asked.writable = false;
    asked.version = 0xC0000004u;
    OA_CHECK(!helper::put_back_needs_elevation(install, &hooks));
    OA_CHECK(helper::put_back_kept(updates, install, &hooks, &error) == helper::PutBack::done);
    OA_CHECK(asked.raised == 0);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");

    // A missing install asks nobody.
    asked.version = 0x00000006u;
    OA_CHECK(
        helper::put_back_kept(updates, scratch.path / "missing", &hooks, &error) ==
        helper::PutBack::failed
    );
    OA_CHECK(asked.raised == 0);

    // A marker the take_pending of a later run finds goes back the same way.
    half_installed();
    write_file(marker, helper::utf8_of(install));
    const helper::UpdatesGateResult taken = helper::take_pending(updates, &hooks);
    OA_CHECK(taken.gate == helper::UpdatesGate::ready);
    OA_CHECK(asked.raised == 1);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(!fs::exists(marker));
    half_installed();
    write_file(marker, helper::utf8_of(install));
    asked.cancel = true;
    const helper::UpdatesGateResult declined = helper::take_pending(updates, &hooks);
    OA_CHECK(declined.gate == helper::UpdatesGate::restore_failed);
    OA_CHECK(declined.detail == helper::permission_not_given);
    OA_CHECK(fs::exists(marker));

#if !defined(_WIN32)
    // The real write check: a folder this account cannot write. An account
    // that writes anywhere has nothing to check.
    if (::geteuid() != 0) {
        const fs::path locked = scratch.path / "locked";
        fs::create_directories(locked);
        fs::permissions(locked, fs::perms::owner_read | fs::perms::owner_exec);
        helper::PutBackHooks real_check;
        real_check.context = &asked;
        real_check.windows_version = raised_version;
        OA_CHECK(helper::put_back_needs_elevation(locked, &real_check));
        fs::permissions(locked, fs::perms::owner_all);
        OA_CHECK(!helper::put_back_needs_elevation(locked, &real_check));
    }
#endif
}

void a_marker_is_put_back_once_and_no_run_starts_another() {
    Scratch scratch("no-loop");
    const fs::path install = scratch.path / "Open Annihilation";
    const fs::path updates = scratch.path / "updates";
    const fs::path marker = updates / "installer-pending";
    // The setup could not replace the game while another copy ran; its kept
    // files and marker are left, and the player cannot write the install.
    write_file(updates / "previous" / "open-annihilation.exe", "old-game");
    write_file(marker, helper::utf8_of(install));
    write_file(install / "open-annihilation.exe", "half-new");
    update::Plan plan = folder_plan(scratch, install);
    plan.route = update::Route::windows_installer;
    plan.wait_for = 0;
    OtherCopy copy;
    copy.program = helper::utf8_of(install / "open-annihilation.exe");
    helper::CopiesHooks copies;
    copies.context = &copy;
    copies.now_ms = other_copy_now;
    copies.sleep_ms = other_copy_sleep;
    copies.running = other_copy_running;
    copies.waiting = other_copy_line;
    RaisedPutBack asked;
    const helper::PutBackHooks put_back = raised_hooks(asked);
    helper::OpeningHooks hooks;
    hooks.copies = &copies;
    hooks.put_back = &put_back;

    // While the other copy runs, every helper a quit starts stops before the
    // marker: no prompt, nothing put back, and the run starts nothing.
    for (int attempt = 0; attempt < 3; ++attempt) {
        copy.now = 0;
        const helper::OpeningResult refused =
            helper::open_run(plan, false, 7, hooks, helper::JournalHooks{});
        OA_CHECK(refused.opening == helper::Opening::copies_running);
    }
    OA_CHECK(asked.raised == 0);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "half-new");

    // Once it has closed, one run puts the files back with raised rights,
    // and the runs after it find nothing left to put back.
    copy.closed = true;
    const helper::OpeningResult first =
        helper::open_run(plan, false, 7, hooks, helper::JournalHooks{});
    OA_CHECK(first.opening == helper::Opening::carry_on);
    OA_CHECK(asked.raised == 1);
    OA_CHECK(read_file(install / "open-annihilation.exe") == "old-game");
    OA_CHECK(!fs::exists(marker));
    const helper::OpeningResult second =
        helper::open_run(plan, false, 7, hooks, helper::JournalHooks{});
    OA_CHECK(second.opening == helper::Opening::carry_on);
    OA_CHECK(asked.raised == 1);
}

void an_open_file_keeps_its_folder() {
    Scratch scratch("held-folder");
    const fs::path folder = scratch.path / "fonts";
    const fs::path font_file = folder / "face.ttf";
    const fs::path moved = scratch.path / "fonts-moved";
    write_file(font_file, "font");
    OA_CHECK(fs::exists(font_file));
#if defined(_WIN32)
    std::FILE* held = _wfsopen(font_file.c_str(), L"rb", _SH_DENYNO);
    OA_CHECK(held != nullptr);
    if (held != nullptr) {
        std::error_code io;
        fs::rename(folder, moved, io);
        if (!io) {
            // The folder moved while the font file was open. A desktop
            // Windows refuses that rename, and the checks below still
            // require the refusal there. A system that renames anyway has
            // no lock to require; the file went with the folder.
            std::fclose(held);
        } else {
            OA_CHECK(fs::exists(font_file));
            std::fclose(held);
            io.clear();
            fs::rename(folder, moved, io);
            OA_CHECK(!io);
        }
        OA_CHECK(fs::exists(moved / "face.ttf"));
    }
#endif
}

} // namespace

void test_windows_routes() {
    version_and_elevation();
    retries();
    wininit_text_and_setup_arguments();
    installer_operations();
    folder_operations();
    the_failure_line_follows_the_journal();
    the_roll_back_failure_line_follows_the_journal();
    a_failed_setup_leaves_the_helper_alone();
    restore_and_markers();
    journal_restores_the_folder_and_the_installer();
    postponed_rename_undoes_as_nothing();
    a_folder_swap_keeps_the_game_and_replaces_the_helper();
    the_running_helper_steps_aside_before_the_swap();
    a_closed_window_can_let_go_of_font_files();
    nothing_moves_while_another_copy_runs();
    putting_kept_files_back_asks_for_rights_where_needed();
    a_marker_is_put_back_once_and_no_run_starts_another();
    an_open_file_keeps_its_folder();
}
