// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/route_windows.hpp"

#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/run.hpp"
#include "oa/update_helper/start.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
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
#include <shellapi.h>
#endif

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

constexpr std::string_view reboot_marker_name = "reboot-pending";
constexpr std::string_view installer_marker_name = "installer-pending";
constexpr std::string_view preferences_name = "preferences.conf";
constexpr std::string_view previous_name = "previous";
constexpr std::string_view scratch_name = "oa-write-check.tmp";
constexpr std::string_view uninstall_name = "Uninstall.exe";
#if defined(_WIN32)
constexpr std::string_view game_name = "open-annihilation.exe";
#endif

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

bool same_name(const fs::path& name, std::string_view expected) {
    return utf8_of(name) == expected;
}

bool list_children(const fs::path& folder, std::vector<fs::path>& children, std::string* why) {
    std::error_code io;
    fs::directory_iterator it(folder, io);
    while (!io && it != fs::directory_iterator()) {
        children.push_back(it->path());
        it.increment(io);
    }
    if (io) {
        set_error(why, "a folder could not be read");
        return false;
    }
    std::sort(children.begin(), children.end(), [](const fs::path& left, const fs::path& right) {
        return utf8_of(left.filename()) < utf8_of(right.filename());
    });
    return true;
}

/// One staged top folder, or a refusal. An empty path is not a folder.
bool staged_top(const fs::path& staged, fs::path& top, bool& present, std::string* why) {
    present = false;
    std::error_code io;
    if (!fs::is_directory(staged, io) || io)
        return true;
    std::vector<fs::path> children;
    if (!list_children(staged, children, why))
        return false;
    if (children.size() != 1 || !fs::is_directory(fs::symlink_status(children[0])) ||
        fs::is_symlink(fs::symlink_status(children[0]))) {
        set_error(why, "the staged package is not one folder");
        return false;
    }
    top = children[0];
    present = true;
    return true;
}

void push_remove(std::vector<Operation>& operations, const fs::path& path) {
    Operation operation;
    operation.kind = Operation::Kind::remove_tree;
    operation.path = path;
    operations.push_back(std::move(operation));
}

void push_transfer(
    std::vector<Operation>& operations,
    Operation::Kind kind,
    const fs::path& from,
    const fs::path& to
) {
    Operation operation;
    operation.kind = kind;
    operation.from = from;
    operation.to = to;
    operations.push_back(std::move(operation));
}

bool empty_previous(
    const fs::path& previous,
    bool skip_preferences,
    std::vector<Operation>& operations,
    std::string* why
) {
    std::error_code io;
    if (!fs::exists(previous, io) || io)
        return true;
    if (!fs::is_directory(previous, io) || io) {
        set_error(why, "the kept folder is not a folder");
        return false;
    }
    std::vector<fs::path> children;
    if (!list_children(previous, children, why))
        return false;
    for (const fs::path& child : children) {
        if (skip_preferences && same_name(child.filename(), preferences_name))
            continue;
        push_remove(operations, child);
    }
    return true;
}

std::string folder_text(std::string_view folder) {
    std::string text(folder);
    while (!text.empty() && (text.front() == '"' || text.front() == '\''))
        text.erase(text.begin());
    while (!text.empty() && (text.back() == '"' || text.back() == '\''))
        text.pop_back();
    for (char& character : text) {
        if (character == '/')
            character = '\\';
    }
    const bool drive_root = text.size() == 3 && text[1] == ':' && text[2] == '\\';
    while (text.size() > 1 && text.back() == '\\' && !drive_root)
        text.pop_back();
    return text;
}

#if defined(_WIN32)
bool write_bytes(const fs::path& path, std::string_view bytes, std::string* error) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        set_error(error, "a file could not be written");
        return false;
    }
    if (!bytes.empty())
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        set_error(error, "a file could not be written");
        std::error_code ignored;
        fs::remove(path, ignored);
        return false;
    }
    return true;
}
#endif

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return {};
    const std::istreambuf_iterator<char> first(in);
    std::string text(first, std::istreambuf_iterator<char>{});
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' '))
        text.pop_back();
    return text;
}

void drop_journal_files(const fs::path& updates) {
    std::error_code io;
    fs::remove(updates / "journal.json", io);
    fs::remove_all(updates / "journal-held", io);
}

#if defined(_WIN32)
void remove_stage(const fs::path& stage) {
    std::error_code io;
    fs::remove_all(stage, io);
}
#endif

bool restore_entry(const fs::path& from, const fs::path& to, std::string* error) {
    std::error_code io;
    const fs::file_status status = fs::symlink_status(from, io);
    if (io || !fs::exists(status)) {
        set_error(error, "cannot read " + utf8_of(from));
        return false;
    }
    if (fs::is_symlink(status)) {
        set_error(error, "refusing a link");
        return false;
    }
    if (fs::is_directory(status)) {
        if (fs::exists(to, io) && !io && !fs::is_directory(to, io)) {
            fs::remove(to, io);
            if (io) {
                set_error(error, "cannot put back " + utf8_of(to));
                return false;
            }
        }
        fs::create_directories(to, io);
        if (io) {
            set_error(error, "cannot put back " + utf8_of(to));
            return false;
        }
        std::vector<fs::path> children;
        if (!list_children(from, children, error))
            return false;
        for (const fs::path& child : children) {
            if (!restore_entry(child, to / child.filename(), error))
                return false;
        }
        return true;
    }
    if (!fs::is_regular_file(status)) {
        set_error(error, "refusing a special file");
        return false;
    }
    fs::create_directories(to.parent_path(), io);
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, io);
    if (io) {
        set_error(error, "cannot put back " + utf8_of(to));
        return false;
    }
    return true;
}

#if defined(_WIN32)
void add_bookkeeping(
    std::vector<Operation>& operations,
    const update::Plan& plan,
    const Verified& verified,
    const fs::path& updates,
    const fs::path& kept
) {
    std::error_code io;
    fs::create_directories(updates / previous_name, io);
    const fs::path preferences = updates / previous_name / preferences_name;
    if (fs::exists(preferences, io) && !io)
        push_remove(operations, preferences);
    push_transfer(operations, Operation::Kind::copy_tree, path_from(plan.preferences), preferences);

    update::State state;
    state.installed_version = verified.version;
    state.installed_release = verified.release;
    state.kept_version = plan.from_version;
    state.kept_path = utf8_of(kept);
    state.kept_route = plan.route;
    state.preferences_copy = utf8_of(preferences);
    state.confirmed = false;
    state.failed_starts = 0;
    Operation record;
    record.kind = Operation::Kind::write_file;
    record.path = updates / "state.json";
    record.bytes = update::state_text(state);
    operations.push_back(std::move(record));
}

struct ExeStamp {
    bool present{false};
    uintmax_t bytes{0};
    fs::file_time_type when{};
};

ExeStamp stamp_exe(const fs::path& file) {
    ExeStamp stamp;
    std::error_code io;
    if (!fs::is_regular_file(file, io) || io)
        return stamp;
    const auto size = fs::file_size(file, io);
    if (io)
        return stamp;
    const auto when = fs::last_write_time(file, io);
    if (io)
        return stamp;
    stamp.present = true;
    stamp.bytes = size;
    stamp.when = when;
    return stamp;
}

bool exe_changed(const ExeStamp& before, const ExeStamp& after) {
    if (!after.present)
        return false;
    if (!before.present)
        return true;
    return before.bytes != after.bytes || before.when != after.when;
}

bool folder_accepts_file(const fs::path& folder) {
    std::error_code io;
    if (!fs::is_directory(folder, io) || io)
        return false;
    const fs::path scratch = folder / scratch_name;
    {
        std::ofstream out(scratch, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out << 'x';
        if (!out) {
            fs::remove(scratch, io);
            return false;
        }
    }
    fs::remove(scratch, io);
    return true;
}

// GetVersion is deprecated and the Windows build treats that warning as an
// error. The high bit is still the 9x test, so the call goes through the
// exported function without naming it in this file.
uint32_t current_windows_version() {
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (kernel == nullptr)
        return 0;
    using GetVersionFn = DWORD(WINAPI*)();
    auto* function = reinterpret_cast<GetVersionFn>(GetProcAddress(kernel, "GetVersion"));
    if (function == nullptr)
        return 0;
    return static_cast<uint32_t>(function());
}

fs::path this_program() {
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
}

std::wstring wide_from_utf8(std::string_view text) {
    if (text.empty())
        return {};
    const int size = static_cast<int>(text.size());
    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, nullptr, 0);
    if (needed <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    const int wrote = MultiByteToWideChar(CP_UTF8, 0, text.data(), size, wide.data(), needed);
    if (wrote <= 0)
        return {};
    return wide;
}

std::string acp_from_wide(const std::wstring& wide) {
    if (wide.empty())
        return {};
    const int needed = WideCharToMultiByte(
        CP_ACP, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr
    );
    if (needed <= 0)
        return {};
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    const int wrote = WideCharToMultiByte(
        CP_ACP,
        0,
        wide.c_str(),
        static_cast<int>(wide.size()),
        narrow.data(),
        needed,
        nullptr,
        nullptr
    );
    if (wrote <= 0)
        return {};
    return narrow;
}

std::string acp_from_wide_z(const wchar_t* wide) {
    if (wide == nullptr || wide[0] == L'\0')
        return {};
    const int needed = WideCharToMultiByte(CP_ACP, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1)
        return {};
    std::string narrow(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_ACP, 0, wide, -1, narrow.data(), needed, nullptr, nullptr) <= 0)
        return {};
    while (!narrow.empty() && narrow.back() == '\0')
        narrow.pop_back();
    return narrow;
}

// A wide call that is a stub on Windows 95 fails with this and the A form is used.
bool not_implemented(DWORD code) {
    return code == ERROR_CALL_NOT_IMPLEMENTED;
}

int move_file_win(void*, const fs::path& from, const fs::path& to) {
    // COPY_ALLOWED covers a staged file that lives on another volume. The
    // destination is not replaced: the old file is moved away first.
    if (MoveFileExW(from.wstring().c_str(), to.wstring().c_str(), MOVEFILE_COPY_ALLOWED))
        return 0;
    DWORD code = GetLastError();
    if (!not_implemented(code))
        return static_cast<int>(code);
    if (MoveFileExA(from.string().c_str(), to.string().c_str(), MOVEFILE_COPY_ALLOWED))
        return 0;
    code = GetLastError();
    if (!not_implemented(code))
        return static_cast<int>(code);
    if (MoveFileA(from.string().c_str(), to.string().c_str()))
        return 0;
    return static_cast<int>(GetLastError());
}

void pause_ms(void*, int milliseconds) {
    if (milliseconds > 0)
        Sleep(static_cast<DWORD>(milliseconds));
}

struct MoveState {
    JournalHooks saved{};
    bool on_9x{false};
    std::vector<PostponedRename> postponed;
};

void chained_note(void* context, std::string_view line) {
    auto* state = static_cast<MoveState*>(context);
    if (state->saved.note != nullptr)
        state->saved.note(state->saved.context, line);
}

MoveOutcome
windows_move(void* context, const fs::path& from, const fs::path& to, std::string* error) {
    auto* state = static_cast<MoveState*>(context);
    const MoveAttempt attempt =
        move_with_retries(move_file_win, pause_ms, nullptr, state->on_9x, from, to);
    const MoveOutcome outcome = classify_move(attempt, state->on_9x);
    if (outcome == MoveOutcome::postponed) {
        PostponedRename row;
        row.from = utf8_of(from);
        row.to = utf8_of(to);
        state->postponed.push_back(std::move(row));
        return outcome;
    }
    if (outcome == MoveOutcome::failed)
        set_error(error, "cannot rename " + utf8_of(from));
    return outcome;
}

struct ShellWait {
    bool started{false};
    bool cancelled{false};
    uint32_t exit_code{1};
    std::string error;
};

ShellWait wait_on(HANDLE process) {
    ShellWait wait;
    if (process == nullptr) {
        wait.error = "the program could not be started";
        return wait;
    }
    const DWORD waited = WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    const BOOL read = GetExitCodeProcess(process, &code);
    CloseHandle(process);
    if (waited != WAIT_OBJECT_0 || read == FALSE || code == STILL_ACTIVE) {
        wait.error = "the program could not be started";
        return wait;
    }
    wait.started = true;
    wait.exit_code = code;
    return wait;
}

ShellWait
shell_a(const std::wstring& verb, const std::wstring& file, const std::wstring& parameters) {
    const std::string verb_a = acp_from_wide(verb);
    const std::string file_a = acp_from_wide(file);
    const std::string parameters_a = acp_from_wide(parameters);
    SHELLEXECUTEINFOA info{};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = verb_a.c_str();
    info.lpFile = file_a.c_str();
    info.lpParameters = parameters_a.empty() ? nullptr : parameters_a.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExA(&info) == FALSE) {
        ShellWait wait;
        if (GetLastError() == ERROR_CANCELLED)
            wait.cancelled = true;
        else
            wait.error = "the program could not be started";
        return wait;
    }
    return wait_on(info.hProcess);
}

ShellWait
shell_w(const std::wstring& verb, const std::wstring& file, const std::wstring& parameters) {
    SHELLEXECUTEINFOW info{};
    info.cbSize = static_cast<DWORD>(sizeof(info));
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = verb.c_str();
    info.lpFile = file.c_str();
    info.lpParameters = parameters.empty() ? nullptr : parameters.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&info) == FALSE) {
        const DWORD code = GetLastError();
        if (not_implemented(code))
            return shell_a(verb, file, parameters);
        ShellWait wait;
        if (code == ERROR_CANCELLED)
            wait.cancelled = true;
        else
            wait.error = "the program could not be started";
        return wait;
    }
    return wait_on(info.hProcess);
}

std::string quote_argument(std::string_view argument) {
    if (argument.find_first_of(" \t\"") == std::string_view::npos)
        return std::string(argument);
    std::string quoted;
    quoted.push_back('"');
    for (const char character : argument) {
        if (character == '"')
            quoted.push_back('\\');
        quoted.push_back(character);
    }
    quoted.push_back('"');
    return quoted;
}

std::string join_plain(const std::vector<std::string>& arguments) {
    std::string line;
    for (const std::string& argument : arguments) {
        if (!line.empty())
            line.push_back(' ');
        line += argument;
    }
    return line;
}

std::string join_quoted(const std::vector<std::string>& arguments) {
    std::string line;
    for (const std::string& argument : arguments) {
        if (!line.empty())
            line.push_back(' ');
        line += quote_argument(argument);
    }
    return line;
}

std::wstring verb_wide(std::string_view verb) {
    return verb == "runas" ? std::wstring(L"runas") : std::wstring(L"open");
}

std::string short_ansi(const fs::path& path) {
    const std::wstring wide = path.wstring();
    wchar_t buffer[MAX_PATH];
    DWORD length = GetShortPathNameW(wide.c_str(), buffer, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return acp_from_wide_z(buffer);
    const std::string narrow = path.string();
    char ansi[MAX_PATH];
    length = GetShortPathNameA(narrow.c_str(), ansi, MAX_PATH);
    if (length > 0 && length < MAX_PATH)
        return std::string(ansi);
    return {};
}

// A file that is not there yet has no short name. The parent's short name
// plus the file's own name is what WININIT.INI can store.
std::string short_or_parent(const fs::path& path) {
    const std::string direct = short_ansi(path);
    if (!direct.empty())
        return direct;
    const std::string parent = short_ansi(path.parent_path());
    if (parent.empty())
        return {};
    std::string joined = parent;
    if (joined.back() != '\\')
        joined.push_back('\\');
    joined += path.filename().string();
    return joined;
}

std::string wininit_file() {
    char directory[MAX_PATH];
    const UINT length = GetWindowsDirectoryA(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::string(directory) + "\\WININIT.INI";
}

bool profile_rename(const std::string& destination, const char* source) {
    const std::string ini = wininit_file();
    if (ini.empty())
        return false;
    const BOOL wrote =
        WritePrivateProfileStringA("rename", destination.c_str(), source, ini.c_str());
    return wrote != FALSE;
}

void forget_profile_keys(const std::vector<std::string>& keys) {
    for (const std::string& key : keys)
        static_cast<void>(profile_rename(key, nullptr));
    const std::string ini = wininit_file();
    if (!ini.empty())
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini.c_str());
}

bool commit_wininit(
    const std::vector<PostponedRename>& rows,
    std::string_view version,
    const fs::path& updates,
    std::string* error
) {
    std::vector<PostponedRename> shorts;
    shorts.reserve(rows.size());
    for (const PostponedRename& row : rows) {
        PostponedRename short_row;
        short_row.from = short_or_parent(path_from(row.from));
        short_row.to = short_or_parent(path_from(row.to));
        if (short_row.from.empty() || short_row.to.empty()) {
            set_error(error, "a short name could not be made");
            return false;
        }
        shorts.push_back(std::move(short_row));
    }
    std::vector<std::string> written;
    for (const PostponedRename& row : shorts) {
        if (!profile_rename(row.to, row.from.c_str())) {
            forget_profile_keys(written);
            set_error(error, "WININIT.INI could not be written");
            return false;
        }
        written.push_back(row.to);
    }
    const std::string ini = wininit_file();
    if (!ini.empty())
        WritePrivateProfileStringA(nullptr, nullptr, nullptr, ini.c_str());
    if (!write_bytes(updates / reboot_marker_name, version, error)) {
        forget_profile_keys(written);
        return false;
    }
    return true;
}

void clear_postponed_wininit(const fs::path& updates) {
    const std::vector<PostponedRenameRecord> rows = postponed_renames(updates);
    std::vector<std::string> keys;
    for (const PostponedRenameRecord& row : rows) {
        const std::string destination = short_or_parent(path_from(row.to));
        if (!destination.empty())
            keys.push_back(destination);
    }
    forget_profile_keys(keys);
}

#else

void clear_postponed_wininit(const fs::path&) {
}

#endif

} // namespace

bool windows_9x(uint32_t version) noexcept {
    return (version & 0x80000000u) != 0u;
}

bool windows_vista_or_later(uint32_t version) noexcept {
    return (version & 0x80000000u) == 0u && (version & 0xffu) >= 6u;
}

bool folder_needs_elevation(bool writable, uint32_t version) noexcept {
    return !writable && windows_vista_or_later(version) && !windows_9x(version);
}

std::vector<std::string> setup_arguments(std::string_view install_folder) {
    // NSIS reads /D= as the remainder of the line, so the path is not quoted.
    return {"/S", std::string("/D=") + folder_text(install_folder)};
}

std::string wininit_rename_text(std::span<const PostponedRename> rows) {
    std::string text = "[rename]\r\n";
    for (const PostponedRename& row : rows) {
        text += row.to;
        text.push_back('=');
        text += row.from;
        text += "\r\n";
    }
    return text;
}

std::string restart_line(std::string_view version) {
    std::string text(restart_to_finish);
    const std::string::size_type at = text.find("{version}");
    if (at != std::string::npos)
        text.replace(at, std::string("{version}").size(), version);
    return text;
}

MoveAttempt move_with_retries(
    MoveFn move,
    PauseFn pause,
    void* context,
    bool on_windows_9x,
    const fs::path& from,
    const fs::path& to
) {
    MoveAttempt attempt;
    if (move == nullptr) {
        attempt.error = 1;
        return attempt;
    }
    for (int try_index = 0; try_index < move_tries; ++try_index) {
        const int code = move(context, from, to);
        attempt.tries += 1;
        if (code == 0) {
            attempt.moved = true;
            attempt.error = 0;
            return attempt;
        }
        attempt.error = code;
        if (code != file_held)
            return attempt;
        if (try_index + 1 == move_tries)
            break;
        attempt.pauses_ms.push_back(move_pause_ms);
        if (pause != nullptr)
            pause(context, move_pause_ms);
    }
    if (on_windows_9x && attempt.error == file_held)
        attempt.postponed = true;
    return attempt;
}

MoveOutcome classify_move(const MoveAttempt& attempt, bool on_windows_9x) noexcept {
    if (attempt.moved)
        return MoveOutcome::moved;
    if (attempt.postponed)
        return MoveOutcome::postponed;
    if (on_windows_9x && attempt.error == destination_exists)
        return MoveOutcome::postponed;
    return MoveOutcome::failed;
}

std::vector<Operation>
windows_installer_operations(const update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    const fs::path install = path_from(plan.install);
    std::error_code io;
    if (!fs::is_directory(install, io) || io) {
        set_error(why, "the install folder is missing");
        return {};
    }
    fs::path top;
    bool listed = false;
    if (!staged_top(staged, top, listed, why))
        return {};

    const fs::path previous = path_from(plan.updates) / previous_name;
    std::vector<Operation> operations;
    // preferences.conf is replaced by the bookkeeping step, not here.
    if (!empty_previous(previous, true, operations, why))
        return {};

    std::vector<fs::path> names;
    if (listed) {
        if (!list_children(top, names, why))
            return {};
    } else if (!list_children(install, names, why)) {
        return {};
    }
    for (const fs::path& child : names) {
        const fs::path name = child.filename();
        if (same_name(name, previous_name) || same_name(name, scratch_name))
            continue;
        if (listed && same_name(name, uninstall_name))
            continue;
        const fs::path source = listed ? install / name : child;
        if (listed && (!fs::exists(source, io) || io))
            continue;
        push_transfer(operations, Operation::Kind::copy_tree, source, previous / name);
    }

    Operation setup;
    setup.kind = Operation::Kind::run_and_wait;
    setup.program = plan.file;
    setup.arguments = setup_arguments(plan.install);
    operations.push_back(std::move(setup));
    return operations;
}

std::vector<Operation>
windows_folder_operations(const update::Plan& plan, const fs::path& staged, std::string* why) {
    if (why != nullptr)
        why->clear();
    const fs::path install = path_from(plan.install);
    std::error_code io;
    if (!fs::is_directory(install, io) || io) {
        set_error(why, "the install folder is missing");
        return {};
    }
    fs::path top;
    bool listed = false;
    if (!staged_top(staged, top, listed, why))
        return {};
    if (!listed) {
        set_error(why, "the staged package is not one folder");
        return {};
    }
    const fs::path previous = install / previous_name;
    std::vector<Operation> operations;
    if (!empty_previous(previous, false, operations, why))
        return {};
    std::vector<fs::path> names;
    if (!list_children(top, names, why))
        return {};
    for (const fs::path& child : names) {
        const fs::path name = child.filename();
        if (same_name(name, previous_name) || same_name(name, scratch_name))
            continue;
        const fs::path source = install / name;
        if (!fs::exists(source, io) || io)
            continue;
        push_transfer(operations, Operation::Kind::rename, source, previous / name);
    }
    for (const fs::path& child : names) {
        const fs::path name = child.filename();
        if (same_name(name, previous_name) || same_name(name, scratch_name))
            continue;
        push_transfer(operations, Operation::Kind::rename, child, install / name);
    }
    return operations;
}

FolderPlan plan_windows_folder(
    const update::Plan& plan,
    const fs::path& staged,
    bool writable,
    uint32_t version,
    const fs::path& helper,
    const fs::path& plan_file,
    std::string* why
) {
    FolderPlan result;
    std::string local;
    std::string* reason = why != nullptr ? why : &local;
    result.operations = windows_folder_operations(plan, staged, reason);
    if (!reason->empty()) {
        result.operations.clear();
        return result;
    }
    if (folder_needs_elevation(writable, version)) {
        result.elevate = true;
        result.verb = "runas";
        result.program = utf8_of(helper);
        result.arguments = {"--elevated-swap", "--plan", utf8_of(plan_file)};
        result.operations.clear();
    }
    return result;
}

bool restore_kept_copy(const fs::path& previous, const fs::path& install, std::string* error) {
    std::error_code io;
    if (!fs::is_directory(previous, io) || io) {
        set_error(error, "the kept folder is missing");
        return false;
    }
    if (!fs::is_directory(install, io) || io) {
        set_error(error, "the install folder is missing");
        return false;
    }
    std::vector<fs::path> children;
    if (!list_children(previous, children, error))
        return false;
    for (const fs::path& child : children) {
        if (same_name(child.filename(), preferences_name))
            continue;
        if (!restore_entry(child, install / child.filename(), error))
            return false;
    }
    return true;
}

UpdatesGateResult take_pending(const fs::path& updates) {
    UpdatesGateResult result;
    std::error_code io;
    const fs::path reboot = updates / reboot_marker_name;
    if (fs::is_regular_file(reboot, io) && !io) {
        result.version = read_bytes(reboot);
        drop_journal_files(updates);
        fs::remove(reboot, io);
        result.gate = UpdatesGate::restart;
        return result;
    }
    clear_postponed_wininit(updates);
    const fs::path marker = updates / installer_marker_name;
    if (fs::is_regular_file(marker, io) && !io) {
        const std::string install_text = read_bytes(marker);
        std::string restore_error;
        if (!restore_kept_copy(updates / previous_name, path_from(install_text), &restore_error)) {
            result.gate = UpdatesGate::restore_failed;
            result.detail =
                restore_error.empty() ? "the kept copy could not be put back" : restore_error;
            return result;
        }
        fs::remove(marker, io);
    }
    return result;
}

namespace {

int refuse_windows(
    const update::Plan& plan,
    Log& log,
    ProgressWindow& window,
    bool elevated_swap,
    std::string_view line,
    int code
) {
    log.line(line);
    window.show(plan.title, line, 0, true);
    if (!elevated_swap) {
        std::string error;
        if (!start_detached(path_from(plan.program), plan.arguments, false, {}, &error))
            log.line(std::string("could not start the game: ") + error);
    }
    window.close();
    return code;
}

#if defined(_WIN32)
bool undo_or_log(const fs::path& updates, const JournalHooks& hooks, Log& log) {
    std::string error;
    if (!undo_journal(updates, hooks, &error)) {
        if (!error.empty())
            log.line(error);
        return false;
    }
    return true;
}
#endif

} // namespace

int finish_windows_route(
    const update::Plan& plan,
    const Verified& verified,
    const fs::path& plan_path,
    const fs::path& stage,
    bool elevated_swap,
    Log& log,
    ProgressWindow& window,
    JournalHooks& hooks
) {
#if !defined(_WIN32)
    (void)verified;
    (void)plan_path;
    (void)stage;
    (void)hooks;
    return refuse_windows(
        plan, log, window, elevated_swap, "this route is not carried out", exit_usage
    );
#else
    const fs::path updates = path_from(plan.updates);
    const fs::path install = path_from(plan.install);
    const bool installer = plan.route == update::Route::windows_installer;
    std::error_code io;

    auto failed = [&](std::string_view line, int code) {
        return refuse_windows(plan, log, window, elevated_swap, line, code);
    };

    if (installer) {
        std::string why;
        std::vector<Operation> operations = windows_installer_operations(plan, {}, &why);
        if (!why.empty() || operations.empty() ||
            operations.back().kind != Operation::Kind::run_and_wait) {
            log.line(why.empty() ? "the setup could not be prepared" : why);
            return failed(could_not_check, exit_refused);
        }
        Operation setup = std::move(operations.back());
        operations.pop_back();
        const fs::path kept = updates / previous_name;
        add_bookkeeping(operations, plan, verified, updates, kept);
        const fs::path marker = updates / installer_marker_name;
        const ExeStamp before = stamp_exe(install / game_name);
        std::string error;
        if (apply_journal(updates, operations, hooks, &error) != JournalResult::finished) {
            log.line(error.empty() ? "a step failed" : error);
            return failed(could_not_check, exit_undone);
        }
        if (!write_bytes(marker, plan.install, &error)) {
            log.line(error);
            undo_or_log(updates, hooks, log);
            return failed(could_not_check, exit_undone);
        }

        const uint32_t version = current_windows_version();
        const std::wstring verb = verb_wide(windows_vista_or_later(version) ? "runas" : "open");
        const std::wstring file = path_from(setup.program).wstring();
        const std::wstring parameters = wide_from_utf8(join_plain(setup.arguments));
        const ShellWait ran = file.empty() || (parameters.empty() && !setup.arguments.empty())
                                  ? ShellWait{}
                                  : shell_w(verb, file, parameters);
        if (ran.cancelled) {
            fs::remove(marker, io);
            undo_or_log(updates, hooks, log);
            return failed(update_was_cancelled, exit_refused);
        }

        auto put_back = [&]() {
            std::string restore_error;
            if (!restore_kept_copy(kept, install, &restore_error)) {
                log.line(
                    restore_error.empty() ? "the kept copy could not be put back" : restore_error
                );
                return false;
            }
            fs::remove(marker, io);
            undo_or_log(updates, hooks, log);
            return true;
        };

        const bool setup_ok = ran.started && ran.exit_code == 0 &&
                              exe_changed(before, stamp_exe(install / game_name));
        if (!setup_ok) {
            if (!ran.error.empty())
                log.line(ran.error);
            else if (ran.started && ran.exit_code != 0)
                log.line("the setup did not finish");
            else
                log.line("the setup did not replace the game");
            static_cast<void>(put_back());
            return failed(could_not_check, exit_undone);
        }

        fs::remove(marker, io);
        if (!elevated_swap &&
            !start_detached(
                path_from(plan.program), plan.arguments, true, plan.from_version, &error
            )) {
            log.line(error.empty() ? "the game could not be started" : error);
            static_cast<void>(put_back());
            return failed(could_not_check, exit_undone);
        }
        drop_journal_files(updates);
        log.line(elevated_swap ? "swapped the install" : "started the new version");
        window.close();
        return exit_swapped;
    }

    const uint32_t version = current_windows_version();
    const bool writable = folder_accepts_file(install);
    std::string why;
#if defined(_WIN32)
    const fs::path helper = this_program();
#else
    const fs::path helper;
#endif
    const FolderPlan folder =
        plan_windows_folder(plan, stage, writable, version, helper, plan_path, &why);
    if (!why.empty()) {
        log.line(why);
        remove_stage(stage);
        return failed(could_not_check, exit_refused);
    }
    if (folder.elevate && !elevated_swap) {
        const std::wstring file = path_from(folder.program).wstring();
        const std::wstring parameters = wide_from_utf8(join_quoted(folder.arguments));
        const ShellWait ran =
            file.empty() ? ShellWait{} : shell_w(verb_wide(folder.verb), file, parameters);
        if (ran.cancelled)
            return failed(update_was_cancelled, exit_refused);
        if (!ran.started || ran.exit_code != 0) {
            if (!ran.error.empty())
                log.line(ran.error);
            else
                log.line("the elevated swap did not finish");
            undo_or_log(updates, hooks, log);
            remove_stage(stage);
            return failed(could_not_check, exit_undone);
        }
        std::string error;
        if (!start_detached(
                path_from(plan.program), plan.arguments, true, plan.from_version, &error
            )) {
            log.line(error.empty() ? "the game could not be started" : error);
            undo_or_log(updates, hooks, log);
            remove_stage(stage);
            return failed(could_not_check, exit_undone);
        }
        drop_journal_files(updates);
        remove_stage(stage);
        log.line("started the new version");
        window.close();
        return exit_swapped;
    }

    fs::create_directories(install / previous_name, io);
    if (io) {
        log.line("the kept folder could not be made");
        remove_stage(stage);
        return failed(could_not_check, exit_refused);
    }
    std::vector<Operation> operations = folder.operations;
    if (folder.elevate) {
        // The elevated copy does the moves. It does not ask to elevate again.
        operations = windows_folder_operations(plan, stage, &why);
        if (!why.empty()) {
            log.line(why);
            remove_stage(stage);
            return failed(could_not_check, exit_refused);
        }
    }
    add_bookkeeping(operations, plan, verified, updates, install / previous_name);

    MoveState moving;
    moving.saved = hooks;
    moving.on_9x = windows_9x(version);
    hooks.context = &moving;
    hooks.note = chained_note;
    hooks.move = windows_move;
    std::string error;
    const JournalResult applied = apply_journal(updates, operations, hooks, &error);
    hooks = moving.saved;
    if (applied != JournalResult::finished) {
        log.line(error.empty() ? "a step failed" : error);
        remove_stage(stage);
        return failed(could_not_check, exit_undone);
    }
    if (!moving.postponed.empty()) {
        if (elevated_swap || !commit_wininit(moving.postponed, verified.version, updates, &error)) {
            if (!error.empty())
                log.line(error);
            undo_or_log(updates, hooks, log);
            remove_stage(stage);
            return failed(could_not_check, exit_undone);
        }
        drop_journal_files(updates);
        fs::remove(updates / reboot_marker_name, io);
        window.show(plan.title, restart_line(verified.version), 0, true);
        window.close();
        return exit_swapped;
    }
    if (!elevated_swap &&
        !start_detached(path_from(plan.program), plan.arguments, true, plan.from_version, &error)) {
        log.line(error.empty() ? "the game could not be started" : error);
        undo_or_log(updates, hooks, log);
        remove_stage(stage);
        return failed(could_not_check, exit_undone);
    }
    if (!elevated_swap) {
        drop_journal_files(updates);
        remove_stage(stage);
        log.line("started the new version");
    } else {
        log.line("swapped the install");
    }
    window.close();
    return exit_swapped;
#endif
}

} // namespace oa::update_helper
