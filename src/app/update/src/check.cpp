// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The preference values that say when to look for a new release.
#include "oa/app/update/check.hpp"

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

std::optional<int64_t> read_checked(const platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::update_checked));
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

void write_checked(platform::preferences::Values& values, int64_t checked) {
    values[std::string(settings::key::update_checked)] = std::to_string(checked);
}

bool check_due(CheckMode mode, std::optional<int64_t> checked, int64_t now) noexcept {
    if (mode != CheckMode::automatically)
        return false;
    if (!checked)
        return true;
    if (*checked > now)
        return true;
    return now - *checked >= check_day_seconds;
}

} // namespace oa::app::update
