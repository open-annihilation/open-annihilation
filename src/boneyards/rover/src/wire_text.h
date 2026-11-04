// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The wire-text helpers the record writer and the report slots share.
#pragma once

#include "oa/rover/wire_text.h"

// Room for one interface string after Windows-1252 expands to UTF-8.
constexpr int32_t kRoverInterfaceTextBytes = 8192;

/// Selects the encoding of text that crosses the slot table.
///
/// Clear, the default, means the caller passes and receives UTF-8. Set, the
/// slot table speaks Windows-1252. The drop-in library sets it once when it
/// loads. Open Annihilation leaves it clear. It is independent of whether the
/// server accepts UTF-8 on the wire.
///
/// @param windows1252 true when interface text is Windows-1252
void rover_set_interface_windows1252(bool windows1252);

/// Returns whether interface text is Windows-1252.
///
/// @return true when strings that cross the slot table are Windows-1252
bool rover_interface_windows1252(void);

/// Turns interface text into the UTF-8 Rover stores.
///
/// While the interface is UTF-8, text is returned as it stands. While it is
/// Windows-1252, each byte is read as that code page and written into out.
///
/// @param[out] out buffer used only while the interface is Windows-1252
/// @param capacity size of out in bytes
/// @param text interface text, or null
/// @return text, out, or null when text is null
const char* rover_take_text(char* out, int32_t capacity, const char* text);

/// Turns stored UTF-8 into the text the interface receives.
///
/// While the interface is UTF-8, or the text is ASCII, text is returned as it
/// stands. Otherwise it is written as Windows-1252, with "?" for a character
/// that code page cannot hold. A converted pointer stays valid until several
/// further calls have converted text of their own, and only until the caller
/// returns.
///
/// @param text stored UTF-8, or null
/// @return text, a Windows-1252 copy, or null when text is null
const char* rover_show(const char* text);

/// Copies the longest prefix of UTF-8 text whose wire form is at most limit bytes.
///
/// The copy ends on a character boundary. A negative limit adds no bound beyond the buffer.
///
/// @param[out] out buffer of capacity bytes; always terminated when capacity is positive
/// @param capacity size of out in bytes
/// @param text UTF-8 text; may be null when text_length is 0
/// @param text_length bytes of text
/// @param limit most wire bytes the prefix may become, or negative for none
/// @return bytes copied, not counting the terminator
int32_t rover_keep_for_wire(
    char* out, int32_t capacity, const char* text, int32_t text_length, int32_t limit
);
