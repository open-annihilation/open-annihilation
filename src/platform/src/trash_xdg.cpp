// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Linux: the freedesktop.org trash under $XDG_DATA_HOME/Trash (or
// ~/.local/share/Trash). The file is moved into files/ and a note is written
// beside it. A file on another device is copied there and the original is
// removed only once the copy is in the trash. A failure leaves the original.

#include "oa/platform/trash.hpp"

#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"

#include <cerrno>
#include <ctime>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include <fcntl.h>
#include <unistd.h>

namespace oa::platform {
namespace {

namespace fs = std::filesystem;

/// Percent-encodes a path for a trash note, leaving slashes as they are.
///
/// @param text the absolute path, UTF-8
/// @return the encoded path
std::string percent_encode(std::string_view text) {
    std::string encoded;
    encoded.reserve(text.size());
    constexpr char hex[] = "0123456789ABCDEF";
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        const bool plain = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
                           (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
                           byte == '.' || byte == '~' || byte == '/';
        if (plain) {
            encoded.push_back(static_cast<char>(byte));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex[byte >> 4U]);
        encoded.push_back(hex[byte & 0x0FU]);
    }
    return encoded;
}

/// The local time a trash note dates a deletion, `YYYY-MM-DDThh:mm:ss`.
///
/// @return the time; 1970-01-01T00:00:00 when the clock cannot say
std::string deletion_date() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &now) != 0)
        return "1970-01-01T00:00:00";
#else
    if (localtime_r(&now, &parts) == nullptr)
        return "1970-01-01T00:00:00";
#endif
    char text[32];
    const std::size_t length = std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%S", &parts);
    if (length == 0)
        return "1970-01-01T00:00:00";
    return std::string(text, length);
}

/// The home trash, or an empty path when this process has no home.
///
/// `$XDG_DATA_HOME` is used only when it is absolute. Otherwise the trash is
/// `~/.local/share/Trash`.
///
/// @return the Trash folder; empty when neither variable names a home
fs::path home_trash() {
    const std::optional<std::string> data_home = environment_value("XDG_DATA_HOME");
    if (data_home && !data_home->empty() && data_home->front() == '/')
        return path_from_utf8(*data_home) / "Trash";
    const std::optional<std::string> home = environment_value("HOME");
    if (!home || home->empty())
        return {};
    return path_from_utf8(*home) / ".local" / "share" / "Trash";
}

/// Makes a folder, and its missing parents, and sets 0700 only on a folder
/// this call created. A folder that is already there keeps its mode.
///
/// @param folder the folder
/// @param[out] failure why it could not be made
/// @return true when the folder is there
bool make_directory(const fs::path& folder, std::error_code& failure) {
    if (fs::is_directory(folder, failure))
        return !failure;
    failure.clear();
    const fs::path parent = folder.parent_path();
    if (!parent.empty() && parent != folder && !make_directory(parent, failure))
        return false;
    failure.clear();
    if (fs::is_directory(folder, failure))
        return !failure;
    failure.clear();
    if (!fs::create_directory(folder, failure)) {
        failure.clear();
        return fs::is_directory(folder, failure) && !failure;
    }
    std::error_code mode_error;
    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace, mode_error);
    return true;
}

/// What writing one trash note did.
enum class NoteWrite {
    written, ///< the note is there, and no other name holds it
    taken,   ///< that name was already a note
    failed,  ///< the note could not be written
};

/// Creates one trash note, and no other file of that name.
///
/// The note is created with O_CREAT | O_EXCL before the file is moved, as
/// the freedesktop.org specification asks. A name already taken is refused.
///
/// @param note the note's path
/// @param original the file's path before it is moved, absolute
/// @return whether the note was created
NoteWrite write_note(const fs::path& note, const fs::path& original) {
    const int opened = ::open(note.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (opened < 0)
        return errno == EEXIST ? NoteWrite::taken : NoteWrite::failed;
    const std::string body = "[Trash Info]\nPath=" + percent_encode(path_to_utf8(original)) +
                             "\nDeletionDate=" + deletion_date() + "\n";
    const char* data = body.data();
    std::size_t left = body.size();
    bool ok = true;
    while (left > 0) {
        const ssize_t wrote = ::write(opened, data, left);
        if (wrote < 0) {
            if (errno == EINTR)
                continue;
            ok = false;
            break;
        }
        if (wrote == 0) {
            ok = false;
            break;
        }
        data += wrote;
        left -= static_cast<std::size_t>(wrote);
    }
    if (::close(opened) != 0)
        ok = false;
    if (!ok) {
        ::unlink(note.c_str());
        return NoteWrite::failed;
    }
    return NoteWrite::written;
}

} // namespace

bool system_has_trash() noexcept {
    return true;
}

TrashMove move_to_trash(const std::filesystem::path& file) {
    TrashMove out;
    std::error_code failure;
    if (!fs::is_regular_file(file, failure) || failure) {
        out.error = "the file is not there";
        return out;
    }
    const fs::path root = home_trash();
    if (root.empty()) {
        out.error = "this process has no home, so it has nowhere to keep the trash";
        return out;
    }
    const fs::path files = root / "files";
    const fs::path info = root / "info";
    if (!make_directory(files, failure) || !make_directory(info, failure)) {
        out.error = "the trash could not be made";
        return out;
    }
    failure.clear();
    const std::string filename = path_to_utf8(file.filename());
    const std::size_t dot = filename.rfind('.');
    const bool split = dot != std::string::npos && dot != 0;
    const std::string stem = split ? filename.substr(0, dot) : filename;
    const std::string extension = split ? filename.substr(dot) : std::string{};
    fs::path original = file;
    if (original.is_relative())
        original = fs::absolute(original, failure);
    if (failure) {
        out.error = "the file's path cannot be written";
        return out;
    }
    for (int attempt = 1; attempt < 1000; ++attempt) {
        const std::string name =
            attempt == 1 ? filename : stem + " " + std::to_string(attempt) + extension;
        const fs::path destination = files / path_from_utf8(name);
        const fs::path note = info / path_from_utf8(name + ".trashinfo");
        failure.clear();
        if (fs::exists(destination, failure) || failure)
            continue;
        // The note is created first, and only then is the file moved. A note
        // that cannot be written moves nothing. The name in files/ is claimed
        // the same way, so a name taken in between is not replaced.
        const NoteWrite wrote = write_note(note, original);
        if (wrote == NoteWrite::taken)
            continue;
        if (wrote != NoteWrite::written) {
            out.error = "the trash could not record the file";
            return out;
        }
        const int claimed = ::open(destination.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (claimed < 0) {
            ::unlink(note.c_str());
            if (errno == EEXIST)
                continue;
            out.error = "the trash could not take the file";
            return out;
        }
        ::close(claimed);
        const auto drop_claim = [&] {
            std::error_code ignored;
            fs::remove(destination, ignored);
            ::unlink(note.c_str());
        };
        failure.clear();
        fs::rename(file, destination, failure);
        if (failure == std::errc::cross_device_link) {
            failure.clear();
            fs::copy_file(file, destination, fs::copy_options::overwrite_existing, failure);
            if (!failure)
                fs::remove(file, failure);
        }
        if (failure) {
            drop_claim();
            out.error = "the trash could not take the file";
            return out;
        }
        out.moved = true;
        out.place = TrashPlace::trash;
        out.kept_at = destination;
        out.note_at = note;
        return out;
    }
    out.error = "the trash already holds this name";
    return out;
}

} // namespace oa::platform
