// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Linux versions route, against scratch folders and real links. Undoing
// either update puts the folder back. A system that cannot hold a link skips
// the cases. Nothing is fetched and no program is started.

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/routes.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
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
               (std::string("oa-v10-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

/// True when this system can make a symlink.
///
/// @param folder a folder this test may write
/// @return false when the probe link cannot be made
bool symlinks_work(const fs::path& folder) {
    std::error_code io;
    fs::create_symlink("target", folder / "probe-link", io);
    const bool made = !io;
    fs::remove(folder / "probe-link", io);
    return made;
}

void write_text(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out << text;
    OA_CHECK(static_cast<bool>(out));
}

std::string read_text(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// One directory entry, as a byte-for-byte picture of the tree. A link is not followed.
struct Entry {
    std::string name;
    char kind{'?'};
    std::string body;

    bool operator==(const Entry& other) const {
        return name == other.name && kind == other.kind && body == other.body;
    }
};

void collect(const fs::path& root, const fs::path& folder, std::vector<Entry>& out) {
    std::error_code io;
    fs::directory_iterator it(folder, io);
    while (!io && it != fs::directory_iterator()) {
        const fs::path child = it->path();
        const fs::file_status status = fs::symlink_status(child, io);
        Entry entry;
        std::error_code relative_error;
        entry.name = helper::utf8_of(fs::relative(child, root, relative_error));
        if (fs::is_symlink(status)) {
            entry.kind = 'l';
            std::error_code link_error;
            entry.body = helper::utf8_of(fs::read_symlink(child, link_error));
        } else if (fs::is_directory(status)) {
            entry.kind = 'd';
            out.push_back(entry);
            collect(root, child, out);
            it.increment(io);
            continue;
        } else if (fs::is_regular_file(status)) {
            entry.kind = 'f';
            entry.body = read_text(child);
            const auto mode = static_cast<unsigned>(status.permissions());
            entry.body.push_back('\n');
            entry.body += std::to_string(mode);
        }
        out.push_back(std::move(entry));
        it.increment(io);
    }
    OA_CHECK(!io);
}

std::vector<Entry> snapshot(const fs::path& root) {
    std::vector<Entry> entries;
    collect(root, root, entries);
    std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) {
        return left.name < right.name;
    });
    return entries;
}

void expect_same(const std::vector<Entry>& before, const std::vector<Entry>& after) {
    OA_CHECK(before.size() == after.size());
    if (before.size() != after.size())
        return;
    for (std::size_t index = 0; index < before.size(); ++index) {
        OA_CHECK(before[index].name == after[index].name);
        OA_CHECK(before[index].kind == after[index].kind);
        OA_CHECK(before[index].body == after[index].body);
    }
}

std::size_t count_phrase(std::string_view text, std::string_view phrase) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(phrase, at)) != std::string_view::npos;
         at += phrase.size())
        ++count;
    return count;
}

update::Plan linux_plan(const fs::path& install, std::string_view package, std::string_view from) {
    update::Plan plan;
    plan.route = update::Route::linux_versions;
    plan.install = helper::utf8_of(install);
    plan.package = std::string(package);
    plan.from_version = std::string(from);
    return plan;
}

void stage_package(const fs::path& stage) {
    const fs::path top = stage / "open-annihilation-0.8.7-steam-deck";
    write_text(top / "open-annihilation", "new-game");
    write_text(top / "oa-tool", "new-tool");
    write_text(top / "LICENSE", "new-license");
    write_text(top / "languages/en.txt", "new-lang");
    write_text(top / "steam-deck/art.txt", "new-art");
}

bool apply_plan(const update::Plan& plan, const fs::path& stage, const fs::path& updates) {
    std::string why;
    const std::vector<helper::Operation> operations = helper::plan_operations(plan, stage, &why);
    OA_CHECK(why.empty());
    if (!why.empty())
        return false;
    fs::create_directories(updates);
    std::string error;
    const helper::JournalResult result =
        helper::apply_journal(updates, operations, helper::JournalHooks{}, &error);
    OA_CHECK(result == helper::JournalResult::finished);
    OA_CHECK(error.empty());
    return result == helper::JournalResult::finished;
}

void first_update_keeps_a_players_file(const fs::path& root) {
    const fs::path install = root / "install";
    const fs::path stage = root / "stage";
    const fs::path updates = root / "updates";
    write_text(install / "open-annihilation", "old-game");
    write_text(install / "oa-update", "old-update");
    write_text(install / "LICENSE", "old-license");
    write_text(install / "languages/en.txt", "old-lang");
    write_text(install / "steam-deck/art.txt", "old-art");
    write_text(install / "notes.txt", "player");
    stage_package(stage);
    const std::vector<Entry> before = snapshot(install);

    const update::Plan plan = linux_plan(install, "steam-deck", "0.8.6");
    if (!apply_plan(plan, stage, updates))
        return;

    OA_CHECK(read_text(install / "notes.txt") == "player");
    OA_CHECK(!fs::exists(install / "open-annihilation.new"));
    OA_CHECK(read_text(install / "versions/0.8.6/open-annihilation") == "old-game");
    OA_CHECK(read_text(install / "versions/0.8.6/steam-deck/art.txt") == "old-art");
    OA_CHECK(read_text(install / "versions/0.8.7/open-annihilation") == "new-game");
    OA_CHECK(!fs::exists(install / "versions/0.8.7/steam-deck"));
    OA_CHECK(read_text(install / "steam-deck/art.txt") == "new-art");
    OA_CHECK(fs::is_symlink(fs::symlink_status(install / "current")));
    OA_CHECK(helper::utf8_of(fs::read_symlink(install / "current")) == "versions/0.8.6");
    OA_CHECK(fs::is_symlink(fs::symlink_status(install / "next")));
    OA_CHECK(helper::utf8_of(fs::read_symlink(install / "next")) == "versions/0.8.7");
    const std::string launcher = read_text(install / "open-annihilation");
    OA_CHECK(launcher == std::string(helper::linux_launcher_script()));
    OA_CHECK(launcher.starts_with("#!/bin/sh\n"));
    OA_CHECK(count_phrase(launcher, "mv -T") == 2);
#if !defined(_WIN32)
    const fs::perms bits = fs::status(install / "open-annihilation").permissions();
    const fs::perms expect = fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                             fs::perms::others_read | fs::perms::others_exec;
    OA_CHECK((bits & expect) == expect);
#endif

    std::string error;
    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    OA_CHECK(error.empty());
    expect_same(before, snapshot(install));
}

void later_update_replaces_next_and_drops_old_versions(const fs::path& root) {
    const fs::path install = root / "install";
    const fs::path stage = root / "stage";
    const fs::path updates = root / "updates";
    write_text(install / "versions/0.8.4/open-annihilation", "older-game");
    write_text(install / "versions/0.8.5/open-annihilation", "old-game");
    write_text(install / "steam-deck/art.txt", "old-art");
    write_text(install / "notes.txt", "player");
    write_text(install / "open-annihilation", "launcher-bytes\n");
    std::error_code io;
    fs::create_symlink("versions/0.8.5", install / "current", io);
    OA_CHECK(!io);
    fs::create_symlink("versions/0.8.4", install / "next", io);
    OA_CHECK(!io);
    stage_package(stage);
    const std::vector<Entry> before = snapshot(install);

    const update::Plan plan = linux_plan(install, "steam-deck", "0.8.5");
    if (!apply_plan(plan, stage, updates))
        return;

    OA_CHECK(read_text(install / "notes.txt") == "player");
    OA_CHECK(read_text(install / "open-annihilation") == "launcher-bytes\n");
    OA_CHECK(helper::utf8_of(fs::read_symlink(install / "current")) == "versions/0.8.5");
    OA_CHECK(helper::utf8_of(fs::read_symlink(install / "next")) == "versions/0.8.7");
    OA_CHECK(!fs::exists(install / "versions/0.8.4"));
    OA_CHECK(read_text(install / "versions/0.8.5/open-annihilation") == "old-game");
    OA_CHECK(read_text(install / "versions/0.8.7/open-annihilation") == "new-game");
    OA_CHECK(!fs::exists(install / "versions/0.8.7/steam-deck"));
    OA_CHECK(read_text(install / "steam-deck/art.txt") == "new-art");

    std::string error;
    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    OA_CHECK(error.empty());
    expect_same(before, snapshot(install));
}

void restoring_points_current_at_the_kept_version(const fs::path& root) {
    const fs::path install = root / "install";
    const fs::path updates = root / "updates";
    fs::create_directories(install / "versions" / "0.8.6");
    fs::create_directories(install / "versions" / "0.8.7");
    write_text(install / "versions/0.8.6/open-annihilation", "old-game");
    write_text(install / "versions/0.8.7/open-annihilation", "new-game");
    write_text(install / "notes.txt", "player");
    write_text(install / "update-starts", "2\n");
    std::error_code io;
    fs::create_symlink("versions/0.8.7", install / "current", io);
    OA_CHECK(!io);
    const std::vector<Entry> before = snapshot(install);

    update::State state;
    state.installed_version = "0.8.7";
    state.kept_version = "0.8.6";
    state.kept_path = helper::utf8_of(install / "versions" / "0.8.6");
    state.kept_route = update::Route::linux_versions;
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::linux_restore_operations(state, install, &why);
    OA_CHECK(why.empty());
    fs::create_directories(updates);
    std::string error;
    OA_CHECK(
        helper::apply_journal(updates, operations, helper::JournalHooks{}, &error) ==
        helper::JournalResult::finished
    );
    OA_CHECK(helper::utf8_of(fs::read_symlink(install / "current")) == "versions/0.8.6");
    OA_CHECK(read_text(install / "notes.txt") == "player");
    OA_CHECK(read_text(install / "versions/0.8.7/open-annihilation") == "new-game");
    OA_CHECK(!fs::exists(install / "update-starts"));

    OA_CHECK(helper::undo_journal(updates, helper::JournalHooks{}, &error));
    OA_CHECK(error.empty());
    expect_same(before, snapshot(install));
}

} // namespace

void test_linux_versions_route() {
    Scratch probe("links");
    if (!symlinks_work(probe.path))
        return;
    {
        Scratch first("linux-first");
        first_update_keeps_a_players_file(first.path);
    }
    {
        Scratch later("linux-later");
        later_update_replaces_next_and_drops_old_versions(later.path);
    }
    {
        Scratch restored("linux-restore");
        restoring_points_current_at_the_kept_version(restored.path);
    }
}
