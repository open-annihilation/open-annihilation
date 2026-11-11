// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/present/photo/photo_file_name.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::present::photo {
namespace {

/// One UTF-8 character of a map name, kept whole while the name is shortened.
struct Piece {
    std::string bytes;
    char32_t code = 0;
};

/// Reads one UTF-8 character. A byte that does not begin a well-formed
/// sequence stands alone, as the character of that value.
///
/// @param text the map
/// @param at the first byte
/// @return the character, or an empty piece at the end
Piece read_piece(std::string_view text, std::size_t at) {
    Piece piece;
    if (at >= text.size())
        return piece;
    const auto lead = static_cast<unsigned char>(text[at]);
    std::size_t need = 1;
    char32_t code = lead;
    if (lead < 0x80) {
        need = 1;
        code = lead;
    } else if ((lead & 0xe0) == 0xc0 && lead >= 0xc2) {
        need = 2;
        code = lead & 0x1f;
    } else if ((lead & 0xf0) == 0xe0) {
        need = 3;
        code = lead & 0x0f;
    } else if ((lead & 0xf8) == 0xf0 && lead <= 0xf4) {
        need = 4;
        code = lead & 0x07;
    }
    if (need > 1 && at + need <= text.size()) {
        bool well = true;
        for (std::size_t index = 1; index < need; ++index) {
            const auto next = static_cast<unsigned char>(text[at + index]);
            if ((next & 0xc0) != 0x80) {
                well = false;
                break;
            }
            code = (code << 6) | (next & 0x3f);
        }
        const bool overlong = (need == 2 && code < 0x80) || (need == 3 && code < 0x800) ||
                              (need == 4 && code < 0x10000);
        const bool too_large = code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff);
        if (well && !overlong && !too_large) {
            piece.bytes.assign(text.data() + at, need);
            piece.code = code;
            return piece;
        }
    }
    piece.bytes.assign(1, static_cast<char>(lead));
    piece.code = lead;
    return piece;
}

/// Tells whether a character cannot stand in a file name.
///
/// @param code the character
/// @return true for a control or for `<>:"/\\|?*`
bool refused(char32_t code) {
    if (code < 0x20 || code == 0x7f)
        return true;
    switch (code) {
    case '<':
    case '>':
    case ':':
    case '"':
    case '/':
    case '\\':
    case '|':
    case '?':
    case '*':
        return true;
    default:
        return false;
    }
}

/// The map as characters, with refused characters and a trailing run of dots
/// and spaces written as spaces.
///
/// @param map_name the map
/// @return the characters
std::vector<Piece> sanitized_map(std::string_view map_name) {
    std::vector<Piece> pieces;
    for (std::size_t at = 0; at < map_name.size();) {
        Piece piece = read_piece(map_name, at);
        if (piece.bytes.empty())
            break;
        at += piece.bytes.size();
        if (refused(piece.code)) {
            piece.bytes = " ";
            piece.code = ' ';
        }
        pieces.push_back(std::move(piece));
    }
    for (std::size_t index = pieces.size(); index > 0; --index) {
        const char32_t code = pieces[index - 1].code;
        if (code != '.' && code != ' ')
            break;
        pieces[index - 1].bytes = " ";
        pieces[index - 1].code = ' ';
    }
    return pieces;
}

/// Appends two digits.
///
/// @param[in,out] text the name
/// @param value a number from 0 to 99
void append_two(std::string& text, int value) {
    const int tens = value / 10;
    const int ones = value % 10;
    text.push_back(static_cast<char>('0' + tens));
    text.push_back(static_cast<char>('0' + ones));
}

/// The date and time, `Photo YYYY-MM-DD HH.MM.SS`.
///
/// @param moment the local date and time
/// @return the prefix
std::string prefix_of(const LocalMoment& moment) {
    std::string text = "Photo ";
    const int year = moment.year < 0 ? 0 : moment.year;
    text.push_back(static_cast<char>('0' + (year / 1000) % 10));
    text.push_back(static_cast<char>('0' + (year / 100) % 10));
    text.push_back(static_cast<char>('0' + (year / 10) % 10));
    text.push_back(static_cast<char>('0' + year % 10));
    text.push_back('-');
    append_two(text, moment.month);
    text.push_back('-');
    append_two(text, moment.day);
    text.push_back(' ');
    append_two(text, moment.hour);
    text.push_back('.');
    append_two(text, moment.minute);
    text.push_back('.');
    append_two(text, moment.second);
    return text;
}

/// Joins the characters of the map.
///
/// @param pieces the map
/// @return the map's text
std::string join_map(const std::vector<Piece>& pieces) {
    std::string map;
    for (const Piece& piece : pieces)
        map += piece.bytes;
    return map;
}

#ifdef _WIN32
/// How many UTF-16 units one character takes.
///
/// @param code the character
/// @return 2 above U+FFFF, otherwise 1
std::size_t utf16_units(char32_t code) noexcept {
    return code > 0xffff ? 2 : 1;
}
#endif

} // namespace

std::size_t path_characters(std::string_view text) noexcept {
#ifdef _WIN32
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size();) {
        const Piece piece = read_piece(text, at);
        if (piece.bytes.empty())
            break;
        at += piece.bytes.size();
        count += utf16_units(piece.code);
    }
    return count;
#else
    return text.size();
#endif
}

std::string photo_file_name(
    const LocalMoment& moment,
    std::string_view map_name,
    int copy,
    std::size_t folder_characters,
    std::size_t path_budget
) {
    const std::string prefix = prefix_of(moment);
    std::string suffix;
    if (copy >= 2) {
        suffix.push_back(' ');
        suffix += std::to_string(copy);
    }
    suffix += ".png";
    std::vector<Piece> pieces = sanitized_map(map_name);
    const auto fits = [&](const std::string& name) {
        return folder_characters + 1 + path_characters(name) <= path_budget;
    };
    std::string name;
    for (;;) {
        const std::string map = join_map(pieces);
        name = prefix;
        if (!map.empty()) {
            name.push_back(' ');
            name += map;
        }
        name += suffix;
        if (fits(name) || pieces.empty())
            break;
        pieces.pop_back();
    }
    return name;
}

} // namespace oa::present::photo
