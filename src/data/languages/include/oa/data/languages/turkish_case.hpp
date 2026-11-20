// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How Turkish and Azerbaijani write a capital i. While the interface is in
// either language, a small i becomes İ and a small ı becomes I, and a
// capital I becomes ı. ç, ğ, ö, ş and ü are read as letters, not as bytes.
// Every other language is left to the caller, which keeps its ASCII case.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::data::languages {

/// One character at the start of UTF-8 text.
struct Utf8Character {
    char32_t character{}; ///< the character; the first byte when it is not well-formed UTF-8
    uint8_t bytes{};      ///< how many bytes it takes; 0 when the text is empty
};

/// Tells whether the interface language writes i the Turkish way.
///
/// Turkish and Azerbaijani do, including a tag whose first part is `tr` or
/// `az` (`tr-TR`, `az-Latn`). Any other language does not.
///
/// @return true for those two languages
[[nodiscard]] bool interface_uses_turkish_case() noexcept;

/// Reads the first character of UTF-8 text.
///
/// A well-formed sequence of two to four bytes is one character. Anything
/// else, an ASCII byte among it, is one byte, so a caller that walks with
/// this and leaves non-letters unchanged agrees with a walk over bytes.
///
/// @param text the text
/// @return the character; no bytes when `text` is empty
[[nodiscard]] Utf8Character leading_character(std::string_view text) noexcept;

/// Appends a character as UTF-8.
///
/// A surrogate, or a value past the last Unicode character, appends nothing.
///
/// @param[in,out] text the text
/// @param character the character
void append_character(std::string& text, char32_t character);

/// Tells whether a character is a small letter Turkish case capitalises.
///
/// The small ASCII letters, and ç ğ ı ö ş ü.
///
/// @param character the character
/// @return true for one of those letters
[[nodiscard]] bool small_turkish_letter(char32_t character) noexcept;

/// Tells whether a character is a capital Turkish case makes small.
///
/// The capital ASCII letters, and Ç Ğ İ Ö Ş Ü.
///
/// @param character the character
/// @return true for one of those letters
[[nodiscard]] bool capital_turkish_letter(char32_t character) noexcept;

/// Returns the capital a small letter has in Turkish and Azerbaijani.
///
/// i becomes İ and ı becomes I. Any other character stays.
///
/// @param character the character
/// @return its capital, or itself
[[nodiscard]] char32_t turkish_capital(char32_t character) noexcept;

/// Returns the small letter a capital has in Turkish and Azerbaijani.
///
/// I becomes ı and İ becomes i. Any other character stays.
///
/// @param character the character
/// @return its small letter, or itself
[[nodiscard]] char32_t turkish_small(char32_t character) noexcept;

} // namespace oa::data::languages
