// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// MD5 (RFC 1321).

#include "md5.h"

#include <string.h>

namespace {

constexpr uint32_t kInitialState[4] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u};

// Per-round shift amounts, four per round.
constexpr uint32_t kShifts[4][4] = {
    {7, 12, 17, 22},
    {5, 9, 14, 20},
    {4, 11, 16, 23},
    {6, 10, 15, 21},
};

// floor(abs(sin(i + 1)) * 2^32).
constexpr uint32_t kSineTable[64] = {
    0xd76aa478u, 0xe8c7b756u, 0x242070dbu, 0xc1bdceeeu, 0xf57c0fafu, 0x4787c62au, 0xa8304613u,
    0xfd469501u, 0x698098d8u, 0x8b44f7afu, 0xffff5bb1u, 0x895cd7beu, 0x6b901122u, 0xfd987193u,
    0xa679438eu, 0x49b40821u, 0xf61e2562u, 0xc040b340u, 0x265e5a51u, 0xe9b6c7aau, 0xd62f105du,
    0x02441453u, 0xd8a1e681u, 0xe7d3fbc8u, 0x21e1cde6u, 0xc33707d6u, 0xf4d50d87u, 0x455a14edu,
    0xa9e3e905u, 0xfcefa3f8u, 0x676f02d9u, 0x8d2a4c8au, 0xfffa3942u, 0x8771f681u, 0x6d9d6122u,
    0xfde5380cu, 0xa4beea44u, 0x4bdecfa9u, 0xf6bb4b60u, 0xbebfbc70u, 0x289b7ec6u, 0xeaa127fau,
    0xd4ef3085u, 0x04881d05u, 0xd9d4d039u, 0xe6db99e5u, 0x1fa27cf8u, 0xc4ac5665u, 0xf4292244u,
    0x432aff97u, 0xab9423a7u, 0xfc93a039u, 0x655b59c3u, 0x8f0ccc92u, 0xffeff47du, 0x85845dd1u,
    0x6fa87e4fu, 0xfe2ce6e0u, 0xa3014314u, 0x4e0811a1u, 0xf7537e82u, 0xbd3af235u, 0x2ad7d2bbu,
    0xeb86d391u,
};

uint32_t rotate_left(uint32_t value, uint32_t count) {
    return (value << count) | (value >> (32u - count));
}

/// Mixes one 64-byte block into the state.
///
/// @param[in,out] state the four state words
/// @param block 64 message bytes, read as little-endian words
void transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t words[16];
    for (int32_t i = 0; i < 16; ++i) {
        words[i] = static_cast<uint32_t>(block[i * 4]) |
                   static_cast<uint32_t>(block[i * 4 + 1]) << 8 |
                   static_cast<uint32_t>(block[i * 4 + 2]) << 16 |
                   static_cast<uint32_t>(block[i * 4 + 3]) << 24;
    }
    uint32_t a = state[0];
    uint32_t b = state[1];
    uint32_t c = state[2];
    uint32_t d = state[3];
    for (int32_t step = 0; step < 64; ++step) {
        int32_t round = step / 16;
        uint32_t mixed = 0;
        int32_t word = 0;
        switch (round) {
        case 0:
            mixed = (b & c) | (~b & d);
            word = step;
            break;
        case 1:
            mixed = (b & d) | (c & ~d);
            word = (5 * step + 1) % 16;
            break;
        case 2:
            mixed = b ^ c ^ d;
            word = (3 * step + 5) % 16;
            break;
        default:
            mixed = c ^ (b | ~d);
            word = (7 * step) % 16;
            break;
        }
        uint32_t rotated =
            rotate_left(a + mixed + kSineTable[step] + words[word], kShifts[round][step % 4]);
        a = d;
        d = c;
        c = b;
        b += rotated;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

} // namespace

void rover_md5_init(RoverMd5& md5) {
    md5 = RoverMd5{};
    memcpy(md5.state, kInitialState, sizeof md5.state);
}

void rover_md5_update(RoverMd5& md5, const void* data, uint32_t length) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t used = (md5.bit_count[0] >> 3) & 0x3fu;
    uint32_t low = md5.bit_count[0] + (length << 3);
    if (low < md5.bit_count[0])
        ++md5.bit_count[1];
    md5.bit_count[0] = low;
    md5.bit_count[1] += length >> 29;
    uint32_t room = 64u - used;
    uint32_t at = 0;
    if (length >= room) {
        memcpy(md5.buffer + used, bytes, room);
        transform(md5.state, md5.buffer);
        for (at = room; at + 63u < length; at += 64u)
            transform(md5.state, bytes + at);
        used = 0;
    }
    memcpy(md5.buffer + used, bytes + at, length - at);
}

void rover_md5_final(RoverMd5& md5, uint8_t digest[kRoverMd5DigestSize]) {
    uint8_t length[8];
    for (int32_t i = 0; i < 8; ++i)
        length[i] = static_cast<uint8_t>(md5.bit_count[i / 4] >> ((i % 4) * 8));
    uint32_t used = (md5.bit_count[0] >> 3) & 0x3fu;
    uint32_t padding = used < 56u ? 56u - used : 120u - used;
    static constexpr uint8_t kPadding[64] = {0x80};
    rover_md5_update(md5, kPadding, padding);
    rover_md5_update(md5, length, sizeof length);
    for (int32_t i = 0; i < kRoverMd5DigestSize; ++i)
        digest[i] = static_cast<uint8_t>(md5.state[i / 4] >> ((i % 4) * 8));
}
