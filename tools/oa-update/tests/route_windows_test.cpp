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
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

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
    write_file(install / "notes.txt", "player");
    write_file(top / "open-annihilation.exe", "new-game");
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
        if (helper::utf8_of(operation.from.filename()) == "notes.txt" ||
            helper::utf8_of(operation.path.filename()) == "notes.txt")
            moved_notes = true;
    }
    OA_CHECK(removed);
    OA_CHECK(moved_old);
    OA_CHECK(moved_new);
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

} // namespace

void test_windows_routes() {
    version_and_elevation();
    retries();
    wininit_text_and_setup_arguments();
    installer_operations();
    folder_operations();
    the_failure_line_follows_the_journal();
    a_failed_setup_leaves_the_helper_alone();
    restore_and_markers();
    journal_restores_the_folder_and_the_installer();
    postponed_rename_undoes_as_nothing();
}
