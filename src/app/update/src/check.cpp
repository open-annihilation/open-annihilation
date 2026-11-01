// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The preference values that say when to look for a new release, which feed
// to read and when a check was last reported, and the hour counted from when
// this run's automatic check began.
#include "oa/app/update/check.hpp"

#include "oa/data/engine_feed/feed.hpp"
#include "oa/ui/engine_settings.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::update {
namespace {

namespace settings = oa::ui::engine_settings;

/// The stored word for a mode.
///
/// @param mode the mode
/// @return `automatically`, `when-asked` or `never`
[[nodiscard]] std::string_view mode_text(CheckMode mode) noexcept {
    if (mode == CheckMode::when_asked)
        return "when-asked";
    if (mode == CheckMode::never)
        return "never";
    return "automatically";
}

/// Reads a whole number of seconds stored under `key`.
///
/// @param values the preference values
/// @param key the key
/// @return the number, or nothing when it is absent or not a whole number
[[nodiscard]] std::optional<int64_t>
stored_seconds(const platform::preferences::Values& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    if (found == values.end() || found->second.empty())
        return std::nullopt;
    int64_t value = 0;
    const char* const begin = found->second.data();
    const char* const end = begin + found->second.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return value;
}

} // namespace

CheckMode read_check_mode(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::update_check));
    if (found == values.end())
        return CheckMode::automatically;
    if (found->second == "when-asked")
        return CheckMode::when_asked;
    if (found->second == "never")
        return CheckMode::never;
    return CheckMode::automatically;
}

void write_check_mode(platform::preferences::Values& values, CheckMode mode) {
    values[std::string(settings::key::update_check)] = std::string(mode_text(mode));
}

Channel read_channel(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::update_channel));
    if (found != values.end() && found->second == channel_name(Channel::testing))
        return Channel::testing;
    return Channel::stable;
}

std::string_view channel_name(Channel channel) noexcept {
    if (channel == Channel::testing)
        return oa::data::engine_feed::testing_channel;
    return oa::data::engine_feed::stable_channel;
}

std::optional<Channel> parse_channel(std::string_view text) noexcept {
    if (text == channel_name(Channel::stable))
        return Channel::stable;
    if (text == channel_name(Channel::testing))
        return Channel::testing;
    return std::nullopt;
}

Channel run_channel(std::string_view option, const platform::preferences::Values& values) {
    if (const std::optional<Channel> named = parse_channel(option))
        return *named;
    return read_channel(values);
}

std::string channel_feed_address(std::string_view address, Channel channel) {
    if (channel == Channel::stable)
        return std::string(address);
    // The testing feed is the file beside the stable one.
    const std::size_t slash = address.rfind('/');
    std::string folder =
        slash == std::string_view::npos ? std::string{} : std::string(address.substr(0, slash + 1));
    folder += channel_name(channel);
    folder += "-2.json";
    return folder;
}

std::optional<int64_t> read_checked(const platform::preferences::Values& values) {
    return stored_seconds(values, settings::key::update_checked);
}

void write_checked(platform::preferences::Values& values, int64_t checked) {
    values[std::string(settings::key::update_checked)] = std::to_string(checked);
}

std::optional<int64_t> read_pinged(const platform::preferences::Values& values) {
    return stored_seconds(values, settings::key::update_pinged);
}

void write_pinged(platform::preferences::Values& values, int64_t pinged) {
    values[std::string(settings::key::update_pinged)] = std::to_string(pinged);
}

bool ping_due(std::optional<int64_t> pinged, int64_t now) noexcept {
    if (!pinged)
        return true;
    // A report in the future is a clock set back; it does not stop the next.
    if (*pinged > now)
        return true;
    return now - *pinged >= ping_interval_seconds;
}

bool check_due(
    CheckMode mode, std::optional<int64_t> /*checked*/, std::optional<int64_t> began, int64_t now
) noexcept {
    if (mode != CheckMode::automatically)
        return false;
    // The first of a run does not wait on the last check that reached the server.
    if (!began)
        return true;
    if (*began > now)
        return true;
    return now - *began >= check_interval_seconds;
}

bool automatic_check_starts(
    CheckMode mode,
    std::optional<int64_t> checked,
    std::optional<int64_t> began,
    int64_t now,
    CheckPlace place,
    bool running
) noexcept {
    if (running)
        return false;
    if (!check_due(mode, checked, began, now))
        return false;
    // The first of a run waits for the main menu. An hour later, anywhere.
    if (!began && place != CheckPlace::menu)
        return false;
    return true;
}

bool update_notice_shows(CheckPlace place) noexcept {
    return place == CheckPlace::menu;
}

} // namespace oa::app::update
