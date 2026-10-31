// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which install layout this machine can use. The self-check folder, the
// two Windows layouts and a Linux versions folder are answered here, and
// macOS replaces a signed bundle where it stands. Nothing here starts a
// program. The writable check creates and deletes one scratch file.
#pragma once

#include "oa/app/update/build.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/data/engine_feed/feed.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app::update {

/// Shown once when this copy is installed for the whole system.
inline constexpr std::string_view whole_system_copy =
    "This copy is installed for the whole system, so it cannot replace itself. Install the new "
    "release the way this copy was installed.";

/// Shown on a Steam Deck, and by the helper, when the update waits for the next start.
/// {version} is the release being installed.
inline constexpr std::string_view installs_next_start =
    "{version} installs the next time you start Open Annihilation.";

/// What the machine, and the options of this run, say about an install.
///
/// A test passes any platform's facts. `this_machine` fills the ones this
/// process can see, and leaves the option, the test feed and the offer's
/// way for the caller to set.
struct RouteFacts {
    std::filesystem::path executable;     ///< the program to start again
    std::filesystem::path program_folder; ///< the folder that holds that program
    bool folder_writable{false};          ///< the program folder accepted a scratch file
    bool uninstall_beside{false};         ///< Uninstall.exe lies beside the program
    std::string install_location;         ///< the uninstall key's folder; empty when absent
    bool owned_by_player{true};           ///< the install's owner is this user
    bool appimage{false};                 ///< APPIMAGE is set
    bool flatpak{false};                  ///< FLATPAK_ID is set
    bool launcher_present{false};         ///< the versions layout's launcher is there
    std::string platform;                 ///< the package platform name
    std::optional<std::filesystem::path> install_folder; ///< `--update-install-folder`, when set
    bool test_feed{false};                               ///< this run fetched a test feed
    oa::data::engine_feed::Way way{
        oa::data::engine_feed::Way::none
    }; ///< how the release is offered
    /// The application bundle, when the executable sits in `Contents/MacOS`.
    /// Empty when this program is not a bundle.
    std::filesystem::path bundle;
    /// macOS is running the bundle from a protected place.
    bool translocated{false};
    /// The signing team of the running app. Empty for an ad hoc signature,
    /// and empty when this program is not a bundle.
    std::string signing_team;
    /// This build is a self-check build. A test sets it directly.
    bool self_check{false};
    /// A check that must be told there is no install route. A player run
    /// leaves this false, and it wins over every route.
    bool no_install_route{false};
};

/// The route a download may use, or why it may not.
struct Where {
    std::optional<Route> route;    ///< set when this install can be replaced
    std::filesystem::path install; ///< the folder or bundle the route replaces
    std::filesystem::path program; ///< the program to start afterwards
    /// The feed's installer: on Windows for an installed copy.
    bool installer{false};
    std::string why; ///< why there is no route; empty when there is one
};

/// The route for this build on these facts.
///
/// A release offered on its page has none. A development build has none
/// unless a test feed is in use. A check that sets `no_install_route` has
/// none on every platform, including the self-check folder. Otherwise the
/// self-check install folder selects `check-folder`. A Windows platform
/// selects `windows-installer` when Uninstall.exe lies beside the program
/// and the uninstall key names that folder, and `windows-folder` otherwise.
/// On macOS, a bundle that is not running from a protected place and is
/// signed with a team uses `macos-bundle` with the zip, wherever it is and
/// whoever owns it: the helper asks for an administrator's password when
/// the player cannot make the moves. An ad hoc signature has no route,
/// except a self-check run with a test feed. A Linux install in a
/// folder the player owns selects `linux-versions`, unless it lives under
/// `/opt`, `/usr`, `/snap` or `/var/lib/flatpak`, or `APPIMAGE` or
/// `FLATPAK_ID` is set: `why` is then the whole-system sentence. Every other
/// platform is still without a route of its own: `why` is "no route for this
/// platform yet".
///
/// @param build the build that is running
/// @param facts what the machine and this run say
/// @return the route, or why there is none
[[nodiscard]] Where find_route(const ThisBuild& build, const RouteFacts& facts);

/// The facts this process can see for itself.
///
/// The executable is this process's own file. The program folder is the one
/// passed in. Writable means a scratch file could be created there and
/// deleted. On Windows, Uninstall.exe beside the program and the uninstall
/// key's InstallLocation are filled in. On macOS the bundle, the
/// protected-place flag and the signing team are filled from this process. The install's owner,
/// `APPIMAGE`, `FLATPAK_ID` and the versions-layout launcher are filled in
/// for a Linux copy. The install folder, the test feed and the offer's way
/// are left unset, for the caller to fill before `find_route`.
///
/// @param program_folder the folder that holds the program
/// @return the facts
[[nodiscard]] RouteFacts this_machine(const std::filesystem::path& program_folder);

/// True when two install locations name the same folder.
///
/// Empty text does not match, including two empty strings. Slashes and
/// backslashes are the same, quotes are dropped, `.` and `..` are folded,
/// a trailing separator is dropped except on a drive root, and ASCII case
/// does not matter.
///
/// @param left one location
/// @param right the other
/// @return true when both name the same folder
[[nodiscard]] bool same_install_folder(std::string_view left, std::string_view right);

} // namespace oa::app::update
