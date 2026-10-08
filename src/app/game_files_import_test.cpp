// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The import core (game_files_import.hpp) over synthetic folders: the
// leave-out table case by case, safe names, the name check and its nested
// search, case folding, the parts and switches, the space a copy needs, the
// listing, the scan, the copy with its stops, expiry, failures and resumes,
// the chunked copy, the commit, recovery from each state, adopting what the
// player copied, additions and mods, removals waiting for the next start, and
// the installed summary. Scripted GameFilesHooks stand in for the platform.
// With --data: a subset of the installed game OA_GAME_DIR names copied,
// checked and committed, and the demo route over the installer
// OA_DEMO_INSTALLER names (each skipped without its variable).
#include "oa/app/game_files_import.hpp"
#include "oa/base/threads.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/system.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <vector>

namespace {

using namespace oa::app::game_files;
using oa::app::CopyOutcome;
using oa::app::demo_1997;
using oa::app::FileCopy;
using oa::app::GameFilesHooks;
using oa::app::GameInstall;
using oa::app::ModChoice;
using oa::app::path_from_utf8;
using oa::app::path_to_utf8;
using oa::app::SourceEntry;

namespace threads = oa::base::threads;

/// The modified time every synthetic source file gets, seconds since 1970.
constexpr int64_t source_time = 1'000'000'000;
/// A time a changed source file gets.
constexpr int64_t changed_time = 1'100'000'000;
/// Bytes per mebibyte.
constexpr uint64_t mebibyte = 1u << 20;
/// A mod profile the engine resolves.
constexpr std::string_view example_profile = "oamod: 1\n"
                                             "id: example\n"
                                             "name: Example mod\n"
                                             "version: \"1.0\"\n"
                                             "requires: {base: ta-3.1c, catalogue: 1}\n"
                                             "author:\n"
                                             "  name: unknown\n"
                                             "packaging:\n"
                                             "  revision: 1\n"
                                             "  date: 2026-10-04\n"
                                             "  packager: Open Annihilation\n"
                                             "identity: {display-version: \"9.9\", "
                                             "network-version: [9, 9], side-names: [Red, Blue]}\n";

/// Writes bytes to a file, making its folders.
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary)
        .write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    fs::last_write_time(path, file_time_from_seconds(source_time));
}

/// Writes text to a file, making its folders.
void write_text(const fs::path& path, std::string_view text) {
    write_bytes(path, std::vector<uint8_t>(text.begin(), text.end()));
}

/// Writes a file of a given size, its bytes counting up.
void write_sized(const fs::path& path, uint64_t size, uint8_t seed = 0) {
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(index * 7 + seed);
    write_bytes(path, bytes);
}

/// Writes a file of the demo installer's size without writing its bytes.
void write_installer_sized(const fs::path& path) {
    fs::create_directories(path.parent_path());
    {
        std::ofstream out(path, std::ios::binary);
    }
    fs::resize_file(path, demo_1997.installer_size);
    fs::last_write_time(path, file_time_from_seconds(source_time));
}

/// Reads a file whole.
std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// Makes an archive holding the named files.
std::vector<uint8_t> archive_of(std::initializer_list<const char*> names) {
    std::vector<oa::HpiWriteFile> files;
    for (const char* name : names)
        files.push_back({name, {'T', 'A'}});
    return oa::write_hpi(files);
}

/// Makes an archive holding the four resources the frontend opens first: a playable folder.
std::vector<uint8_t> playable_archive() {
    return archive_of(
        {"guis/mainmenu.gui", "palettes/palette.pal", "gamedata/sidedata.tdf", "gamedata/sound.tdf"}
    );
}

/// Tells whether a path exists, a link included.
bool there(const fs::path& path) {
    std::error_code error;
    return fs::exists(fs::symlink_status(path, error));
}

/// Finds a planned file by its source path.
const PlannedFile* planned(const ImportPlan& plan, std::string_view path) {
    for (const auto& file : plan.files)
        if (file.path == path)
            return &file;
    return nullptr;
}

/// Finds a left-out file by its source path.
const LeftOutFile* left(const ImportPlan& plan, std::string_view path) {
    for (const auto& file : plan.left_out)
        if (file.path == path)
            return &file;
    return nullptr;
}

/// Makes a listing entry of a file.
SourceEntry file_entry(std::string path, uint64_t size = 10, bool remote = false) {
    SourceEntry entry;
    entry.path = std::move(path);
    entry.size = size;
    entry.modified = source_time;
    entry.remote = remote;
    return entry;
}

/// What the scripted platform hooks do and saw.
struct Script {
    int calls{};                                ///< copy_file calls
    int copies{};                               ///< of which reached the copy
    int fail_at{-1};                            ///< the call that fails, from 0
    CopyOutcome fail_with{CopyOutcome::copied}; ///< how it fails
    int stop_at{-1};                            ///< the call that presses Stop first
    int expire_at{-1};                          ///< the call the background time ends at
    ImportRun* run{};                           ///< the run Stop is pressed on
    bool free_known = true;                     ///< free_space answers
    uint64_t free{std::numeric_limits<uint64_t>::max() / 2}; ///< bytes free
    std::vector<std::pair<std::string, bool>> backed_up{};   ///< set_backed_up calls
    std::vector<bool> keep_running{};                        ///< keep_running calls
    void (*expiring)(void* userdata){};                      ///< keep_running's expiry
    void* expiring_userdata{};                               ///< passed to it
    std::vector<SourceEntry> listing{};                      ///< what list_source reports
    std::vector<uint32_t> depths{};                          ///< list_source's depths asked
};

/// Copies one file as the platform would, with the script's interruptions.
CopyOutcome scripted_copy(void* context, const FileCopy& file, std::string* error) {
    auto& script = *static_cast<Script*>(context);
    const int call = script.calls++;
    if (call == script.stop_at && script.run != nullptr)
        script.run->stop();
    if (call == script.expire_at && script.expiring != nullptr)
        script.expiring(script.expiring_userdata);
    if (call == script.fail_at) {
        *error = "scripted";
        return script.fail_with;
    }
    ++script.copies;
    return file.copy(file.source, file, error);
}

/// Answers the free space from the script.
bool scripted_free_space(void* context, const char*, uint64_t* bytes) {
    const auto& script = *static_cast<Script*>(context);
    *bytes = script.free;
    return script.free_known;
}

/// Records a time-away request.
void scripted_keep_running(
    void* context, bool running, void (*expiring)(void* userdata), void* userdata
) {
    auto& script = *static_cast<Script*>(context);
    script.keep_running.push_back(running);
    script.expiring = running ? expiring : nullptr;
    script.expiring_userdata = running ? userdata : nullptr;
}

/// Records a backup setting.
bool scripted_backed_up(void* context, const char* path, bool backed_up) {
    static_cast<Script*>(context)->backed_up.emplace_back(path, backed_up);
    return true;
}

/// Lists the script's entries down to the depth asked.
bool scripted_list(
    void* context,
    const char*,
    uint32_t max_depth,
    void (*entry)(void* userdata, const SourceEntry& entry),
    void* userdata,
    std::string*
) {
    auto& script = *static_cast<Script*>(context);
    script.depths.push_back(max_depth);
    for (const auto& listed : script.listing)
        if (static_cast<uint32_t>(std::count(listed.path.begin(), listed.path.end(), '/')) <=
            max_depth)
            entry(userdata, listed);
    return true;
}

/// Makes hooks over a script: copy, free space, time away and backups; the listing too when
/// `listing` is set.
GameFilesHooks hooks_of(Script& script, bool listing = false) {
    GameFilesHooks hooks;
    hooks.context = &script;
    hooks.copy_file = scripted_copy;
    hooks.free_space = scripted_free_space;
    hooks.keep_running = scripted_keep_running;
    hooks.set_backed_up = scripted_backed_up;
    if (listing)
        hooks.list_source = scripted_list;
    return hooks;
}

/// Waits for a scan to end.
ScanSnapshot finish(SourceScan& scan) {
    for (int waited = 0; waited < 60'000 && scan.busy(); ++waited)
        threads::sleep_ms(1);
    return scan.snapshot();
}

/// Waits for a run to end.
RunSnapshot finish(ImportRun& run) {
    run.join();
    return run.snapshot();
}

/// The folders of one case: <root>/Documents/Total Annihilation and <root>/data folder.
ImportPaths case_paths(const fs::path& root) {
    fs::create_directories(root / "Documents");
    return import_paths(root / "Documents" / "Total Annihilation", root / "data folder");
}

/// Scans a folder and returns the plan, which the case expects to be made.
std::shared_ptr<const ImportPlan> scan_plan(
    const GameFilesHooks& hooks,
    const ImportPaths& paths,
    const fs::path& folder,
    SourceKind kind = SourceKind::game_folder,
    ScanSnapshot* out = nullptr
) {
    SourceScan scan;
    ScanRequest request;
    request.kind = kind;
    request.paths = {path_to_utf8(folder)};
    std::string error;
    OA_CHECK(scan.start(hooks, paths, request, &error));
    const auto snapshot = finish(scan);
    OA_CHECK(snapshot.stage == ScanStage::planned);
    OA_CHECK(snapshot.plan != nullptr);
    if (out != nullptr)
        *out = snapshot;
    return snapshot.plan ? snapshot.plan : std::make_shared<const ImportPlan>();
}

/// Runs a plan to its end and returns the last snapshot.
RunSnapshot run_plan(
    const GameFilesHooks& hooks,
    const ImportPaths& paths,
    std::shared_ptr<const ImportPlan> plan,
    ImportMode mode = ImportMode::replace,
    Script* script = nullptr,
    const Switches& switches = {}
) {
    ImportRun run;
    if (script != nullptr)
        script->run = &run;
    RunRequest request;
    request.mode = mode;
    std::string error;
    OA_CHECK(run.start(hooks, paths, std::move(plan), switches, request, &error));
    const auto snapshot = finish(run);
    if (script != nullptr)
        script->run = nullptr;
    return snapshot;
}

/// The leave-out table, case by case, and the installer-size exception.
void leave_out_table() {
    const auto reason = [](std::string_view path, uint64_t size = 10, bool archives = true) {
        return left_out(path, size, archives);
    };
    for (const auto* name :
         {"Setup.exe",
          "LIB.DLL",
          "patch.msi",
          "data1.cab",
          "Launch Total Annihilation.lnk",
          "saver.scr",
          "tools/editor.EXE"})
        OA_CHECK(reason(name) == LeftOutReason::windows_program);
    for (const auto* name :
         {"game.ico",
          "manual.pdf",
          "readme.doc",
          "notes.RTF",
          "TAE.hlp",
          "help.chm",
          "help.cnt",
          "Docs/readme.doc"})
        OA_CHECK(reason(name) == LeftOutReason::help_file);
    for (const auto* name : {"unins000.exe", "unins000.dat", "unins12.ini", "UNINS000.MSG"})
        OA_CHECK(reason(name) == LeftOutReason::uninstaller);
    OA_CHECK(reason("unins.exe") == LeftOutReason::windows_program);
    OA_CHECK(!reason("unins000.txt"));
    OA_CHECK(!reason("uninstall.txt"));
    for (const auto* name :
         {".DS_Store",
          "._totala1.hpi",
          "Thumbs.db",
          "music/desktop.ini",
          "__MACOSX/totala1.hpi",
          "a/.hidden/x.hpi",
          "THUMBS.DB"})
        OA_CHECK(reason(name) == LeftOutReason::system_clutter);
    for (const auto* name :
         {"totala1.hpi",
          "readme.txt",
          "totala.ini",
          "music/1.mp3",
          "Data/1.ZRB",
          "camps/briefs/x.txt",
          "webcache.zip",
          "mods/x/oamod.yaml"})
        OA_CHECK(!reason(name));
    // A folder without archives keeps a file of the installer's size whatever its name.
    OA_CHECK(!reason("Total Annihilation.exe", demo_1997.installer_size, false));
    OA_CHECK(!reason("unins000.exe", demo_1997.installer_size, false));
    OA_CHECK(
        reason("Total Annihilation.exe", demo_1997.installer_size, true) ==
        LeftOutReason::windows_program
    );
    OA_CHECK(
        reason("Total Annihilation.exe", demo_1997.installer_size + 1, false) ==
        LeftOutReason::windows_program
    );
    OA_CHECK(reason(".DS_Store", demo_1997.installer_size, false) == LeftOutReason::system_clutter);
}

/// Unsafe names are left out, as the zip module refuses them.
void unsafe_names() {
    for (const std::string& name :
         {std::string{},
          std::string("/totala1.hpi"),
          std::string("C:/x.hpi"),
          std::string("a/../b.hpi"),
          std::string("a\\b.hpi"),
          std::string("a//b.hpi"),
          std::string("./a.hpi"),
          std::string("a/"),
          std::string("a\0b.hpi", 7),
          std::string("..")})
        OA_CHECK(left_out(name, 10, true) == LeftOutReason::unsafe_name);
    // Unsafe names in a listing are left out of the plan, not copied.
    const std::vector<SourceEntry> entries{
        file_entry("totala1.hpi"), file_entry("a/../escape.hpi"), file_entry("x\\y.txt")
    };
    const auto plan = plan_import(SourceKind::game_folder, "/source", "Source", entries);
    OA_CHECK(plan.files.size() == 1);
    OA_CHECK(
        left(plan, "a/../escape.hpi") &&
        left(plan, "a/../escape.hpi")->reason == LeftOutReason::unsafe_name
    );
    OA_CHECK(
        left(plan, "x\\y.txt") && left(plan, "x\\y.txt")->reason == LeftOutReason::unsafe_name
    );
}

/// The name check: the top, two levels down stopping at the first level that has a game, a
/// top holding oamod.yaml or the installer, the game folder itself and a folder beside it.
void name_check(const fs::path& scratch) {
    const auto root = scratch / "names";
    const auto paths = case_paths(root);
    const GameFilesHooks hooks;
    write_bytes(root / "top" / "TOTALA1.HPI", {1});
    OA_CHECK(check_names(hooks, paths, root / "top").look == TopLook::game);
    write_text(root / "mod" / "OAMod.yaml", "oamod: 1\n");
    OA_CHECK(check_names(hooks, paths, root / "mod").look == TopLook::game);
    write_installer_sized(root / "installer" / "anything.bin");
    OA_CHECK(check_names(hooks, paths, root / "installer").look == TopLook::game);
    // An installer kept beside a game folder: the game folder below is offered.
    write_installer_sized(root / "beside" / "Total Annihilation.exe");
    write_bytes(root / "beside" / "Total Annihilation" / "totala1.hpi", {1});
    auto beside = check_names(hooks, paths, root / "beside");
    OA_CHECK(beside.look == TopLook::nested);
    OA_CHECK(beside.nested == std::vector<std::string>{"Total Annihilation"});
    write_bytes(root / "update" / "rev31.gp3", {1});
    OA_CHECK(check_names(hooks, paths, root / "update").look == TopLook::game);
    write_bytes(root / "other gp3" / "modrev.gp3", {1});
    OA_CHECK(check_names(hooks, paths, root / "other gp3").look == TopLook::nothing);

    // The first level that has any wins over the level below it.
    write_bytes(root / "drive" / "Games" / "Total Annihilation" / "totala1.hpi", {1});
    write_bytes(root / "drive" / "Other" / "ccdata.ccx", {1});
    auto check = check_names(hooks, paths, root / "drive");
    OA_CHECK(check.look == TopLook::nested);
    OA_CHECK(check.nested == std::vector<std::string>{"Other"});
    write_bytes(root / "drive2" / "Games" / "Total Annihilation" / "totala1.hpi", {1});
    write_bytes(root / "drive2" / "Games" / "TA2" / "rev31.gp3", {1});
    write_text(root / "drive2" / "readme.txt", "x");
    check = check_names(hooks, paths, root / "drive2");
    OA_CHECK(check.look == TopLook::nested);
    OA_CHECK(check.nested == (std::vector<std::string>{"Games/TA2", "Games/Total Annihilation"}));
    // Three levels down is too far, and clutter folders are not searched.
    write_bytes(root / "deep" / "a" / "b" / "c" / "totala1.hpi", {1});
    OA_CHECK(check_names(hooks, paths, root / "deep").look == TopLook::nothing);
    write_bytes(root / "trash" / ".Trash" / "totala1.hpi", {1});
    OA_CHECK(check_names(hooks, paths, root / "trash").look == TopLook::nothing);
    fs::create_directories(root / "empty");
    check = check_names(hooks, paths, root / "empty");
    OA_CHECK(check.look == TopLook::nothing && check.error.empty() && check.nested.empty());
    check = check_names(hooks, paths, root / "missing");
    OA_CHECK(check.look == TopLook::nothing && !check.error.empty());

    write_bytes(paths.game_folder / "totala1.hpi", {1});
    OA_CHECK(check_names(hooks, paths, paths.game_folder).look == TopLook::already_there);
    write_bytes(paths.documents / "TA Commander Pack" / "totala1.hpi", {1});
    OA_CHECK(
        check_names(hooks, paths, paths.documents / "TA Commander Pack").look ==
        TopLook::in_documents
    );
    // A picker's path may end with a separator.
    const auto with_separator =
        path_from_utf8(path_to_utf8(paths.documents / "TA Commander Pack") + "/");
    OA_CHECK(check_names(hooks, paths, with_separator).look == TopLook::in_documents);
    write_text(paths.documents / "Notes" / "readme.txt", "x");
    OA_CHECK(check_names(hooks, paths, paths.documents / "Notes").look == TopLook::nothing);
}

/// Names that differ only in capital letters: folders fold onto the first spelling, and the
/// second of two such files is left out with the warning.
void case_folding() {
    const std::vector<SourceEntry> entries{
        file_entry("music/2.mp3"),
        file_entry("Music/2.mp3"),
        file_entry("music/3.mp3"),
        file_entry("totala1.hpi"),
        file_entry("TOTALA1.HPI"),
        file_entry("Data/1.zrb"),
        file_entry("DATA/2.zrb"),
    };
    const auto plan = plan_import(SourceKind::game_folder, "/source", "Source", entries);
    OA_CHECK(plan.files.size() == 5);
    OA_CHECK(planned(plan, "Music/2.mp3") && planned(plan, "Music/2.mp3")->target == "Music/2.mp3");
    OA_CHECK(planned(plan, "music/3.mp3") && planned(plan, "music/3.mp3")->target == "Music/3.mp3");
    OA_CHECK(planned(plan, "DATA/2.zrb") && planned(plan, "DATA/2.zrb")->target == "DATA/2.zrb");
    OA_CHECK(planned(plan, "Data/1.zrb") && planned(plan, "Data/1.zrb")->target == "DATA/1.zrb");
    OA_CHECK(planned(plan, "TOTALA1.HPI"));
    const auto* music = left(plan, "music/2.mp3");
    OA_CHECK(
        music && music->reason == LeftOutReason::case_clash && music->kept_path == "Music/2.mp3"
    );
    const auto* archive = left(plan, "totala1.hpi");
    OA_CHECK(
        archive && archive->reason == LeftOutReason::case_clash &&
        archive->kept_path == "TOTALA1.HPI"
    );
    OA_CHECK(plan.warnings.size() == 2);
    OA_CHECK(
        std::find(
            plan.warnings.begin(),
            plan.warnings.end(),
            "Music/2.mp3 and music/2.mp3 differ only in capital letters; the game reads one of "
            "them. The first is copied."
        ) != plan.warnings.end()
    );
    // A file whose name a folder takes is a clash too.
    const std::vector<SourceEntry> clash{file_entry("data"), file_entry("Data/1.zrb")};
    const auto folded = plan_import(SourceKind::game_folder, "/source", "Source", clash);
    OA_CHECK(folded.files.size() == 1 && left(folded, "data/1.zrb") == nullptr);
    OA_CHECK(left(folded, "Data/1.zrb") || left(folded, "data"));
}

/// The parts Ready to copy shows, the movies and music rules, mods, the demo's installer and
/// the switches.
void parts_and_switches(const fs::path& scratch) {
    std::vector<SourceEntry> entries;
    for (const auto* name :
         {"totala1.hpi",        "TOTALA2.HPI",  "totala3.hpi",  "totala4.hpi",    "worlds.hpi",
          "rev31.gp3",          "ccdata.ccx",   "CCMAPS.ccx",   "ccmiss.ccx",     "btdata.ccx",
          "btmaps.ccx",         "tactics1.hpi", "Tactics8.HPI", "tactics9.hpi",   "AFark.ufo",
          "extra.ccx",          "modrev.gp3",   "music/0.mp3",  "music/1.MP3",    "music/2.ogg",
          "music/readme.txt",   "Data/1.ZRB",   "Data/2.zrb",   "Data/notes.txt", "Data/sub/x.zrb",
          "mods/notamod/x.txt", "totala.ini",   "camps/x.tdf",  "Setup.exe",      "Thumbs.db"})
        entries.push_back(file_entry(name, 100));
    entries.push_back(file_entry("mods/Example/oamod.yaml", 50, true));
    entries.push_back(file_entry("mods/Example/units/x.fbi", 70));
    SourceEntry link = file_entry("shortcut.hpi", 0);
    link.link = true;
    entries.push_back(link);
    SourceEntry folder = file_entry("music", 0);
    folder.folder = true;
    entries.push_back(folder);
    const auto plan = plan_import(SourceKind::game_folder, "/source/TA", {}, entries);
    OA_CHECK(plan.location == "TA");
    OA_CHECK(plan.has_archives && !plan.demo);
    const auto part = [&](Part which) -> const PartSummary& {
        return plan.parts[static_cast<std::size_t>(which)];
    };
    OA_CHECK(part(Part::game_archives).files == 5 && part(Part::game_archives).bytes == 500);
    OA_CHECK(part(Part::game_archives).names.front() == "TOTALA2.HPI");
    OA_CHECK(part(Part::update_31c).files == 1 && part(Part::update_31c).found);
    OA_CHECK(part(Part::core_contingency).files == 3);
    OA_CHECK(part(Part::battle_tactics).files == 4);
    OA_CHECK(part(Part::extra).files == 4);
    OA_CHECK(part(Part::music).files == 4 && part(Part::music).music_tracks == 3);
    OA_CHECK(part(Part::movies).files == 2);
    OA_CHECK(part(Part::mods).files == 2 && part(Part::mods).bytes == 120);
    OA_CHECK(part(Part::other).files == 5);
    OA_CHECK(!part(Part::demo).found);
    OA_CHECK(plan.mods.size() == 1 && plan.mods.front().folder == "mods/Example");
    OA_CHECK(plan.mods.front().id == "Example" && plan.mods.front().errors.empty());
    OA_CHECK(planned(plan, "mods/Example/units/x.fbi")->mod == 0);
    OA_CHECK(planned(plan, "totala.ini")->mod == PlannedFile::no_mod);
    OA_CHECK(plan.left_out.size() == 3);
    OA_CHECK(left(plan, "shortcut.hpi")->reason == LeftOutReason::link);
    OA_CHECK(plan.total_bytes == 28 * 100 + 120);
    OA_CHECK(plan.remote_bytes == 50 && !plan.sizes_unknown);
    OA_CHECK(import_mode_of(plan) == ImportMode::replace);

    // The switches: music off leaves music out of what is copied.
    const auto root = scratch / "parts";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    Switches switches;
    auto need = space_need(hooks, paths, plan, switches);
    OA_CHECK(need.copy_bytes == plan.total_bytes);
    switches.parts[static_cast<std::size_t>(Part::music)] = false;
    switches.parts[static_cast<std::size_t>(Part::game_archives)] = false; // no switch: ignored
    switches.mods = {0};
    need = space_need(hooks, paths, plan, switches);
    OA_CHECK(need.copy_bytes == plan.total_bytes - 400 - 120);

    // A folder holding only the demo's installer plans it as the demo.
    std::vector<SourceEntry> demo_entries{
        file_entry("Total Annihilation.exe", demo_1997.installer_size), file_entry("readme.txt", 5)
    };
    const auto demo = plan_import(SourceKind::game_folder, "/source/demo", "demo", demo_entries);
    OA_CHECK(demo.demo && !demo.has_archives && demo.files.size() == 2);
    OA_CHECK(planned(demo, "Total Annihilation.exe")->part == Part::demo);

    // Mods read from disk: a usable profile names its id; one with errors keeps its folder name.
    write_text(root / "source" / "mods" / "Good" / "oamod.yaml", example_profile);
    write_text(root / "source" / "mods" / "Broken" / "oamod.yaml", "oamod: 1\nid: Broken\n");
    write_bytes(root / "source" / "totala1.hpi", playable_archive());
    const auto mods = scan_plan(GameFilesHooks{}, paths, root / "source");
    OA_CHECK(mods->mods.size() == 2);
    for (const auto& mod : mods->mods) {
        if (mod.folder == "mods/Good")
            OA_CHECK(mod.id == "example" && mod.name == "Example mod" && mod.errors.empty());
        else
            OA_CHECK(mod.folder == "mods/Broken" && mod.id == "Broken" && !mod.errors.empty());
    }
}

/// The space a copy needs: the margin, staged files that match, the demo's unpacking, the
/// switches and the parts whose turning off would make it fit, largest first.
void space_arithmetic(const fs::path& scratch) {
    const auto root = scratch / "space";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    const std::vector<SourceEntry> entries{
        file_entry("totala1.hpi", 400'000'000),
        file_entry("btdata.ccx", 500'000'000),
        file_entry("music/1.mp3", 300'000'000),
        file_entry("rev31.gp3", 5),
    };
    const auto plan = plan_import(SourceKind::game_folder, root / "src", "src", entries);
    script.free = 2'000'000'000;
    auto need = space_need(hooks, paths, plan, {});
    OA_CHECK(need.copy_bytes == 1'200'000'005);
    OA_CHECK(need.need_bytes == 1'200'000'005 + space_margin_bytes);
    OA_CHECK(need.free_known && need.free_bytes == 2'000'000'000 && need.fits);
    OA_CHECK(need.fitting_off.empty());
    // Short: turning off Battle Tactics (500 MB) makes it fit; the music (300 MB) does once
    // 5 more bytes are free. Battle Tactics comes first, as the larger.
    script.free = 1'100'000'004;
    need = space_need(hooks, paths, plan, {});
    OA_CHECK(!need.fits);
    OA_CHECK(need.fitting_off == (std::vector<Part>{Part::battle_tactics}));
    script.free = 1'100'000'005;
    need = space_need(hooks, paths, plan, {});
    OA_CHECK(!need.fits);
    OA_CHECK(need.fitting_off == (std::vector<Part>{Part::battle_tactics, Part::music}));
    script.free = 800'000'000;
    need = space_need(hooks, paths, plan, {});
    OA_CHECK(!need.fits && need.fitting_off.empty());
    script.free = 1'200'000'000;
    Switches switches;
    switches.parts[static_cast<std::size_t>(Part::battle_tactics)] = false;
    need = space_need(hooks, paths, plan, switches);
    OA_CHECK(need.fits && need.copy_bytes == 700'000'005);
    script.free_known = false;
    need = space_need(hooks, paths, plan, {});
    OA_CHECK(!need.free_known && need.fits);
    script.free_known = true;

    // A staged file that matches its source by size and time is not counted.
    const std::vector<SourceEntry> small{
        file_entry("totala1.hpi", 1000), file_entry("rev31.gp3", 30)
    };
    const auto small_plan = plan_import(SourceKind::game_folder, root / "src", "src", small);
    write_sized(paths.staging / "totala1.hpi", 1000);
    need = space_need(hooks, paths, small_plan, {});
    OA_CHECK(need.copy_bytes == 30);
    fs::last_write_time(paths.staging / "totala1.hpi", file_time_from_seconds(changed_time));
    need = space_need(hooks, paths, small_plan, {});
    OA_CHECK(need.copy_bytes == 1030);

    // The demo's unpacking, unless its archive is already unpacked.
    const std::vector<SourceEntry> installer{file_entry("Setup.exe", demo_1997.installer_size)};
    auto demo = plan_import(SourceKind::demo_installer, root / "Setup.exe", "Setup.exe", installer);
    OA_CHECK(demo.demo);
    need = space_need(hooks, paths, demo, {});
    OA_CHECK(
        need.need_bytes == demo_1997.installer_size + demo_1997.archive_size + space_margin_bytes
    );
    // A movable installer is moved, not copied.
    demo.movable = true;
    need = space_need(hooks, paths, demo, {});
    OA_CHECK(
        need.copy_bytes == 0 && need.need_bytes == demo_1997.archive_size + space_margin_bytes
    );
    const auto archive = paths.data_folder / "demo-1997" / "TADemo.hpi";
    fs::create_directories(archive.parent_path());
    {
        std::ofstream out(archive, std::ios::binary);
    }
    fs::resize_file(archive, demo_1997.archive_size);
    need = space_need(hooks, paths, demo, {});
    OA_CHECK(need.need_bytes == space_margin_bytes);
    // A folder moved into place takes no space.
    auto moved = plan;
    moved.move_in_place = true;
    need = space_need(hooks, paths, moved, {});
    OA_CHECK(need.copy_bytes == 0);
    // Without hooks, the system's free space.
    need = space_need(GameFilesHooks{}, paths, plan, {});
    OA_CHECK(need.free_known && need.free_bytes > 0);
}

/// The default listing: links reported and never followed, the depth limit, the stop flag and
/// progress; the hooks' listing when they have one.
void listing(const fs::path& scratch) {
    const auto root = scratch / "listing";
    write_text(root / "a.txt", "a");
    write_text(root / "sub" / "b.txt", "bb");
    write_text(root / "sub" / "deeper" / "c.txt", "ccc");
    fs::create_symlink(root / "a.txt", root / "link");
    fs::create_directory_symlink(root / "sub", root / "linked folder");
    const GameFilesHooks none;
    const auto names = [](const std::vector<SourceEntry>& entries) {
        std::vector<std::string> out;
        for (const auto& entry : entries)
            out.push_back(entry.path + (entry.folder ? "/" : "") + (entry.link ? "@" : ""));
        std::sort(out.begin(), out.end());
        return out;
    };
    std::vector<SourceEntry> entries;
    std::string error;
    OA_CHECK(list_source(none, root, 0, &entries, &error));
    OA_CHECK(
        names(entries) == (std::vector<std::string>{"a.txt", "link@", "linked folder@", "sub/"})
    );
    entries.clear();
    OA_CHECK(list_source(none, root, 1, &entries, &error));
    OA_CHECK(
        names(entries) == (std::vector<std::string>{
                              "a.txt", "link@", "linked folder@", "sub/", "sub/b.txt", "sub/deeper/"
                          })
    );
    entries.clear();

    struct Count {
        uint32_t files{};
        uint64_t bytes{};
    } count;

    const auto counted = [](void* userdata, uint32_t files, uint64_t bytes) {
        auto& seen = *static_cast<Count*>(userdata);
        seen.files = files;
        seen.bytes = bytes;
    };
    OA_CHECK(list_source(
        none, root, std::numeric_limits<uint32_t>::max(), &entries, &error, nullptr, counted, &count
    ));
    OA_CHECK(entries.size() == 7);
    OA_CHECK(count.files == 3 && count.bytes == 6);
    for (const auto& entry : entries) {
        if (entry.path == "sub/b.txt")
            OA_CHECK(entry.size == 2 && entry.modified == source_time && !entry.remote);
        OA_CHECK(entry.path.rfind("linked folder/", 0) != 0);
    }
    const std::atomic<bool> stop{true};
    entries.clear();
    OA_CHECK(!list_source(none, root, 3, &entries, &error, &stop));
    OA_CHECK(error.empty());
    OA_CHECK(!list_source(none, root / "missing", 3, &entries, &error));
    OA_CHECK(!error.empty());
    // The platform's listing, depth passed through.
    Script script;
    script.listing = {file_entry("totala1.hpi", 10, true), file_entry("music/1.mp3", 5)};
    const auto hooks = hooks_of(script, true);
    entries.clear();
    OA_CHECK(list_source(hooks, root, 0, &entries, &error));
    OA_CHECK(entries.size() == 1 && entries.front().remote);
    OA_CHECK(script.depths == std::vector<uint32_t>{0});
}

/// A whole first-run route over synthetic archives: scan, copy, check, commit.
void full_copy(const fs::path& scratch) {
    const auto root = scratch / "full";
    const auto paths = case_paths(root);
    const auto source = root / "source" / "Total Annihilation";
    write_bytes(source / "totala1.hpi", playable_archive());
    write_bytes(source / "rev31.gp3", archive_of({"anims/base.gaf"}));
    write_sized(source / "music" / "1.mp3", 3 * mebibyte + 17);
    write_sized(source / "Setup.exe", 100);
    write_sized(source / "Thumbs.db", 10);
    fs::create_symlink(source / "totala1.hpi", source / "copy of totala1.hpi");
    Script script;
    const auto hooks = hooks_of(script);
    ScanSnapshot scanned;
    const auto plan = scan_plan(hooks, paths, source, SourceKind::game_folder, &scanned);
    OA_CHECK(plan->files.size() == 3 && plan->left_out.size() == 3);
    OA_CHECK(scanned.source_check && oa::app::usable(*scanned.source_check));
    OA_CHECK(!scanned.source_check_skipped && scanned.failure == ScanSnapshot::Failure::none);
    OA_CHECK(scanned.files == 5);
    OA_CHECK(plan->location == "Total Annihilation");

    const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    OA_CHECK(run.check && oa::app::usable(*run.check));
    OA_CHECK(run.files_done == 3 && run.files_total == 3 && run.files_skipped == 0);
    OA_CHECK(run.bytes_done == plan->total_bytes && run.bytes_total == plan->total_bytes);
    OA_CHECK(run.parts_done[static_cast<std::size_t>(Part::music)] == 2);
    OA_CHECK(run.parts_done[static_cast<std::size_t>(Part::core_contingency)] == 0);
    OA_CHECK(script.keep_running == (std::vector<bool>{true, false}));
    OA_CHECK(script.copies == 3);
    OA_CHECK(read_text(paths.staging / "music" / "1.mp3") == read_text(source / "music" / "1.mp3"));
    OA_CHECK(
        seconds_since_1970(fs::last_write_time(paths.staging / "music" / "1.mp3")) == source_time
    );
    OA_CHECK(!there(paths.staging / "Setup.exe") && !there(paths.staging / "copy of totala1.hpi"));
    std::string error;
    const auto state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->phase == ImportPhase::checked && state->mode == ImportMode::replace);
    OA_CHECK(state && state->files == 3 && state->bytes == plan->total_bytes);
    OA_CHECK(state && state->location == "Total Annihilation");

    const auto commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(commit.ok && commit.old_folder.empty());
    OA_CHECK(
        there(paths.game_folder / "totala1.hpi") && there(paths.game_folder / "music" / "1.mp3")
    );
    OA_CHECK(!there(paths.staging) && !there(paths.state_file));
    OA_CHECK(
        std::find(
            script.backed_up.begin(),
            script.backed_up.end(),
            std::make_pair(path_to_utf8(paths.game_folder), false)
        ) != script.backed_up.end()
    );
    OA_CHECK(oa::app::usable(oa::app::inspect_game_install(paths.game_folder)));
    // The source is untouched.
    OA_CHECK(there(source / "totala1.hpi") && there(source / "Setup.exe"));
}

/// Commits over an existing game folder, which is set aside as "(old)", then " 2"; a copy
/// the check refuses leaves the game folder as it was.
void commit_over_folder(const fs::path& scratch) {
    const auto root = scratch / "commit";
    const auto paths = case_paths(root);
    const GameFilesHooks hooks;
    write_text(paths.game_folder / "mine.txt", "first");
    const auto stage = [&](std::string_view text) {
        write_text(paths.staging / "new.txt", text);
        ImportState state;
        state.phase = ImportPhase::checked;
        std::string error;
        OA_CHECK(write_import_state(paths.state_file, state, &error));
    };
    stage("second");
    auto commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(commit.ok);
    OA_CHECK(commit.old_folder == paths.documents / "Total Annihilation (old)");
    OA_CHECK(read_text(commit.old_folder / "mine.txt") == "first");
    OA_CHECK(read_text(paths.game_folder / "new.txt") == "second");
    stage("third");
    commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(commit.ok && commit.old_folder == paths.documents / "Total Annihilation (old) 2");
    OA_CHECK(read_text(paths.game_folder / "new.txt") == "third");
    OA_CHECK(read_text(paths.documents / "Total Annihilation (old) 2" / "new.txt") == "second");
    OA_CHECK(old_folder_name(1) == "Total Annihilation (old)");
    OA_CHECK(old_folder_name(3) == "Total Annihilation (old) 3");
    // Nothing to commit.
    commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(!commit.ok && !commit.error.empty());
    OA_CHECK(read_text(paths.game_folder / "new.txt") == "third");

    // A copy the engine's check refuses is not committed, and the game folder stays.
    const auto source = root / "lacking";
    write_bytes(source / "tactics1.hpi", archive_of({"gamedata/sound.tdf"}));
    ScanSnapshot scanned;
    const auto plan = scan_plan(hooks, paths, source, SourceKind::game_folder, &scanned);
    OA_CHECK(scanned.source_check && !oa::app::usable(*scanned.source_check));
    const auto run = run_plan(hooks, paths, plan);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::not_usable);
    OA_CHECK(run.check && !oa::app::usable(*run.check));
    std::string error;
    const auto state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->phase == ImportPhase::copying);
    OA_CHECK(read_text(paths.game_folder / "new.txt") == "third");
    const auto at_start = recover_import(hooks, paths, false);
    OA_CHECK(at_start.outcome == Recovery::copy_waiting);
    OA_CHECK(read_text(paths.game_folder / "new.txt") == "third");
    OA_CHECK(discard_import(paths, &error) && !there(paths.staging) && !there(paths.state_file));
}

/// Makes a source of three files of a few mebibytes each.
fs::path three_files(const fs::path& root) {
    const auto source = root / "source";
    write_bytes(source / "totala1.hpi", playable_archive());
    write_sized(source / "a.bin", 2 * mebibyte + 3, 1);
    write_sized(source / "b.bin", 2 * mebibyte + 5, 2);
    return source;
}

/// Stop keeps what was copied, or discards it; the time away running out stops as Stop does,
/// marked as the expiry.
void stop_and_expiry(const fs::path& scratch) {
    for (const bool keep : {true, false}) {
        const auto root = scratch / (keep ? "stop keep" : "stop discard");
        const auto paths = case_paths(root);
        Script script;
        const auto hooks = hooks_of(script);
        const auto plan = scan_plan(hooks, paths, three_files(root));
        script.stop_at = 1;
        const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
        OA_CHECK(run.stage == RunStage::stopped && !run.stopped_by_expiry);
        OA_CHECK(run.files_done == 1);
        OA_CHECK(script.keep_running == (std::vector<bool>{true, false}));
        const auto first = plan->files.front();
        OA_CHECK(there(paths.staging / first.target));
        for (const auto& file : plan->files)
            OA_CHECK(!there(paths.staging / (file.target + ".part")));
        OA_CHECK(staged_bytes(paths.staging) == first.size);
        std::string error;
        if (keep) {
            const auto at_start = recover_import(hooks, paths, false);
            OA_CHECK(at_start.outcome == Recovery::copy_waiting);
            OA_CHECK(at_start.staged_bytes == first.size);
            OA_CHECK(at_start.state && at_start.state->location == "source");
            OA_CHECK(at_start.state && at_start.state->bytes == plan->total_bytes);
            OA_CHECK(!there(paths.game_folder));
        } else {
            OA_CHECK(discard_import(paths, &error));
            OA_CHECK(!there(paths.staging) && !there(paths.state_file));
            OA_CHECK(recover_import(hooks, paths, false).outcome == Recovery::nothing);
        }
    }
    const auto root = scratch / "expiry";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    const auto plan = scan_plan(hooks, paths, three_files(root));
    script.expire_at = 2;
    const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
    OA_CHECK(run.stage == RunStage::stopped && run.stopped_by_expiry);
    OA_CHECK(run.files_done == 2);
    OA_CHECK(
        there(paths.staging / plan->files[0].target) && there(paths.staging / plan->files[1].target)
    );
    OA_CHECK(!there(paths.staging / plan->files[2].target));
    OA_CHECK(!there(paths.staging / (plan->files[2].target + ".part")));
    OA_CHECK(script.keep_running == (std::vector<bool>{true, false}));
    // ImportRun::expire marks a run the same way.
    ImportRun expired;
    expired.expire();
    OA_CHECK(!expired.busy());
}

/// Resuming: a staged file with the same size and time is skipped, a changed one copied
/// again, a stale .part restarted, and a staged file the plan does not copy removed.
void resume(const fs::path& scratch) {
    const auto root = scratch / "resume";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    const auto source = three_files(root);
    const auto plan = scan_plan(hooks, paths, source);
    fs::create_directories(paths.staging);
    fs::copy_file(source / "a.bin", paths.staging / "a.bin");
    fs::last_write_time(paths.staging / "a.bin", file_time_from_seconds(source_time));
    write_sized(paths.staging / "b.bin", 100, 9);
    write_sized(paths.staging / "totala1.hpi.part", 50);
    write_text(paths.staging / "stale" / "zzz.txt", "x");
    const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    OA_CHECK(run.files_skipped == 1 && run.files_done == 3);
    OA_CHECK(script.copies == 2);
    OA_CHECK(read_text(paths.staging / "b.bin") == read_text(source / "b.bin"));
    OA_CHECK(read_text(paths.staging / "totala1.hpi") == read_text(source / "totala1.hpi"));
    OA_CHECK(!there(paths.staging / "totala1.hpi.part"));
    OA_CHECK(!there(paths.staging / "stale"));
    OA_CHECK(run.bytes_done == plan->total_bytes);

    // Music switched off on a second run: the staged music goes, so the commit cannot bring it.
    write_sized(source / "music" / "1.mp3", 1000);
    const auto with_music = scan_plan(hooks, paths, source);
    auto all = run_plan(hooks, paths, with_music, ImportMode::replace, &script);
    OA_CHECK(all.stage == RunStage::checked && there(paths.staging / "music" / "1.mp3"));
    Switches switches;
    switches.parts[static_cast<std::size_t>(Part::music)] = false;
    all = run_plan(hooks, paths, with_music, ImportMode::replace, &script, switches);
    OA_CHECK(all.stage == RunStage::checked && all.files_total == 3 && all.files_skipped == 3);
    OA_CHECK(!there(paths.staging / "music"));
    std::string error;
    const auto state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && !state->parts[static_cast<std::size_t>(Part::music)]);

    // A source changed since it was listed: the copy stops with changed.
    const auto again = scan_plan(hooks, paths, source);
    fs::remove_all(paths.staging);
    fs::last_write_time(source / "b.bin", file_time_from_seconds(changed_time));
    const auto changed = run_plan(GameFilesHooks{}, paths, again);
    OA_CHECK(changed.stage == RunStage::failed && changed.failure == RunFailure::changed);
    OA_CHECK(changed.file == "b.bin" && !changed.error.empty());
    OA_CHECK(!there(paths.staging / "b.bin.part"));
}

/// The platform's copy failing after some files: each outcome maps to the run's failure,
/// naming the file, and what was copied stays.
void scripted_failures(const fs::path& scratch) {
    const std::vector<std::pair<CopyOutcome, RunFailure>> outcomes{
        {CopyOutcome::no_space, RunFailure::no_space},
        {CopyOutcome::unreadable, RunFailure::unreadable},
        {CopyOutcome::gone, RunFailure::gone},
        {CopyOutcome::offline, RunFailure::offline},
        {CopyOutcome::denied, RunFailure::denied},
        {CopyOutcome::changed, RunFailure::changed},
    };
    int number = 0;
    for (const auto& [outcome, failure] : outcomes) {
        const auto root = scratch / ("failure " + std::to_string(number++));
        const auto paths = case_paths(root);
        Script script;
        script.fail_at = 2;
        script.fail_with = outcome;
        const auto hooks = hooks_of(script);
        const auto plan = scan_plan(hooks, paths, three_files(root));
        const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
        OA_CHECK(run.stage == RunStage::failed && run.failure == failure);
        OA_CHECK(run.file == plan->files[2].target && run.error == "scripted");
        OA_CHECK(run.files_done == 2);
        OA_CHECK(
            there(paths.staging / plan->files[0].target) &&
            there(paths.staging / plan->files[1].target)
        );
        OA_CHECK(!there(paths.staging / (plan->files[2].target + ".part")));
        OA_CHECK(script.keep_running == (std::vector<bool>{true, false}));
        std::string error;
        const auto state = read_import_state(paths.state_file, &error);
        OA_CHECK(state && state->phase == ImportPhase::copying);
    }
    // A file of unknown size is copied only with the margin free.
    const auto root = scratch / "unknown size";
    const auto paths = case_paths(root);
    Script script;
    script.free = space_margin_bytes - 1;
    const auto hooks = hooks_of(script);
    write_sized(root / "source" / "x.ufo", 10);
    std::vector<SourceEntry> entries{file_entry("x.ufo", 0)};
    entries.front().size_known = false;
    auto plan = std::make_shared<const ImportPlan>(
        plan_import(SourceKind::additions_folder, root / "source", "source", entries)
    );
    OA_CHECK(plan->sizes_unknown);
    auto run = run_plan(hooks, paths, plan, ImportMode::add, &script);
    OA_CHECK(run.stage == RunStage::failed && run.failure == RunFailure::no_space);
    script.free = space_margin_bytes * 2;
    run = run_plan(hooks, paths, plan, ImportMode::add, &script);
    OA_CHECK(run.stage == RunStage::checked && there(paths.staging / "x.ufo"));
}

/// What the test's copying thread and the test share.
struct ChunkedCopyRun {
    const char* source{};
    const FileCopy* file{};
    std::string* error{};
    CopyOutcome outcome{CopyOutcome::copied};
};

/// Runs one chunked copy on the thread the test starts.
void run_chunked_copy(void* argument) {
    auto& run = *static_cast<ChunkedCopyRun*>(argument);
    run.outcome = chunked_copy(run.source, *run.file, run.error);
}

/// The chunked copy: the time stamped, the .part renamed, a changed or missing source, and
/// the stop flag before and between chunks.
void chunked_copies(const fs::path& scratch) {
    const auto root = scratch / "chunked";
    write_sized(root / "source.bin", 3 * mebibyte + 11);
    const auto source = path_to_utf8(root / "source.bin");
    const auto target = path_to_utf8(root / "out" / "copy.bin.part");
    std::atomic<uint64_t> done{0};
    std::atomic<bool> stop{false};
    FileCopy file;
    file.source = source.c_str();
    file.target = target.c_str();
    file.size = 3 * mebibyte + 11;
    file.modified = source_time;
    file.bytes_done = &done;
    file.stop = &stop;
    file.copy = chunked_copy;
    std::string error;
    OA_CHECK(chunked_copy(source.c_str(), file, &error) == CopyOutcome::copied);
    OA_CHECK(done.load() == file.size);
    OA_CHECK(!there(root / "out" / "copy.bin.part"));
    OA_CHECK(read_text(root / "out" / "copy.bin") == read_text(root / "source.bin"));
    OA_CHECK(seconds_since_1970(fs::last_write_time(root / "out" / "copy.bin")) == source_time);
    PlannedFile planned_file;
    planned_file.size = file.size;
    planned_file.modified = source_time;
    OA_CHECK(staged_matches(root / "out" / "copy.bin", planned_file));
    planned_file.modified = changed_time;
    OA_CHECK(!staged_matches(root / "out" / "copy.bin", planned_file));
    fs::remove(root / "out" / "copy.bin");

    // A planned time that differs from the source's: changed, and nothing left behind.
    file.modified = changed_time;
    OA_CHECK(chunked_copy(source.c_str(), file, &error) == CopyOutcome::changed);
    OA_CHECK(!there(root / "out" / "copy.bin.part") && !there(root / "out" / "copy.bin"));
    file.modified = source_time;
    file.size = 5;
    OA_CHECK(chunked_copy(source.c_str(), file, &error) == CopyOutcome::changed);
    file.size = 3 * mebibyte + 11;
    // An unknown size takes whatever is there, stamped with the planned time.
    file.size_known = false;
    OA_CHECK(chunked_copy(source.c_str(), file, &error) == CopyOutcome::copied);
    OA_CHECK(fs::file_size(root / "out" / "copy.bin") == 3 * mebibyte + 11);
    fs::remove(root / "out" / "copy.bin");
    file.size_known = true;
    // A source that went away.
    const auto missing = path_to_utf8(root / "missing.bin");
    OA_CHECK(chunked_copy(missing.c_str(), file, &error) == CopyOutcome::gone && !error.empty());
    // Stop before the first chunk.
    stop.store(true);
    OA_CHECK(chunked_copy(source.c_str(), file, &error) == CopyOutcome::stopped);
    OA_CHECK(!there(root / "out" / "copy.bin.part"));
    stop.store(false);

    // Stop between chunks of a larger file: pressed as soon as the first chunk is written.
    write_sized(root / "large.bin", 48 * mebibyte, 3);
    const auto large = path_to_utf8(root / "large.bin");
    file.size = 48 * mebibyte;
    done.store(0);
    ChunkedCopyRun run{large.c_str(), &file, &error};
    threads::Thread copier{};
    OA_CHECK(threads::start_thread(copier, run_chunked_copy, &run));
    while (done.load() == 0)
        threads::sleep_ms(0);
    stop.store(true);
    threads::join_thread(copier);
    OA_CHECK(run.outcome == CopyOutcome::stopped);
    OA_CHECK(done.load() < 48 * mebibyte);
    OA_CHECK(!there(root / "out" / "copy.bin.part") && !there(root / "out" / "copy.bin"));
}

/// Writes a hand-made state file.
void write_state(const fs::path& file, std::string_view text) {
    fs::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

/// Recovery from a hand-written state in each phase and mode.
void recovery(const fs::path& scratch) {
    Script script;
    const auto hooks = hooks_of(script);
    std::string error;
    {
        // Committing, nothing renamed yet.
        const auto paths = case_paths(scratch / "recover a");
        write_text(paths.game_folder / "old.txt", "old");
        write_text(paths.staging / "new.txt", "new");
        write_state(paths.state_file, "phase = committing\nmode = replace\n");
        const auto result = recover_import(hooks, paths, true);
        OA_CHECK(result.outcome == Recovery::finished_commit && result.commit.ok);
        OA_CHECK(read_text(paths.game_folder / "new.txt") == "new");
        OA_CHECK(read_text(paths.documents / "Total Annihilation (old)" / "old.txt") == "old");
        OA_CHECK(!there(paths.staging) && !there(paths.state_file));
        OA_CHECK(!script.backed_up.empty() && script.backed_up.back().second);
    }
    {
        // Committing, the game folder set aside, the copy not yet in place.
        const auto paths = case_paths(scratch / "recover b");
        write_text(paths.documents / "Total Annihilation (old)" / "old.txt", "old");
        write_text(paths.staging / "new.txt", "new");
        write_state(paths.state_file, "phase = committing\nmode = replace\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::finished_commit && result.commit.ok);
        OA_CHECK(read_text(paths.game_folder / "new.txt") == "new");
        OA_CHECK(!there(paths.documents / "Total Annihilation (old) 2"));
    }
    {
        // Committing, every rename done, only the state left.
        const auto paths = case_paths(scratch / "recover c");
        write_text(paths.game_folder / "new.txt", "new");
        write_state(paths.state_file, "phase = committing\nmode = replace\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::finished_commit && result.commit.ok);
        OA_CHECK(!there(paths.state_file) && read_text(paths.game_folder / "new.txt") == "new");
    }
    {
        // A replacement checked and waiting for this start.
        const auto paths = case_paths(scratch / "recover d");
        write_text(paths.game_folder / "old.txt", "old");
        write_text(paths.staging / "new.txt", "new");
        write_state(paths.state_file, "phase = checked\nmode = replace\nlocation = Somewhere\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::applied && result.commit.ok);
        OA_CHECK(result.commit.old_folder == paths.documents / "Total Annihilation (old)");
        OA_CHECK(read_text(paths.game_folder / "new.txt") == "new");
    }
    {
        // A copy interrupted: left for the continue banner.
        const auto paths = case_paths(scratch / "recover e");
        write_sized(paths.staging / "a.ufo", 100);
        write_sized(paths.staging / "b.ufo.part", 40);
        write_state(
            paths.state_file,
            "phase = copying\nmode = replace\nkind = game-folder\nlocation = A \\\\ B\nbytes = "
            "900\nfiles = 3\npart.music = 0\nunknown = ignored\n"
        );
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::copy_waiting);
        OA_CHECK(result.staged_bytes == 100);
        OA_CHECK(result.state && result.state->location == "A \\ B" && result.state->bytes == 900);
        OA_CHECK(result.state && result.state->files == 3);
        OA_CHECK(result.state && !result.state->parts[static_cast<std::size_t>(Part::music)]);
        OA_CHECK(result.state && result.state->parts[static_cast<std::size_t>(Part::movies)]);
        OA_CHECK(there(paths.staging / "a.ufo") && there(paths.state_file));
    }
    {
        // Removals the player confirmed, matched without case; emptied folders go too.
        const auto paths = case_paths(scratch / "recover f");
        write_text(paths.game_folder / "music" / "1.mp3", "1");
        write_text(paths.game_folder / "music" / "2.mp3", "2");
        write_text(paths.game_folder / "totala1.hpi", "a");
        write_state(
            paths.state_file,
            "phase = checked\nmode = remove\nremove = music/1.mp3\nremove = MUSIC/2.MP3\nremove = "
            "../escape\n"
        );
        write_text(paths.documents / "escape", "stays");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::applied && result.commit.ok);
        OA_CHECK(!there(paths.game_folder / "music") && there(paths.game_folder / "totala1.hpi"));
        OA_CHECK(there(paths.documents / "escape") && !there(paths.state_file));
    }
    {
        // Remove all.
        const auto paths = case_paths(scratch / "recover g");
        write_text(paths.game_folder / "totala1.hpi", "a");
        write_state(paths.state_file, "phase = checked\nmode = remove\nremove = *\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::applied && !there(paths.game_folder));
        OA_CHECK(there(paths.documents));
    }
    {
        // An addition's commit cut short: the missing moves are made, never over a file.
        const auto paths = case_paths(scratch / "recover h");
        write_text(paths.game_folder / "TOTALA1.HPI", "installed");
        write_text(paths.game_folder / "moved.ufo", "moved before");
        write_text(paths.staging / "totala1.hpi", "added");
        write_text(paths.staging / "new.ufo", "new");
        write_state(paths.state_file, "phase = committing\nmode = add\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::finished_commit && result.commit.ok);
        OA_CHECK(read_text(paths.game_folder / "TOTALA1.HPI") == "installed");
        OA_CHECK(read_text(paths.game_folder / "new.ufo") == "new");
        OA_CHECK(!there(paths.staging) && !there(paths.state_file));
    }
    {
        // A checked mod waiting: moved into mods/<id>.
        const auto paths = case_paths(scratch / "recover i");
        write_text(paths.game_folder / "totala1.hpi", "a");
        write_text(paths.staging / "mods" / "example" / "oamod.yaml", example_profile);
        write_state(paths.state_file, "phase = checked\nmode = mod\nmod-id = example\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::applied && result.commit.ok);
        OA_CHECK(there(paths.game_folder / "mods" / "example" / "oamod.yaml"));
    }
    {
        // An interrupted addition carrying removals: the removals apply, the copy waits.
        const auto paths = case_paths(scratch / "recover j");
        write_text(paths.game_folder / "music" / "1.mp3", "1");
        write_text(paths.staging / "x.ufo", "x");
        write_state(paths.state_file, "phase = copying\nmode = add\nremove = music\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::copy_waiting);
        OA_CHECK(!there(paths.game_folder / "music"));
        const auto state = read_import_state(paths.state_file, &error);
        OA_CHECK(state && state->removals.empty() && state->mode == ImportMode::add);
    }
    {
        // A folder moved into place.
        const auto paths = case_paths(scratch / "recover n");
        write_text(paths.documents / "TA Pack" / "totala1.hpi", "a");
        // Written by write_import_state, which escapes the backslashes of a Windows path.
        ImportState moved;
        moved.phase = ImportPhase::committing;
        moved.mode = ImportMode::replace;
        moved.parts.fill(true);
        moved.move_source = path_to_utf8(paths.documents / "TA Pack");
        OA_CHECK(write_import_state(paths.state_file, moved, &error));
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::finished_commit && result.commit.ok);
        OA_CHECK(there(paths.game_folder / "totala1.hpi") && !there(paths.documents / "TA Pack"));
    }
    {
        // States that cannot be read are left for the player.
        const auto paths = case_paths(scratch / "recover k");
        write_state(paths.state_file, "phase = sideways\n");
        auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::unreadable && !result.error.empty());
        OA_CHECK(there(paths.state_file));
        write_state(paths.state_file, "mode = replace\n");
        result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::unreadable);
        write_state(paths.state_file, "phase = checked\nmode = shuffle\n");
        result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::unreadable);
        OA_CHECK(discard_import(paths, &error) && !there(paths.state_file));
        OA_CHECK(recover_import(hooks, paths, false).outcome == Recovery::nothing);
    }
    {
        // A checked replacement whose copy has gone does not come back at every start.
        const auto paths = case_paths(scratch / "recover m");
        write_state(paths.state_file, "phase = checked\nmode = replace\n");
        const auto result = recover_import(hooks, paths, false);
        OA_CHECK(result.outcome == Recovery::applied && !result.commit.ok);
        OA_CHECK(!there(paths.state_file));
    }
}

/// The state file written and read back, every field and escaped text included.
void state_file(const fs::path& scratch) {
    const auto paths = case_paths(scratch / "state");
    ImportState state;
    state.phase = ImportPhase::committing;
    state.mode = ImportMode::mod;
    state.kind = SourceKind::additions_folder;
    state.location = "Drive \xe2\x80\xba Games\nwith a line break \\ and = signs";
    state.bytes = 1'234'567'890'123;
    state.files = 42;
    state.parts.fill(true);
    state.parts[static_cast<std::size_t>(Part::movies)] = false;
    state.mods_off = {"mods/one", "mods/two"};
    state.mod_id = "example";
    state.removals = {"music/1.mp3", "*"};
    state.move_source = "/a/b";
    std::string error;
    OA_CHECK(write_import_state(paths.state_file, state, &error));
    OA_CHECK(!there(fs::path(paths.state_file.string() + ".new")));
    const auto read = read_import_state(paths.state_file, &error);
    OA_CHECK(read.has_value());
    if (read) {
        OA_CHECK(
            read->phase == state.phase && read->mode == state.mode && read->kind == state.kind
        );
        OA_CHECK(read->location == state.location && read->bytes == state.bytes);
        OA_CHECK(read->files == state.files && read->parts == state.parts);
        OA_CHECK(read->mods_off == state.mods_off && read->mod_id == state.mod_id);
        OA_CHECK(read->removals == state.removals && read->move_source == state.move_source);
    }
    OA_CHECK(!read_import_state(paths.import_root / "none", &error) && error.empty());
}

/// "I have copied it": the game folder, a folder under another name, loose archives.
void adopting(const fs::path& scratch) {
    {
        const auto paths = case_paths(scratch / "adopt folder");
        OA_CHECK(find_copied_files(paths).find == CopiedFind::nothing);
        write_text(paths.documents / "Notes" / "readme.txt", "x");
        write_text(paths.documents / "Total Annihilation (old)" / "totala1.hpi", "old");
        write_text(paths.documents / "TA Commander Pack" / "totala1.hpi", "mine");
        const auto found = find_copied_files(paths);
        OA_CHECK(found.find == CopiedFind::misnamed_folder && found.folder == "TA Commander Pack");
        std::string error;
        OA_CHECK(adopt_copied_files(paths, found, &error));
        OA_CHECK(read_text(paths.game_folder / "totala1.hpi") == "mine");
        OA_CHECK(!there(paths.documents / "TA Commander Pack"));
        OA_CHECK(find_copied_files(paths).find == CopiedFind::game_folder);
    }
    {
        const auto paths = case_paths(scratch / "adopt loose");
        write_text(paths.documents / "totala1.hpi", "a");
        write_text(paths.documents / "REV31.GP3", "b");
        write_text(paths.documents / "readme.txt", "c");
        const auto found = find_copied_files(paths);
        OA_CHECK(found.find == CopiedFind::loose_archives);
        OA_CHECK(found.archives == (std::vector<std::string>{"REV31.GP3", "totala1.hpi"}));
        std::string error;
        OA_CHECK(adopt_copied_files(paths, found, &error));
        OA_CHECK(
            there(paths.game_folder / "totala1.hpi") && there(paths.game_folder / "REV31.GP3")
        );
        OA_CHECK(there(paths.documents / "readme.txt") && !there(paths.documents / "totala1.hpi"));
    }
}

/// Additions: nothing already there is replaced (matched without case) and the names kept
/// are listed; folders fold onto the installed spelling; a folder holding oamod.yaml at its
/// top goes to mods/<id>/; archives chosen one by one go to the top.
void additions(const fs::path& scratch) {
    const auto root = scratch / "additions";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    write_bytes(paths.game_folder / "TOTALA1.HPI", playable_archive());
    write_text(paths.game_folder / "Music" / "1.mp3", "one");
    write_text(paths.game_folder / "ReadMe.txt", "installed");
    const auto source = root / "Expansion";
    write_text(source / "readme.txt", "not this one");
    write_text(source / "music" / "20.mp3", "twenty");
    write_bytes(source / "newmap.ufo", archive_of({"maps/new.tnt"}));
    write_text(source / "Setup.exe", "x");
    ScanSnapshot scanned;
    const auto plan = scan_plan(hooks, paths, source, SourceKind::additions_folder, &scanned);
    OA_CHECK(import_mode_of(*plan) == ImportMode::add);
    OA_CHECK(plan->kept == std::vector<std::string>{"ReadMe.txt"});
    OA_CHECK(plan->files.size() == 2);
    OA_CHECK(
        planned(*plan, "music/20.mp3") && planned(*plan, "music/20.mp3")->target == "Music/20.mp3"
    );
    OA_CHECK(scanned.source_check && oa::app::usable(*scanned.source_check));
    const auto run = run_plan(hooks, paths, plan, ImportMode::add, &script);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    // A file that appeared since the plan is not replaced either.
    write_text(paths.game_folder / "NEWMAP.UFO", "the player's own");
    const auto commit = commit_import(hooks, paths, ImportMode::add, false);
    OA_CHECK(commit.ok);
    OA_CHECK(read_text(paths.game_folder / "Music" / "20.mp3") == "twenty");
    OA_CHECK(read_text(paths.game_folder / "NEWMAP.UFO") == "the player's own");
    OA_CHECK(read_text(paths.game_folder / "ReadMe.txt") == "installed");
    OA_CHECK(!there(paths.staging) && !there(paths.state_file));
    OA_CHECK(there(source / "music" / "20.mp3"));

    // A mod.
    const auto mod = root / "My Mod";
    write_text(mod / "oamod.yaml", example_profile);
    write_text(mod / "units" / "x.fbi", "unit");
    const auto mod_plan = scan_plan(hooks, paths, mod, SourceKind::additions_folder, &scanned);
    OA_CHECK(import_mode_of(*mod_plan) == ImportMode::mod);
    OA_CHECK(mod_plan->mods.size() == 1 && mod_plan->mods.front().id == "example");
    OA_CHECK(mod_plan->mods.front().folder.empty() && mod_plan->mods.front().name == "Example mod");
    OA_CHECK(
        planned(*mod_plan, "units/x.fbi") &&
        planned(*mod_plan, "units/x.fbi")->target == "mods/example/units/x.fbi"
    );
    OA_CHECK(planned(*mod_plan, "units/x.fbi")->part == Part::mods);
    OA_CHECK(scanned.source_check && oa::app::usable(*scanned.source_check));
    const auto mod_run = run_plan(hooks, paths, mod_plan, ImportMode::mod, &script);
    OA_CHECK(mod_run.stage == RunStage::checked && mod_run.failure == RunFailure::none);
    std::string error;
    const auto state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->mod_id == "example" && state->mode == ImportMode::mod);
    OA_CHECK(commit_import(hooks, paths, ImportMode::mod, false).ok);
    OA_CHECK(read_text(paths.game_folder / "mods" / "example" / "units" / "x.fbi") == "unit");

    // Archives chosen one by one, from two folders.
    write_bytes(root / "picked" / "a" / "extra1.ufo", archive_of({"units/a.fbi"}));
    write_bytes(root / "picked" / "b" / "TOTALA1.HPI", archive_of({"units/b.fbi"}));
    SourceScan scan;
    ScanRequest request;
    request.kind = SourceKind::archives;
    request.paths = {
        path_to_utf8(root / "picked" / "a" / "extra1.ufo"),
        path_to_utf8(root / "picked" / "b" / "TOTALA1.HPI")
    };
    OA_CHECK(scan.start(hooks, paths, request, &error));
    const auto picked = finish(scan);
    OA_CHECK(picked.stage == ScanStage::planned && picked.plan);
    if (picked.plan) {
        OA_CHECK(picked.plan->source == root / "picked");
        OA_CHECK(
            picked.plan->files.size() == 1 && picked.plan->files.front().target == "extra1.ufo"
        );
        OA_CHECK(picked.plan->kept == std::vector<std::string>{"TOTALA1.HPI"});
        OA_CHECK(import_mode_of(*picked.plan) == ImportMode::add);
    }
}

/// A link in the game folder: nothing is written through it, and a path through one counts as
/// leading out of the folder.
void links_in_the_game_folder(const fs::path& scratch) {
    const auto root = scratch / "links";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    write_bytes(paths.game_folder / "TOTALA1.HPI", playable_archive());
    write_text(paths.game_folder / "Music" / "1.mp3", "one");
    const auto source = root / "Expansion";
    write_text(source / "music" / "20.mp3", "twenty");
    write_bytes(source / "newmap.ufo", archive_of({"maps/new.tnt"}));
    const auto plan = scan_plan(hooks, paths, source, SourceKind::additions_folder, nullptr);
    OA_CHECK(plan && planned(*plan, "music/20.mp3"));
    if (!plan)
        return;
    const auto run = run_plan(hooks, paths, plan, ImportMode::add, &script);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    // The music folder became a link to a folder outside the game folder.
    const auto elsewhere = root / "elsewhere";
    fs::create_directories(elsewhere);
    fs::remove_all(paths.game_folder / "Music");
    std::error_code error;
    fs::create_directory_symlink(elsewhere, paths.game_folder / "Music", error);
    // Some systems report a link made and make none.
    std::error_code status_error;
    if (!error && !fs::is_symlink(fs::symlink_status(paths.game_folder / "Music", status_error)))
        error = std::make_error_code(std::errc::no_such_file_or_directory);
    if (error) {
        std::printf("skipped the link case: %s\n", error.message().c_str());
        return;
    }
    OA_CHECK(passes_through_link(paths.game_folder, paths.game_folder / "Music" / "20.mp3"));
    OA_CHECK(passes_through_link(paths.game_folder, paths.game_folder / "Music"));
    OA_CHECK(!passes_through_link(paths.game_folder, paths.game_folder / "TOTALA1.HPI"));
    OA_CHECK(!passes_through_link(paths.game_folder, paths.game_folder / "maps" / "new.tnt"));
    OA_CHECK(passes_through_link(paths.game_folder, root / "elsewhere" / "20.mp3"));
    OA_CHECK(commit_import(hooks, paths, ImportMode::add, false).ok);
    OA_CHECK(!there(elsewhere / "20.mp3"));
    OA_CHECK(there(paths.game_folder / "newmap.ufo"));
}

/// Removals waiting for the next start merge, and a waiting replacement holds back other
/// changes until it is cancelled.
void scheduling(const fs::path& scratch) {
    const auto paths = case_paths(scratch / "schedule");
    std::string error;
    const std::vector<std::string> first{"music/1.mp3"};
    OA_CHECK(schedule_removal(paths, first, &error));
    const std::vector<std::string> second{"MUSIC/1.MP3", "rev31.gp3"};
    OA_CHECK(schedule_removal(paths, second, &error));
    auto state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->mode == ImportMode::remove);
    OA_CHECK(state && state->removals == (std::vector<std::string>{"music/1.mp3", "rev31.gp3"}));
    const std::vector<std::string> everything{"*"};
    OA_CHECK(schedule_removal(paths, everything, &error));
    state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->removals == std::vector<std::string>{"*"});
    OA_CHECK(cancel_scheduled(paths, &error) && !there(paths.state_file));
    OA_CHECK(cancel_scheduled(paths, &error));

    // A discarded copy keeps the removals waiting with it.
    OA_CHECK(schedule_removal(paths, first, &error));
    write_text(paths.staging / "x.ufo", "x");
    OA_CHECK(discard_import(paths, &error));
    state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->mode == ImportMode::remove && state->removals == first);
    OA_CHECK(cancel_scheduled(paths, &error));

    OA_CHECK(!schedule_replacement(paths, &error) && !error.empty());
    write_text(paths.staging / "totala1.hpi", "a");
    ImportState checked;
    checked.phase = ImportPhase::copying;
    OA_CHECK(write_import_state(paths.state_file, checked, &error));
    OA_CHECK(schedule_replacement(paths, &error));
    state = read_import_state(paths.state_file, &error);
    OA_CHECK(state && state->phase == ImportPhase::checked && state->mode == ImportMode::replace);
    OA_CHECK(!schedule_removal(paths, first, &error) && !error.empty());
    ImportRun run;
    const auto plan = std::make_shared<const ImportPlan>();
    RunRequest add;
    add.mode = ImportMode::add;
    OA_CHECK(!run.start(GameFilesHooks{}, paths, plan, {}, add, &error) && !error.empty());
    OA_CHECK(cancel_scheduled(paths, &error));
    OA_CHECK(!there(paths.staging) && !there(paths.state_file));
}

/// What is installed: the parts, the mods, the sizes, the demo, the demo's unused data and the
/// folders set aside; and the files of a part, for Remove.
void installed_summary(const fs::path& scratch) {
    const auto root = scratch / "installed";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    OA_CHECK(!summarize_installed(hooks, paths).present);
    write_sized(paths.game_folder / "totala1.hpi", 100);
    write_sized(paths.game_folder / "rev31.gp3", 20);
    write_sized(paths.game_folder / "ccdata.ccx", 30);
    write_sized(paths.game_folder / "music" / "1.mp3", 5);
    write_sized(paths.game_folder / "music" / "2.ogg", 6);
    write_sized(paths.game_folder / "Data" / "1.zrb", 7);
    write_text(paths.game_folder / "mods" / "Example" / "oamod.yaml", example_profile);
    write_sized(paths.game_folder / "mods" / "Example" / "units" / "x.fbi", 8);
    write_sized(paths.game_folder / "totala.ini", 9);
    write_sized(paths.documents / "Total Annihilation (old)" / "a", 10);
    write_sized(paths.documents / "Total Annihilation (old) 2" / "b", 5);
    write_sized(paths.documents / "Total Annihilation (old) 02" / "c", 1);
    write_sized(paths.documents / "Total Annihilation (older)" / "d", 1);
    write_sized(paths.data_folder / "demo-1997" / "TADemo.hpi", 40);
    const auto summary = summarize_installed(hooks, paths);
    OA_CHECK(summary.present && !summary.demo);
    const auto part = [&](Part which) -> const PartSummary& {
        return summary.parts[static_cast<std::size_t>(which)];
    };
    OA_CHECK(part(Part::game_archives).found && part(Part::game_archives).bytes == 100);
    OA_CHECK(part(Part::update_31c).found && part(Part::core_contingency).files == 1);
    OA_CHECK(!part(Part::battle_tactics).found);
    OA_CHECK(part(Part::music).files == 2 && part(Part::music).music_tracks == 2);
    OA_CHECK(part(Part::movies).files == 1 && part(Part::mods).files == 2);
    OA_CHECK(part(Part::other).files == 1);
    OA_CHECK(summary.mods.size() == 1 && summary.mods.front().id == "example");
    OA_CHECK(summary.mods.front().folder == "mods/Example");
    OA_CHECK(summary.bytes == 100 + 20 + 30 + 5 + 6 + 7 + example_profile.size() + 8 + 9);
    OA_CHECK(summary.demo_data_bytes == 40 && summary.demo_data_unused);
    OA_CHECK(summary.old_folders.size() == 2 && summary.old_folder_bytes == 15);
    OA_CHECK(!summary.listing.empty());

    auto files = part_files(hooks, paths, Part::music);
    std::sort(files.begin(), files.end());
    OA_CHECK(files == (std::vector<std::string>{"music/1.mp3", "music/2.ogg"}));
    files = part_files(hooks, paths, Part::mods, "example");
    std::sort(files.begin(), files.end());
    OA_CHECK(
        files == (std::vector<std::string>{"mods/Example/oamod.yaml", "mods/Example/units/x.fbi"})
    );
    OA_CHECK(part_files(hooks, paths, Part::mods, "nothing").empty());
    OA_CHECK(
        part_files(hooks, paths, Part::game_archives) == std::vector<std::string>{"totala1.hpi"}
    );

    // Removing a part: scheduled, then applied at the next start.
    std::string error;
    OA_CHECK(schedule_removal(paths, part_files(hooks, paths, Part::music), &error));
    OA_CHECK(recover_import(hooks, paths, false).outcome == Recovery::applied);
    OA_CHECK(!there(paths.game_folder / "music") && there(paths.game_folder / "totala1.hpi"));
    // The folders set aside and the demo's data go at once, after the player confirmed.
    OA_CHECK(remove_folder_now(summary.old_folders.front(), &error));
    OA_CHECK(!there(paths.documents / "Total Annihilation (old)"));
    OA_CHECK(!remove_folder_now({}, &error));

    // A folder holding only the demo's installer.
    const auto demo_paths = case_paths(root / "demo");
    write_installer_sized(demo_paths.game_folder / "Total Annihilation.exe");
    const auto demo = summarize_installed(hooks, demo_paths);
    OA_CHECK(demo.demo && demo.parts[static_cast<std::size_t>(Part::demo)].found);

    // Backups: the game folder, the staging folder and the demo's data.
    script.backed_up.clear();
    fs::create_directories(paths.staging);
    apply_backup_setting(hooks, paths, true);
    OA_CHECK(script.backed_up.size() == 3);
    for (const auto& [path, backed_up] : script.backed_up)
        OA_CHECK(backed_up);
    apply_backup_setting(GameFilesHooks{}, paths, false);
}

/// Scans: nested folders resumed with one, cancelling, a folder that is no game, the game
/// folder itself, a folder in the game folder's parent moved into place, files in the cloud.
void scans(const fs::path& scratch) {
    const auto root = scratch / "scans";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    std::string error;
    write_bytes(root / "drive" / "Games" / "TA" / "totala1.hpi", playable_archive());
    {
        SourceScan scan;
        ScanRequest request;
        request.paths = {path_to_utf8(root / "drive")};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        auto snapshot = finish(scan);
        OA_CHECK(snapshot.stage == ScanStage::nested);
        OA_CHECK(snapshot.names.nested == std::vector<std::string>{"Games/TA"});
        OA_CHECK(!scan.use_nested(1, &error));
        OA_CHECK(scan.use_nested(0, &error));
        snapshot = finish(scan);
        OA_CHECK(snapshot.stage == ScanStage::planned && snapshot.plan);
        if (snapshot.plan) {
            OA_CHECK(snapshot.plan->source == root / "drive" / "Games" / "TA");
            OA_CHECK(snapshot.plan->location == "drive \xe2\x80\xba Games \xe2\x80\xba TA");
        }
    }
    {
        SourceScan scan;
        ScanRequest request;
        request.paths = {path_to_utf8(root / "drive")};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        OA_CHECK(finish(scan).stage == ScanStage::nested);
        scan.cancel();
        const auto snapshot = scan.snapshot();
        OA_CHECK(snapshot.stage == ScanStage::failed && snapshot.error.empty());
        OA_CHECK(snapshot.failure == ScanSnapshot::Failure::none);
    }
    {
        write_text(root / "letters" / "readme.txt", "x");
        SourceScan scan;
        ScanRequest request;
        request.paths = {path_to_utf8(root / "letters")};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        const auto snapshot = finish(scan);
        OA_CHECK(snapshot.stage == ScanStage::failed);
        OA_CHECK(snapshot.failure == ScanSnapshot::Failure::not_a_game && !snapshot.error.empty());
    }
    {
        SourceScan scan;
        ScanRequest request;
        request.paths = {path_to_utf8(root / "missing")};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        const auto snapshot = finish(scan);
        OA_CHECK(snapshot.failure == ScanSnapshot::Failure::unreadable && !snapshot.error.empty());
        OA_CHECK(!scan.start(hooks, paths, ScanRequest{}, &error));
    }
    {
        write_bytes(paths.game_folder / "totala1.hpi", playable_archive());
        SourceScan scan;
        ScanRequest request;
        request.paths = {path_to_utf8(paths.game_folder)};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        OA_CHECK(finish(scan).stage == ScanStage::already_there);
    }
    {
        // A folder beside the game folder is moved into place whole: no copy.
        const auto pack = paths.documents / "TA Commander Pack";
        write_bytes(pack / "totala1.hpi", playable_archive());
        write_text(pack / "Setup.exe", "stays with the folder");
        const auto plan = scan_plan(hooks, paths, pack);
        OA_CHECK(plan->move_in_place);
        script.copies = 0;
        const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
        OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
        OA_CHECK(script.copies == 0 && run.bytes_total == 0);
        const auto commit = commit_import(hooks, paths, ImportMode::replace, false);
        OA_CHECK(commit.ok && commit.old_folder == paths.documents / "Total Annihilation (old)");
        OA_CHECK(!there(pack) && there(paths.game_folder / "Setup.exe"));
        OA_CHECK(!there(paths.staging) && !there(paths.state_file));
    }
    {
        // Files held in the cloud: nothing is read before COPY, so the check waits.
        Script cloud;
        cloud.listing = {file_entry("totala1.hpi", 100, true), file_entry("rev31.gp3", 10)};
        const auto listed = hooks_of(cloud, true);
        ScanSnapshot snapshot;
        const auto plan =
            scan_plan(listed, paths, root / "cloud folder", SourceKind::game_folder, &snapshot);
        OA_CHECK(!snapshot.source_check && snapshot.source_check_skipped);
        OA_CHECK(plan->remote_bytes == 100 && plan->total_bytes == 110);
        OA_CHECK(snapshot.files == 2 && snapshot.bytes == 110);
    }
    {
        // Far too large: the whole plan, marked so the screen asks first.
        Script huge;
        huge.listing = {file_entry("totala1.hpi", far_too_large_bytes + 1, true)};
        const auto listed = hooks_of(huge, true);
        ScanSnapshot snapshot;
        const auto plan =
            scan_plan(listed, paths, root / "whole drive", SourceKind::game_folder, &snapshot);
        OA_CHECK(snapshot.failure == ScanSnapshot::Failure::too_large && plan->files.size() == 1);
    }
}

/// The demo route over a synthetic file: refused unless it has the installer's size; a
/// movable installer is moved into staging, and the check reads it there.
void demo_route(const fs::path& scratch) {
    const auto root = scratch / "demo route";
    const auto paths = case_paths(root);
    Script script;
    const auto hooks = hooks_of(script);
    std::string error;
    write_text(root / "Downloads" / "Setup.exe", "small");
    {
        SourceScan scan;
        ScanRequest request;
        request.kind = SourceKind::demo_installer;
        request.paths = {path_to_utf8(root / "Downloads" / "Setup.exe")};
        OA_CHECK(scan.start(hooks, paths, request, &error));
        const auto snapshot = finish(scan);
        OA_CHECK(snapshot.stage == ScanStage::failed);
        OA_CHECK(snapshot.failure == ScanSnapshot::Failure::not_demo_installer);
        OA_CHECK(snapshot.error.find("Setup.exe") != std::string::npos);
        OA_CHECK(snapshot.plan && snapshot.plan->files.size() == 1);
    }
    const auto installer = root / "tmp" / "Total Annihilation.exe";
    write_installer_sized(installer);
    SourceScan scan;
    ScanRequest request;
    request.kind = SourceKind::demo_installer;
    request.paths = {path_to_utf8(installer)};
    request.movable = true;
    OA_CHECK(scan.start(hooks, paths, request, &error));
    const auto snapshot = finish(scan);
    OA_CHECK(snapshot.stage == ScanStage::planned && snapshot.plan);
    if (!snapshot.plan)
        return;
    const auto& plan = *snapshot.plan;
    OA_CHECK(plan.demo && plan.movable && plan.files.size() == 1);
    OA_CHECK(
        plan.files.front().part == Part::demo &&
        plan.files.front().target == "Total Annihilation.exe"
    );
    OA_CHECK(plan.location == "Total Annihilation.exe");
    OA_CHECK(import_mode_of(plan) == ImportMode::replace);
    script.copies = 0;
    const auto run = run_plan(hooks, paths, snapshot.plan, ImportMode::replace, &script);
    OA_CHECK(script.copies == 0);
    OA_CHECK(!there(installer) && there(paths.staging / "Total Annihilation.exe"));
    // Zeros are not the release the engine recognises: the check refuses it.
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::not_usable);
    OA_CHECK(run.check && run.check->demo.outcome == oa::app::DemoOutcome::unrecognised);
}

/// Copies files of the installed game into a scratch folder, as a player's source.
fs::path copy_game_subset(const fs::path& game, const fs::path& folder) {
    fs::create_directories(folder);
    for (const auto* name : {"totala1.hpi", "rev31.gp3", "tactics1.hpi"}) {
        fs::path found;
        std::error_code error;
        for (fs::directory_iterator entry{game, error}, end; !error && entry != end;
             entry.increment(error)) {
            auto file = path_to_utf8(entry->path().filename());
            std::transform(file.begin(), file.end(), file.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (file == name)
                found = entry->path();
        }
        OA_CHECK(!found.empty());
        if (!found.empty())
            fs::copy_file(found, folder / found.filename());
    }
    return folder;
}

/// A subset of the installed game copied, checked usable and committed.
void installed_game_subset(const fs::path& game, const fs::path& scratch) {
    const auto root = scratch / "game data";
    const auto paths = case_paths(root);
    const auto source = copy_game_subset(game, root / "source" / "Total Annihilation");
    Script script;
    const auto hooks = hooks_of(script);
    ScanSnapshot scanned;
    const auto plan = scan_plan(hooks, paths, source, SourceKind::game_folder, &scanned);
    OA_CHECK(plan->files.size() == 3);
    OA_CHECK(plan->parts[static_cast<std::size_t>(Part::game_archives)].files == 1);
    OA_CHECK(plan->parts[static_cast<std::size_t>(Part::update_31c)].files == 1);
    OA_CHECK(plan->parts[static_cast<std::size_t>(Part::battle_tactics)].files == 1);
    OA_CHECK(scanned.source_check && oa::app::usable(*scanned.source_check));
    const auto run = run_plan(hooks, paths, plan, ImportMode::replace, &script);
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    OA_CHECK(run.check && oa::app::usable(*run.check) && run.check->skipped.empty());
    OA_CHECK(run.bytes_done == plan->total_bytes);
    const auto commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(commit.ok);
    const auto install = oa::app::inspect_game_install(paths.game_folder, paths.data_folder);
    OA_CHECK(oa::app::usable(install) && install.archives.size() == 3);
    std::printf(
        "game files import data: %zu files, %llu bytes copied, checked and committed\n",
        plan->files.size(),
        static_cast<unsigned long long>(plan->total_bytes)
    );
}

/// The demo route over the demo's own installer: moved, unpacked by the check, committed.
void demo_installer_route(const fs::path& installer, const fs::path& scratch) {
    const auto root = scratch / "demo data";
    const auto paths = case_paths(root);
    const auto picked = root / "tmp" / path_to_utf8(installer.filename());
    fs::create_directories(picked.parent_path());
    fs::copy_file(installer, picked);
    Script script;
    const auto hooks = hooks_of(script);
    SourceScan scan;
    ScanRequest request;
    request.kind = SourceKind::demo_installer;
    request.paths = {path_to_utf8(picked)};
    request.movable = true;
    std::string error;
    OA_CHECK(scan.start(hooks, paths, request, &error));
    const auto scanned = finish(scan);
    OA_CHECK(scanned.stage == ScanStage::planned && scanned.plan && scanned.plan->demo);
    if (!scanned.plan)
        return;
    auto need = space_need(hooks, paths, *scanned.plan, {});
    OA_CHECK(
        need.copy_bytes == 0 && need.need_bytes == demo_1997.archive_size + space_margin_bytes
    );
    const auto run = run_plan(hooks, paths, scanned.plan, ImportMode::replace, &script);
    OA_CHECK(!there(picked));
    OA_CHECK(run.stage == RunStage::checked && run.failure == RunFailure::none);
    OA_CHECK(run.check && oa::app::usable(*run.check));
    OA_CHECK(
        run.check && run.check->demo.outcome == oa::app::DemoOutcome::ready &&
        run.check->demo.unpacked
    );
    const auto commit = commit_import(hooks, paths, ImportMode::replace, false);
    OA_CHECK(commit.ok);
    const auto install = oa::app::inspect_game_install(paths.game_folder, paths.data_folder);
    OA_CHECK(oa::app::usable(install) && install.demo.outcome == oa::app::DemoOutcome::ready);
    OA_CHECK(!install.demo.unpacked);
    const auto summary = summarize_installed(hooks, paths);
    OA_CHECK(
        summary.demo && summary.demo_data_bytes == demo_1997.archive_size &&
        !summary.demo_data_unused
    );
    std::printf("game files import data: the demo's installer moved, unpacked and committed\n");
}

/// Runs the cases that read the installed game and the demo's installer.
int data_cases() {
    const auto scratch = oa::test::make_scratch_directory("oa-game-files-import-data");
    bool ran = false;
    try {
        const auto game = oa::test::game_directory();
        if (!game.empty()) {
            installed_game_subset(game, scratch);
            ran = true;
        } else if (oa::test::game_data_required()) {
            oa::test::missing_game_directory(
                "the game files import data cases", "OA_GAME_DIR is not set"
            );
        } else {
            std::printf("skipped the installed game's case: OA_GAME_DIR is not set\n");
        }
        const auto named = oa::platform::environment_value("OA_DEMO_INSTALLER");
        if (named && !named->empty()) {
            const auto installer = path_from_utf8(*named);
            std::error_code error;
            OA_CHECK(fs::is_regular_file(installer, error));
            if (fs::is_regular_file(installer, error))
                demo_installer_route(installer, scratch);
            ran = true;
        } else {
            std::printf("skipped the demo route: OA_DEMO_INSTALLER is not set\n");
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        ++oa::test::failed_checks();
    }
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    if (!ran)
        oa::test::skip_test(
            "the game files import data cases", "neither OA_GAME_DIR nor OA_DEMO_INSTALLER is set"
        );
    return oa::test::check_exit_status();
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        return data_cases();
    leave_out_table();
    unsafe_names();
    case_folding();
    const auto scratch = oa::test::make_scratch_directory("oa-game-files-import");
    try {
        name_check(scratch);
        parts_and_switches(scratch);
        space_arithmetic(scratch);
        listing(scratch);
        full_copy(scratch);
        commit_over_folder(scratch);
        stop_and_expiry(scratch);
        resume(scratch);
        scripted_failures(scratch);
        chunked_copies(scratch);
        recovery(scratch);
        state_file(scratch);
        adopting(scratch);
        additions(scratch);
        links_in_the_game_folder(scratch);
        scheduling(scratch);
        installed_summary(scratch);
        scans(scratch);
        demo_route(scratch);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAILED: %s\n", error.what());
        ++oa::test::failed_checks();
    }
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    const int status = oa::test::check_exit_status();
    if (status == 0)
        std::puts("game files import checks passed");
    return status;
}
