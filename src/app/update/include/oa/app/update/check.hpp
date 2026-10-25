// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When Open Annihilation looks for a new release, and which feed it asks.
// The mode and the time of the last check that reached the server are
// preference values. Nothing here fetches.
#pragma once

#include "oa/platform/preferences.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {

/// How often the game looks for a new release of itself.
enum class CheckMode : uint8_t {
    automatically, ///< at the settled main menu, at most once a day
    when_asked,    ///< only when the player asks
    never,         ///< no request is ever made
};

/// The published stable feed. A test feed replaces it for one run.
inline constexpr std::string_view feed_address =
    "http://downloads.coreprime.net/v1/engine/stable.json";

/// Seconds since 1970 between automatic checks that reached the server.
inline constexpr int64_t check_day_seconds = 86400;

/// Reads Check for updates.
///
/// Absent, or any text other than `when-asked` and `never`, is Automatically.
///
/// @param values the preference values
/// @return the mode
[[nodiscard]] CheckMode read_check_mode(const platform::preferences::Values& values);

/// Writes Check for updates.
///
/// @param[in,out] values the preference values
/// @param mode the mode
void write_check_mode(platform::preferences::Values& values, CheckMode mode);

/// Reads the time of the last check that reached the server.
///
/// The value is seconds since 1970. Absent, or any text that is not a whole
/// number, means a check has never reached the server.
///
/// @param values the preference values
/// @return the time, or nothing
[[nodiscard]] std::optional<int64_t> read_checked(const platform::preferences::Values& values);

/// Writes the time of a check that reached the server.
///
/// @param[in,out] values the preference values
/// @param checked seconds since 1970
void write_checked(platform::preferences::Values& values, int64_t checked);

/// Tells whether an automatic check is due.
///
/// Due only when the mode is Automatically and the last check is absent, at
/// least a day old, or in the future. Only when I ask, and Never, are never due.
///
/// @param mode the mode
/// @param checked the last check that reached the server, or nothing
/// @param now seconds since 1970
/// @return true when an automatic check should run
[[nodiscard]] bool check_due(CheckMode mode, std::optional<int64_t> checked, int64_t now) noexcept;

/// One key that may sign a test feed: an id and `ed25519:` followed by the key.
struct FeedKey {
    std::string id;   ///< the key's id
    std::string text; ///< the key's ed25519 text
};

/// Where a check fetches the feed, and which keys may sign it.
///
/// An empty `keys` means the engine's pinned keys. `test` is a feed given
/// for one run, which may be checked in an unattended or headless run.
struct Source {
    std::string feed_url{
        std::string(feed_address)
    }; ///< the feed; the signature is this plus `.sig`
    std::vector<FeedKey> keys; ///< empty means the pinned keys
    bool test{};               ///< a feed given for this run
};

} // namespace oa::app::update
