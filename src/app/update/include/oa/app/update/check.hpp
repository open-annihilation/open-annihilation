// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// When Open Annihilation looks for a new release, and which feed it asks.
// The mode, the channel, the time of the last check that reached the server
// and the time of the last report of a check are preference values. An hour
// is counted from when this run's automatic check began. Nothing here
// fetches.
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
    automatically, ///< at every start, and every hour while the game runs
    when_asked,    ///< only when the player asks
    never,         ///< no request is ever made
};

/// Where the player is, for when an automatic check may start and when a
/// release may be shown.
enum class CheckPlace : uint8_t {
    menu,      ///< the main menu, including under a dialog
    match,     ///< a match
    elsewhere, ///< another screen
};

/// The published stable feed. A test feed replaces it for one run. Copies
/// before 0.8.12 read `stable.json` in the same folder, which never names a
/// package those copies' helper cannot install.
inline constexpr std::string_view feed_address =
    "http://downloads.coreprime.net/v1/engine/stable-2.json";

/// Seconds between automatic checks of one run.
inline constexpr int64_t check_interval_seconds = 3600;

/// The feed a check reads. Testing offers a release before Stable does, to
/// the copies whose player chose it. Its feed sits beside the stable one and
/// is signed by the same keys.
enum class Channel : uint8_t {
    stable,  ///< `stable-2.json`, which every copy reads unless told otherwise
    testing, ///< `testing-2.json`, in the same folder
};

/// Reads Update channel.
///
/// Absent, or any text other than `testing`, is Stable.
///
/// @param values the preference values
/// @return the channel
[[nodiscard]] Channel read_channel(const platform::preferences::Values& values);

/// The channel's name: the word the preference and the launch option use,
/// and the `channel` its feed names.
///
/// @param channel the channel
/// @return `stable` or `testing`
[[nodiscard]] std::string_view channel_name(Channel channel) noexcept;

/// Reads a channel's name.
///
/// @param text `stable` or `testing`
/// @return the channel, or nothing for any other text
[[nodiscard]] std::optional<Channel> parse_channel(std::string_view text) noexcept;

/// The channel this run reads: the launch option's when the run was given
/// one, and Update channel otherwise.
///
/// @param option `--update-channel`'s value; empty when the run was given none
/// @param values the preference values
/// @return the channel
[[nodiscard]] Channel
run_channel(std::string_view option, const platform::preferences::Values& values);

/// The feed a channel reads.
///
/// Stable reads `address` as it is. Testing reads `testing-2.json` in the
/// folder that holds `address`: beside the published stable feed, or beside
/// a test feed.
///
/// @param address the stable feed's address, with no query
/// @param channel the channel
/// @return the address the check fetches; its signature is this plus `.sig`
[[nodiscard]] std::string channel_feed_address(std::string_view address, Channel channel);

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

/// Seconds that pass before a check is reported again.
inline constexpr int64_t ping_interval_seconds = 3600;

/// Reads the time a check was last reported.
///
/// The value is seconds since 1970. Absent, or any text that is not a whole
/// number, means no check has been reported.
///
/// @param values the preference values
/// @return the time, or nothing
[[nodiscard]] std::optional<int64_t> read_pinged(const platform::preferences::Values& values);

/// Writes the time a check was reported.
///
/// @param[in,out] values the preference values
/// @param pinged seconds since 1970
void write_pinged(platform::preferences::Values& values, int64_t pinged);

/// Tells whether a check may be reported now.
///
/// It may when none has been reported, when `ping_interval_seconds` have
/// passed since the last report, or when that report's time is in the
/// future, so a clock set back does not stop reports for good.
///
/// @param pinged the last report, or nothing
/// @param now seconds since 1970
/// @return true when the check may be reported
[[nodiscard]] bool ping_due(std::optional<int64_t> pinged, int64_t now) noexcept;

/// Tells whether an automatic check is due.
///
/// Due only for Automatically. The first of a run (`began` empty) is due
/// whatever `checked` says, including a check that reached the server a
/// minute ago. Another is due `check_interval_seconds` after `began`, when
/// the last automatic check of this run began, including one that failed.
/// A `began` in the future is due, so a clock set back does not wait out
/// the hour. Only when I ask, and Never, are never due. `checked` is the
/// last check that reached the server and does not set the hour.
///
/// @param mode the mode
/// @param checked the last check that reached the server, or nothing
/// @param began when the last automatic check of this run began, or nothing
/// @param now seconds since 1970
/// @return true when an automatic check should run
[[nodiscard]] bool check_due(
    CheckMode mode, std::optional<int64_t> checked, std::optional<int64_t> began, int64_t now
) noexcept;

/// Tells whether this frame should start an automatic check.
///
/// The first of a run starts only at the main menu. An hour after `began`,
/// another starts at a menu, in a match or on another screen. One already
/// running is left to finish. Only when I ask, and Never, never start.
///
/// @param mode the mode
/// @param checked the last check that reached the server, or nothing
/// @param began when the last automatic check of this run began, or nothing
/// @param now seconds since 1970
/// @param place where the player is
/// @param running true while a check is still in progress
/// @return true when an automatic check should start
[[nodiscard]] bool automatic_check_starts(
    CheckMode mode,
    std::optional<int64_t> checked,
    std::optional<int64_t> began,
    int64_t now,
    CheckPlace place,
    bool running
) noexcept;

/// Tells whether a release may be shown here.
///
/// A new release is shown on the main menu, as that menu shows one today,
/// and never over a match. One found during a match waits for the next main
/// menu.
///
/// @param place where the player is
/// @return true when the main menu may show the release
[[nodiscard]] bool update_notice_shows(CheckPlace place) noexcept;

/// One key that may sign a test feed: an id and `ed25519:` followed by the key.
struct FeedKey {
    std::string id;   ///< the key's id
    std::string text; ///< the key's ed25519 text
};

/// Where a check fetches the feed, which keys may sign it, and the channel
/// the feed must name.
///
/// An empty `keys` means the engine's pinned keys. `test` is a feed given
/// for one run, which may be checked in an unattended or headless run.
struct Source {
    std::string feed_url{
        std::string(feed_address)
    }; ///< the feed; the signature is this plus `.sig`
    std::vector<FeedKey> keys;        ///< empty means the pinned keys
    bool test{};                      ///< a feed given for this run
    Channel channel{Channel::stable}; ///< the channel the feed must name
};

} // namespace oa::app::update
