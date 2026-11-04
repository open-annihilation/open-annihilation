// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Words the player reads. A 3.1c translation wins. Otherwise the Boneyards
// catalogue speaks, and otherwise the English in the source is shown.
#pragma once

#include "oa/boneyards/utf8.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/translation.hpp"

#include <cstring>
#include <optional>
#include <string>
#include <string_view>

namespace oa::boneyards {

/// A GUI caption holds this many bytes. A longer translation is not shown.
inline constexpr std::size_t k_shown_text_limit = 127;

/// Translates one line for a screen file.
///
/// A 3.1c translation is used when it fits a caption. Otherwise the catalogue
/// is used when it has this line. Nothing means the English stays.
///
/// @param english the line as the screen file writes it
/// @return the line to show, or nothing to keep `english`
[[nodiscard]] inline std::optional<std::string> translate_line(std::string_view english) {
    if (english.empty())
        return std::nullopt;
    if (auto game = oa::data::languages::translation_of(english)) {
        if (game->size() <= k_shown_text_limit)
            return std::move(*game);
    }
    const std::string_view own = oa::data::languages::interface_text(english);
    if (own != english)
        return std::string(own);
    return std::nullopt;
}

/// The words a player sees for one English line.
///
/// The pointer stays valid until the next call on this thread. Copy it before
/// calling again. A null or empty line is returned as it is.
///
/// @param english the line as the source writes it; may be null
/// @return the line to show; never null
inline const char* shown_text(const char* english) {
    static thread_local std::string storage;
    if (english == nullptr || english[0] == '\0')
        return english != nullptr ? english : "";
    // Copy first: `english` may point into `storage` from the previous call.
    const std::string source(english);
    if (auto game = oa::data::languages::translation_of(source)) {
        storage = std::move(*game);
        return storage.c_str();
    }
    const std::string_view own = oa::data::languages::interface_text(source);
    if (own != source) {
        storage.assign(own.begin(), own.end());
        return storage.c_str();
    }
    return english;
}

/// Copies a line into a field in the language the player is reading.
///
/// @param[out] out field receiving the line; null is ignored
/// @param capacity size of the field, terminator included
/// @param english the line as the source writes it; may be null
inline void copy_shown(char* out, std::size_t capacity, const char* english) {
    utf8::copy(out, capacity, shown_text(english), 0);
}

/// The printf conversions a template holds, in order, flags and width kept.
///
/// "%s, %d:%02d" gives "s|d|02d|". `%%` is text and is left out. A lone `%`
/// at the end is kept as `!`, so it never matches a whole template.
///
/// @param pattern a printf template
/// @return the conversions, each ended by `|`
[[nodiscard]] inline std::string printf_conversions(std::string_view pattern) {
    std::string kinds;
    for (std::size_t at = 0; at < pattern.size(); ++at) {
        if (pattern[at] != '%')
            continue;
        std::size_t end = at + 1;
        while (end < pattern.size() && pattern[end] != '\0' &&
               std::strchr("-+ #0123456789.*hljztL", pattern[end]) != nullptr)
            ++end;
        if (end >= pattern.size()) {
            kinds += '!';
            break;
        }
        if (pattern[end] != '%') {
            kinds.append(pattern.substr(at + 1, end - at));
            kinds += '|';
        }
        at = end;
    }
    return kinds;
}

/// Copies a printf template into a field in the language the player is reading.
///
/// A translation is used only when it takes the same arguments as the English,
/// after it is cut to the field. A language pack's mistake shows the English.
///
/// @param[out] out field receiving the template; null is ignored
/// @param capacity size of the field, terminator included
/// @param english the template as the source writes it; may be null
inline void copy_shown_pattern(char* out, std::size_t capacity, const char* english) {
    if (out == nullptr || capacity == 0)
        return;
    copy_shown(out, capacity, english);
    const char* source = english != nullptr ? english : "";
    if (printf_conversions(out) != printf_conversions(source))
        utf8::copy(out, capacity, source, 0);
}

} // namespace oa::boneyards
