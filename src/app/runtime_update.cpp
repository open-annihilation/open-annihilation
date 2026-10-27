// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One check for a new release. The main menu starts it when it is due, a
// match stops it, and a result that reached the server is remembered. The
// feed an earlier check stored is read again at the first menu, so a
// release out, or one already downloaded, is known without a request. The
// guard counts an unconfirmed start before the game loads; a start the
// player quits takes its count back.
#include "oa/app/runtime.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/plan.hpp"
#include "runtime_update_start.hpp"
#include "update_state.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(__APPLE__)
#include <TargetConditionals.h>
#include <mach-o/dyld.h>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// Frames in a row the main menu shows before an automatic check.
constexpr uint32_t kUpdateMenuFrames = 2;

/// Seconds since 1970.
int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// The feed this run checks. A named feed is a test feed.
update::Source source_for(const Options& options) {
    update::Source source;
    if (!options.update_feed.has_value())
        return source;
    source.feed_url = *options.update_feed;
    source.test = true;
    source.keys.reserve(options.update_keys.size());
    for (const std::string& key : options.update_keys) {
        const std::size_t split = key.find('=');
        if (split == std::string::npos || split == 0)
            continue;
        update::FeedKey parsed;
        parsed.id = key.substr(0, split);
        parsed.text = key.substr(split + 1);
        source.keys.push_back(std::move(parsed));
    }
    return source;
}

/// This program's build. The base path is cached for the process.
update::ThisBuild program_build() {
    // SDL caches this string for the process. The caller does not free it.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

/// `<data>/updates`, or nothing when the data folder cannot be named.
std::optional<fs::path> updates_folder(const Options& options, std::string& failure) {
    try {
        const fs::path data =
            options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
        return data / "updates";
    } catch (const std::exception& thrown) {
        failure = thrown.what();
        return std::nullopt;
    }
}

/// One line for a finished check. Checking and idle say nothing.
void log_result(int64_t release, const update::CheckSnapshot& latest) {
    if (latest.remembered) {
        if (latest.state == update::CheckState::offered && latest.offer.release != nullptr)
            std::cerr << "open-annihilation: update check: the stored feed offers "
                      << latest.offer.release->version_text << '\n';
        return;
    }
    if (latest.state == update::CheckState::up_to_date) {
        std::cerr << "open-annihilation: update check: up to date (release " << release << ")\n";
        return;
    }
    if (latest.state == update::CheckState::offered && latest.offer.release != nullptr) {
        std::cerr << "open-annihilation: update check: offered "
                  << latest.offer.release->version_text << " (release "
                  << latest.offer.release->release << ")\n";
        return;
    }
    if (latest.state == update::CheckState::failed) {
        std::cerr << "open-annihilation: update check: failed: " << latest.failure << '\n';
        return;
    }
    if (latest.state == update::CheckState::cancelled)
        std::cerr << "open-annihilation: update check: cancelled\n";
}

} // namespace

void Runtime::destroy_update_state(UpdateState* state) noexcept {
    delete state;
}

void Runtime::cancel_update_check() noexcept {
    if (update_ != nullptr && update_->checker != nullptr)
        update_->checker->cancel();
}

const update::CheckSnapshot& Runtime::update_snapshot() const {
    if (update_ == nullptr) {
        static const update::CheckSnapshot empty;
        return empty;
    }
    return update_->held;
}

void Runtime::start_update_check(bool asked) {
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    const auto found = preference_values_.find(std::string(oa::ui::first_run::preference_key));
    const bool setup_done = found != preference_values_.end() &&
                            oa::ui::engine_settings::language_notice_trim(found->second) ==
                                oa::ui::first_run::preference_done &&
                            !first_run_blocks_language_notice();
    if (!setup_done)
        return;
    // An unattended or headless run stays quiet unless this run named a feed.
    if (!options_.update_feed.has_value() && (options_.unattended || options_.headless_check))
        return;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    const update::CheckMode mode = update::read_check_mode(preference_values_);
    if (mode == update::CheckMode::never)
        return;
    if (!asked && mode != update::CheckMode::automatically)
        return;
    if (!asked && update_->ran)
        return;
    if (!asked &&
        !update::check_due(mode, update::read_checked(preference_values_), wall_seconds()))
        return;
    if (update_->checker == nullptr) {
        std::string failure;
        const std::optional<fs::path> folder = updates_folder(options_, failure);
        if (!folder) {
            if (!update_->logged_folder) {
                update_->logged_folder = true;
                std::cerr << "open-annihilation: update check: failed: " << failure << '\n';
            }
            return;
        }
        update_->checker = std::make_unique<update::Checker>(*folder);
    }
    const update::ThisBuild built = program_build();
    update_->release = built.release;
    update_->checker->start(source_for(options_), built, asked);
    if (!asked)
        update_->ran = true;
}

void Runtime::tick_update_check() {
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (match_ != nullptr || screen_ == Screen::match)
        cancel_update_check();
    // The first menu reads the feed an earlier check stored, once, with no
    // request: a release out is offered again (Later asks at the next
    // start), and a package already downloaded is found. Never reads nothing.
    if (update_->checker == nullptr && screen_ == Screen::main_menu) {
        const auto found = preference_values_.find(std::string(oa::ui::first_run::preference_key));
        const bool setup_done = found != preference_values_.end() &&
                                oa::ui::engine_settings::language_notice_trim(found->second) ==
                                    oa::ui::first_run::preference_done;
        const bool quiet_run =
            !options_.update_feed.has_value() && (options_.unattended || options_.headless_check);
        if (setup_done && !quiet_run &&
            update::read_check_mode(preference_values_) != update::CheckMode::never) {
            std::string failure;
            if (const std::optional<fs::path> folder = updates_folder(options_, failure)) {
                update_->checker = std::make_unique<update::Checker>(*folder);
                const update::ThisBuild built = program_build();
                update_->release = built.release;
                static_cast<void>(update_->checker->remember(source_for(options_), built));
            }
        }
    }
    if (update_->checker != nullptr) {
        const update::CheckSnapshot latest = update_->checker->snapshot();
        if (latest.generation != update_->seen_generation) {
            update_->seen_generation = latest.generation;
            update_->held = latest;
            log_result(update_->release, latest);
            if (latest.checked_at.has_value() && (latest.state == update::CheckState::up_to_date ||
                                                  latest.state == update::CheckState::offered ||
                                                  latest.state == update::CheckState::failed)) {
                update::write_checked(preference_values_, *latest.checked_at);
                preferences_dirty_ = true;
                try {
                    flush_preferences();
                } catch (const std::exception& failure) {
                    std::cerr << "open-annihilation: update check: failed: " << failure.what()
                              << '\n';
                }
            }
        }
    }
    // Ask while the main menu is up, including while a dialog or Settings
    // keeps the question waiting. A match took the branch above and cancels
    // there; the question is not offered over it. A protected place is asked
    // to move before any update is offered.
    offer_move_to_applications();
    offer_update();
    // The download, the ready prompt and the Updated notice run while Settings
    // is open. The two-frame wait below is only the automatic check.
    tick_update_install();
    const bool settled = update_menu_settled();
    if (!settled) {
        update_->menu_frames = 0;
        return;
    }
    if (update_->menu_frames < kUpdateMenuFrames)
        ++update_->menu_frames;
    if (update_->menu_frames >= kUpdateMenuFrames)
        start_update_check(false);
}

namespace {

namespace fs = std::filesystem;

/// The start the guard counted in this process, so a quit can take it back.
struct CountedStart {
    bool counted{};                ///< the guard counted this start
    std::filesystem::path updates; ///< the data folder's updates directory
};

/// The process's counted start. The guard runs once, before any runtime.
CountedStart& counted_start() {
    static CountedStart counted;
    return counted;
}

/// Reads a state file, capped at the plan's limit. Nothing when it cannot be read.
std::optional<update::State> read_state_file(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return std::nullopt;
    std::vector<char> raw(
        (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
    );
    if (raw.size() > update::max_plan_bytes)
        return std::nullopt;
    const std::vector<uint8_t> bytes(raw.begin(), raw.end());
    std::string error;
    return update::read_state(bytes, &error);
}

/// True when this start must not be counted.
///
/// A check, a headless check and an unattended run are decided from the
/// arguments before any state file is read.
bool guard_skipped(int argc, char** argv, const Options& options) {
    if (options.unattended || options.headless_check)
        return true;
    if (argv == nullptr)
        return false;
    for (int index = 1; index < argc; ++index) {
        const char* const argument = argv[index];
        if (argument == nullptr)
            continue;
        const std::string_view text{argument};
        if (text.starts_with("--check-") || text == "--headless-check" || text == "--unattended")
            return true;
    }
    return false;
}

// The early guard runs where a helper can take a start back: Windows and macOS.
#if defined(_WIN32) || (defined(__APPLE__) && !TARGET_OS_IPHONE)

/// The file this process was started from. Empty when the system will not name it.
fs::path guard_executable() {
#if defined(_WIN32)
    std::wstring wide(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, wide.data(), static_cast<DWORD>(wide.size()));
        if (length == 0) {
            std::string narrow(MAX_PATH, '\0');
            const DWORD narrow_length =
                GetModuleFileNameA(nullptr, narrow.data(), static_cast<DWORD>(narrow.size()));
            if (narrow_length == 0 || narrow_length >= narrow.size())
                return {};
            narrow.resize(narrow_length);
            return fs::path(narrow);
        }
        if (length < wide.size()) {
            wide.resize(length);
            return fs::path(wide);
        }
        if (wide.size() >= 32768)
            return {};
        wide.resize(wide.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buffer(static_cast<std::size_t>(size) + 1U, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) != 0)
        return {};
    return fs::path(buffer.data());
#endif
}

/// An absolute path as UTF-8.
std::string guard_absolute(const fs::path& path) {
    std::error_code error;
    const fs::path full = fs::absolute(path, error);
    if (error || full.empty())
        return path_to_utf8(path);
    return path_to_utf8(full);
}

/// One argument of the program the helper starts again, when this run named it.
void guard_add_argument(update::Plan& plan, std::string flag, std::string value) {
    plan.arguments.push_back(std::move(flag));
    plan.arguments.push_back(std::move(value));
}

/// The arguments the kept version is started with, before the roll-back flags.
void guard_restart_arguments(update::Plan& plan, const Options& options) {
    if (options.game_dir_named && !options.game_dir.empty())
        guard_add_argument(plan, "--game-dir", guard_absolute(options.game_dir));
    if (options.data_dir && !options.data_dir->empty())
        guard_add_argument(plan, "--data-dir", guard_absolute(*options.data_dir));
    if (options.user_folder && !options.user_folder->empty())
        guard_add_argument(plan, "--user-folder", guard_absolute(*options.user_folder));
    if (options.preferences_file && !options.preferences_file->empty())
        guard_add_argument(plan, "--preferences-file", guard_absolute(*options.preferences_file));
    if (options.log_dir && !options.log_dir->empty())
        guard_add_argument(plan, "--log-dir", guard_absolute(*options.log_dir));
    if (options.skip_intro)
        plan.arguments.emplace_back("--skip-intro");
    if (options.mute)
        plan.arguments.emplace_back("--mute");
}

/// Starts the helper and does not wait for it.
bool guard_spawn(const fs::path& helper, const fs::path& plan_file, std::string& why) {
#if defined(_WIN32)
    const std::wstring helper_text = helper.wstring();
    const std::wstring plan_text = plan_file.wstring();
    std::wstring command = L"\"" + helper_text + L"\" --plan \"" + plan_text + L"\"";
    STARTUPINFOW wide_info{};
    wide_info.cb = sizeof wide_info;
    PROCESS_INFORMATION process{};
    if (CreateProcessW(
            nullptr,
            command.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS,
            nullptr,
            nullptr,
            &wide_info,
            &process
        )) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        return true;
    }
    const std::string helper_narrow = path_to_utf8(helper);
    const std::string plan_narrow = path_to_utf8(plan_file);
    std::string narrow = "\"" + helper_narrow + "\" --plan \"" + plan_narrow + "\"";
    STARTUPINFOA narrow_info{};
    narrow_info.cb = sizeof narrow_info;
    if (!CreateProcessA(
            helper_narrow.c_str(),
            narrow.data(),
            nullptr,
            nullptr,
            FALSE,
            DETACHED_PROCESS,
            nullptr,
            nullptr,
            &narrow_info,
            &process
        )) {
        why = "cannot start the helper";
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#elif defined(__APPLE__)
    std::string helper_text = path_to_utf8(helper);
    std::string plan_flag = "--plan";
    std::string plan_text = path_to_utf8(plan_file);
    char* args[] = {helper_text.data(), plan_flag.data(), plan_text.data(), nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, helper_text.c_str(), nullptr, nullptr, args, environ);
    if (spawned != 0) {
        why = "cannot start the helper";
        return false;
    }
    return true;
#endif
}

#endif

} // namespace

int guard_unconfirmed_start(int argc, char** argv, const Options& options) {
    if (guard_skipped(argc, argv, options))
        return -1;
#if !defined(_WIN32) && !defined(__APPLE__)
    return -1;
#elif defined(__APPLE__) && TARGET_OS_IPHONE
    return -1;
#else
    const fs::path executable = guard_executable();
    if (executable.empty())
        return -1;
    fs::path data;
    try {
        data = options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
    } catch (const std::exception& thrown) {
        std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
        return -1;
    }
    const fs::path updates = data / "updates";
    const std::optional<update::State> loaded = read_state_file(updates / "state.json");
    if (!loaded)
        return -1;
    update::State state = *loaded;
    const update::ThisBuild built = update::this_build({});
    if (state.confirmed || state.installed_version != built.version_text)
        return -1;
    if (state.kept_version.empty() || state.kept_path.empty()) {
        std::cerr << "open-annihilation: update: nothing is kept, so this start continues\n";
        return -1;
    }
    const int32_t counted = update::count_failed_start(state);
    std::string write_error;
    if (!update::write_state(updates / "state.json", state, &write_error)) {
        std::cerr << "open-annihilation: update: " << write_error << '\n';
        return -1;
    }
    counted_start().counted = true;
    counted_start().updates = updates;
    if (counted < update::failed_starts_that_roll_back)
        return -1;

    const fs::path executable_dir = executable.parent_path();
#if defined(__APPLE__)
    const fs::path helper = executable_dir / "oa-update";
    const bool bundle = executable_dir.filename() == "MacOS";
    const fs::path install = bundle ? executable_dir.parent_path().parent_path() : executable_dir;
    const update::Route route = update::Route::macos_bundle;
    const bool installer = false;
#else
    const fs::path helper = executable_dir / "oa-update.exe";
    const fs::path install = executable_dir;
    std::error_code uninstall_error;
    const bool installer = fs::is_regular_file(executable_dir / "Uninstall.exe", uninstall_error);
    const update::Route route =
        installer ? update::Route::windows_installer : update::Route::windows_folder;
#endif
    fs::path preferences;
    try {
        preferences = options.preferences_file && !options.preferences_file->empty()
                          ? *options.preferences_file
                          : oa::platform::preferences::default_file();
    } catch (const std::exception& thrown) {
        std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
        return -1;
    }

    update::Plan plan;
    plan.plan = update::plan_format;
    plan.action = update::Action::roll_back;
    plan.roll_back_reason = update::RollBackReason::failed_starts;
    plan.route = route;
#if defined(_WIN32)
    plan.wait_for = static_cast<int64_t>(GetCurrentProcessId());
#else
    plan.wait_for = static_cast<int64_t>(getpid());
#endif
    plan.install = guard_absolute(install);
    plan.program = guard_absolute(executable);
    guard_restart_arguments(plan, options);
    plan.from_version = built.version_text;
    plan.feed = guard_absolute(updates / "feed.json");
    plan.signature = guard_absolute(updates / "feed.json.sig");
    plan.release = state.installed_release;
    plan.package = "kept";
    plan.file = guard_absolute(updates / "kept.pkg");
    plan.installer = installer;
    plan.preferences = guard_absolute(preferences);
    plan.updates = guard_absolute(updates);
    plan.title = "Going back to " + state.kept_version;
    if (plan.title.size() > update::max_window_line_bytes)
        plan.title.resize(update::max_window_line_bytes);
    plan.line.clear();

    std::error_code directory_error;
    fs::create_directories(updates, directory_error);
    if (directory_error) {
        std::cerr << "open-annihilation: update: " << directory_error.message() << '\n';
        return -1;
    }
    const fs::path plan_file = updates / "roll-back.json";
    std::string why;
    const std::string text = update::plan_text(plan);
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    if (!oa::platform::replace_file(plan_file, bytes, &why)) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return -1;
    }
    if (!guard_spawn(helper, plan_file, why)) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return -1;
    }
    std::cerr << "open-annihilation: update: going back to " << state.kept_version << '\n';
    return 0;
#endif
}

fs::path update_starts_file() {
#if defined(__linux__)
    fs::path folder{oa::platform::program_directory()};
    if (folder.empty())
        return {};
    if (folder.filename().empty())
        folder = folder.parent_path();
    fs::path install = folder;
    if (folder.filename() == "current")
        install = folder.parent_path();
    else if (folder.parent_path().filename() == "versions")
        install = folder.parent_path().parent_path();
    else if (folder.parent_path().filename() == "current")
        install = folder.parent_path().parent_path();
    return install / "update-starts";
#else
    return {};
#endif
}

void release_quit_start(const Options& options) {
    fs::path updates = counted_start().updates;
#if defined(__linux__)
    // The launcher counted this start, whatever ran it.
    if (updates.empty()) {
        try {
            const fs::path data =
                options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
            updates = data / "updates";
        } catch (const std::exception& thrown) {
            std::cerr << "open-annihilation: update: " << thrown.what() << '\n';
            return;
        }
    }
#else
    (void)options;
    // On macOS and Windows only a start the guard counted is taken back.
    if (!counted_start().counted)
        return;
#endif
    if (updates.empty())
        return;
    std::optional<update::State> state = read_state_file(updates / "state.json");
    if (!state || state->confirmed ||
        state->installed_version != update::this_build({}).version_text)
        return;
    update::uncount_quit_start(*state);
    std::string why;
    if (!update::write_state(updates / "state.json", *state, &why)) {
        std::cerr << "open-annihilation: update: " << why << '\n';
        return;
    }
    counted_start().counted = false;
    const fs::path starts = update_starts_file();
    std::error_code error;
    if (starts.empty() || !fs::is_regular_file(starts, error))
        return;
    std::ifstream input(starts);
    long long count = 0;
    if (!(input >> count) || count <= 0)
        return;
    input.close();
    const std::string text = std::to_string(count - 1) + "\n";
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    if (!oa::platform::replace_file(starts, bytes, &why))
        std::cerr << "open-annihilation: update: " << why << '\n';
    else
        std::cerr << "open-annihilation: update: a quit before the main menu is not counted\n";
}

} // namespace oa::app
