// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's standing choices about an update, kept as preference values:
// a release scheduled to install when the game quits, a release held after
// it failed to install, how a ready release is installed, and taking back a
// skip. Download and install are separate decisions. Nothing here fetches or
// installs.
#pragma once

#include "oa/platform/preferences.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::app::update {

namespace key {

/// The release number that installs the next time the game quits. Absent, or
/// any text that is not a whole number, schedules nothing.
inline constexpr std::string_view install_on_quit = "open-annihilation.update-install-on-quit";

/// The release number of a release that did not install. It is not offered,
/// downloaded or installed by itself until the player tries it again.
/// Absent, or any text that is not a whole number, holds nothing.
inline constexpr std::string_view held = "open-annihilation.update-held";

/// `ask` or `when-i-quit`. How a release that is ready is installed. Absent,
/// or any other text, reads as `ask`.
inline constexpr std::string_view install_policy = "open-annihilation.update-install";

} // namespace key

/// How a release that has been downloaded and checked is installed.
enum class InstallPolicy : uint8_t {
    ask,         ///< the ready prompt asks; nothing installs until the player chooses
    when_i_quit, ///< a ready release is scheduled to install when the game quits
};

/// Reads Install updates.
///
/// @param values the preference values
/// @return the policy; anything but `when-i-quit` is ask
[[nodiscard]] InstallPolicy read_install_policy(const platform::preferences::Values& values);

/// Writes Install updates.
///
/// @param[in,out] values the preference values
/// @param policy the policy
void write_install_policy(platform::preferences::Values& values, InstallPolicy policy);

/// Reads the release scheduled to install when the game quits.
///
/// @param values the preference values
/// @return the release number, or nothing
[[nodiscard]] std::optional<int64_t> read_scheduled(const platform::preferences::Values& values);

/// Schedules a release to install when the game quits.
///
/// @param[in,out] values the preference values
/// @param release the release number
void write_scheduled(platform::preferences::Values& values, int64_t release);

/// Takes back a scheduled install. Quitting then installs nothing.
///
/// @param[in,out] values the preference values
void clear_scheduled(platform::preferences::Values& values);

/// Tells whether quitting installs a ready release.
///
/// Only a release scheduled for that number installs. Install updates set to
/// install when I quit schedules a release as it becomes ready, so this
/// reads the schedule alone.
///
/// @param values the preference values
/// @param ready_release the release whose package is ready
/// @return true when quitting should hand that release to the helper
[[nodiscard]] bool
installs_at_quit(const platform::preferences::Values& values, int64_t ready_release);

/// Reads the release held after it failed to install.
///
/// @param values the preference values
/// @return the release number, or nothing
[[nodiscard]] std::optional<int64_t> read_held(const platform::preferences::Values& values);

/// Holds a release that failed to install until the player tries it again.
///
/// @param[in,out] values the preference values
/// @param release the release number
void write_held(platform::preferences::Values& values, int64_t release);

/// Lets a held release be offered and installed again: the player chose Try again.
///
/// @param[in,out] values the preference values
void clear_held(platform::preferences::Values& values);

/// Tells whether a release is held.
///
/// The held release and every older one wait for Try again. A newer release
/// is not held.
///
/// @param values the preference values
/// @param release the release number
/// @return true when that release waits for Try again
[[nodiscard]] bool release_held(const platform::preferences::Values& values, int64_t release);

/// Takes back a skip: the player chose Try this version.
///
/// The skipped number is removed, so the release that is out asks again.
///
/// @param[in,out] values the preference values
void clear_skipped(platform::preferences::Values& values);

} // namespace oa::app::update
