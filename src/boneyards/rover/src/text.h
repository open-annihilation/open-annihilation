// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The bounded copy 3.1c makes: at most capacity - 1 bytes, always terminated.
#ifndef OA_ROVER_TEXT_H
#define OA_ROVER_TEXT_H

#include <stdint.h>

/// Copies source up to its NUL or capacity - 1 bytes and always terminates; the rest of destination
/// is left as it was.
///
/// @param[out] destination buffer of capacity bytes
/// @param source NUL-terminated text to copy
/// @param capacity size of destination in bytes; nothing is written when it is 0 or less
inline void rover_copy_text(char* destination, const char* source, int32_t capacity) {
    if (capacity <= 0)
        return;
    int32_t i = 0;
    for (; i < capacity - 1 && source[i]; ++i)
        destination[i] = source[i];
    destination[i] = 0;
}

#endif
