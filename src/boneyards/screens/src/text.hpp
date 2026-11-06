// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Small string helpers shared by the Boneyards screens.
#pragma once

#include "oa/boneyards/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// Copies text into a field and always terminates. The cut is on a character
// boundary, so a character that would not fit is left out.
inline void copy_bounded(char* out, std::size_t capacity, const char* text) {
    utf8::copy(out, capacity, text, 0);
}

// Copies at most `max_chars` characters, and never a character that would not fit.
inline void copy_chars(char* out, std::size_t capacity, const char* text, int32_t max_chars) {
    utf8::copy(out, capacity, text, max_chars);
}

inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive ASCII equality; null compares unequal to everything.
inline bool equals_nocase(const char* a, const char* b) {
    if (a == nullptr || b == nullptr)
        return false;
    for (;; ++a, ++b) {
        if (ascii_lower(*a) != ascii_lower(*b))
            return false;
        if (*a == '\0')
            return true;
    }
}

namespace word_sort {

// The default table's symbol order; the hyphen and apostrophe are not in it.
inline constexpr const char kSymbols[] = " !\"#$%&()*,./:;?@[\\]^_`{|}~+<=>";

inline bool is_special(unsigned char c) {
    return c == '-' || c == '\'';
}

// First-level weight: symbols, then digits, then letters without case;
// 0 for the special characters and controls, which that level skips. Bytes
// above 0x7F follow the letters in code order.
inline int weight(unsigned char c) {
    if (c < 0x20 || c == 0x7f || is_special(c))
        return 0;
    for (int i = 0; kSymbols[i] != '\0'; ++i)
        if (static_cast<unsigned char>(kSymbols[i]) == c)
            return 1 + i;
    if (c >= '0' && c <= '9')
        return 0x40 + (c - '0');
    const char lower = ascii_lower(static_cast<char>(c));
    if (lower >= 'a' && lower <= 'z')
        return 0x50 + (lower - 'a');
    return 0x100 + c;
}

inline const char* next_weighted(const char* p) {
    while (*p != '\0' && weight(static_cast<unsigned char>(*p)) == 0)
        ++p;
    return p;
}

inline const char* next_special(const char* p) {
    while (*p != '\0' && !is_special(static_cast<unsigned char>(*p)))
        ++p;
    return p;
}

} // namespace word_sort

// Compares ASCII text in word-sort order, as 3.1c orders names. Weights
// compare first, then case with lower case first, then the
// hyphens and apostrophes by position (a string without one sorts first).
// Returns <0, 0 or >0.
inline int compare_word_sort(const char* a, const char* b) {
    using namespace word_sort;
    const char* p = next_weighted(a);
    const char* q = next_weighted(b);
    for (; *p != '\0' && *q != '\0'; p = next_weighted(p + 1), q = next_weighted(q + 1)) {
        const int wp = weight(static_cast<unsigned char>(*p));
        const int wq = weight(static_cast<unsigned char>(*q));
        if (wp != wq)
            return wp < wq ? -1 : 1;
    }
    if (*p != '\0' || *q != '\0')
        return *p != '\0' ? 1 : -1;
    p = next_weighted(a);
    q = next_weighted(b);
    for (; *p != '\0'; p = next_weighted(p + 1), q = next_weighted(q + 1)) {
        const bool upper_p = *p >= 'A' && *p <= 'Z';
        const bool upper_q = *q >= 'A' && *q <= 'Z';
        if (upper_p != upper_q)
            return upper_p ? 1 : -1;
    }
    p = next_special(a);
    q = next_special(b);
    for (; *p != '\0' && *q != '\0'; p = next_special(p + 1), q = next_special(q + 1)) {
        if (p - a != q - b)
            return p - a < q - b ? -1 : 1;
        if (*p != *q)
            return *p == '\'' ? -1 : 1;
    }
    if (*p != '\0' || *q != '\0')
        return *p != '\0' ? 1 : -1;
    return 0;
}

} // namespace oa::boneyards::screens
