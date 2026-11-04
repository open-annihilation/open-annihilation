// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* The lobby links' byte cipher. Every byte a link sends or receives is XORed
 * with the low byte of a 32-bit counter that advances by one per byte and
 * wraps at 32 bits. Each direction of a link keeps its own counter, which
 * starts at OA_ROVER_CIPHER_FIRST_COUNTER when the link connects, so a stream
 * deciphers the same however it was split into sends. Applying the cipher
 * twice from the same counter restores the bytes; it hides the record text
 * from casual view and gives no security.
 *
 * Compiles as C11 and C++20. */
#ifndef OA_ROVER_CIPHER_H
#define OA_ROVER_CIPHER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Counter a link direction applies to its first byte after connecting. */
#define OA_ROVER_CIPHER_FIRST_COUNTER 1u

/// Enciphers or deciphers bytes in place, advancing a direction's counter once per byte.
///
/// @param[in,out] counter the direction's counter; advances by length, wrapping at 32 bits
/// @param[in,out] bytes bytes XORed in place with the counter's low byte
/// @param length number of bytes; nothing happens below 1
void rover_cipher_apply(uint32_t* counter, uint8_t* bytes, int32_t length);

#ifdef __cplusplus
}
#endif

#endif
