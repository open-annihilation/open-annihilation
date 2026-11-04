// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The record reader and parser behind slots 33 and 37: escapes, field
// syntax, the number conversions, read-text line handling, lazy
// entry parsing, and both slots over synthetic files. With --data, the
// shipped Profile.def and Ladders.def of the BY1117a install OA_GAME_DIR
// names instead.

#include "../src/heap.h"
#include "../src/records.h"

#include "oa/rover/interface.h"
#include "oa/rover/strings.h"
#include "oa/boneyards_test/installs.hpp"
#include "oa/platform/files.hpp"

#include <new>
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

const RoverInterface* rover = nullptr;

bool reads(const RoverString& string, const char* text) {
    return strcmp(rover_string_text(&string), text) == 0 && string.length == strlen(text);
}

void write_file(const char* path, const char* bytes, size_t length) {
    FILE* file = oa::platform::open_file(path, "wb");
    fwrite(bytes, 1, length, file);
    fclose(file);
}

void write_file(const char* path, const char* text) {
    write_file(path, text, strlen(text));
}

/// Checks the record escape digits and the decoding of escaped record values.
void check_escapes() {
    const char digits[] = "012345678";
    const char bytes[] = "{}[]<>:=%";
    bool all = true;
    for (int i = 0; i < 9; ++i)
        all = all && rover_record_escaped_byte(digits[i]) == static_cast<uint8_t>(bytes[i]);
    check(all, "escape digits 0..8 name the reserved bytes in order");
    check(rover_record_escaped_byte('9') == 0xff, "escape digit 9 is invalid");
    check(rover_record_escaped_byte('/') == 0xff, "the byte before '0' is invalid");
    check(rover_record_escaped_byte(0) == 0xff, "NUL is invalid");
    check(rover_record_escaped_byte(static_cast<char>(0xff)) == 0xff, "0xff is invalid");

    RoverString value{};
    check(
        rover_record_decode_value(&value, "42", 2) && reads(value, "42"), "no escapes: plain copy"
    );
    check(rover_record_decode_value(&value, "a%7b%8c", 7) && reads(value, "a=b%c"), "two escapes");
    check(
        rover_record_decode_value(&value, "%0%1%2%3%4%5%6%7%8", 18) && reads(value, "{}[]<>:=%"),
        "every escape back to back"
    );
    check(
        rover_record_decode_value(&value, "%3%2xy", 6) && reads(value, "][xy"), "leading escapes"
    );
    check(
        rover_record_decode_value(&value, "a%9b", 4) && reads(
                                                            value,
                                                            "a\xff"
                                                            "b"
                                                        ),
        "an invalid digit decodes to 0xff, as in 3.1c"
    );
    check(!rover_record_decode_value(&value, "ab%", 3), "a trailing '%' fails");
    check(rover_record_decode_value(&value, "", 0) && reads(value, ""), "an empty value");
    rover_string_release(&value);
}

struct FieldCase {
    const char* text;
    bool parses;
    int32_t length;
    char type;
    const char* key;
    const char* value;
};

void check_field_syntax() {
    const FieldCase cases[] = {
        {"<S:x=42>", true, 8, 'S', "x", "42"},
        {"<S:x=-42>", true, 9, 'S', "x", "-42"},
        {"<I:msg=a%7b%8c>", true, 15, 'I', "msg", "a=b%c"},
        {"<U:id=1f>]", true, 9, 'U', "id", "1f"},
        {"<S:a b=raw <[:= kept>", true, 21, 'S', "a b", "raw <[:= kept"},
        {"<S:k=v>junk", true, 7, 'S', "k", "v"},
        {"<s:x=1>", false, 0, 0, nullptr, nullptr},
        {"<X:x=1>", false, 0, 0, nullptr, nullptr},
        {"<Ix=1>", false, 0, 0, nullptr, nullptr},
        {"x<I:k=1>", false, 0, 0, nullptr, nullptr},
        {"<I:", false, 0, 0, nullptr, nullptr},
        {"<I:k1>", false, 0, 0, nullptr, nullptr},
        {"<I:=1>", false, 0, 0, nullptr, nullptr},
        {"<I:a:b=1>", false, 0, 0, nullptr, nullptr},
        {"<I:a%b=1>", false, 0, 0, nullptr, nullptr},
        {"<I:k=>", false, 0, 0, nullptr, nullptr},
        {"<I:k=1", false, 0, 0, nullptr, nullptr},
        {"<I:k=1%>", false, 0, 0, nullptr, nullptr},
    };
    for (const FieldCase& c : cases) {
        RoverRecordField field{};
        int32_t length = -1;
        bool parsed =
            rover_record_parse_field(c.text, static_cast<int32_t>(strlen(c.text)), &field, &length);
        char what[96];
        snprintf(what, sizeof what, "field %s", c.text);
        if (c.parses)
            check(
                parsed && length == c.length && field.type == c.type && reads(field.key, c.key) &&
                    reads(field.value, c.value),
                what
            );
        else
            check(!parsed && length == 0, what);
        rover_string_release(&field.key);
        rover_string_release(&field.value);
    }
    // The span bounds the search: a '>' past it does not close the value.
    RoverRecordField field{};
    int32_t length = 0;
    check(
        !rover_record_parse_field("<I:k=1>", 6, &field, &length),
        "the value must close inside the span"
    );
    rover_string_release(&field.key);
    rover_string_release(&field.value);
}

void check_numbers() {
    struct Decimal {
        const char* text;
        int32_t value;
    };

    const Decimal decimals[] = {
        {"42", 42},
        {"-7", -7},
        {"+91", 91},
        {" \t\n\v\f\r58", 58},
        {"37abc", 37},
        {"abc", 0},
        {"", 0},
        {"- 5", 0},
        {"0x10", 0},
        {"4294967297", 1},
        {"2147483648", -2147483647 - 1},
        {"-2147483648", -2147483647 - 1},
    };
    for (const Decimal& d : decimals) {
        char what[64];
        snprintf(what, sizeof what, "decimal \"%s\"", d.text);
        check(rover_parse_decimal(d.text) == d.value, what);
    }

    struct Hexadecimal {
        const char* text;
        uint32_t value;
    };

    const Hexadecimal hexadecimals[] = {
        {"2a", 42},
        {"ff", 255},
        {"FFFFFFFF", 0xffffffffu},
        {"-1", 0xffffffffu},
        {"  1f", 31},
        {"1fz", 31},
        {"0x2a", 42},
        {"0X2A", 42},
        {"+A", 10},
        {"zz", 0},
        {"", 0},
        {"0x", 0},
        {"123456789", 0xffffffffu},
        {"-123456789", 1},
    };
    for (const Hexadecimal& h : hexadecimals) {
        char what[64];
        snprintf(what, sizeof what, "hexadecimal \"%s\"", h.text);
        check(rover_parse_hexadecimal(h.text) == h.value, what);
    }
}

// Reads every record of a file into their entry texts, joined by '|'.
bool read_all(const char* path, char* out, size_t capacity, RoverReadStatus* last) {
    RoverRecordReader reader{};
    rover_record_reader_init(&reader, path);
    if (rover_record_reader_open(&reader) != 0)
        return false;
    out[0] = 0;
    RoverFileRecord* record = nullptr;
    while ((*last = rover_record_reader_next(&reader, &record)) == RoverReadStatus::ok) {
        size_t used = strlen(out);
        snprintf(
            out + used, capacity - used, "%s%.*s", used ? "|" : "", record->length, record->text
        );
        rover_file_record_destroy(record);
    }
    rover_record_reader_close(&reader);
    return true;
}

void check_reader() {
    const char* path = "rover-records-test-reader.txt";
    char out[8192];
    RoverReadStatus last = RoverReadStatus::ok;

    RoverRecordReader reader{};
    rover_record_reader_init(&reader, "rover-records-test-missing.txt");
    check(rover_record_reader_open(&reader) == -1, "a missing file does not open");
    RoverFileRecord* record = nullptr;
    check(
        rover_record_reader_next(&reader, &record) == RoverReadStatus::not_open && !record,
        "reading an unopened reader"
    );
    rover_record_reader_init(&reader, "");
    check(rover_record_reader_open(&reader) == -1, "an empty path does not open");

    write_file(path, "{[A<S:k=v>]}\r\n{[B<I:n=5>]}\r\n");
    check(
        read_all(path, out, sizeof out, &last) && strcmp(out, "{[A<S:k=v>]}|{[B<I:n=5>]}") == 0 &&
            last == RoverReadStatus::end_of_file,
        "one record per line, CR LF skipped"
    );

    write_file(path, "junk {[A<S:k=1>]} mid {[B<S:k=2>]}tail\n{[C<S:k=3>]}");
    check(
        read_all(path, out, sizeof out, &last) &&
            strcmp(out, "{[A<S:k=1>]}|{[B<S:k=2>]}|{[C<S:k=3>]}") == 0,
        "records sharing a line, text between them skipped"
    );

    write_file(path, "{[A<S:k=a\r\nb\rc>]}\r\n");
    check(
        read_all(path, out, sizeof out, &last) && strcmp(out, "{[A<S:k=a\nb\rc>]}") == 0,
        "a record spans lines; read-text keeps a lone CR"
    );

    write_file(path, "{[A<S:k=1>]}\n\x1a{[B<S:k=2>]}\n");
    check(
        read_all(path, out, sizeof out, &last) && strcmp(out, "{[A<S:k=1>]}") == 0,
        "Ctrl-Z ends the file"
    );

    write_file(path, "{[A<S:k=1>]\n");
    check(
        read_all(path, out, sizeof out, &last) && out[0] == 0 &&
            last == RoverReadStatus::end_of_file,
        "a record still open at the end is dropped"
    );

    // Longer than the 0x7fe-character line read: the rest of the line is
    // appended to the open record.
    static char text[kRoverRecordTextCapacity + 64];
    memset(text, 'v', sizeof text);
    memcpy(text, "{[A<S:k=", 8);
    memcpy(text + 3000, ">]}\n", 5);
    write_file(path, text, 3004);
    check(
        read_all(path, out, sizeof out, &last) && strlen(out) == 3003 &&
            last == RoverReadStatus::end_of_file,
        "a record over several line reads"
    );

    memset(text, 'v', sizeof text);
    memcpy(text, "{[A<S:k=", 8);
    memcpy(text + kRoverRecordTextCapacity - 3, ">]}", 3);
    write_file(path, text, kRoverRecordTextCapacity);
    check(
        read_all(path, out, sizeof out, &last) && strlen(out) == kRoverRecordTextCapacity,
        "a record of exactly 4096 bytes"
    );
    text[kRoverRecordTextCapacity - 3] = 'v';
    memcpy(text + kRoverRecordTextCapacity - 2, ">]}", 3);
    write_file(path, text, kRoverRecordTextCapacity + 1);
    check(
        read_all(path, out, sizeof out, &last) && out[0] == 0 && last == RoverReadStatus::too_long,
        "a record of 4097 bytes is too long"
    );
    remove(path);
}

RoverFileRecord* record_of(const char* text) {
    RoverFileRecord* record = new (rover_heap_alloc(sizeof(RoverFileRecord))) RoverFileRecord{};
    record->length = static_cast<int32_t>(strlen(text));
    memcpy(record->text, text, strlen(text));
    return record;
}

void check_entries() {
    RoverFileRecord* record =
        record_of("{[Map<S:name=Hub><I:id=45><U:mask=ff><S:alpha=a>][Second<S:x=1>]}");
    RoverRecordEntry* entry = rover_record_first_entry(record);
    check(entry && entry->next && !entry->next->next, "two entries");
    check(
        rover_record_entry_is(entry, "Map") && !rover_record_entry_is(entry, "map"), "entry name"
    );
    const char* order[] = {"alpha", "id", "mask", "name"};
    const RoverRecordField* field = rover_record_entry_fields(entry);
    bool ordered = true;
    for (const char* key : order) {
        ordered = ordered && field && reads(field->key, key);
        field = field ? field->next : nullptr;
    }
    check(ordered && !field, "fields in key order");
    check(rover_record_entry_int(entry, "id", 7) == 45, "a present I field");
    check(
        rover_record_entry_int(entry, "name", 7) == -1, "a present field of another type reads -1"
    );
    check(rover_record_entry_int(entry, "absent", 7) == 7, "an absent field reads the fallback");
    check(strcmp(rover_record_entry_text(entry, "id", "?"), "45") == 0, "text of an I field");
    check(strcmp(rover_record_entry_text(entry, "absent", "?"), "?") == 0, "text fallback");
    check(rover_record_field_uint(rover_record_entry_find(entry, "mask"), 9) == 0xff, "a U field");
    check(
        rover_record_field_uint(rover_record_entry_find(entry, "id"), 9) == 9,
        "a U read of an I field"
    );
    rover_file_record_destroy(record);

    record = record_of("{[Map<S:a=1><S:a=2>]}");
    entry = rover_record_first_entry(record);
    check(
        rover_record_entry_is(entry, "Map") && !rover_record_entry_fields(entry) &&
            !rover_record_entry_find(entry, "a"),
        "a repeated key keeps the name but fails every field"
    );
    rover_file_record_destroy(record);

    record = record_of("{[Map<S:a=1>junk<S:b=2>]}");
    entry = rover_record_first_entry(record);
    check(
        rover_record_entry_is(entry, "Map") && !rover_record_entry_fields(entry),
        "junk between fields"
    );
    rover_file_record_destroy(record);

    record = record_of("{[M:p<S:a=1>]}");
    entry = rover_record_first_entry(record);
    check(
        entry && !rover_record_entry_name(entry) && !rover_record_entry_fields(entry),
        "a reserved byte in the name"
    );
    rover_file_record_destroy(record);

    record = record_of("{<S:x=1>]}");
    entry = rover_record_first_entry(record);
    check(entry && !rover_record_entry_name(entry), "a scalar entry has no name");
    rover_file_record_destroy(record);

    const char* malformed[] = {"{}", "{abc}", "{[A<S:a=1>]xyz}"};
    for (const char* text : malformed) {
        record = record_of(text);
        char what[64];
        snprintf(what, sizeof what, "no entry list in %s", text);
        check(!rover_record_first_entry(record), what);
        check(!rover_record_first_entry(record), "a failed split fails again");
        rover_file_record_destroy(record);
    }
    check(
        !rover_record_entry_fields(nullptr) && !rover_record_entry_name(nullptr),
        "a missing entry has no name or fields"
    );
}

struct Delivered {
    int count;
    char summary[4096];
};

// Appends one bag's entries as key:type:value in key order.
void summarise_bag(RoverPropertyBag* bag, char* out, size_t capacity) {
    RoverString text{};
    for (RoverBagEntry* entry = rover->get_first_bag_entry(bag); entry;
         entry = rover->get_next_bag_entry(bag)) {
        RoverString name{};
        rover->get_entry_name(entry, &name);
        size_t used = strlen(out);
        int32_t signed_value = 0;
        uint32_t unsigned_value = 0;
        if (rover->get_entry_int(entry, &signed_value))
            snprintf(
                out + used, capacity - used, " %s:I:%d", rover_string_text(&name), signed_value
            );
        else if (rover->get_entry_uint(entry, &unsigned_value))
            snprintf(
                out + used, capacity - used, " %s:U:%u", rover_string_text(&name), unsigned_value
            );
        else if (rover->get_entry_string(entry, &text))
            snprintf(
                out + used,
                capacity - used,
                " %s:S:%s",
                rover_string_text(&name),
                rover_string_text(&text)
            );
        rover_string_release(&name);
    }
    rover_string_release(&text);
}

bool ROVER_CALL on_parsed(
    const char* command,
    const char* id,
    const char* name,
    const char* sort,
    const char* category,
    const char* headline,
    const char* file,
    RoverPropertyBag* bag,
    void* user
) {
    auto* delivered = static_cast<Delivered*>(user);
    ++delivered->count;
    size_t used = strlen(delivered->summary);
    snprintf(
        delivered->summary + used,
        sizeof delivered->summary - used,
        "%s[%s|%s|%s|%s|%s|%s|%s]",
        used ? "\n" : "",
        command,
        id,
        name,
        sort,
        category,
        headline,
        file
    );
    summarise_bag(bag, delivered->summary, sizeof delivered->summary);
    return true;
}

Delivered enumerated{};

void ROVER_CALL on_enumerated(RoverPropertyBag* bag, bool is_last) {
    ++enumerated.count;
    size_t used = strlen(enumerated.summary);
    snprintf(
        enumerated.summary + used,
        sizeof enumerated.summary - used,
        "%s%s",
        used ? "\n" : "",
        is_last ? "last" : "more"
    );
    summarise_bag(bag, enumerated.summary, sizeof enumerated.summary);
}

void check_parse_record_file() {
    const char* path = "rover-records-test-parse.txt";
    write_file(
        path,
        "{[Profile<S:command=ladder_template><I:sort=1><S:file=a.htm><I:type=4><U:mask=1f>]}\r\n"
        "{[Ladder<S:command=headline><I:id=2><S:headline=Winner%1><S:note=x%7y>]}\r\n"
        "{broken}\r\n"
        "{[Ladder<S:command=category><I:id=3><S:name=Galactic War>]}\r\n"
    );
    Delivered delivered{};
    check(rover->parse_record_file(path, on_parsed, &delivered), "parse_record_file");
    check(delivered.count == 4, "every record is delivered, a malformed one without fields");
    check(
        strcmp(
            delivered.summary,
            "[ladder_template|||1|||a.htm] mask:U:31 type:I:4\n"
            "[headline|2||||Winner}|] note:S:x=y\n"
            "[||||||]\n"
            "[category|3|Galactic War||||]"
        ) == 0,
        "named fields as text, the rest in the bag by type"
    );
    check(
        !rover->parse_record_file("rover-records-test-missing.txt", on_parsed, &delivered),
        "a missing file fails"
    );

    enumerated = Delivered{};
    check(
        rover->enumerate_record_file(path, on_enumerated) == false,
        "enumeration stops at the record without an entry list"
    );
    check(
        enumerated.count == 2 &&
            strcmp(
                enumerated.summary,
                "more command:S:ladder_template file:S:a.htm mask:U:31 sort:I:1 type:I:4\n"
                "more command:S:headline headline:S:Winner} id:I:2 note:S:x=y"
            ) == 0,
        "enumerate_record_file bags every field"
    );

    write_file(path, "{[A<S:command=one><I:n=1>]}\n{[B<I:n=2>]}\n");
    enumerated = Delivered{};
    check(rover->enumerate_record_file(path, on_enumerated), "enumerate_record_file");
    check(
        enumerated.count == 2 &&
            strcmp(enumerated.summary, "more command:S:one n:I:1\nlast n:I:2") == 0,
        "is_last marks the record after which no other could be read"
    );
    check(!rover->enumerate_record_file(path, nullptr), "a null callback fails");
    check(
        !rover->enumerate_record_file("rover-records-test-missing.txt", on_enumerated),
        "a missing file fails enumeration"
    );
    write_file(path, "no records\n");
    check(!rover->enumerate_record_file(path, on_enumerated), "a file without records fails");
    remove(path);
}

/// Checks that the installed Profile.def and Ladders.def parse and start with their known records.
///
/// @param game_dir the BY1117a install holding the Boneyards files
void check_shipped_files(const char* game_dir) {
    char path[1024];
    snprintf(path, sizeof path, "%s/Boneyards/Profile/Profile.def", game_dir);
    Delivered delivered{};
    check(rover->parse_record_file(path, on_parsed, &delivered), "the shipped Profile.def parses");
    check(delivered.count > 10, "Profile.def holds its templates");
    const char* profile =
        "[ladder_template|||1|||Boneyards/Profile/GWarArm_LadderPU.htm] subtype:I:1 type:I:4\n";
    check(
        strncmp(delivered.summary, profile, strlen(profile)) == 0, "the first Profile.def record"
    );

    snprintf(path, sizeof path, "%s/Boneyards/Ladders/Ladders.def", game_dir);
    delivered = Delivered{};
    check(rover->parse_record_file(path, on_parsed, &delivered), "the shipped Ladders.def parses");
    const char* ladders = "[category|1|Two Player|1|||]\n";
    check(
        strncmp(delivered.summary, ladders, strlen(ladders)) == 0, "the first Ladders.def record"
    );
}

} // namespace

int main(int argc, char** argv) {
    rover_get_interface(&rover);
    if (oa::test::game_data_requested(argc, argv)) {
        const auto game_dir = oa::boneyards_test::require_by_install(
            "the shipped Boneyards record files",
            {"Boneyards/Profile/Profile.def", "Boneyards/Ladders/Ladders.def"}
        );
        check_shipped_files(game_dir.string().c_str());
    } else {
        check_escapes();
        check_field_syntax();
        check_numbers();
        check_reader();
        check_entries();
        check_parse_record_file();
    }
    if (failures)
        return 1;
    puts("rover records: ok");
    return 0;
}
