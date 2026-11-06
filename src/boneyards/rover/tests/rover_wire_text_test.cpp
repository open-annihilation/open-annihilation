// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Text at the lobby wire: every Windows-1252 byte round-trips, a character that code page cannot
// hold becomes "?", a cut ends on a character boundary, and UTF-8 is kept while the server is
// known to accept it. The flag is never set from a record.

#include "../src/record.h"
#include "../src/wire_text.h"

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

bool same_bytes(const char* got, int32_t got_length, const char* want, int32_t want_length) {
    return got_length == want_length && memcmp(got, want, static_cast<size_t>(want_length)) == 0;
}

void check_round_trip() {
    rover_set_wire_utf8(false);
    for (int32_t byte = 0; byte < 256; ++byte) {
        char wire[1] = {static_cast<char>(byte)};
        char utf8[8];
        int32_t decoded =
            rover_decode_wire_text(utf8, static_cast<int32_t>(sizeof utf8), wire, 1, -1);
        char back[8];
        int32_t used = 0;
        int32_t encoded = rover_encode_wire_text(
            back, static_cast<int32_t>(sizeof back), utf8, decoded, -1, &used
        );
        if (encoded != 1 || used != decoded || static_cast<uint8_t>(back[0]) != byte) {
            fprintf(stderr, "FAIL: byte 0x%02x came back as %d bytes\n", byte, encoded);
            ++failures;
            return;
        }
    }

    char wire[255];
    for (int32_t byte = 1; byte < 256; ++byte)
        wire[byte - 1] = static_cast<char>(byte);
    char utf8[255 * 3 + 1];
    int32_t decoded = rover_decode_wire_text(
        utf8, static_cast<int32_t>(sizeof utf8), wire, static_cast<int32_t>(sizeof wire), -1
    );
    char back[256];
    int32_t used = 0;
    int32_t encoded =
        rover_encode_wire_text(back, static_cast<int32_t>(sizeof back), utf8, decoded, -1, &used);
    check(
        same_bytes(back, encoded, wire, static_cast<int32_t>(sizeof wire)) && used == decoded,
        "every Windows-1252 character round-trips in one string"
    );
}

void check_missing_character() {
    rover_set_wire_utf8(false);
    const char tokyo[] = "\xe6\x9d\xb1\xe4\xba\xac";
    char out[8];
    int32_t used = 0;
    int32_t encoded = rover_encode_wire_text(
        out,
        static_cast<int32_t>(sizeof out),
        tokyo,
        static_cast<int32_t>(sizeof tokyo) - 1,
        -1,
        &used
    );
    check(
        encoded == 2 && used == static_cast<int32_t>(sizeof tokyo) - 1 && out[0] == '?' &&
            out[1] == '?',
        "a character Windows-1252 cannot hold is written as ?"
    );
}

void check_one_cut(const char* text, int32_t limit, const char* want, const char* what) {
    int32_t length = static_cast<int32_t>(strlen(text));
    char encoded[16];
    int32_t used = 0;
    int32_t written = rover_encode_wire_text(
        encoded, static_cast<int32_t>(sizeof encoded), text, length, limit, &used
    );
    char kept[16];
    int32_t kept_length =
        rover_keep_for_wire(kept, static_cast<int32_t>(sizeof kept), text, length, limit);
    bool encoded_ok = same_bytes(encoded, written, want, static_cast<int32_t>(strlen(want)));
    bool kept_ok = used == kept_length && same_bytes(kept, kept_length, text, used);
    if (!encoded_ok || !kept_ok)
        fprintf(stderr, "FAIL: %s wrote %d bytes, kept %d\n", what, written, kept_length);
    check(encoded_ok && kept_ok, what);
}

void check_cuts() {
    const char suns[] = "\xe6\x97\xa5\xe6\x97\xa5";
    const char one_sun[] = "\xe6\x97\xa5";
    rover_set_wire_utf8(true);
    check_one_cut(suns, 1, "", "UTF-8 drops a character that would pass the limit");
    check_one_cut(suns, 2, "", "UTF-8 drops a character that would pass the limit by two");
    check_one_cut(suns, 3, one_sun, "UTF-8 keeps a character that fills the limit");
    check_one_cut(suns, 4, one_sun, "UTF-8 does not split the next character");
    check_one_cut(suns, 5, one_sun, "UTF-8 still does not split the next character");
    check_one_cut(suns, 6, suns, "UTF-8 keeps both characters when they fit");
    check_one_cut("a\xe6\x97\xa5", 2, "a", "UTF-8 keeps the character before the one that spills");
    const char emoji[] = "\xf0\x9f\x98\x80";
    check_one_cut(emoji, 3, "", "UTF-8 drops a four-byte character that does not fit");

    rover_set_wire_utf8(false);
    check_one_cut(suns, 1, "?", "Windows-1252 keeps one ? and not half a character");
    check_one_cut(emoji, 1, "?", "Windows-1252 writes ? for a character past the code page");
    check_one_cut("a\xe6\x97\xa5", 2, "a?", "Windows-1252 cuts after whole characters");
}

void check_utf8_flag() {
    rover_set_wire_utf8(true);
    const char tokyo[] = "\xe6\x9d\xb1\xe4\xba\xac";
    char out[16];
    int32_t used = 0;
    int32_t encoded =
        rover_encode_wire_text(out, static_cast<int32_t>(sizeof out), tokyo, 6, -1, &used);
    check(same_bytes(out, encoded, tokyo, 6) && used == 6, "UTF-8 is written as it is");
    char back[16];
    int32_t decoded =
        rover_decode_wire_text(back, static_cast<int32_t>(sizeof back), out, encoded, -1);
    check(same_bytes(back, decoded, tokyo, 6), "UTF-8 is read as it is");

    const char cafe[] = "caf\xc3\xa9";
    encoded = rover_encode_wire_text(out, static_cast<int32_t>(sizeof out), cafe, 5, -1, &used);
    check(same_bytes(out, encoded, cafe, 5), "a character inside UTF-8 stays UTF-8");

    char lone[] = {'\xe9'};
    decoded = rover_decode_wire_text(back, static_cast<int32_t>(sizeof back), lone, 1, -1);
    check(
        same_bytes(back, decoded, "\xc3\xa9", 2),
        "a byte that is not UTF-8 is still read as Windows-1252"
    );
    check(rover_wire_utf8(), "reading text does not clear the flag");

    rover_set_wire_utf8(false);
    encoded = rover_encode_wire_text(out, static_cast<int32_t>(sizeof out), tokyo, 6, -1, &used);
    check(encoded == 2 && !rover_wire_utf8(), "writing text does not set the flag");
}

RoverRecord* receive_text(const char* text, int32_t length) {
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    RoverRecordStatus status = rover_record_receive(&pending, text, length, &consumed, &record);
    if (status != RoverRecordStatus::Ok) {
        rover_record_destroy(&pending);
        return nullptr;
    }
    return record;
}

void check_record_text() {
    rover_set_wire_utf8(false);
    RoverRecord* record = rover_record_create();
    RoverEntry* entry = rover_record_add_entry(*record, true, "GameManager");
    rover_entry_set_string(*entry, "message", "caf\xc3\xa9\xe6\x9d\xb1\xe4\xba\xac");
    char text[kRoverRecordTextLimit];
    int32_t length = 0;
    const char* want = "{[GameManager<S:message=caf\xe9?"
                       "?>]}";
    RoverRecordStatus status = rover_record_format(*record, text, sizeof text, &length);
    check(
        status == RoverRecordStatus::Ok &&
            same_bytes(text, length, want, static_cast<int32_t>(strlen(want))),
        "a built record writes Windows-1252"
    );
    rover_record_destroy(&record);

    RoverRecord* parsed = receive_text(want, static_cast<int32_t>(strlen(want)));
    const RoverField* field =
        parsed ? rover_entry_find(*rover_record_end_entry(*parsed, false), "message") : nullptr;
    check(
        field && same_bytes(field->value, field->value_length, "caf\xc3\xa9??", 7),
        "a received record is read back as UTF-8"
    );
    rover_record_destroy(&parsed);

    const char* arrived = "{[Rover<S:v=\xe9>]}";
    parsed = receive_text(arrived, static_cast<int32_t>(strlen(arrived)));
    status = parsed ? rover_record_format(*parsed, text, sizeof text, &length)
                    : RoverRecordStatus::Malformed;
    check(
        status == RoverRecordStatus::Ok &&
            same_bytes(text, length, arrived, static_cast<int32_t>(strlen(arrived))),
        "an unchanged entry is written as the bytes it arrived as"
    );
    field = parsed ? rover_entry_find(*rover_record_end_entry(*parsed, false), "v") : nullptr;
    check(
        field && same_bytes(field->value, field->value_length, "\xc3\xa9", 2),
        "those bytes are still read as UTF-8"
    );
    rover_record_destroy(&parsed);

    constexpr int32_t euros = 700;
    char wide[32 + euros];
    const char* head = "{[Rover<S:v=";
    int32_t head_length = static_cast<int32_t>(strlen(head));
    memcpy(wide, head, static_cast<size_t>(head_length));
    memset(wide + head_length, 0x80, static_cast<size_t>(euros));
    memcpy(wide + head_length + euros, ">]}", 4);
    parsed = receive_text(wide, head_length + euros + 3);
    field = parsed ? rover_entry_find(*rover_record_end_entry(*parsed, false), "v") : nullptr;
    check(
        field && field->value_length == euros * 3 && same_bytes(field->value, 3, "\xe2\x82\xac", 3),
        "a string read from the wire keeps three UTF-8 bytes per wire byte"
    );
    rover_record_destroy(&parsed);
}

} // namespace

int main() {
    rover_set_wire_utf8(false);
    check_round_trip();
    check_missing_character();
    check_cuts();
    check_utf8_flag();
    check_record_text();
    rover_set_wire_utf8(false);
    if (failures)
        return 1;
    puts("rover wire text: ok");
    return 0;
}
