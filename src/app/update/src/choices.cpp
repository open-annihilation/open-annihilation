// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The player's standing choices about an update, as preference values.
#include "oa/app/update/choices.hpp"

#include "oa/ui/engine_settings.hpp"

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace oa::app::update {
namespace {

/// Reads a whole release number under `name`.
///
/// @param values the preference values
/// @param name the key
/// @return the number, or nothing when it is absent or not a whole number
std::optional<int64_t>
read_release(const platform::preferences::Values& values, std::string_view name) {
    const auto found = values.find(std::string(name));
    if (found == values.end() || found->second.empty())
        return std::nullopt;
    int64_t number = 0;
    const char* const begin = found->second.data();
    const char* const end = begin + found->second.size();
    const std::from_chars_result parsed = std::from_chars(begin, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end)
        return std::nullopt;
    return number;
}

} // namespace

InstallPolicy read_install_policy(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(key::install_policy));
    if (found != values.end() && found->second == "when-i-quit")
        return InstallPolicy::when_i_quit;
    return InstallPolicy::ask;
}

void write_install_policy(platform::preferences::Values& values, InstallPolicy policy) {
    values[std::string(key::install_policy)] =
        policy == InstallPolicy::when_i_quit ? "when-i-quit" : "ask";
}

std::optional<int64_t> read_scheduled(const platform::preferences::Values& values) {
    return read_release(values, key::install_on_quit);
}

void write_scheduled(platform::preferences::Values& values, int64_t release) {
    values[std::string(key::install_on_quit)] = std::to_string(release);
}

void clear_scheduled(platform::preferences::Values& values) {
    values.erase(std::string(key::install_on_quit));
}

bool installs_at_quit(const platform::preferences::Values& values, int64_t ready_release) {
    const std::optional<int64_t> scheduled = read_scheduled(values);
    return scheduled.has_value() && *scheduled == ready_release && ready_release > 0;
}

std::optional<int64_t> read_held(const platform::preferences::Values& values) {
    return read_release(values, key::held);
}

void write_held(platform::preferences::Values& values, int64_t release) {
    values[std::string(key::held)] = std::to_string(release);
}

void clear_held(platform::preferences::Values& values) {
    values.erase(std::string(key::held));
}

bool release_held(const platform::preferences::Values& values, int64_t release) {
    const std::optional<int64_t> held = read_held(values);
    return held.has_value() && release <= *held;
}

void clear_skipped(platform::preferences::Values& values) {
    values.erase(std::string(oa::ui::engine_settings::key::update_skipped));
}

} // namespace oa::app::update
