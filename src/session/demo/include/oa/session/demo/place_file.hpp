// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Putting a finished recording, or a file that goes with one, where it is to be kept: in the
// recordings folder or at the path a player named, which may be on another volume, a USB drive
// or a network share.

#include <filesystem>
#include <string>
#include <system_error>

namespace oa::session::demo {

/// Ways of moving a file that need no copy, which a test refuses so that the copy across
/// volumes is the way a file is moved; each null member uses std::filesystem.
struct PlaceHooks {
    void* context{};
    /// Renames `from` to `to`; null uses std::filesystem::rename. Sets `error` when it fails.
    void (*rename)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error
    ){};
    /// Makes `to` a second name of the file `from`; null uses std::filesystem::create_hard_link.
    /// Sets `error` when it fails, std::errc::file_exists when the name is taken.
    void (*link)(
        void* context,
        const std::filesystem::path& from,
        const std::filesystem::path& to,
        std::error_code& error
    ){};
};

/// Puts a finished file where it goes.
///
/// The file is renamed, or linked, where the volume allows. Across volumes, and on a file system
/// that cannot link, its bytes are copied (oa::platform::copy_file_contents), so that the copy is
/// made with the permissions of any new file and takes none from the file it copies, and the
/// original is removed once the copy is whole. A copy that fails leaves no file at the
/// destination and the original where it was.
///
/// With `replace`, a file that is there is replaced. Without it nothing is replaced: the file
/// takes the first name free, `to` itself or with " (2)", " (3)" and so on before its extension,
/// and its name is made exclusively, by a link or by a copy that is refused where the name is
/// taken, so that two copies of the game finishing at once never take one name.
///
/// @param from the finished file
/// @param to where it goes; its folder is made when it is missing
/// @param replace whether a file that is there is replaced
/// @param[out] placed where it went
/// @param[out] error why it could not be placed
/// @param hooks stand-ins for the file system's rename and link
/// @return false when it could not
[[nodiscard]] bool place_file(
    const std::filesystem::path& from,
    const std::filesystem::path& to,
    bool replace,
    std::filesystem::path& placed,
    std::string& error,
    const PlaceHooks& hooks = {}
);

} // namespace oa::session::demo
