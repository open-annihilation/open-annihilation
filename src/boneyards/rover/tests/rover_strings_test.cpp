// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The reference-counted string object the game and Rover exchange: buffer layout and
// capacity rounding, the share-count rules of assignment, growth and release,
// and the comparisons the containers order keys with.

#include "oa/rover/strings.h"

#include <stdio.h>
#include <string.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

uint8_t share_count(const RoverString& string) {
    return reinterpret_cast<const uint8_t*>(string.data)[-1];
}

bool holds(const RoverString& string, const char* text) {
    return string.length == strlen(text) && string.data &&
           memcmp(string.data, text, string.length) == 0 && string.data[string.length] == 0;
}

void check_assign_text() {
    RoverString s{};
    rover_string_assign_text(&s, "abc", 3);
    check(holds(s, "abc"), "assign_text copies the bytes and terminates");
    check(s.capacity == 0x1f, "a short string gets capacity 31");
    check(share_count(s) == 0, "a fresh buffer has one owner");

    char long_text[40];
    memset(long_text, 'x', sizeof long_text);
    rover_string_assign_text(&s, long_text, sizeof long_text);
    check(
        s.length == 40 && s.capacity == (40 | 0x1f),
        "capacity is the length with the low five bits set"
    );

    rover_string_assign_text(&s, "abc", 3);
    check(
        holds(s, "abc") && s.capacity == 0x1f,
        "trimming assignment gives up a buffer above 31 characters"
    );

    char* kept = s.data;
    rover_string_assign_text(&s, "abcdef", 6);
    check(s.data == kept, "a buffer of 31 is reused when it fits");

    rover_string_assign_text(&s, "", 0);
    check(
        s.data == nullptr && s.length == 0 && s.capacity == 0,
        "assigning nothing empties the string"
    );
    check(strcmp(rover_string_text(&s), "") == 0, "an empty string reads as \"\"");
    rover_string_release(&s);
}

void check_sharing() {
    RoverString a{};
    RoverString b{};
    rover_string_assign_text(&a, "shared", 6);
    rover_string_assign(&b, &a);
    check(b.data == a.data && b.length == 6 && b.capacity == a.capacity, "whole assignment shares");
    check(share_count(a) == 1, "sharing counts the second owner");

    RoverString c{};
    rover_string_assign(&c, &b);
    check(c.data == a.data && share_count(a) == 2, "a third owner");

    rover_string_release(&c);
    check(
        share_count(a) == 1 && c.data == nullptr, "release of a shared buffer only drops a share"
    );
    rover_string_release(&a);
    check(share_count(b) == 0, "the last owner is left alone");
    check(holds(b, "shared"), "the text survives releases by other owners");

    // Writing through a shared string copies it first.
    rover_string_assign(&a, &b);
    rover_string_assign_text(&a, "other", 5);
    check(a.data != b.data && holds(a, "other") && holds(b, "shared"), "assignment unshares");
    check(share_count(b) == 0, "unsharing gives the share back");

    RoverString empty{};
    rover_string_assign(&a, &empty);
    check(a.data == nullptr && a.length == 0, "assigning an empty string empties, not shares");

    rover_string_release(&a);
    rover_string_release(&b);
}

void check_share_limit() {
    RoverString source{};
    rover_string_assign_text(&source, "limit", 5);
    // A count of 0xfd still shares and reaches 0xfe; from then on assignment copies.
    source.data[-1] = static_cast<char>(0xfd);
    RoverString a{};
    rover_string_assign(&a, &source);
    check(a.data == source.data && share_count(source) == 0xfe, "0xfd is still shareable");
    RoverString b{};
    rover_string_assign(&b, &source);
    check(
        b.data != source.data && holds(b, "limit") && share_count(b) == 0,
        "a count of 0xfe copies instead of sharing"
    );
    check(share_count(source) == 0xfe, "copying leaves the count alone");

    // Frozen buffers are copied by assignment, owned by grow and freed by release.
    RoverString frozen{};
    rover_string_assign_text(&frozen, "frozen", 6);
    frozen.data[-1] = static_cast<char>(0xff);
    RoverString c{};
    rover_string_assign(&c, &frozen);
    check(c.data != frozen.data, "a frozen buffer is not shared");
    char* kept = frozen.data;
    check(
        rover_string_grow(&frozen, 4, false) && frozen.data == kept,
        "grow treats a frozen buffer as owned"
    );
    rover_string_release(&frozen);

    source.data[-1] = 1;
    rover_string_release(&a);
    rover_string_release(&b);
    rover_string_release(&c);
    rover_string_release(&source);
}

void check_grow() {
    RoverString s{};
    check(!rover_string_grow(&s, 0, true), "growing to nothing reports false");
    check(
        rover_string_grow(&s, 10, false) && s.data && s.capacity == 0x1f && s.length == 0,
        "growing an empty string allocates"
    );
    memcpy(s.data, "0123456789", 10);
    s.length = 10;
    s.data[10] = 0;

    char* kept = s.data;
    check(
        rover_string_grow(&s, 5, false) && s.data == kept && s.length == 10,
        "a smaller request without trim keeps the buffer and text"
    );
    check(
        rover_string_grow(&s, 40, false) && s.data != kept && holds(s, "0123456789") &&
            s.capacity == 0x3f,
        "growing past the capacity moves the text"
    );

    check(
        rover_string_grow(&s, 8, true) && s.capacity == 0x1f && s.length == 0,
        "trim drops a buffer above 31 without keeping its text"
    );

    rover_string_assign_text(&s, "abc", 3);
    check(
        !rover_string_grow(&s, 0, false) && s.data && s.length == 0 && s.data[0] == 0,
        "growing to nothing without trim only truncates"
    );

    RoverString other{};
    rover_string_assign_text(&s, "abc", 3);
    rover_string_assign(&other, &s);
    check(
        !rover_string_grow(&other, 0, true) && other.data == nullptr && share_count(s) == 0,
        "growing a shared string to nothing drops the share"
    );
    rover_string_assign(&other, &s);
    check(
        rover_string_grow(&other, 2, false) && other.data != s.data && holds(other, "abc") &&
            share_count(s) == 0,
        "growing a shared string copies its whole text and drops the share"
    );
    rover_string_release(&other);
    rover_string_release(&s);
}

void check_split_and_self_assignment() {
    RoverString a{};
    RoverString b{};
    rover_string_assign_text(&a, "text", 4);
    rover_string_assign(&b, &a);
    rover_string_assign(&b, &b);
    check(
        b.data != a.data && holds(b, "text") && share_count(a) == 0,
        "assigning a shared string to itself unshares it"
    );
    char* kept = b.data;
    rover_string_assign(&b, &b);
    check(b.data == kept, "assigning an unshared string to itself changes nothing");

    // The private copy stops at the first NUL.
    rover_string_assign_text(&a, "ab\0cd", 5);
    rover_string_assign(&b, &a);
    rover_string_split(&b);
    check(holds(b, "ab") && a.length == 5, "split copies up to the first NUL");
    rover_string_release(&a);
    rover_string_release(&b);
}

void check_compare() {
    RoverString s{};
    rover_string_assign_text(&s, "hello world", 11);
    check(rover_string_compare(&s, 0, 5, "hello", 5) == 0, "equal range");
    check(rover_string_compare(&s, 0, 5, "hell", 4) == 1, "longer range orders after its prefix");
    check(rover_string_compare(&s, 0, 4, "hello", 5) == -1, "shorter range orders first");
    check(rover_string_compare(&s, 0, 5, "hellp", 5) == -1, "byte order, normalised to -1");
    check(rover_string_compare(&s, 0, 5, "hella", 5) == 1, "byte order, normalised to 1");
    check(rover_string_compare(&s, 6, 5, "world", 5) == 0, "range from a position");
    check(rover_string_compare(&s, 6, 10, "world", 5) == 0, "the count is clamped to the text");
    check(rover_string_equals(&s, "hello world"), "equals the whole text");
    check(!rover_string_equals(&s, "Hello world"), "equality is case-sensitive");
    check(!rover_string_equals(&s, "hello"), "equality needs the whole text");

    RoverString empty{};
    check(rover_string_compare(&empty, 0, 0, "", 0) == 0, "empty equals empty");
    check(rover_string_equals(&empty, ""), "an empty string equals \"\"");
    check(rover_string_compare(&empty, 0, 0, "a", 1) == -1, "empty orders first");

    RoverString high{};
    rover_string_assign_text(&high, "\xe9", 1);
    check(rover_string_compare(&high, 0, 1, "z", 1) == 1, "bytes compare unsigned");
    rover_string_release(&high);
    rover_string_release(&s);
}

} // namespace

int main() {
    check_assign_text();
    check_sharing();
    check_share_limit();
    check_grow();
    check_split_and_self_assignment();
    check_compare();
    if (failures)
        return 1;
    puts("rover strings: ok");
    return 0;
}
