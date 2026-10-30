// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The texts a registration writes, the running program's path, the choice of how this system
// registers, and what a withdrawal does about the installer's association for every user.

#include "oa/platform/file_types.hpp"

#include "utf8.hpp"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <string>
#include <system_error>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif
#if defined(__APPLE__) && !TARGET_OS_IPHONE
#include "apple_handlers.hpp"
#endif

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

namespace {

/// The program a desktop entry should name.
///
/// When the executable sits in `versions/<version>/` and the folder above that holds the
/// launcher, the entry names the launcher, so it does not point into `versions/`.
///
/// @param executable the program, absolute
/// @return the launcher when this is a versions layout, otherwise `executable`
fs::path program_for_desktop(const fs::path& executable) {
    const fs::path folder = executable.parent_path();
    const fs::path above = folder.parent_path();
    if (above.filename() != "versions")
        return executable;
    const fs::path launcher = above.parent_path() / "open-annihilation";
    std::error_code error;
    const fs::file_status status = fs::symlink_status(launcher, error);
    if (error || !(fs::is_regular_file(status) || fs::is_symlink(status)))
        return executable;
    return launcher;
}

/// Returns a desktop entry that opens files of `opening` with `executable`: the file a bare
/// argument, or after `-c` for a launch file.
///
/// @param executable the program, absolute
/// @param types the file types the entry lists, of those `opening` covers
/// @param opening the file types the entry opens
/// @param comment what the entry says it does
/// @return the entry's text, UTF-8; empty when the path cannot be written in an entry
std::string
entry_for(const fs::path& executable, TypeSet types, Opening opening, std::string_view comment) {
    const std::string path = detail::utf8_of(program_for_desktop(executable));
    const std::string argument = desktop_exec_argument(path);
    if (argument.empty())
        return {};
    std::string entry;
    entry += "[Desktop Entry]\n";
    entry += "Type=Application\n";
    entry += "Name=Open Annihilation\n";
    entry += "Comment=" + std::string(comment) + "\n";
    // %f: the file opened. Started with no file, the entry drops it and the game starts as
    // usual. A bare argument naming one of the files the game installs or adds opens that
    // file; -c FILE starts the game with that launch.
    if (opening == Opening::launch)
        entry += "Exec=" + argument + " " + std::string(launch_option) + " %f\n";
    else
        entry += "Exec=" + argument + " %f\n";
    // The entry is passed over once the executable is gone.
    entry += "TryExec=" + desktop_string(path) + "\n";
    entry += "Icon=" + std::string(desktop_id) + "\n";
    entry += "Terminal=false\n";
    entry += "Categories=Game;StrategyGame;\n";
    std::string mime_types;
    for (const FileType& type : file_types) {
        if (type.opening != opening || !types.contains(type.extension))
            continue;
        mime_types += type.mime_type;
        mime_types += ';';
    }
    entry += "MimeType=" + mime_types + "\n";
    // An opener of the file types that "Open With" lists, not an entry in the menus.
    entry += "NoDisplay=true\n";
    return entry;
}

} // namespace

std::string desktop_entry(const fs::path& executable, TypeSet types) {
    return entry_for(
        executable, types, Opening::install, "Plays Total Annihilation from your own game files"
    );
}

std::string launch_desktop_entry(const fs::path& executable) {
    return entry_for(
        executable,
        TypeSet{},
        Opening::launch,
        "Opens a launch file's battle room in Open Annihilation"
    );
}

std::string mime_package(TypeSet types) {
    std::string package;
    package += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    package += "<mime-info xmlns=\"http://www.freedesktop.org/standards/shared-mime-info\">\n";
    for (const FileType& type : file_types) {
        if (!types.contains(type.extension))
            continue;
        package += "  <mime-type type=\"" + std::string(type.mime_type) + "\">\n";
        package += "    <comment>" + std::string(type.type_name) + "</comment>\n";
        package += "    <sub-class-of type=\"" + std::string(type.parent_mime_type) + "\"/>\n";
        package += "    <glob pattern=\"*." + std::string(type.extension) + "\"/>\n";
        // A launch is known by its first bytes too, whatever its name, as `cat` and a
        // download leave one.
        if (type.opening == Opening::launch) {
            package += "    <magic priority=\"50\">\n";
            package += "      <match type=\"string\" value=\"" + std::string(launch_magic) +
                       "\" offset=\"0\"/>\n";
            package += "    </magic>\n";
        }
        package += "  </mime-type>\n";
    }
    package += "</mime-info>\n";
    return package;
}

std::string open_command(const fs::path& executable, const Opening opening) {
    const std::string_view option = opening == Opening::launch ? launch_option : open_option;
    return "\"" + detail::utf8_of(executable) + "\" " + std::string(option) + " \"%1\"";
}

std::string default_icon(const fs::path& executable) {
    return detail::utf8_of(executable) + ",0";
}

UserOverrideStep user_override_step(
    const std::string_view program_id,
    const std::optional<std::string_view> machine_default,
    const std::optional<std::string_view> user_default
) noexcept {
    const bool shared_is_the_games = machine_default && *machine_default == program_id;
    if (!user_default)
        return shared_is_the_games ? UserOverrideStep::write : UserOverrideStep::none;
    if (*user_default == user_override_program_id)
        return shared_is_the_games ? UserOverrideStep::keep : UserOverrideStep::remove;
    // The user's own (Default) names a program: it already decides for this user.
    return UserOverrideStep::none;
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

Registration register_file_types(
    const fs::path& executable,
    std::span<const uint8_t> icon_png,
    const Places& places,
    const TypeSet types
) {
#ifdef _WIN32
    static_cast<void>(icon_png);
    return register_windows(executable, places, types);
#elif defined(__linux__) && !defined(__ANDROID__)
    return register_xdg(executable, icon_png, places, types);
#else
    // The bundle's Info.plist registers the type; nothing is written at run time.
    static_cast<void>(executable);
    static_cast<void>(icon_png);
    static_cast<void>(places);
    static_cast<void>(types);
    return Registration{};
#endif
}

Registration withdraw_file_types(const TypeSet types, const Places& places) {
#ifdef _WIN32
    return withdraw_windows(types, places);
#elif defined(__linux__) && !defined(__ANDROID__)
    return withdraw_xdg(types, places);
#else
    static_cast<void>(types);
    static_cast<void>(places);
    return Registration{};
#endif
}

namespace {

/// The exported content type of `extension`, or null when it is not one of the eight.
const ExportedContentType* exported_of(std::string_view extension) noexcept {
    for (const ExportedContentType& type : exported_content_types)
        if (type.extension == extension)
            return &type;
    return nullptr;
}

/// Reports whether two bundle identifiers name the same program, ignoring ASCII case.
bool same_bundle(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    const auto lower = [](unsigned char letter) noexcept {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a')
                                              : static_cast<char>(letter);
    };
    for (std::size_t index = 0; index < left.size(); ++index)
        if (lower(static_cast<unsigned char>(left[index])) !=
            lower(static_cast<unsigned char>(right[index])))
            return false;
    return true;
}

/// Sets the switch `extension` names.
void set_open(TypeSet& open, std::string_view extension, bool on) noexcept {
    if (extension == mod_extension)
        open.mod = on;
    else if (extension == language_extension)
        open.language = on;
    else if (extension == map_extension)
        open.map = on;
    else if (extension == mutator_extension)
        open.mutator = on;
    else if (extension == brain_extension)
        open.brain = on;
    else if (extension == keys_extension)
        open.keys = on;
    else if (extension == registry_extension)
        open.registry = on;
    else if (extension == launch_extension)
        open.launch = on;
}

/// Appends `more` to `failure`, after a semicolon when `failure` already holds one.
void add_failure(std::string& failure, std::string_view more) {
    if (more.empty())
        return;
    if (!failure.empty())
        failure += "; ";
    failure.append(more);
}

/// What a log line calls `handler`.
std::string handler_name(const Handler& handler) {
    if (!handler.bundle_id.empty())
        return handler.bundle_id;
    return handler.application_url;
}

} // namespace

bool default_opener_is_a_setting() noexcept {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    return true;
#else
    return false;
#endif
}

HandlerCalls system_handler_calls() {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    return apple_handler_calls();
#else
    return {};
#endif
}

Handler running_application() {
#if defined(__APPLE__) && !TARGET_OS_IPHONE
    return apple_running_application();
#else
    return {};
#endif
}

OpenerReport reported_openers(
    const HandlerCalls& calls, const std::string_view bundle_id, const TypeSet fallback
) {
    OpenerReport report;
    report.open = fallback;
    if (calls.default_handler == nullptr || bundle_id.empty())
        return report;
    for (const ExportedContentType& type : exported_content_types) {
        std::string failure;
        const Handler opener = calls.default_handler(calls.context, type.content_type, failure);
        if (!failure.empty()) {
            add_failure(report.error, std::string(type.extension) + ": " + failure);
            continue;
        }
        set_open(report.open, type.extension, same_bundle(opener.bundle_id, bundle_id));
    }
    return report;
}

std::vector<std::string> extensions_still_opened(const TypeSet asked, const TypeSet open) {
    std::vector<std::string> stayed;
    const auto add = [&](const bool asked_on, const bool now_on, const std::string_view extension) {
        if (!asked_on && now_on)
            stayed.emplace_back(std::string(".") + std::string(extension));
    };
    // The order Settings lists the switches.
    add(asked.mod, open.mod, mod_extension);
    add(asked.map, open.map, map_extension);
    add(asked.language, open.language, language_extension);
    add(asked.registry, open.registry, registry_extension);
    add(asked.mutator, open.mutator, mutator_extension);
    add(asked.brain, open.brain, brain_extension);
    add(asked.keys, open.keys, keys_extension);
    add(asked.launch, open.launch, launch_extension);
    return stayed;
}

Registration apply_opener_changes(
    const HandlerCalls& calls,
    const Handler& application,
    const std::span<const OpenerChange> changes
) {
    Registration done;
    if (calls.set_handler == nullptr || calls.default_handler == nullptr) {
        done.lines.emplace_back("skipped: this system does not choose a default opener");
        return done;
    }
    done.supported = true;
    if (application.bundle_id.empty() && application.application_url.empty()) {
        done.lines.emplace_back("skipped: this copy is not an application");
        done.error = "this copy is not an application";
        return done;
    }
    for (const OpenerChange& change : changes) {
        const ExportedContentType* type = exported_of(change.extension);
        if (type == nullptr) {
            add_failure(done.error, "unknown file type " + std::string(change.extension));
            continue;
        }
        const std::string extension(type->extension);
        // A set is kept only when the opener read afterwards is the one asked for. The
        // switch is drawn from that read, and a change that did not take is logged.
        const auto kept = [&](const Handler& asked, std::string success) {
            std::string failure;
            if (!calls.set_handler(calls.context, type->content_type, asked, failure)) {
                add_failure(done.error, extension + ": " + failure);
                return;
            }
            const Handler now = calls.default_handler(calls.context, type->content_type, failure);
            if (!failure.empty() || !same_bundle(now.bundle_id, asked.bundle_id)) {
                add_failure(done.error, extension + ": the change did not take");
                return;
            }
            done.changed = true;
            done.lines.emplace_back(std::move(success));
        };
        if (change.open) {
            kept(application, "open " + extension);
            continue;
        }
        std::string parent_failure;
        const Handler handed =
            calls.default_handler(calls.context, type->hands_back_to, parent_failure);
        if (!parent_failure.empty() || handler_name(handed).empty()) {
            if (parent_failure.empty())
                parent_failure = "nothing opens " + std::string(type->hands_back_to);
            add_failure(done.error, extension + ": " + parent_failure);
            continue;
        }
        kept(handed, "hand back " + extension + " to " + handler_name(handed));
    }
    return done;
}

} // namespace oa::platform::file_types
