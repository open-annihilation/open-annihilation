// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the game's fonts travel: the folder SDL_GetBasePath() names for the
// game's other files, found here from the executable's own path so that the
// module needs no SDL: the bundle's Contents/Resources on macOS, the
// executable's folder elsewhere.

#include "oa/platform/text_font.hpp"

#include <string>
#include <system_error>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <mach-o/dyld.h>

#include <cstdint>
#include <vector>
#endif

namespace oa::platform::text_font {

namespace {

/// Returns the running executable's path; empty when the system does not say.
std::filesystem::path executable_path() {
#if defined(_WIN32)
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length == 0) {
            // A Windows whose wide entry points are stubs — Windows 95's are —
            // answers nothing here, so the system-character-set form is asked
            // instead. The wide one is asked first because only it carries a
            // name outside that character set.
            std::string narrow(MAX_PATH, '\0');
            const DWORD narrow_length =
                GetModuleFileNameA(nullptr, narrow.data(), static_cast<DWORD>(narrow.size()));
            if (narrow_length == 0 || narrow_length >= narrow.size())
                return {};
            narrow.resize(narrow_length);
            return std::filesystem::path(narrow);
        }
        if (length < path.size()) {
            path.resize(length);
            return std::filesystem::path(path);
        }
        if (path.size() >= 32768)
            return {};
        path.resize(path.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size + 1U, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
        return {};
    std::error_code error;
    auto resolved = std::filesystem::canonical(std::filesystem::path(path.data()), error);
    return error ? std::filesystem::path(path.data()) : resolved;
#else
    std::error_code error;
    auto resolved = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : resolved;
#endif
}

} // namespace

std::filesystem::path bundled_font_directory() {
    const auto executable = executable_path();
    if (executable.empty())
        return {};
    auto folder = executable.parent_path();
#if defined(__APPLE__)
    // An executable in an application bundle's Contents/MacOS keeps its files
    // in the bundle's Contents/Resources.
    if (folder.filename() == "MacOS" && folder.parent_path().filename() == "Contents")
        folder = folder.parent_path() / "Resources";
#endif
    return folder / std::filesystem::path(font_folder);
}

} // namespace oa::platform::text_font
