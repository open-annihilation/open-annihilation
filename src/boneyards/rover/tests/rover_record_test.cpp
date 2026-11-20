// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby message record: building, writing, receiving and reading fields.

#include "../src/record.h"

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

// Formats a record and compares it with the expected text.
void check_text(const RoverRecord& record, const char* expected, const char* what) {
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    RoverRecordStatus status = rover_record_format(record, text, kRoverRecordTextLimit, &length);
    bool same = status == RoverRecordStatus::Ok &&
                length == static_cast<int32_t>(strlen(expected)) &&
                memcmp(text, expected, static_cast<size_t>(length)) == 0;
    if (!same)
        fprintf(stderr, "  got %.*s\n  want %s\n", static_cast<int>(length), text, expected);
    check(same, what);
}

// Feeds a whole text to a fresh receive; returns the record, or null.
RoverRecord* receive_all(const char* text, RoverRecordStatus* out_status) {
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    int32_t length = static_cast<int32_t>(strlen(text));
    *out_status = rover_record_receive(&pending, text, length, &consumed, &record);
    if (*out_status != RoverRecordStatus::Ok) {
        rover_record_destroy(&pending);
        return nullptr;
    }
    return record;
}

void check_building() {
    RoverRecord* record = rover_record_create();
    RoverEntry* entry = rover_record_add_entry(*record, true, "UserManager");
    check(entry != nullptr, "entry added");
    check(rover_entry_set_string(*entry, "command", "login"), "command set");
    check(rover_entry_set_string(*entry, "type", "registered"), "type set");
    check(rover_entry_set_string(*entry, "username", "test"), "username set");
    check(rover_entry_set_string(*entry, "password", "a=b%c"), "password set");
    check(!rover_entry_set_string(*entry, "type", "anonymous"), "a key is never replaced");
    check(!rover_entry_set_string(*entry, "empty", ""), "an empty text value is refused");
    check(!rover_entry_set_int(*entry, "bad=key", 1), "a reserved byte in a key is refused");
    check(rover_entry_set_int(*entry, "result", -5), "int set");
    check(rover_entry_set_uint(*entry, "user_id", 0x2au), "uint set");
    check_text(
        *record,
        "{[UserManager<S:command=login><S:password=a%7b%8c><I:result=-5><S:type=registered>"
        "<U:user_id=2a><S:username=test>]}",
        "fields are written in key order with escaped values"
    );
    check(
        rover_record_add_entry(*record, true, "Bad<Name") == nullptr, "a reserved byte in a name"
    );
    RoverEntry* front = rover_record_add_entry(*record, false, "Rover");
    rover_entry_set_string(*front, "command", "_x");
    check(rover_record_end_entry(*record, false) == front, "front entry comes first");
    check(rover_record_end_entry(*record, true) == entry, "back entry comes last");
    check(rover_record_entry_count(*record) == 2, "two entries");
    rover_record_remove_first_entry(*record);
    check(rover_record_end_entry(*record, false) == entry, "first entry dropped");
    rover_record_destroy(&record);
    check(record == nullptr, "destroy clears the pointer");

    RoverRecord* reserved = rover_record_create();
    RoverEntry* all = rover_record_add_entry(*reserved, true, "Rover");
    rover_entry_set_string(*all, "v", "{}[]<>:=%");
    check_text(*reserved, "{[Rover<S:v=%0%1%2%3%4%5%6%7%8>]}", "every reserved byte escaped");
    rover_record_destroy(&reserved);

    RoverRecord* empty = rover_record_create();
    char text[64];
    int32_t length = 0;
    check(
        rover_record_format(*empty, text, sizeof text, &length) == RoverRecordStatus::NoEntries,
        "a record needs an entry"
    );
    rover_record_add_entry(*empty, true, "Rover");
    check(
        rover_record_format(*empty, text, sizeof text, &length) == RoverRecordStatus::NoFields,
        "an entry needs a field"
    );
    rover_record_destroy(&empty);

    RoverRecord* big = rover_record_create();
    RoverEntry* big_entry = rover_record_add_entry(*big, true, "ChatManager");
    char long_text[kRoverRecordTextLimit];
    memset(long_text, 'x', sizeof long_text - 1);
    long_text[sizeof long_text - 1] = '\0';
    rover_entry_set_string(*big_entry, "message", long_text);
    char out[kRoverRecordTextLimit];
    check(
        rover_record_format(*big, out, kRoverRecordTextLimit, &length) ==
                RoverRecordStatus::TooLong &&
            length == 0,
        "a record over 0x400 bytes is refused"
    );
    rover_record_destroy(&big);
}

void check_receiving() {
    const char* wire =
        "{[Rover<S:command=connect_info><S:seed=a%7b><S:type=client><S:compression=text>]}";
    RoverRecord* pending = nullptr;
    RoverRecord* record = nullptr;
    int32_t consumed = 0;
    check(
        rover_record_receive(&pending, wire, 20, &consumed, &record) ==
                RoverRecordStatus::Incomplete &&
            consumed == 20 && record == nullptr && pending != nullptr,
        "a split record waits for its closing brace"
    );
    int32_t rest = static_cast<int32_t>(strlen(wire)) - 20;
    check(
        rover_record_receive(&pending, wire + 20, rest + 3, &consumed, &record) ==
                RoverRecordStatus::Ok &&
            consumed == rest && record == pending,
        "the record completes at its brace and leaves the bytes after it"
    );
    RoverEntry* entry = rover_record_end_entry(*record, false);
    check(entry && rover_text_equals(entry->name, entry->name_length, "Rover"), "entry name");
    const char* seed = nullptr;
    check(
        rover_entry_get_string(*entry, "seed", &seed, nullptr) && strcmp(seed, "a=b") == 0,
        "seed decoded"
    );
    check(rover_entry_find(*entry, "missing") == nullptr, "absent field");
    const char* fallback = nullptr;
    check(
        !rover_entry_get_string(*entry, "missing", &fallback, "dflt") &&
            strcmp(fallback, "dflt") == 0,
        "fallback for an absent field"
    );
    check_text(*record, wire, "an unchanged parsed entry is written as it arrived");
    rover_entry_set_string(*entry, "added", "1");
    check_text(
        *record,
        "{[Rover<S:added=1><S:command=connect_info><S:compression=text><S:seed=a%7b><S:type=client>"
        "]}",
        "a changed entry is written from its fields"
    );
    rover_record_destroy(&record);

    RoverRecordStatus status = RoverRecordStatus::Ok;
    RoverRecord* typed = receive_all(
        "{[Rover<S:command=login_confirmed><U:user_id=2A><U:area_id=1><I:result=-3><S:text=7>]"
        "[YardMaster<S:command=x>]}",
        &status
    );
    check(typed != nullptr, "two-entry record");
    if (typed) {
        RoverEntry* first = rover_record_end_entry(*typed, false);
        uint32_t user_id = 0;
        int32_t result = 0;
        check(rover_entry_get_uint(*first, "user_id", &user_id, 0) && user_id == 0x2au, "U field");
        check(rover_entry_get_int(*first, "result", &result, 0) && result == -3, "I field");
        check(
            rover_entry_get_int(*first, "text", &result, 9) && result == -1, "an S field read as I"
        );
        check(
            rover_entry_get_uint(*first, "result", &user_id, 9) && user_id == 0,
            "an I field read as U"
        );
        check(rover_record_entry_count(*typed) == 2, "entry count");
        RoverEntry* second = rover_record_end_entry(*typed, true);
        check(rover_text_equals(second->name, second->name_length, "YardMaster"), "second entry");
        rover_record_destroy(&typed);
    }

    struct Refused {
        const char* text;
        RoverRecordStatus status;
        const char* what;
    };

    const Refused refused[] = {
        {"x{[Rover<S:a=1>]}", RoverRecordStatus::Malformed, "not starting at a brace"},
        {"{}", RoverRecordStatus::Malformed, "no entry"},
        {"{[Rover<S:a=1><S:a=2>]}", RoverRecordStatus::Malformed, "a key twice"},
        {"{[Rover<I:n=>]}", RoverRecordStatus::Malformed, "an empty number"},
        {"{[Rover<U:n=>]}", RoverRecordStatus::Malformed, "an empty hexadecimal number"},
        {"{[Rover<s:a=1>]}", RoverRecordStatus::Malformed, "a lower-case type"},
        {"{[Rover<S:=1>]}", RoverRecordStatus::Malformed, "an empty key"},
        {"{[Rover<S:a=1%>]}", RoverRecordStatus::Malformed, "a trailing escape marker"},
        {"{[Rover]}", RoverRecordStatus::Malformed, "an entry without fields"},
        {"{[<S:a=1>]}", RoverRecordStatus::Malformed, "an entry without a name"},
        {"{<S:a=1>]}", RoverRecordStatus::Malformed, "a bare field"},
        {"{[Rover<S:a=1>]x}", RoverRecordStatus::Malformed, "garbage between entries"},
        {"{[Rover<S:a=1]}", RoverRecordStatus::Malformed, "a field without its close"},
    };
    for (const Refused& case_ : refused) {
        RoverRecord* none = receive_all(case_.text, &status);
        check(none == nullptr && status == case_.status, case_.what);
    }

    RoverRecord* blank = receive_all("{[Rover<S:name=><S:kept=yes>]}", &status);
    check(blank && status == RoverRecordStatus::Ok, "an empty string value parses");
    if (blank) {
        RoverEntry* blank_entry = rover_record_end_entry(*blank, false);
        const char* name = nullptr;
        const char* kept = nullptr;
        check(
            blank_entry && rover_entry_get_string(*blank_entry, "name", &name, "missing") && name &&
                strcmp(name, "") == 0,
            "an empty string is present and empty"
        );
        check(
            rover_entry_get_string(*blank_entry, "kept", &kept, nullptr) &&
                strcmp(kept, "yes") == 0,
            "the field after an empty string is intact"
        );
        check_text(
            *blank,
            "{[Rover<S:name=><S:kept=yes>]}",
            "an unchanged empty string is written as it arrived"
        );
        rover_record_destroy(&blank);
    }

    RoverRecord* odd = receive_all("{[Rover<S:v=a%9b>]}", &status);
    check(odd && status == RoverRecordStatus::Ok, "an escape digit past 8 is accepted");
    if (odd) {
        const RoverField* field = rover_entry_find(*rover_record_end_entry(*odd, false), "v");
        check(
            field && field->value_length == 4 && static_cast<uint8_t>(field->value[0]) == 'a' &&
                static_cast<uint8_t>(field->value[1]) == 0xc3 &&
                static_cast<uint8_t>(field->value[2]) == 0xbf &&
                static_cast<uint8_t>(field->value[3]) == 'b',
            "an invalid digit is byte 0xff, read as U+00FF"
        );
        check_text(
            *odd, "{[Rover<S:v=a%9b>]}", "an unchanged entry keeps the escape it arrived with"
        );
        rover_record_destroy(&odd);
    }

    char huge[kRoverRecordTextLimit + 8];
    memset(huge, 'x', sizeof huge);
    huge[0] = '{';
    RoverRecord* pending_huge = nullptr;
    RoverRecord* out = nullptr;
    check(
        rover_record_receive(&pending_huge, huge, kRoverRecordTextLimit, &consumed, &out) ==
            RoverRecordStatus::Incomplete,
        "0x400 bytes without a brace still wait"
    );
    check(
        rover_record_receive(&pending_huge, huge, 1, &consumed, &out) == RoverRecordStatus::TooLong,
        "one byte more is too long"
    );
    rover_record_destroy(&pending_huge);
}

void check_numbers() {
    check(rover_decimal_value(" -42x") == -42, "atol sign and stop");
    check(rover_decimal_value("4294967297") == 1, "atol wraps");
    check(rover_decimal_value("abc") == 0, "atol without digits");
    check(rover_hex_value("2a") == 0x2au, "strtoul hex");
    check(rover_hex_value(" 0X2A") == 0x2au, "strtoul prefix");
    check(rover_hex_value("ffffffff") == 0xffffffffu, "strtoul full range");
    check(rover_hex_value("100000000") == 0xffffffffu, "strtoul clamps on overflow");
    check(rover_hex_value("-1") == 0xffffffffu, "strtoul negates");
    check(rover_hex_value("zz") == 0, "strtoul without digits");
}

// A received entry's fields read through the accessors the handlers use: in
// key order whatever the wire order, each with its key, type letter and
// decoded text.
void check_field_accessors() {
    RoverRecordStatus status = RoverRecordStatus::Ok;
    RoverRecord* record =
        receive_all("{[Rover<U:user_id=2a><S:command=relogin_confirmed><I:result=-3>]}", &status);
    check(status == RoverRecordStatus::Ok && record, "a received record to read");
    if (!record)
        return;
    const RoverEntry* entry = rover_record_end_entry(*record, false);
    const RoverField* command = entry ? entry->first_field : nullptr;
    const RoverField* result = command ? rover_field_next(*command) : nullptr;
    const RoverField* user = result ? rover_field_next(*result) : nullptr;
    check(
        command && result && user && rover_field_next(*user) == nullptr, "three fields, then none"
    );
    if (command && result && user) {
        check(
            rover_field_type(*command) == RoverFieldType::String &&
                memcmp(rover_field_key(*command), "command", 7) == 0 && command->key_length == 7 &&
                memcmp(command->value, "relogin_confirmed", 17) == 0,
            "the first field in key order"
        );
        check(
            rover_field_type(*result) == RoverFieldType::Int &&
                memcmp(rover_field_key(*result), "result", 6) == 0 &&
                rover_field_int(*result, 0) == -3,
            "a signed field"
        );
        check(
            rover_field_type(*user) == RoverFieldType::Uint &&
                memcmp(rover_field_key(*user), "user_id", 7) == 0 &&
                rover_field_uint(*user, 0) == 0x2a,
            "an unsigned field"
        );
    }
    rover_record_destroy(&record);
}

// The formatter's reserved-byte scan, with std::string::find_first_of
// semantics: first match from the start offset, a NUL compared like any byte,
// nothing found from past the end or in an empty set.
void check_find_char_in_set() {
    const char reserved[] = "{}[]<>:=%";
    const int32_t reserved_length = 9;
    check(
        rover_find_char_in_set("user_id", 7, reserved, 0, reserved_length) == kRoverCharNotFound,
        "a plain key holds no reserved byte"
    );
    check(
        rover_find_char_in_set("a=b<c", 5, reserved, 0, reserved_length) == 1,
        "the first reserved byte's offset"
    );
    check(
        rover_find_char_in_set("a=b<c", 5, reserved, 2, reserved_length) == 3,
        "the scan starts at the offset given"
    );
    check(
        rover_find_char_in_set("a=b", 3, reserved, 3, reserved_length) == kRoverCharNotFound,
        "an offset at the end finds nothing"
    );
    check(
        rover_find_char_in_set("a=b", 3, reserved, 0, 0) == kRoverCharNotFound,
        "an empty set finds nothing"
    );
    const char with_nul[] = {'a', '\0', ':'};
    check(
        rover_find_char_in_set(with_nul, 3, reserved, 0, reserved_length) == 2,
        "a NUL does not end the text"
    );
    check(
        rover_find_char_in_set(with_nul, 3, reserved, 0, reserved_length + 1) == 1,
        "a set counted past its last byte includes its terminator"
    );
}

} // namespace

int main() {
    check_find_char_in_set();
    check_field_accessors();
    check_building();
    check_receiving();
    check_numbers();
    if (failures)
        return 1;
    puts("rover record: ok");
    return 0;
}
