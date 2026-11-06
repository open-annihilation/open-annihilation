// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby links' byte cipher: known bytes from the first counter, the
// round trip, 32-bit wrap-around and a stream split into pieces.

#include "oa/rover/cipher.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

/// Reports a failed check and counts it.
///
/// @param condition the checked condition
/// @param what the behaviour checked, printed when it fails
void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

/// Checks the bytes a fresh direction sends for a short input, and that the receiving side restores them.
void test_first_bytes() {
    constexpr int32_t kLength = 6;
    const uint8_t plain[kLength] = {0x00, 0x80, 0xff, 'A', 'B', 'C'};
    const uint8_t enciphered[kLength] = {0x01, 0x82, 0xfc, 0x45, 0x47, 0x45};
    uint8_t bytes[kLength];
    memcpy(bytes, plain, sizeof bytes);
    uint32_t send = OA_ROVER_CIPHER_FIRST_COUNTER;
    rover_cipher_apply(&send, bytes, kLength);
    check(memcmp(bytes, enciphered, sizeof bytes) == 0, "a fresh direction XORs with 1, 2, 3, ...");
    check(send == OA_ROVER_CIPHER_FIRST_COUNTER + kLength, "the counter advances once per byte");

    uint32_t receive = OA_ROVER_CIPHER_FIRST_COUNTER;
    rover_cipher_apply(&receive, bytes, kLength);
    check(memcmp(bytes, plain, sizeof bytes) == 0, "the receiving counter restores the bytes");
}

/// Checks that the counter wraps at 32 bits and only its low byte is applied.
void test_wrap() {
    constexpr uint32_t kTwoBeforeWrap = 0xfffffffeu;
    constexpr int32_t kLength = 4;
    uint8_t zeros[kLength] = {};
    uint32_t counter = kTwoBeforeWrap;
    rover_cipher_apply(&counter, zeros, kLength);
    const uint8_t expected[kLength] = {0xfe, 0xff, 0x00, 0x01};
    check(
        memcmp(zeros, expected, sizeof zeros) == 0,
        "the low byte follows the counter through the wrap"
    );
    check(counter == 2, "the counter wraps at 32 bits");
}

/// Checks that a stream transformed in pieces matches the stream transformed whole.
void test_split_stream() {
    constexpr int32_t kStreamBytes = 1025;
    constexpr int32_t kPieceBytes = 7;
    constexpr uint8_t kFill = 0x5a;
    uint8_t whole[kStreamBytes];
    uint8_t split[kStreamBytes];
    memset(whole, kFill, sizeof whole);
    memset(split, kFill, sizeof split);

    uint32_t whole_counter = OA_ROVER_CIPHER_FIRST_COUNTER;
    rover_cipher_apply(&whole_counter, whole, kStreamBytes);
    uint32_t split_counter = OA_ROVER_CIPHER_FIRST_COUNTER;
    for (int32_t at = 0; at < kStreamBytes; at += kPieceBytes) {
        const int32_t left = kStreamBytes - at;
        rover_cipher_apply(&split_counter, split + at, left < kPieceBytes ? left : kPieceBytes);
    }
    check(memcmp(whole, split, sizeof whole) == 0, "pieces encipher as the whole stream does");
    check(whole_counter == split_counter, "pieces leave the counter where the whole stream does");
    check(
        whole_counter == OA_ROVER_CIPHER_FIRST_COUNTER + kStreamBytes,
        "the counter counts every byte"
    );
}

/// Checks that an empty or negative length leaves the bytes and the counter alone.
void test_empty() {
    uint8_t byte = 'x';
    uint32_t counter = OA_ROVER_CIPHER_FIRST_COUNTER;
    rover_cipher_apply(&counter, &byte, 0);
    rover_cipher_apply(&counter, &byte, -1);
    check(byte == 'x' && counter == OA_ROVER_CIPHER_FIRST_COUNTER, "no bytes, no change");
}

} // namespace

int main() {
    test_first_bytes();
    test_wrap();
    test_split_stream();
    test_empty();
    if (failures)
        return 1;
    puts("rover cipher: ok");
    return 0;
}
