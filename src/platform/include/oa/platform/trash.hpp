// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Moving a file to the system's trash, or deleting it where there is none.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace oa::platform {

/// Where a file went.
enum class TrashPlace : uint8_t {
    trash,   ///< the system's trash, which can put it back
    deleted, ///< removed, on a system that has no trash
};

/// What moving one file to the trash did.
struct TrashMove {
    bool moved{};                        ///< the file is no longer where it was
    TrashPlace place{TrashPlace::trash}; ///< where it went, when `moved`
    /// Where the trash kept the file, when the system says. Empty when it
    /// does not, as Windows does not.
    std::filesystem::path kept_at{};
    /// The trash's note of the file, when this program wrote one. Empty on
    /// macOS and Windows, which keep their own.
    std::filesystem::path note_at{};
    /// Why it did not move, when `moved` is false. Empty otherwise.
    std::string error{};
};

/// Tells whether this system has a trash a file can be put back from.
///
/// macOS, Windows and a Linux desktop do. iOS and Android do not: a file
/// there is deleted, and the player is told that.
///
/// @return true when move_to_trash puts a file in the trash
[[nodiscard]] bool system_has_trash() noexcept;

/// Moves one file to the system's trash.
///
/// On macOS this is the file manager's trash. On Windows, from Windows 8 on,
/// the shell is asked only to recycle the file. Before Windows 8 it is asked
/// to delete with undo and with no question, which deletes for good a file
/// the Recycle Bin cannot take, so a file that is not on a fixed drive (a
/// network share, a removable drive) is left first. A file the Recycle Bin
/// cannot take stays where it is, with one gap left before Windows 8: a file
/// on a fixed drive larger than its Recycle Bin can hold is still deleted for
/// good. Windows 9x cannot make the call, so the move fails and the file
/// stays. On Linux it is the freedesktop.org trash under
/// `$XDG_DATA_HOME/Trash`. On iOS and Android, which have no trash, the file
/// is deleted and `place` says so. A failure leaves the file where it was:
/// it is never removed outright on a system that has a trash.
///
/// @param file the file
/// @return what happened; `moved` is false when the file is still there
[[nodiscard]] TrashMove move_to_trash(const std::filesystem::path& file);

} // namespace oa::platform
