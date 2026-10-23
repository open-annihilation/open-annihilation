// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Windows registration: the seven file types, each extension and its program identifier,
// the program's icon and its open command under HKEY_CURRENT_USER, through calls Windows XP has.
// A withdrawal of launch files also hides the installer's association for every user from this
// one, with an override under HKEY_CURRENT_USER; the computer's own key is only read.

#include "oa/platform/file_types.hpp"

#include "utf8.hpp"

#include <exception>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shlobj.h>

namespace oa::platform::file_types {

namespace fs = std::filesystem;

namespace {

/// The Windows name of the key HKEY_CURRENT_USER, for the log.
constexpr std::string_view current_user_name = "HKEY_CURRENT_USER";
/// The Windows name of the key HKEY_LOCAL_MACHINE, for the log.
constexpr std::string_view local_machine_name = "HKEY_LOCAL_MACHINE";
/// The value of a key that the registry calls (Default), for the log.
constexpr std::string_view default_value_name = "(Default)";

/// Returns the handle of a registry root.
///
/// @param root the root
/// @return HKEY_LOCAL_MACHINE or HKEY_CURRENT_USER
HKEY handle_of(const RegistryRoot root) {
    return root == RegistryRoot::local_machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
}

/// Returns the Windows name of a registry root, for the log.
///
/// @param root the root
/// @return its name
std::string_view root_name_of(const RegistryRoot root) {
    return root == RegistryRoot::local_machine ? local_machine_name : current_user_name;
}

/// One string value the registration keeps.
struct RegistryValue {
    std::wstring key{};  ///< under HKEY_CURRENT_USER
    std::wstring name{}; ///< empty for the key's default value
    std::wstring data{};
};

/// Converts UTF-8 text to UTF-16.
///
/// @param text the text, UTF-8
/// @return the text, UTF-16; empty when it is not UTF-8
std::wstring wide_of(std::string_view text) {
    if (text.empty())
        return {};
    const int length = MultiByteToWideChar(
        CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0
    );
    if (length <= 0)
        return {};
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        text.data(),
        static_cast<int>(text.size()),
        wide.data(),
        length
    );
    return wide;
}

/// Converts UTF-16 text to UTF-8, for the log.
///
/// @param text the text, UTF-16
/// @return the text, UTF-8
std::string narrow_of(const std::wstring& text) {
    if (text.empty())
        return {};
    const int length = WideCharToMultiByte(
        CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr
    );
    if (length <= 0)
        return {};
    std::string narrow(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(
        CP_UTF8,
        0,
        text.data(),
        static_cast<int>(text.size()),
        narrow.data(),
        length,
        nullptr,
        nullptr
    );
    return narrow;
}

/// Returns a value's place, for the log: its key under HKEY_CURRENT_USER, then its name.
///
/// @param value the value
/// @return the place
std::string place_of(const RegistryValue& value) {
    return std::string(current_user_name) + "\\" + narrow_of(value.key) + " " +
           (value.name.empty() ? std::string(default_value_name) : narrow_of(value.name));
}

/// Returns a system error's description.
///
/// @param code the error, as the registry calls return it
/// @return its description
std::string error_text(LONG code) {
    return std::system_category().message(static_cast<int>(code));
}

/// Reports whether a value is a string holding `value.data`.
///
/// @param value the value
/// @return true when it is there with that text
bool holds(const RegistryValue& value) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, value.key.c_str(), 0, KEY_QUERY_VALUE, &key) !=
        ERROR_SUCCESS)
        return false;
    const wchar_t* name = value.name.empty() ? nullptr : value.name.c_str();
    DWORD type = 0;
    DWORD size = 0;
    bool same = false;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        // Room for the text and its terminating zero, which a value may lack.
        std::vector<wchar_t> held(size / sizeof(wchar_t) + 2, L'\0');
        DWORD held_size = static_cast<DWORD>((held.size() - 1) * sizeof(wchar_t));
        if (RegQueryValueExW(
                key, name, nullptr, &type, reinterpret_cast<BYTE*>(held.data()), &held_size
            ) == ERROR_SUCCESS &&
            type == REG_SZ) {
            std::size_t length = held_size / sizeof(wchar_t);
            while (length > 0 && held[length - 1] == L'\0')
                --length;
            same = std::wstring(held.data(), length) == value.data;
        }
    }
    RegCloseKey(key);
    return same;
}

/// Returns a string value's text. The key is opened to read only.
///
/// @param root the root the key is under
/// @param key_name the key, under `root`
/// @param value_name the value; empty for the key's default value
/// @return the text; nothing when the value is missing or is not a string
std::optional<std::wstring>
read_string(HKEY root, const std::wstring& key_name, const std::wstring& value_name) {
    HKEY opened = nullptr;
    if (RegOpenKeyExW(root, key_name.c_str(), 0, KEY_QUERY_VALUE, &opened) != ERROR_SUCCESS)
        return std::nullopt;
    const wchar_t* name = value_name.empty() ? nullptr : value_name.c_str();
    DWORD type = 0;
    DWORD size = 0;
    std::optional<std::wstring> text;
    if (RegQueryValueExW(opened, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS &&
        type == REG_SZ) {
        std::vector<wchar_t> held(size / sizeof(wchar_t) + 2, L'\0');
        DWORD held_size = static_cast<DWORD>((held.size() - 1) * sizeof(wchar_t));
        if (RegQueryValueExW(
                opened, name, nullptr, &type, reinterpret_cast<BYTE*>(held.data()), &held_size
            ) == ERROR_SUCCESS &&
            type == REG_SZ) {
            std::size_t length = held_size / sizeof(wchar_t);
            while (length > 0 && held[length - 1] == L'\0')
                --length;
            text = std::wstring(held.data(), length);
        }
    }
    RegCloseKey(opened);
    return text;
}

/// Deletes a string value. A missing key or value is already gone, and is not a change.
///
/// @param[in,out] done the registration's record
/// @param key_name the key, under HKEY_CURRENT_USER
/// @param value_name the value; empty for the key's default value
/// @return false when the registry refused
bool take_value(Registration& done, const std::wstring& key_name, const std::wstring& value_name) {
    HKEY opened = nullptr;
    const LONG opened_result =
        RegOpenKeyExW(HKEY_CURRENT_USER, key_name.c_str(), 0, KEY_SET_VALUE, &opened);
    if (opened_result == ERROR_FILE_NOT_FOUND)
        return true;
    if (opened_result != ERROR_SUCCESS) {
        const RegistryValue value{key_name, value_name, {}};
        done.error = "cannot remove " + place_of(value) + ": " + error_text(opened_result);
        return false;
    }
    const wchar_t* name = value_name.empty() ? nullptr : value_name.c_str();
    const LONG result = RegDeleteValueW(opened, name);
    RegCloseKey(opened);
    if (result == ERROR_FILE_NOT_FOUND)
        return true;
    if (result != ERROR_SUCCESS) {
        const RegistryValue value{key_name, value_name, {}};
        done.error = "cannot remove " + place_of(value) + ": " + error_text(result);
        return false;
    }
    done.changed = true;
    const RegistryValue value{key_name, value_name, {}};
    done.lines.push_back("removed " + place_of(value));
    return true;
}

/// Deletes a key that has no subkeys. A missing key is already gone. A key that still has a
/// subkey is left, and the refusal is recorded: an unknown subkey is never wiped.
///
/// @param[in,out] done the registration's record
/// @param key_name the key, under HKEY_CURRENT_USER
/// @return false when the registry refused
bool take_key(Registration& done, const std::wstring& key_name) {
    const LONG result = RegDeleteKeyW(HKEY_CURRENT_USER, key_name.c_str());
    if (result == ERROR_FILE_NOT_FOUND)
        return true;
    const std::string place = std::string(current_user_name) + "\\" + narrow_of(key_name);
    if (result != ERROR_SUCCESS) {
        done.error = "cannot remove " + place + ": " + error_text(result);
        return false;
    }
    done.changed = true;
    done.lines.push_back("removed " + place);
    return true;
}

/// Tells the shell once that a file association changed, as a write does.
///
/// @param[in,out] done the registration's record
/// @param places whether the shell is told
void tell_shell(Registration& done, const Places& places) {
    if (done.changed && places.notify_shell) {
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        done.lines.push_back("told the shell the file types changed");
    }
}

/// Writes a string value, making its key when it is missing.
///
/// @param value the value
/// @return ERROR_SUCCESS, or the registry's error
LONG write(const RegistryValue& value) {
    HKEY key = nullptr;
    LONG result = RegCreateKeyExW(
        HKEY_CURRENT_USER,
        value.key.c_str(),
        0,
        nullptr,
        REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE,
        nullptr,
        &key,
        nullptr
    );
    if (result != ERROR_SUCCESS)
        return result;
    const wchar_t* name = value.name.empty() ? nullptr : value.name.c_str();
    const auto size = static_cast<DWORD>((value.data.size() + 1) * sizeof(wchar_t));
    result = RegSetValueExW(
        key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.data.c_str()), size
    );
    RegCloseKey(key);
    return result;
}

/// Returns a path's long form, upper-cased without regard to the user's language, for
/// comparing paths as Windows does.
///
/// @param path the path
/// @return its long form upper-cased; the path upper-cased when it has no long form
std::wstring folded(std::wstring path) {
    std::wstring long_form(MAX_PATH, L'\0');
    DWORD length =
        GetLongPathNameW(path.c_str(), long_form.data(), static_cast<DWORD>(long_form.size()));
    if (length >= long_form.size()) {
        long_form.resize(length);
        length =
            GetLongPathNameW(path.c_str(), long_form.data(), static_cast<DWORD>(long_form.size()));
    }
    if (length > 0 && length < long_form.size()) {
        long_form.resize(length);
        path = long_form;
    }
    if (path.empty())
        return path;
    std::wstring upper(path.size(), L'\0');
    const int mapped = LCMapStringW(
        LOCALE_INVARIANT,
        LCMAP_UPPERCASE,
        path.c_str(),
        static_cast<int>(path.size()),
        upper.data(),
        static_cast<int>(upper.size())
    );
    return mapped == static_cast<int>(path.size()) ? upper : path;
}

/// Returns the temporary folder when it holds `executable`, as it does for a copy Explorer
/// started from inside a zip.
///
/// @param executable the program, absolute
/// @return the folder; empty when the program lies outside it
std::wstring temporary_folder_of(const std::wstring& executable) {
    std::wstring folder(MAX_PATH + 1, L'\0');
    const DWORD length = GetTempPathW(static_cast<DWORD>(folder.size()), folder.data());
    if (length == 0 || length > folder.size())
        return {};
    folder.resize(length);
    std::wstring folder_key = folded(folder);
    if (folder_key.empty())
        return {};
    if (folder_key.back() != L'\\')
        folder_key += L'\\';
    const std::wstring executable_key = folded(executable);
    if (executable_key.size() > folder_key.size() &&
        executable_key.compare(0, folder_key.size(), folder_key) == 0)
        return folder;
    return {};
}

/// The registration itself; register_windows catches what it throws.
///
/// @param executable the program, absolute
/// @param places where it writes
/// @param types the file types to register
/// @return what was done
Registration register_in(const fs::path& executable, const Places& places, TypeSet types) {
    Registration done{};
    done.supported = true;
    if (!executable.is_absolute()) {
        done.error = "the program's path " + detail::utf8_of(executable) + " is not absolute";
        return done;
    }
    const std::wstring program = executable.wstring();
    if (const std::wstring temporary = temporary_folder_of(program); !temporary.empty()) {
        done.lines.push_back(
            "skipped: " + narrow_of(program) + " lies in the temporary folder " +
            narrow_of(temporary)
        );
        return done;
    }
    const std::wstring classes = wide_of(places.classes_key);
    const std::wstring icon = wide_of(default_icon(executable));
    bool failed = false;
    for (const FileType& type : file_types) {
        if (!types.contains(type.extension))
            continue;
        const std::wstring extension = classes + L"\\." + wide_of(type.extension);
        const std::wstring program_id = wide_of(type.program_id);
        const std::wstring program_key = classes + L"\\" + program_id;
        // The six values of one type. Nothing else under the type is read or written, so a
        // choice of opener the player made in Windows stays as it is.
        const RegistryValue values[] = {
            {extension, L"", program_id},
            {extension, L"Content Type", wide_of(type.mime_type)},
            {extension + L"\\OpenWithProgids", program_id, L""},
            {program_key, L"", wide_of(type.type_name)},
            {program_key + L"\\DefaultIcon", L"", icon},
            {program_key + L"\\shell\\open\\command",
             L"",
             wide_of(open_command(executable, type.opening))},
        };
        for (const RegistryValue& value : values) {
            if (holds(value))
                continue;
            const LONG result = write(value);
            if (result != ERROR_SUCCESS) {
                done.error = "cannot write " + place_of(value) + ": " + error_text(result);
                failed = true;
                break;
            }
            done.changed = true;
            done.lines.push_back("wrote " + place_of(value));
        }
        if (failed)
            break;
    }
    tell_shell(done, places);
    return done;
}

/// Writes or removes the user's override of the association every user of the computer shares
/// for one of installer_types, as user_override_step says, once the user's own registration is
/// withdrawn. The computer's key is opened to read only.
///
/// @param[in,out] done the withdrawal's record
/// @param classes the user's classes key, under HKEY_CURRENT_USER
/// @param places where the computer's classes are
/// @param type the file type
/// @return false when the registry refused
bool follow_shared_association(
    Registration& done, const std::wstring& classes, const Places& places, const FileType& type
) {
    const std::wstring dotted = L"." + wide_of(type.extension);
    const std::wstring extension = classes + L"\\" + dotted;
    const std::wstring shared_extension = wide_of(places.machine_classes_key) + L"\\" + dotted;
    const std::optional<std::wstring> shared =
        read_string(handle_of(places.machine_classes_root), shared_extension, L"");
    const std::optional<std::wstring> own = read_string(HKEY_CURRENT_USER, extension, L"");
    const std::optional<std::string> shared_text =
        shared ? std::optional<std::string>(narrow_of(*shared)) : std::nullopt;
    const std::optional<std::string> own_text =
        own ? std::optional<std::string>(narrow_of(*own)) : std::nullopt;
    const std::string shared_place = std::string(root_name_of(places.machine_classes_root)) + "\\" +
                                     narrow_of(shared_extension) + " " +
                                     std::string(default_value_name);
    switch (user_override_step(
        type.program_id,
        shared_text ? std::optional<std::string_view>(*shared_text) : std::nullopt,
        own_text ? std::optional<std::string_view>(*own_text) : std::nullopt
    )) {
    case UserOverrideStep::write: {
        const RegistryValue value{extension, L"", wide_of(user_override_program_id)};
        const LONG result = write(value);
        if (result != ERROR_SUCCESS) {
            done.error = "cannot write " + place_of(value) + ": " + error_text(result);
            return false;
        }
        done.changed = true;
        done.lines.push_back(
            "wrote " + place_of(value) + " empty, so that the association for every user, " +
            shared_place + ", no longer opens these files for this user"
        );
        return true;
    }
    case UserOverrideStep::remove: {
        const std::size_t lines_before = done.lines.size();
        if (!take_value(done, extension, L""))
            return false;
        if (done.lines.size() > lines_before)
            done.lines.back() +=
                ", the empty one that hid the association for every user, which no longer "
                "names the game";
        return true;
    }
    case UserOverrideStep::keep:
    case UserOverrideStep::none:
        break;
    }
    return true;
}

/// Removes one type's values that still name this game. Another program's values stay. A type
/// of installer_types then follows the association every user shares
/// (follow_shared_association).
///
/// @param[in,out] done the registration's record
/// @param classes the classes key, under HKEY_CURRENT_USER
/// @param places where the computer's classes are
/// @param type the file type
/// @return false when the registry refused
bool withdraw_type(
    Registration& done, const std::wstring& classes, const Places& places, const FileType& type
) {
    const std::wstring extension = classes + L"\\." + wide_of(type.extension);
    const std::wstring program_id = wide_of(type.program_id);
    const std::wstring program_key = classes + L"\\" + program_id;
    // The OpenWithProgids value is the game's when its name is the program identifier.
    // Another program's value on that key has another name and is left.
    if (!take_value(done, extension + L"\\OpenWithProgids", program_id))
        return false;
    // (Default) is the game's only while its text is still the program identifier.
    if (const std::optional<std::wstring> held = read_string(HKEY_CURRENT_USER, extension, L"");
        held && *held == program_id) {
        if (!take_value(done, extension, L""))
            return false;
    }
    // Content Type is the game's only while its text is still the game's MIME type.
    const std::wstring mime = wide_of(type.mime_type);
    if (const std::optional<std::wstring> held =
            read_string(HKEY_CURRENT_USER, extension, L"Content Type");
        held && *held == mime) {
        if (!take_value(done, extension, L"Content Type"))
            return false;
    }
    // The program-identifier key is the game's by its name. Its own subkeys go leaf first,
    // and a key that still has an unexpected subkey is left rather than wiped.
    const std::wstring leaves[] = {
        program_key + L"\\shell\\open\\command",
        program_key + L"\\shell\\open",
        program_key + L"\\shell",
        program_key + L"\\DefaultIcon",
        program_key,
    };
    for (const std::wstring& leaf : leaves)
        if (!take_key(done, leaf))
            return false;
    if (installer_types.contains(type.extension))
        return follow_shared_association(done, classes, places, type);
    return true;
}

/// The withdrawal itself; withdraw_windows catches what it throws.
///
/// @param types the file types to withdraw
/// @param places where it writes
/// @return what was done
Registration withdraw_in(TypeSet types, const Places& places) {
    Registration done{};
    done.supported = true;
    const std::wstring classes = wide_of(places.classes_key);
    for (const FileType& type : file_types) {
        if (!types.contains(type.extension))
            continue;
        if (!withdraw_type(done, classes, places, type))
            break;
    }
    tell_shell(done, places);
    return done;
}

} // namespace

Registration
register_windows(const fs::path& executable, const Places& places, const TypeSet types) {
    try {
        return register_in(executable, places, types);
    } catch (const std::exception& error) {
        Registration failed{};
        failed.supported = true;
        failed.error = std::string("the file types could not be registered: ") + error.what();
        return failed;
    }
}

Registration withdraw_windows(const TypeSet types, const Places& places) {
    try {
        return withdraw_in(types, places);
    } catch (const std::exception& error) {
        Registration failed{};
        failed.supported = true;
        failed.error = std::string("the file types could not be withdrawn: ") + error.what();
        return failed;
    }
}

} // namespace oa::platform::file_types
