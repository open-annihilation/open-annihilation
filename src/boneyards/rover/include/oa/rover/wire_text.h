// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Text on the lobby wire. The client holds UTF-8. It is written as UTF-8
 * only while the server is known to accept it, and otherwise as Windows-1252,
 * with "?" for a character that code page cannot hold. Text read back is
 * decoded the same way. A server can say it accepts UTF-8 later, through an
 * extension of its API; nothing read from the wire sets that. Until then the
 * flag stays clear unless a caller sets it.
 *
 * A caller that already holds UTF-8 must not encode it before handing it
 * over. The conversion runs when the record is written. A second pass reads
 * those bytes again, and writes "?" for a character the code page cannot
 * hold.
 *
 * Compiles as C11 and C++20. */
#ifndef OA_ROVER_WIRE_TEXT_H
#define OA_ROVER_WIRE_TEXT_H

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

/// Sets whether the server accepts UTF-8 on the wire.
///
/// Nothing read from the wire calls this. It stays clear until a caller sets it.
///
/// @param accepts true to write and read UTF-8; false for Windows-1252
void rover_set_wire_utf8(bool accepts);

/// Returns whether the server accepts UTF-8 on the wire.
///
/// @return true when text is written and read as UTF-8
bool rover_wire_utf8(void);

/// Writes UTF-8 text as the wire carries it.
///
/// With the flag clear, each character becomes its Windows-1252 byte, or "?" when that code page
/// has none or the bytes are not UTF-8. With the flag set, the bytes are kept. The result is at
/// most `limit` bytes when limit is zero or more, and always ends on a character boundary. A
/// negative limit does not add a bound of its own.
///
/// @param[out] out buffer of capacity bytes; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param text UTF-8 text; may be null when text_length is 0
/// @param text_length bytes of text
/// @param limit most wire bytes to write, or negative for no bound beyond the buffer
/// @param[out] source_used bytes of text consumed; may be null
/// @return wire bytes written, not counting the terminator
int32_t rover_encode_wire_text(
    char* out,
    int32_t capacity,
    const char* text,
    int32_t text_length,
    int32_t limit,
    int32_t* source_used
);

/// Reads wire bytes as UTF-8.
///
/// With the flag set, a well-formed UTF-8 sequence is kept and every other byte is read as
/// Windows-1252. With the flag clear, every byte is Windows-1252. The result is at most `limit`
/// bytes when limit is zero or more, and always ends on a character boundary.
///
/// @param[out] out buffer of capacity bytes; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param wire wire bytes; may be null when wire_length is 0
/// @param wire_length bytes of wire
/// @param limit most UTF-8 bytes to write, or negative for no bound beyond the buffer
/// @return UTF-8 bytes written, not counting the terminator
int32_t rover_decode_wire_text(
    char* out, int32_t capacity, const char* wire, int32_t wire_length, int32_t limit
);

#ifdef __cplusplus
}
#endif

#endif
