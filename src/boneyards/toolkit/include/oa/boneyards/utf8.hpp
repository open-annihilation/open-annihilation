// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Character boundaries for text the Boneyards stores and edits. A well-formed
// sequence is one character. A byte that starts none counts as one character
// on its own, so a cut never falls inside a character and a limit counts
// characters.
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oa::boneyards::utf8 {

struct Unit {
    int32_t bytes; // 0 at the end; 1 for a byte that starts no sequence
    char32_t code;
    bool well_formed;
};

struct Slice {
    int32_t bytes;
    int32_t chars;
};

/// Bytes a field needs to hold `chars` characters and a terminator.
///
/// Every character is at most four bytes.
///
/// @param chars characters the field holds
/// @return the field size, terminator included
constexpr std::size_t bytes_for(std::size_t chars) {
    return chars * 4 + 1;
}

/// Reads one character at the start of text.
///
/// A well-formed sequence is returned whole. Anything else, including a
/// sequence cut short or an overlong encoding, is the one leading byte.
///
/// @param text text to read; null or empty reads nothing
/// @return the character
inline Unit read(const char* text) {
    if (text == nullptr || text[0] == '\0') {
        return {};
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    if (lead < 0x80) {
        return {1, lead, true};
    }
    int32_t need = 0;
    char32_t code = 0;
    if ((lead & 0xE0) == 0xC0) {
        need = 2;
        code = lead & 0x1F;
    } else if ((lead & 0xF0) == 0xE0) {
        need = 3;
        code = lead & 0x0F;
    } else if ((lead & 0xF8) == 0xF0) {
        need = 4;
        code = lead & 0x07;
    } else {
        return {1, lead, false};
    }
    for (int32_t i = 1; i < need; ++i) {
        const auto next = static_cast<unsigned char>(text[i]);
        if (text[i] == '\0' || (next & 0xC0) != 0x80) {
            return {1, lead, false};
        }
        code = (code << 6) | (next & 0x3F);
    }
    const bool overlong =
        (need == 2 && code < 0x80) || (need == 3 && code < 0x800) || (need == 4 && code < 0x10000);
    if (overlong || code > 0x10FFFF) {
        return {1, lead, false};
    }
    return {need, code, true};
}

/// Counts characters, one per well-formed sequence and one per other byte.
///
/// @param text text to count; null counts as none
/// @return the number of characters
inline int32_t characters(const char* text) {
    int32_t count = 0;
    if (text == nullptr) {
        return 0;
    }
    for (int32_t bytes = 0; text[bytes] != '\0'; ++count) {
        const Unit unit = read(text + bytes);
        bytes += unit.bytes > 0 ? unit.bytes : 1;
    }
    return count;
}

/// Copies text into a field, stopping on a character boundary.
///
/// The copy always ends with a terminator when `capacity` is at least 1.
/// A character that would not fit is left out. `max_chars` of 0 or less
/// does not limit the number of characters.
///
/// @param[out] out field receiving the text; null is ignored
/// @param capacity size of the field, terminator included
/// @param text text to copy; null copies nothing
/// @param max_chars character limit; 0 or less means no limit
inline void copy(char* out, std::size_t capacity, const char* text, int32_t max_chars) {
    if (out == nullptr || capacity == 0) {
        return;
    }
    if (text == nullptr) {
        out[0] = '\0';
        return;
    }
    std::size_t bytes = 0;
    int32_t chars = 0;
    while (text[bytes] != '\0') {
        if (max_chars > 0 && chars >= max_chars) {
            break;
        }
        const Unit unit = read(text + bytes);
        const int32_t n = unit.bytes > 0 ? unit.bytes : 1;
        if (bytes + static_cast<std::size_t>(n) + 1 > capacity) {
            break;
        }
        bytes += static_cast<std::size_t>(n);
        ++chars;
    }
    if (bytes > 0) {
        std::memcpy(out, text, bytes);
    }
    out[bytes] = '\0';
}

/// Returns the byte index of a character, or of the terminator past the end.
///
/// @param text text to index; null returns 0
/// @param index character index; negative counts as the start
/// @return the byte index
inline int32_t offset(const char* text, int32_t index) {
    if (text == nullptr || index <= 0) {
        return 0;
    }
    int32_t bytes = 0;
    for (int32_t i = 0; i < index && text[bytes] != '\0'; ++i) {
        const Unit unit = read(text + bytes);
        bytes += unit.bytes > 0 ? unit.bytes : 1;
    }
    return bytes;
}

/// Tells whether a code is a character the player can type.
///
/// Spaces through the last defined character are taken, except the controls
/// that follow delete and the surrogates.
///
/// @param code code to test
/// @return true when the code may be typed
inline bool taken(char32_t code) {
    if (code < 0x20 || code > 0x10FFFF) {
        return false;
    }
    if (code >= 0x7F && code <= 0x9F) {
        return false;
    }
    if (code >= 0xD800 && code <= 0xDFFF) {
        return false;
    }
    return true;
}

/// Writes a character's bytes.
///
/// @param code character to write; a code this module takes
/// @param out four bytes of storage
/// @return how many bytes were written
inline int32_t encode(char32_t code, char out[4]) {
    if (code < 0x80) {
        out[0] = static_cast<char>(code);
        return 1;
    }
    if (code < 0x800) {
        out[0] = static_cast<char>(0xC0 | (code >> 6));
        out[1] = static_cast<char>(0x80 | (code & 0x3F));
        return 2;
    }
    if (code < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (code >> 12));
        out[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (code & 0x3F));
        return 3;
    }
    out[0] = static_cast<char>(0xF0 | (code >> 18));
    out[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (code & 0x3F));
    return 4;
}

/// Measures a prefix of at most `max_chars` characters and `max_bytes` bytes.
///
/// A limit of 0 or less does not apply. The cut is never in the middle of a
/// sequence.
///
/// @param src text to measure; null measures nothing
/// @param max_chars character limit; 0 or less means no limit
/// @param max_bytes byte limit, not counting a terminator; 0 or less means
///     no limit
/// @return the bytes and characters that fit
inline Slice measure_limited(const char* src, int32_t max_chars, int32_t max_bytes) {
    Slice slice{};
    if (src == nullptr) {
        return slice;
    }
    while (src[slice.bytes] != '\0') {
        if (max_chars > 0 && slice.chars >= max_chars) {
            break;
        }
        const Unit unit = read(src + slice.bytes);
        if (unit.bytes <= 0) {
            break;
        }
        if (max_bytes > 0 && slice.bytes + unit.bytes > max_bytes) {
            break;
        }
        slice.bytes += unit.bytes;
        ++slice.chars;
    }
    return slice;
}

/// Writes text with `extra` inserted at a character index.
///
/// `dest` holds the bytes of both strings and a terminator.
///
/// @param dest buffer to fill
/// @param text text the caret sits in; null counts as empty
/// @param caret character index in `text`
/// @param extra text inserted at the caret; null counts as empty
inline void splice_into(char* dest, const char* text, int32_t caret, const char* extra) {
    const char* body = text != nullptr ? text : "";
    const char* composed = extra != nullptr ? extra : "";
    const int32_t at = offset(body, caret);
    const auto body_bytes = static_cast<int32_t>(std::strlen(body));
    const auto extra_bytes = static_cast<int32_t>(std::strlen(composed));
    std::memcpy(dest, body, static_cast<std::size_t>(at));
    std::memcpy(dest + at, composed, static_cast<std::size_t>(extra_bytes));
    std::memcpy(dest + at + extra_bytes, body + at, static_cast<std::size_t>(body_bytes - at) + 1);
}

} // namespace oa::boneyards::utf8
