// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Copying a file's bytes into a new file, for a file that has to move to
// another volume: a folder on a USB drive, a network share or a folder
// shared into a virtual machine. The destination is made as the system makes
// any new file and no permission or ownership of the source is copied, so a
// file system that refuses them (exFAT, FAT, many network shares) copies as
// any other does. The standard library's copy_file sets the destination's
// permissions to the source's before it writes a byte, and a refusal there
// leaves an empty file that its owner cannot read.
#pragma once

#include <cstdint>
#include <filesystem>
#include <system_error>

namespace oa::platform {

/// What copy_file_contents does where the destination's name is taken.
enum class CopyOnto : uint8_t {
    /// The name has to be free. It is claimed in the same step that makes the file (`O_EXCL` where
    /// the system has it, `CREATE_NEW` on Windows), so that of several writers after one name
    /// only one gets it. A taken name is refused with std::errc::file_exists.
    must_be_new,
    /// A file that is at the name is written over, whole: its previous bytes do not remain.
    replace,
};

/// Copies the bytes of a file into another file.
///
/// The destination is made with the system's usual permissions for a new file (on a system with
/// a umask, 0666 less the process's umask), never the source's, and no step sets permissions
/// or ownership afterwards. When the copy fails, whatever was made at `to` is removed, so no
/// partly written file is left; a name that is not a regular file is never removed, and a name
/// whose claim was refused is not touched. The source is only read, and is not removed. The
/// source and the destination may be on different volumes.
///
/// With `replace`, a file the destination names is truncated as it is opened, and removed again
/// if the copy then fails, so that a failed copy leaves neither the old bytes nor part of the new
/// ones. A source that is the destination is refused with std::errc::file_exists before any file
/// is opened for writing.
///
/// @param from the file to copy
/// @param to the file to make; its folder must exist
/// @param onto what to do where `to` already names a file
/// @return the reason it failed, or no error when `to` holds the bytes; a taken name under
///         `must_be_new` is std::errc::file_exists, and a failure of the system is its code
[[nodiscard]] std::error_code copy_file_contents(
    const std::filesystem::path& from, const std::filesystem::path& to, CopyOnto onto
) noexcept;

} // namespace oa::platform
