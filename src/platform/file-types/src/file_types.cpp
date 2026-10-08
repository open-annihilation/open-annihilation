// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The texts a registration writes, the running program's path, and the choice of how this
// system registers.

#include "oa/platform/file_types.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <exception>
#include <string>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

namespace oa::platform::file_types {

namespace fs = std::filesystem;

namespace {

/// The most UTF-16 units a Windows path may have.
[[maybe_unused]] constexpr std::size_t longest_windows_path = 32767;
/// The most bytes a Linux path may have, its terminating zero included.
[[maybe_unused]] constexpr std::size_t longest_linux_path = 4096;
/// What Linux appends to /proc/self/exe once the executable was deleted or replaced.
[[maybe_unused]] constexpr std::string_view deleted_mark = " (deleted)";

/// Writes `text` as a value of a desktop entry's string type: each backslash doubled. The
/// caller has refused control characters, the only others the type escapes; a path starts
/// with '/', so no leading space needs escaping.
///
/// @param text the value, UTF-8
/// @return the value as written in the entry
std::string desktop_string(std::string_view text) {
    std::string written;
    written.reserve(text.size());
    for (const char character : text) {
        if (character == '\\')
            written += '\\';
        written += character;
    }
    return written;
}

} // namespace

std::string desktop_exec_argument(std::string_view path_utf8) {
    if (!detail::is_utf8(path_utf8) || detail::holds_control_character(path_utf8))
        return {};
    // The quoting rule first: ", `, $ and \ each take a backslash inside the quotes.
    std::string quoted = "\"";
    for (const char character : path_utf8) {
        if (character == '"' || character == '`' || character == '$' || character == '\\')
            quoted += '\\';
        quoted += character;
    }
    quoted += '"';
    // Then the string type's rule doubles every backslash, and a literal % is %%.
    std::string written;
    written.reserve(quoted.size() + 8);
    for (const char character : quoted) {
        if (character == '\\')
            written += "\\\\";
        else if (character == '%')
            written += "%%";
        else
            written += character;
    }
    return written;
}

std::string desktop_entry(const fs::path& executable) {
    const std::string path = detail::utf8_of(executable);
    const std::string argument = desktop_exec_argument(path);
    if (argument.empty())
        return {};
    std::string entry;
    entry += "[Desktop Entry]\n";
    entry += "Type=Application\n";
    entry += "Name=Open Annihilation\n";
    entry += "Comment=Plays Total Annihilation from your own game files\n";
    // %f: the file opened. Started with no file, the entry drops it and the game starts as
    // usual; a bare argument ending in .oamod installs that file.
    entry += "Exec=" + argument + " %f\n";
    // The entry is passed over once the executable is gone.
    entry += "TryExec=" + desktop_string(path) + "\n";
    entry += "Icon=" + std::string(desktop_id) + "\n";
    entry += "Terminal=false\n";
    entry += "Categories=Game;StrategyGame;\n";
    entry += "MimeType=" + std::string(mod_mime_type) + ";\n";
    // An opener of .oamod files that "Open With" lists, not an entry in the application menus.
    entry += "NoDisplay=true\n";
    return entry;
}

std::string mime_package() {
    std::string package;
    package += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    package += "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
    package += "  <mime-type type=\"" + std::string(mod_mime_type) + "\">\n";
    package += "    <comment>" + std::string(mod_type_name) + "</comment>\n";
    package += "    <sub-class-of type=\"application/zip\"/>\n";
    package += "    <glob pattern=\"*." + std::string(mod_extension) + "\"/>\n";
    package += "  </mime-type>\n";
    package += "</mime-info>\n";
    return package;
}

std::string open_command(const fs::path& executable) {
    return "\"" + detail::utf8_of(executable) + "\" " + std::string(install_option) + " \"%1\"";
}

std::string default_icon(const fs::path& executable) {
    return detail::utf8_of(executable) + ",0";
}

std::optional<fs::path> running_executable() {
    try {
#ifdef _WIN32
        std::wstring name(MAX_PATH, L'\0');
        while (true) {
            const DWORD length =
                GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
            if (length == 0) {
                // A Windows whose wide entry points are stubs — Windows 95's
                // are — answers nothing here, so the system-character-set form
                // is asked instead. The wide one goes first because only it
                // carries a name outside that character set.
                std::string narrow(MAX_PATH, '\0');
                const DWORD narrow_length =
                    GetModuleFileNameA(nullptr, narrow.data(), static_cast<DWORD>(narrow.size()));
                if (narrow_length == 0 || narrow_length >= narrow.size())
                    return std::nullopt;
                narrow.resize(narrow_length);
                return fs::path(narrow);
            }
            // A name that fills the buffer may have been cut short.
            if (length < name.size()) {
                name.resize(length);
                return fs::path(name);
            }
            if (name.size() >= longest_windows_path)
                return std::nullopt;
            name.resize(std::min(name.size() * 2, longest_windows_path));
        }
#elif defined(__linux__)
        std::string link(longest_linux_path, '\0');
        const ssize_t length = readlink("/proc/self/exe", link.data(), link.size());
        if (length <= 0 || static_cast<std::size_t>(length) >= link.size())
            return std::nullopt;
        link.resize(static_cast<std::size_t>(length));
        if (link.size() >= deleted_mark.size() &&
            link.compare(link.size() - deleted_mark.size(), deleted_mark.size(), deleted_mark) == 0)
            return std::nullopt;
        if (link.empty() || link.front() != '/')
            return std::nullopt;
        return fs::path(link);
#else
        return std::nullopt;
#endif
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Registration register_mod_file_type(
    const fs::path& executable, std::span<const uint8_t> icon_png, const Places& places
) {
#ifdef _WIN32
    static_cast<void>(icon_png);
    return register_windows(executable, places);
#elif defined(__linux__) && !defined(__ANDROID__)
    return register_xdg(executable, icon_png, places);
#else
    // The bundle's Info.plist registers the type; nothing is written at run time.
    static_cast<void>(executable);
    static_cast<void>(icon_png);
    static_cast<void>(places);
    return Registration{};
#endif
}

} // namespace oa::platform::file_types
