// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Install IDs, the catalogue-update setting and the downloads folder
// (oa/app/content/settings.hpp).
#include "oa/app/content/settings.hpp"

#include "oa/platform/files.hpp"
#include "oa/platform/random.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::content {
namespace {

/// Bytes of the system's generator an install ID holds.
constexpr std::size_t install_id_bytes = 16;

/// Characters in an install ID, hyphens included.
constexpr std::size_t install_id_text_length = 36;

/// Where the hyphens sit in an install ID.
constexpr std::array<std::size_t, 4> install_id_hyphens{8, 13, 18, 23};

/// The byte after which each hyphen is written, in order.
constexpr std::array<std::size_t, 4> hyphen_before_byte{4, 6, 8, 10};

/// Lower-case hexadecimal digits.
constexpr std::string_view lower_hex = "0123456789abcdef";

/// The gap Settings shows between the head and the tail of an install ID.
constexpr std::string_view install_id_shown_gap = " \u00b7 \u00b7 \u00b7 ";

/// Digits shown before that gap.
constexpr std::size_t install_id_shown_head = 8;

/// Digits shown after that gap.
constexpr std::size_t install_id_shown_tail = 4;

/// Stored word for checking at start, when the Library opens, and on Check now.
constexpr std::string_view updates_automatically = "automatically";

/// Stored word for checking when the Library opens, and on Check now.
constexpr std::string_view updates_library = "library";

/// Stored word for checking only on Check now.
constexpr std::string_view updates_never = "never";

/// Name endings of a downloaded package and of a partial download.
constexpr std::array<std::string_view, 6> download_suffixes{
    ".oalang", ".oamod", ".oamap", ".oalang.part", ".oamod.part", ".oamap.part"
};

/// Reports whether `index` is one of the hyphen positions.
bool hyphen_at(std::size_t index) noexcept {
    for (const std::size_t hyphen : install_id_hyphens) {
        if (index == hyphen)
            return true;
    }
    return false;
}

/// Writes 16 random bytes as an install ID.
std::string format_install_id(const std::array<uint8_t, install_id_bytes>& bytes) {
    std::string text(install_id_text_length, '\0');
    std::size_t at = 0;
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        for (const std::size_t before : hyphen_before_byte) {
            if (index == before)
                text[at++] = '-';
        }
        text[at++] = lower_hex[bytes[index] >> 4];
        text[at++] = lower_hex[bytes[index] & 0x0f];
    }
    return text;
}

/// Reports whether a file name is a downloaded package or a partial download.
bool is_download_name(std::string_view name) noexcept {
    for (const std::string_view suffix : download_suffixes) {
        if (name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix)
            return true;
    }
    return false;
}

/// Reports whether `file` is a regular file directly inside `folder`.
bool is_download_file(const std::filesystem::path& folder, const std::filesystem::path& file) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(file, error);
    if (error || !std::filesystem::is_regular_file(status))
        return false;
    if (file.parent_path().lexically_normal() != folder.lexically_normal())
        return false;
    return is_download_name(oa::platform::path_to_utf8(file.filename()));
}

/// Reports whether `named` is `file`, absolutely or as a name inside `folder`.
bool names_file(
    const std::filesystem::path& file,
    const std::filesystem::path& folder,
    const std::filesystem::path& named
) {
    if (named.empty())
        return false;
    const std::filesystem::path candidate = named.is_absolute() ? named : folder / named;
    std::error_code error;
    const bool same = std::filesystem::equivalent(file, candidate, error);
    if (!error)
        return same;
    return file.lexically_normal() == candidate.lexically_normal();
}

/// Reports whether a download is using `file`.
bool file_in_use(
    const std::filesystem::path& file,
    const std::filesystem::path& folder,
    std::span<const std::filesystem::path> in_use
) {
    for (const std::filesystem::path& named : in_use) {
        if (names_file(file, folder, named))
            return true;
    }
    return false;
}

/// Looks up one preference, or nothing when the key is absent.
const std::string*
find_value(const oa::platform::preferences::Values& values, const std::string& key) {
    const auto found = values.find(key);
    if (found == values.end())
        return nullptr;
    return &found->second;
}

} // namespace

std::string install_id_key(std::string_view registry) {
    std::string key;
    key.reserve(install_id_key_prefix.size() + registry.size());
    key.append(install_id_key_prefix);
    key.append(registry);
    return key;
}

InstallId
read_install_id(const oa::platform::preferences::Values& values, std::string_view registry) {
    const std::string* const stored = find_value(values, install_id_key(registry));
    if (stored == nullptr)
        return {};
    if (*stored == install_id_off_value)
        return {InstallIdState::off, {}};
    if (install_id_text_valid(*stored))
        return {InstallIdState::on, *stored};
    return {};
}

std::optional<std::string> make_install_id_text() {
    std::array<uint8_t, install_id_bytes> bytes{};
    if (!oa::platform::random::fill_random(bytes))
        return std::nullopt;
    return format_install_id(bytes);
}

bool install_id_text_valid(std::string_view text) noexcept {
    if (text.size() != install_id_text_length)
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        const char character = text[index];
        if (hyphen_at(index)) {
            if (character != '-')
                return false;
            continue;
        }
        if (lower_hex.find(character) == std::string_view::npos)
            return false;
    }
    return true;
}

std::string install_id_shown(std::string_view text) {
    if (!install_id_text_valid(text))
        return {};
    std::string shown;
    shown.reserve(install_id_shown_head + install_id_shown_gap.size() + install_id_shown_tail);
    shown.append(text.substr(0, install_id_shown_head));
    shown.append(install_id_shown_gap);
    shown.append(text.substr(text.size() - install_id_shown_tail));
    return shown;
}

std::optional<std::string> ensure_install_id(
    oa::platform::preferences::Values& values, std::string_view registry, bool& changed
) {
    changed = false;
    const InstallId current = read_install_id(values, registry);
    if (current.state == InstallIdState::off)
        return std::nullopt;
    if (current.state == InstallIdState::on)
        return current.text;
    std::optional<std::string> made = make_install_id_text();
    if (!made)
        return std::nullopt;
    values[install_id_key(registry)] = *made;
    changed = true;
    return made;
}

bool reset_install_id(oa::platform::preferences::Values& values, std::string_view registry) {
    std::optional<std::string> made = make_install_id_text();
    if (!made)
        return false;
    values[install_id_key(registry)] = *made;
    return true;
}

void turn_install_id_off(oa::platform::preferences::Values& values, std::string_view registry) {
    values[install_id_key(registry)] = std::string(install_id_off_value);
}

void turn_install_id_on(oa::platform::preferences::Values& values, std::string_view registry) {
    values.erase(install_id_key(registry));
}

CheckForUpdates read_check_for_updates(const oa::platform::preferences::Values& values) {
    const std::string* const stored = find_value(values, std::string(content_updates_key));
    if (stored == nullptr)
        return CheckForUpdates::automatically;
    if (*stored == updates_library)
        return CheckForUpdates::library_only;
    if (*stored == updates_never)
        return CheckForUpdates::never;
    return CheckForUpdates::automatically;
}

void write_check_for_updates(oa::platform::preferences::Values& values, CheckForUpdates check) {
    std::string_view word = updates_automatically;
    if (check == CheckForUpdates::library_only)
        word = updates_library;
    else if (check == CheckForUpdates::never)
        word = updates_never;
    values[std::string(content_updates_key)] = std::string(word);
}

std::filesystem::path downloads_folder(const std::filesystem::path& data_folder) {
    return data_folder / std::string(content_folder_name) / std::string(downloads_folder_name);
}

uint64_t downloaded_bytes(const std::filesystem::path& folder) noexcept {
    try {
        std::error_code error;
        std::filesystem::directory_iterator it(folder, error);
        if (error)
            return 0;
        uint64_t total = 0;
        const std::filesystem::directory_iterator end;
        while (it != end) {
            const std::filesystem::path file = it->path();
            it.increment(error);
            if (error)
                return 0;
            if (!is_download_file(folder, file))
                continue;
            const auto size = std::filesystem::file_size(file, error);
            if (error)
                return 0;
            total += static_cast<uint64_t>(size);
        }
        return total;
    } catch (...) {
        return 0;
    }
}

EmptyResult empty_downloads(
    const std::filesystem::path& folder, std::span<const std::filesystem::path> in_use
) {
    EmptyResult result;
    std::error_code error;
    std::filesystem::directory_iterator it(folder, error);
    if (error)
        return result;
    const std::filesystem::directory_iterator end;
    while (it != end) {
        const std::filesystem::path file = it->path();
        it.increment(error);
        if (error)
            break;
        if (!is_download_file(folder, file))
            continue;
        std::error_code size_error;
        const auto size = std::filesystem::file_size(file, size_error);
        if (size_error)
            continue;
        if (file_in_use(file, folder, in_use)) {
            ++result.kept;
            continue;
        }
        std::error_code remove_error;
        if (!std::filesystem::remove(file, remove_error) || remove_error)
            continue;
        result.removed_bytes += static_cast<uint64_t>(size);
    }
    return result;
}

} // namespace oa::app::content
