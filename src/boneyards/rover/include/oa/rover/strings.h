// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Operations on RoverString, the reference-counted string object the game
 * and Rover exchange through the property-bag slots. The byte before a
 * buffer's text is its share count: 0 for a single
 * owner, one more for each further owner, and 0xfe or above for a buffer that
 * is copied rather than shared (0xff marks one whose characters were handed
 * out for writing). Buffers are allocated from Rover's own heap as
 * [count][text][NUL] with a capacity of the length rounded up to 32n-1.
 *
 * A native host builds and releases the strings it passes to Rover with these
 * functions, so every buffer is freed by the heap that allocated it.
 *
 * Compiles as C11 and C++20. */
#ifndef OA_ROVER_STRINGS_H
#define OA_ROVER_STRINGS_H

#include "oa/rover/interface.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Makes string an empty string that owns no buffer.
///
/// @param[out] string object to initialize; any buffer it held is not released
void rover_string_init(RoverString* string);

/// Returns the string's text.
///
/// @param string string to read
/// @return the text, or "" for a string without a buffer
const char* rover_string_text(const RoverString* string);

/// Drops this owner's share of the buffer when built is true, then empties the object either way.
///
/// An unshared or frozen buffer is freed; a shared one loses one share.
///
/// @param[in,out] string string to empty
/// @param built true when string holds a buffer it owns; false empties it without touching the
///     buffer
void rover_string_tidy(RoverString* string, bool built);

/// Releases the string's buffer share and empties it, as rover_string_tidy(string, true).
///
/// @param[in,out] string string to release
void rover_string_release(RoverString* string);

/// Prepares an unshared buffer that can hold length characters, keeping the current text when it
/// fits.
///
/// A shared buffer is always copied. With trim, an owned buffer above 31 characters or below length
/// is given up and reallocated.
///
/// @param[in,out] string string whose buffer is prepared
/// @param length characters the buffer must hold, excluding the terminator
/// @param trim true to give up a buffer larger than 31 characters
/// @return true when a buffer for length characters is ready; false when length is 0 (the string is
///     then emptied), above 0xfffffffd, or memory ran out
bool rover_string_grow(RoverString* string, uint32_t length, bool trim);

/// Replaces the text with a private copy when the buffer is shared.
///
/// @param[in,out] string string to unshare
/// @quirk The copy stops at the first NUL of the shared text.
void rover_string_split(RoverString* string);

/// Replaces the text with length bytes of text, always in a buffer the string owns alone.
///
/// @param[in,out] string string to assign to
/// @param text bytes to copy; may contain NULs
/// @param length number of bytes to copy
/// @return string
RoverString* rover_string_assign_text(RoverString* string, const char* text, uint32_t length);

/// Replaces the text with all of source's, as an assignment of the whole source does.
///
/// A non-empty buffer whose share count is below 0xfe is shared; anything else is copied into a
/// buffer of string's own.
///
/// @param[in,out] string string to assign to
/// @param source string to copy
/// @return string
/// @quirk Assigning a string to itself only unshares it.
RoverString* rover_string_assign(RoverString* string, const RoverString* source);

/// Orders count characters of string from position against text_length bytes of text.
///
/// Both position and count are clamped to the text; a common prefix puts the shorter range first.
/// A position past the end is clamped to the end, where 3.1c reports a range error.
///
/// @param string string to compare
/// @param position first character of string to compare
/// @param count number of characters of string to compare
/// @param text bytes to compare against
/// @param text_length number of bytes of text
/// @return -1, 0 or 1 as the string range orders before, equal to or after text
int32_t rover_string_compare(
    const RoverString* string,
    uint32_t position,
    uint32_t count,
    const char* text,
    uint32_t text_length
);

/// Tests the whole string for exact, case-sensitive equality with a C string.
///
/// @param string string to compare
/// @param text NUL-terminated text
/// @return true when both hold the same characters
bool rover_string_equals(const RoverString* string, const char* text);

#ifdef __cplusplus
}
#endif

#endif
