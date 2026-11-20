// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The index pass the Library and the playback bar both start. See
// replay_index_pass.hpp.

#include "replay_index_pass.hpp"

#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <SDL3/SDL.h>

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether the game may start other programs"
#endif

#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>
#endif
#endif

#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Lowers a process's priority. A pid of 0 is this process, and is left alone.
///
/// @param pid the process
void lower_index_priority(int pid) {
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    if (pid <= 0)
        return;
#if defined(_WIN32)
    const HANDLE handle = OpenProcess(PROCESS_SET_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return;
    std::ignore = SetPriorityClass(handle, BELOW_NORMAL_PRIORITY_CLASS);
    CloseHandle(handle);
#else
    std::ignore = setpriority(PRIO_PROCESS, pid, 10);
#endif
#else
    std::ignore = pid;
#endif
}

/// Makes the pass's standard output a read that returns at once.
///
/// SDL opens that pipe without blocking. This asks again, so a line the pass
/// has not written yet cannot stop the game.
///
/// @param process the process
void unblock_index_stdout(SDL_Process* process) {
    if (process == nullptr)
        return;
    SDL_IOStream* output = SDL_GetProcessOutput(process);
    if (output == nullptr)
        return;
    const SDL_PropertiesID properties = SDL_GetIOProperties(output);
    if (properties == 0)
        return;
#if defined(_WIN32) && !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    auto* pipe = static_cast<HANDLE>(
        SDL_GetPointerProperty(properties, SDL_PROP_IOSTREAM_WINDOWS_HANDLE_POINTER, nullptr)
    );
    if (pipe == nullptr)
        return;
    DWORD mode = PIPE_NOWAIT;
    std::ignore = SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
#elif !defined(_WIN32) && !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    const auto descriptor = static_cast<int>(
        SDL_GetNumberProperty(properties, SDL_PROP_IOSTREAM_FILE_DESCRIPTOR_NUMBER, -1)
    );
    if (descriptor < 0)
        return;
    const int flags = fcntl(descriptor, F_GETFL);
    if (flags < 0)
        return;
    std::ignore = fcntl(descriptor, F_SETFL, flags | O_NONBLOCK);
#else
    std::ignore = properties;
#endif
}

/// Reads a whole file. Empty when it cannot be read.
///
/// @param file the file
/// @return the bytes as text
std::string read_pass_file(const fs::path& file) {
    if (file.empty())
        return {};
    std::ifstream stream(file, std::ios::binary);
    if (!stream)
        return {};
    std::string text;
    stream.seekg(0, std::ios::end);
    const std::streamoff size = stream.tellg();
    if (size <= 0)
        return {};
    text.resize(static_cast<std::size_t>(size));
    stream.seekg(0, std::ios::beg);
    stream.read(text.data(), size);
    if (!stream)
        return {};
    return text;
}

} // namespace

int index_process_pid(SDL_Process* process) {
    if (process == nullptr)
        return 0;
    const SDL_PropertiesID properties = SDL_GetProcessProperties(process);
    if (properties == 0)
        return 0;
    const Sint64 value = SDL_GetNumberProperty(properties, SDL_PROP_PROCESS_PID_NUMBER, 0);
    if (value <= 0 || value > 2147483647)
        return 0;
    return static_cast<int>(value);
}

std::string index_pass_reason(const fs::path& file) {
    const std::string text = read_pass_file(file);
    std::string reason;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::size_t stop = end == std::string::npos ? text.size() : end;
        std::string_view line(text.data() + at, stop - at);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        constexpr std::string_view mark = "replay index: ";
        const std::size_t found = line.find(mark);
        if (found != std::string_view::npos) {
            const std::string_view tail = line.substr(found + mark.size());
            const bool progress = tail.starts_with("replaying ") || tail.starts_with("replayed ") ||
                                  tail.starts_with("wrote ") || tail.starts_with("progress ");
            if (!progress && !tail.empty())
                reason = std::string(tail);
        }
        if (end == std::string::npos)
            break;
        at = end + 1;
    }
    return reason;
}

IndexPassHandles launch_index_pass(const IndexPassRequest& request) {
    IndexPassHandles handles;
#if defined(__ANDROID__) || !OA_PROCESS_SPAWNING
    std::ignore = request;
    return handles;
#else
    const std::string executable = oa::platform::program_executable();
    if (executable.empty() || request.recording.empty() || request.scratch.empty())
        return handles;
    std::error_code failed;
    fs::create_directories(request.scratch, failed);
    if (failed)
        return handles;
    const fs::path output = request.scratch / request.output_name;
    fs::remove(output, failed);
    const fs::path errors = request.scratch / "errors.txt";
    fs::remove(errors, failed);
    // An empty file is not a preferences file: the pass would stop before it
    // read the replay. The file names the format and asks for no update, so
    // the pass does not phone home. It is not the player's own preferences.
    const fs::path preferences = request.scratch / "preferences.txt";
    {
        std::ofstream made(preferences, std::ios::binary | std::ios::trunc);
        made << "open-annihilation-preferences 1\n"
                "\"open-annihilation.update-check\" \"never\"\n"
                "\"open-annihilation.content-updates\" \"never\"\n";
        if (!made)
            return handles;
    }
    std::vector<std::string> arguments;
    arguments.push_back(executable);
    arguments.push_back("--replay-index");
    arguments.push_back(oa::platform::path_to_utf8(request.recording));
    arguments.push_back("--output");
    arguments.push_back(oa::platform::path_to_utf8(output));
    if (!request.game_dir.empty()) {
        arguments.push_back("--game-dir");
        arguments.push_back(oa::platform::path_to_utf8(request.game_dir));
    }
    if (!request.mod_folder.empty()) {
        arguments.push_back("--mod-dir");
        arguments.push_back(oa::platform::path_to_utf8(request.mod_folder));
    }
    if (!request.map_pack.empty()) {
        arguments.push_back("--replay-map-pack");
        arguments.push_back(oa::platform::path_to_utf8(request.map_pack));
    }
    if (!request.user_folder.empty()) {
        arguments.push_back("--user-folder");
        arguments.push_back(oa::platform::path_to_utf8(request.user_folder));
    }
    arguments.push_back("--preferences-file");
    arguments.push_back(oa::platform::path_to_utf8(preferences));
    arguments.push_back("--skip-intro");
    arguments.push_back("--mute");
    std::vector<const char*> argv;
    argv.reserve(arguments.size() + 1);
    for (const std::string& argument : arguments)
        argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0)
        return handles;
    const int stdout_kind = request.pipe_stdout ? SDL_PROCESS_STDIO_APP : SDL_PROCESS_STDIO_NULL;
    SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
    SDL_SetNumberProperty(properties, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, stdout_kind);
    const std::string errors_utf8 = oa::platform::path_to_utf8(errors);
    SDL_IOStream* errors_stream = SDL_IOFromFile(errors_utf8.c_str(), "wb");
    if (errors_stream != nullptr) {
        SDL_SetNumberProperty(
            properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_REDIRECT
        );
        SDL_SetPointerProperty(properties, SDL_PROP_PROCESS_CREATE_STDERR_POINTER, errors_stream);
    } else {
        SDL_SetNumberProperty(
            properties, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL
        );
    }
    SDL_Process* process = SDL_CreateProcessWithProperties(properties);
    if (process == nullptr && errors_stream != nullptr) {
        SDL_DestroyProperties(properties);
        const SDL_PropertiesID again = SDL_CreateProperties();
        if (again == 0) {
            SDL_CloseIO(errors_stream);
            return handles;
        }
        SDL_SetPointerProperty(again, SDL_PROP_PROCESS_CREATE_ARGS_POINTER, argv.data());
        SDL_SetNumberProperty(again, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER, SDL_PROCESS_STDIO_NULL);
        SDL_SetNumberProperty(again, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER, stdout_kind);
        SDL_SetNumberProperty(again, SDL_PROP_PROCESS_CREATE_STDERR_NUMBER, SDL_PROCESS_STDIO_NULL);
        process = SDL_CreateProcessWithProperties(again);
        SDL_DestroyProperties(again);
    } else {
        SDL_DestroyProperties(properties);
    }
    if (errors_stream != nullptr)
        SDL_CloseIO(errors_stream);
    if (process == nullptr)
        return handles;
    if (request.pipe_stdout)
        unblock_index_stdout(process);
    handles.process = process;
    handles.pid = index_process_pid(process);
    handles.output = output;
    handles.errors = errors_stream != nullptr ? errors : fs::path{};
    handles.started = true;
    lower_index_priority(handles.pid);
    return handles;
#endif
}

} // namespace oa::app
