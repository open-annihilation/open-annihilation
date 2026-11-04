// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MD5 (RFC 1321), which Rover uses only for the login password digest.
#pragma once

#include <stdint.h>

constexpr int32_t kRoverMd5DigestSize = 16;

struct RoverMd5 {
    uint32_t state[4]{};
    uint32_t bit_count[2]{}; // message length in bits, low word first
    uint8_t buffer[64]{};
};

/// Starts a digest: clears the context and loads the initial state.
///
/// @param[out] md5 context to start
void rover_md5_init(RoverMd5& md5);

/// Adds bytes to the digest, buffering a partial 64-byte block between calls.
///
/// @param[in,out] md5 context started by rover_md5_init
/// @param data bytes to add
/// @param length number of bytes
void rover_md5_update(RoverMd5& md5, const void* data, uint32_t length);

/// Pads the message, appends its bit length and writes the digest.
///
/// @param[in,out] md5 context to finish; unusable for further updates afterwards
/// @param[out] digest receives the 16 digest bytes
void rover_md5_final(RoverMd5& md5, uint8_t digest[kRoverMd5DigestSize]);
