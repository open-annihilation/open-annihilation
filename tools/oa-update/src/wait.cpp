// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/wait.hpp"

#include "oa/update_helper/paths.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <thread>
#include <system_error>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <tlhelp32.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#endif

namespace oa::update_helper {
namespace {

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

int64_t real_now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

WaitOutcome wait_with(int64_t pid, const WaitHooks& hooks, std::string* error) {
    const int64_t start = hooks.now_ms(hooks.context);
    while (true) {
        if (!hooks.alive(hooks.context, pid))
            return WaitOutcome::finished;
        const int64_t elapsed = hooks.now_ms(hooks.context) - start;
        if (elapsed >= wait_limit_ms) {
            set_error(error, std::string(still_running_text));
            return WaitOutcome::still_running;
        }
        const int64_t pause = std::min(wait_slice_ms, wait_limit_ms - elapsed);
        hooks.sleep_ms(hooks.context, pause);
    }
}

#if defined(_WIN32)
WaitOutcome wait_real(int64_t pid, std::string* error) {
    // A process id of 0 or less, or one that does not fit, is not running.
    if (pid <= 0 || static_cast<uint64_t>(pid) > MAXDWORD)
        return WaitOutcome::finished;
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return WaitOutcome::finished;
    const DWORD waited = WaitForSingleObject(handle, static_cast<DWORD>(wait_limit_ms));
    CloseHandle(handle);
    if (waited == WAIT_TIMEOUT) {
        set_error(error, std::string(still_running_text));
        return WaitOutcome::still_running;
    }
    return WaitOutcome::finished;
}
#else
bool real_alive(int64_t pid) {
    // kill with a process id of 0 or less signals a group. Never do that.
    if (pid <= 0)
        return false;
    if (pid > static_cast<int64_t>(std::numeric_limits<pid_t>::max()))
        return false;
    if (::kill(static_cast<pid_t>(pid), 0) == 0)
        return true;
    return errno == EPERM;
}

WaitOutcome wait_real(int64_t pid, std::string* error) {
    WaitHooks hooks;
    const int64_t origin = real_now_ms();
    hooks.context = nullptr;

    // The real clock is closed over by the functions below through origin.
    struct Clock {
        int64_t origin;
    };

    Clock clock{origin};
    hooks.context = &clock;
    hooks.now_ms = [](void* context) {
        (void)context;
        return real_now_ms();
    };
    hooks.sleep_ms = [](void*, int64_t milliseconds) {
        std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
    };
    hooks.alive = [](void*, int64_t process) { return real_alive(process); };
    (void)clock;
    return wait_with(pid, hooks, error);
}
#endif

} // namespace

WaitOutcome wait_for_process(int64_t pid, const WaitHooks* hooks, std::string* error) {
    if (pid == 0)
        return WaitOutcome::finished;
    if (hooks != nullptr && hooks->now_ms != nullptr && hooks->sleep_ms != nullptr &&
        hooks->alive != nullptr)
        return wait_with(pid, *hooks, error);
    return wait_real(pid, error);
}

namespace {

/// How often the wait for other copies lists the processes again, in
/// milliseconds. Each look opens every process, so it looks less often
/// than the wait for one process does.
constexpr int64_t copies_look_ms = 500;

/// A path spelled for comparison: forward slashes, no leading `//?/`, no
/// trailing separator but a root's, and ASCII letters in lower case when
/// case is ignored.
std::string comparable_path(std::string_view path, bool ignore_case) {
    std::string text(path);
    for (char& character : text) {
        const unsigned char byte = static_cast<unsigned char>(character);
        if (character == '\\')
            character = '/';
        else if (ignore_case && byte >= 'A' && byte <= 'Z')
            character = static_cast<char>(byte - 'A' + 'a');
    }
    if (text.starts_with("//?/"))
        text.erase(0, 4);
    while (text.size() > 1 && text.back() == '/')
        text.pop_back();
    return text;
}

int64_t clock_now(void*) {
    return real_now_ms();
}

void clock_sleep(void*, int64_t milliseconds) {
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
}

std::vector<RunningProgram> listed_programs(void*) {
    return running_programs();
}

#if defined(_WIN32)
/// Converts `length` wide characters to UTF-8; empty when they do not convert.
std::string utf8_from_wide(const wchar_t* wide, int length) {
    if (wide == nullptr || length <= 0)
        return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide, length, nullptr, 0, nullptr, nullptr);
    if (needed <= 0)
        return {};
    std::string text(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, wide, length, text.data(), needed, nullptr, nullptr) <= 0)
        return {};
    return text;
}

/// Converts a name in the system code page to UTF-8; empty when it does not convert.
std::string utf8_from_code_page(const char* text) {
    if (text == nullptr || text[0] == '\0')
        return {};
    const int needed = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    if (needed <= 1)
        return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), needed) <= 0)
        return {};
    return utf8_from_wide(wide.c_str(), needed - 1);
}

/// A function of kernel32 looked up by name: Windows NT 4 has no process
/// snapshot, Windows 95 has only its code-page forms, and only Vista and
/// later name a process's program from its handle. Null where it is missing.
template <typename Function>
Function kernel_function(const char* name) {
    const HMODULE kernel = GetModuleHandleA("kernel32.dll");
    if (kernel == nullptr)
        return nullptr;
    return reinterpret_cast<Function>(reinterpret_cast<void*>(GetProcAddress(kernel, name)));
}

/// The access that lets Vista and later name a process's program. Older
/// headers do not define it.
constexpr DWORD query_limited_information = 0x1000;

using SnapshotFn = HANDLE(WINAPI*)(DWORD, DWORD);
using ProcessWideFn = BOOL(WINAPI*)(HANDLE, PROCESSENTRY32W*);
using ProcessCodePageFn = BOOL(WINAPI*)(HANDLE, tagPROCESSENTRY32*);
using ModuleWideFn = BOOL(WINAPI*)(HANDLE, MODULEENTRY32W*);
using ImageNameFn = BOOL(WINAPI*)(HANDLE, DWORD, LPWSTR, PDWORD);

/// The full path of a process's program on Windows NT: from the process's
/// handle on Vista and later, else from the first module of its module
/// snapshot, which is the program. Empty when neither answers.
///
/// @param pid the process
/// @param snapshot the snapshot function
/// @param image_name the function that names a process's program; null before Vista
/// @param[in,out] wide room for the name; its size is the most characters read
/// @return the path in UTF-8
std::string
nt_program_path(DWORD pid, SnapshotFn snapshot, ImageNameFn image_name, std::wstring& wide) {
    if (image_name != nullptr) {
        HANDLE process = OpenProcess(query_limited_information, FALSE, pid);
        if (process != nullptr) {
            DWORD length = static_cast<DWORD>(wide.size());
            const BOOL named = image_name(process, 0, wide.data(), &length);
            CloseHandle(process);
            if (named != FALSE && length > 0)
                return utf8_from_wide(wide.c_str(), static_cast<int>(length));
        }
    }
    const auto first_module = kernel_function<ModuleWideFn>("Module32FirstW");
    if (snapshot == nullptr || first_module == nullptr)
        return {};
    HANDLE modules = snapshot(TH32CS_SNAPMODULE, pid);
    if (modules == INVALID_HANDLE_VALUE)
        return {};
    MODULEENTRY32W module{};
    module.dwSize = static_cast<DWORD>(sizeof(module));
    std::string path;
    if (first_module(modules, &module) != FALSE)
        path = utf8_from_wide(module.szExePath, static_cast<int>(wcslen(module.szExePath)));
    CloseHandle(modules);
    return path;
}
#endif

} // namespace

std::vector<int64_t> copies_in_install(
    std::span<const RunningProgram> running,
    std::string_view install,
    int64_t own_pid,
    bool ignore_case
) {
    std::vector<int64_t> copies;
    std::string folder = comparable_path(install, ignore_case);
    if (folder.empty())
        return copies;
    if (folder.back() != '/')
        folder.push_back('/');
    for (const RunningProgram& process : running) {
        if (process.pid <= 0 || process.pid == own_pid || process.program.empty())
            continue;
        const std::string program = comparable_path(process.program, ignore_case);
        if (program.size() > folder.size() && program.starts_with(folder))
            copies.push_back(process.pid);
    }
    return copies;
}

std::vector<RunningProgram> running_programs() {
    std::vector<RunningProgram> programs;
#if defined(_WIN32)
    const auto snapshot = kernel_function<SnapshotFn>("CreateToolhelp32Snapshot");
    if (snapshot == nullptr)
        return programs;
    HANDLE processes = snapshot(TH32CS_SNAPPROCESS, 0);
    if (processes == INVALID_HANDLE_VALUE)
        return programs;
    const auto first_wide = kernel_function<ProcessWideFn>("Process32FirstW");
    const auto next_wide = kernel_function<ProcessWideFn>("Process32NextW");
    if (first_wide != nullptr && next_wide != nullptr) {
        const auto image_name = kernel_function<ImageNameFn>("QueryFullProcessImageNameW");
        // The longest path Windows names, read into one buffer for every process.
        std::wstring wide(32768, L'\0');
        PROCESSENTRY32W entry{};
        entry.dwSize = static_cast<DWORD>(sizeof(entry));
        for (BOOL more = first_wide(processes, &entry); more != FALSE;
             more = next_wide(processes, &entry)) {
            RunningProgram running;
            running.pid = static_cast<int64_t>(entry.th32ProcessID);
            running.program = nt_program_path(entry.th32ProcessID, snapshot, image_name, wide);
            programs.push_back(std::move(running));
        }
    } else {
        // Windows 95, 98 and Me: the entry holds the program's full path.
        const auto first_code_page = kernel_function<ProcessCodePageFn>("Process32First");
        const auto next_code_page = kernel_function<ProcessCodePageFn>("Process32Next");
        if (first_code_page != nullptr && next_code_page != nullptr) {
            tagPROCESSENTRY32 entry{};
            entry.dwSize = static_cast<DWORD>(sizeof(entry));
            for (BOOL more = first_code_page(processes, &entry); more != FALSE;
                 more = next_code_page(processes, &entry)) {
                RunningProgram running;
                running.pid = static_cast<int64_t>(entry.th32ProcessID);
                running.program = utf8_from_code_page(entry.szExeFile);
                programs.push_back(std::move(running));
            }
        }
    }
    CloseHandle(processes);
#elif defined(__APPLE__)
    const int count = proc_listallpids(nullptr, 0);
    if (count <= 0)
        return programs;
    // Room for processes started between the two calls.
    std::vector<pid_t> pids(static_cast<std::size_t>(count) + 64);
    const int listed = proc_listallpids(pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    if (listed <= 0)
        return programs;
    pids.resize(std::min(pids.size(), static_cast<std::size_t>(listed)));
    for (const pid_t pid : pids) {
        RunningProgram running;
        running.pid = static_cast<int64_t>(pid);
        char path[PROC_PIDPATHINFO_MAXSIZE];
        const int length = proc_pidpath(pid, path, sizeof(path));
        if (length > 0)
            running.program.assign(path, static_cast<std::size_t>(length));
        programs.push_back(std::move(running));
    }
#elif defined(__linux__)
    std::error_code io;
    std::filesystem::directory_iterator it("/proc", io);
    for (; !io && it != std::filesystem::directory_iterator(); it.increment(io)) {
        const std::string name = utf8_of(it->path().filename());
        int64_t pid = 0;
        const char* const end = name.data() + name.size();
        const std::from_chars_result parsed = std::from_chars(name.data(), end, pid);
        if (name.empty() || parsed.ec != std::errc{} || parsed.ptr != end || pid <= 0)
            continue;
        RunningProgram running;
        running.pid = pid;
        std::error_code link;
        const std::filesystem::path program =
            std::filesystem::read_symlink(it->path() / "exe", link);
        if (!link)
            running.program = utf8_of(program);
        programs.push_back(std::move(running));
    }
#endif
    return programs;
}

WaitOutcome wait_for_other_copies(
    std::string_view install, int64_t own_pid, const CopiesHooks* hooks, std::string* error
) {
    CopiesHooks used;
    if (hooks != nullptr)
        used = *hooks;
    if (used.now_ms == nullptr)
        used.now_ms = clock_now;
    if (used.sleep_ms == nullptr)
        used.sleep_ms = clock_sleep;
    if (used.running == nullptr)
        used.running = listed_programs;
    const int64_t start = used.now_ms(used.context);
    while (true) {
        const std::vector<RunningProgram> running = used.running(used.context);
        if (copies_in_install(running, install, own_pid, program_paths_ignore_case).empty())
            return WaitOutcome::finished;
        const int64_t elapsed = used.now_ms(used.context) - start;
        if (elapsed >= wait_limit_ms) {
            set_error(error, std::string(other_windows_open_text));
            return WaitOutcome::still_running;
        }
        if (used.waiting != nullptr)
            used.waiting(used.context);
        used.sleep_ms(used.context, std::min(copies_look_ms, wait_limit_ms - elapsed));
    }
}

} // namespace oa::update_helper
