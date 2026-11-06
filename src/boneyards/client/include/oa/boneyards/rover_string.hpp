// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's side of the string object it exchanges with Rover (RoverString):
// the copy-on-write buffer the game builds for a property-bag key and the
// release that follows every call. The buffer is one heap block: a share
// count byte, the characters and a terminator; `data` points past the count.
#pragma once

#include "oa/rover/interface.h"

#include <cstdint>

namespace oa::boneyards {

// Capacities are rounded up to the next value with these low bits set.
inline constexpr uint32_t kRoverStringGranule = 0x1f;
// Largest capacity the rounding may produce; longer requests keep their size.
inline constexpr uint32_t kRoverStringMaxCapacity = 0xfffffffd;
// A sole-owned buffer at or below this capacity is reused by an assignment.
inline constexpr uint32_t kRoverStringSmallCapacity = 0x1f;
// Share count of a buffer nobody may share; a release frees it like a
// buffer with one owner (count 0).
inline constexpr uint8_t kRoverStringUnshared = 0xff;

/// Empties a string object, freeing or unsharing its buffer as asked.
///
/// With `release_buffer` the buffer is freed when this was its only owner
/// (or it is unshared) and otherwise loses one share.
///
/// @param[in,out] string string object to empty
/// @param release_buffer whether to drop this object's hold on the buffer
void rover_string_release(RoverString* string, bool release_buffer);

/// Replaces a string's contents with bytes, once its buffer is writable, and terminates them.
///
/// @param[in,out] string string object
/// @param bytes new contents; may overlap the old ones
/// @param length number of bytes
/// @return `string`
RoverString* rover_string_assign(RoverString* string, const char* bytes, uint32_t length);

/// Makes a string's buffer writable for a length.
///
/// A shared buffer loses one share (length 0) or is copied; a sole-owned one
/// is kept when it is big enough, except that a discarding assignment keeps
/// only a small one (kRoverStringSmallCapacity or less).
///
/// @param[in,out] string string object
/// @param length characters the buffer must hold
/// @param discard whether the old contents may be dropped
/// @return false when the string ends up empty (length 0)
bool rover_string_prepare(RoverString* string, uint32_t length, bool discard);

/// Gives a string a new sole-owned buffer for at least a length, keeping as much of the old
/// contents as fits.
///
/// The capacity is rounded up to the next value with the kRoverStringGranule
/// bits set; allocation failure aborts.
///
/// @param[in,out] string string object
/// @param length characters the buffer must hold
void rover_string_reserve(RoverString* string, uint32_t length);

/// Returns a string's characters.
///
/// @param string string object
/// @return the characters, or "" for the empty string
const char* rover_string_text(const RoverString* string);

} // namespace oa::boneyards
