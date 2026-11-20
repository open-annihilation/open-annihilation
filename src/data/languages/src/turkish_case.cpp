// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/languages/turkish_case.hpp"

#include "oa/data/languages/interface_text.hpp"

#include <string>
#include <string_view>

namespace oa::data::languages {

namespace {

constexpr char32_t smallest_two_byte = 0x80;
constexpr char32_t smallest_three_byte = 0x800;
constexpr char32_t smallest_four_byte = 0x10000;
constexpr char32_t last_code_point = 0x10FFFF;
constexpr char32_t first_surrogate = 0xD800;
constexpr char32_t last_surrogate = 0xDFFF;

constexpr char32_t dotted_capital_i = 0x0130; ///< İ
constexpr char32_t dotless_i = 0x0131;        ///< ı
constexpr char32_t c_cedilla = 0x00E7;        ///< ç
constexpr char32_t c_cedilla_capital = 0x00C7;
constexpr char32_t g_breve = 0x011F; ///< ğ
constexpr char32_t g_breve_capital = 0x011E;
constexpr char32_t o_diaeresis = 0x00F6; ///< ö
constexpr char32_t o_diaeresis_capital = 0x00D6;
constexpr char32_t s_cedilla = 0x015F; ///< ş
constexpr char32_t s_cedilla_capital = 0x015E;
constexpr char32_t u_diaeresis = 0x00FC; ///< ü
constexpr char32_t u_diaeresis_capital = 0x00DC;

/// Lowers one ASCII letter. Any other byte is kept.
///
/// @param byte the byte
/// @return the small letter
char lower_ascii(char byte) noexcept {
    const auto value = static_cast<unsigned char>(byte);
    if (value >= 'A' && value <= 'Z')
        return static_cast<char>(value - 'A' + 'a');
    return static_cast<char>(value);
}

} // namespace

bool interface_uses_turkish_case() noexcept {
    const std::string_view tag = interface_language().tag;
    char primary[2]{};
    std::size_t length = 0;
    for (const char byte : tag) {
        if (byte == '-' || byte == '_')
            break;
        if (length == 2)
            return false;
        primary[length] = lower_ascii(byte);
        ++length;
    }
    if (length != 2)
        return false;
    const bool turkish = primary[0] == 't' && primary[1] == 'r';
    const bool azerbaijani = primary[0] == 'a' && primary[1] == 'z';
    return turkish || azerbaijani;
}

Utf8Character leading_character(std::string_view text) noexcept {
    Utf8Character decoded;
    if (text.empty())
        return decoded;
    const auto at = [&text](std::size_t index) { return static_cast<unsigned char>(text[index]); };
    const unsigned char lead = at(0);
    std::size_t length = 0;
    char32_t code_point = 0;
    char32_t smallest = 0;
    if ((lead & 0xE0U) == 0xC0U) {
        length = 2;
        code_point = lead & 0x1FU;
        smallest = smallest_two_byte;
    } else if ((lead & 0xF0U) == 0xE0U) {
        length = 3;
        code_point = lead & 0x0FU;
        smallest = smallest_three_byte;
    } else if ((lead & 0xF8U) == 0xF0U) {
        length = 4;
        code_point = lead & 0x07U;
        smallest = smallest_four_byte;
    } else {
        decoded.character = lead;
        decoded.bytes = 1;
        return decoded;
    }
    if (text.size() < length) {
        decoded.character = lead;
        decoded.bytes = 1;
        return decoded;
    }
    for (std::size_t index = 1; index < length; ++index) {
        const unsigned char byte = at(index);
        if ((byte & 0xC0U) != 0x80U) {
            decoded.character = lead;
            decoded.bytes = 1;
            return decoded;
        }
        code_point = (code_point << 6) | (byte & 0x3FU);
    }
    if (code_point < smallest || code_point > last_code_point ||
        (code_point >= first_surrogate && code_point <= last_surrogate)) {
        decoded.character = lead;
        decoded.bytes = 1;
        return decoded;
    }
    decoded.character = code_point;
    decoded.bytes = static_cast<uint8_t>(length);
    return decoded;
}

void append_character(std::string& text, char32_t character) {
    const auto push = [&text](uint32_t value) { text.push_back(static_cast<char>(value)); };
    const auto value = static_cast<uint32_t>(character);
    if (value < smallest_two_byte) {
        push(value);
    } else if (value < smallest_three_byte) {
        push(0xC0U | (value >> 6));
        push(0x80U | (value & 0x3FU));
    } else if (value < smallest_four_byte) {
        if (value >= first_surrogate && value <= last_surrogate)
            return;
        push(0xE0U | (value >> 12));
        push(0x80U | ((value >> 6) & 0x3FU));
        push(0x80U | (value & 0x3FU));
    } else if (value <= last_code_point) {
        push(0xF0U | (value >> 18));
        push(0x80U | ((value >> 12) & 0x3FU));
        push(0x80U | ((value >> 6) & 0x3FU));
        push(0x80U | (value & 0x3FU));
    }
}

bool small_turkish_letter(char32_t character) noexcept {
    if (character >= U'a' && character <= U'z')
        return true;
    switch (character) {
    case c_cedilla:
    case g_breve:
    case dotless_i:
    case o_diaeresis:
    case s_cedilla:
    case u_diaeresis:
        return true;
    default:
        return false;
    }
}

bool capital_turkish_letter(char32_t character) noexcept {
    if (character >= U'A' && character <= U'Z')
        return true;
    switch (character) {
    case c_cedilla_capital:
    case g_breve_capital:
    case dotted_capital_i:
    case o_diaeresis_capital:
    case s_cedilla_capital:
    case u_diaeresis_capital:
        return true;
    default:
        return false;
    }
}

char32_t turkish_capital(char32_t character) noexcept {
    if (character == U'i')
        return dotted_capital_i;
    if (character == dotless_i)
        return U'I';
    if (character >= U'a' && character <= U'z')
        return static_cast<char32_t>(character - U'a' + U'A');
    switch (character) {
    case c_cedilla:
        return c_cedilla_capital;
    case g_breve:
        return g_breve_capital;
    case o_diaeresis:
        return o_diaeresis_capital;
    case s_cedilla:
        return s_cedilla_capital;
    case u_diaeresis:
        return u_diaeresis_capital;
    default:
        return character;
    }
}

char32_t turkish_small(char32_t character) noexcept {
    if (character == U'I')
        return dotless_i;
    if (character == dotted_capital_i)
        return U'i';
    if (character >= U'A' && character <= U'Z')
        return static_cast<char32_t>(character - U'A' + U'a');
    switch (character) {
    case c_cedilla_capital:
        return c_cedilla;
    case g_breve_capital:
        return g_breve;
    case o_diaeresis_capital:
        return o_diaeresis;
    case s_cedilla_capital:
        return s_cedilla;
    case u_diaeresis_capital:
        return u_diaeresis;
    default:
        return character;
    }
}

} // namespace oa::data::languages
