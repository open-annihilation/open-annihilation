// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby links' byte cipher.

#include "oa/rover/cipher.h"

void rover_cipher_apply(uint32_t* counter, uint8_t* bytes, int32_t length) {
    for (int32_t i = 0; i < length; ++i) {
        bytes[i] ^= static_cast<uint8_t>(*counter);
        ++*counter;
    }
}
