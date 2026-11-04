// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Rover's record-file reader and lazy record parser, and the two slots that
// turn every record of a file into a property bag (33 and 37).

#include "records.h"

#include "heap.h"
#include "platform_select.h"
#include "slots.h"

#include "oa/rover/strings.h"

#include <new>
#include <stdio.h>
#include <string.h>

namespace {

constexpr char kReservedBytes[] = "{}[]<>:=%";
// Line count handed to the line reader: at most 0x7fe characters.
constexpr int32_t kLineReadCount = kRoverReaderLineCapacity - 1;
constexpr uint8_t kEndOfFileByte = 0x1a;
constexpr int kEndOfFile = -1;
constexpr uint8_t kInvalidEscape = 0xff;
// Length of "<T:" plus "=" and ">" around a field's key and value.
constexpr int32_t kFieldDelimiterLength = 5;

// The seven fields slot 33 hands over as arguments instead of in the bag.
constexpr const char* kParsedRecordKeys[] = {
    "command",
    "id",
    "name",
    "sort",
    "category",
    "headline",
    "file",
};

// The selected table places a relative path: the player's copy, then the
// game's files, then the game folder. With none selected it is the native
// table, so a path opened directly still reads the file named.
const RoverPlatform& platform() {
    return *rover_platform();
}

// The "C" locale's character classes.
bool is_space(uint8_t c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

bool is_digit(uint8_t c) {
    return c >= '0' && c <= '9';
}

bool is_alpha(uint8_t c) {
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

// True when a key or name contains a reserved byte; a NUL inside the
// run counts as one too.
bool has_reserved_byte(const char* text, int32_t length) {
    for (int32_t i = 0; i < length; ++i)
        if (text[i] == 0 || strchr(kReservedBytes, text[i]))
            return true;
    return false;
}

int text_file_raw_byte(RoverTextFile* file, bool consume) {
    if (file->position == file->length) {
        int32_t read = platform().file_read(file->file, file->buffer, sizeof file->buffer);
        if (read <= 0)
            return kEndOfFile;
        file->position = 0;
        file->length = read;
    }
    uint8_t c = file->buffer[file->position];
    if (consume)
        ++file->position;
    return c;
}

int text_file_byte(RoverTextFile* file) {
    if (file->ended)
        return kEndOfFile;
    int c = text_file_raw_byte(file, true);
    if (c == kEndOfFileByte) {
        file->ended = true;
        return kEndOfFile;
    }
    if (c == '\r' && text_file_raw_byte(file, false) == '\n')
        return text_file_raw_byte(file, true);
    return c;
}

// A line read: up to count - 1 characters through a newline.
// False at the end of the file with nothing read.
bool text_file_line(RoverTextFile* file, char* line, int32_t count) {
    int32_t used = 0;
    while (used < count - 1) {
        int c = text_file_byte(file);
        if (c == kEndOfFile)
            break;
        line[used++] = static_cast<char>(c);
        if (c == '\n')
            break;
    }
    line[used] = 0;
    return used > 0;
}

void free_fields(RoverRecordEntry* entry) {
    RoverRecordField* field = entry->fields;
    while (field) {
        RoverRecordField* next = field->next;
        rover_string_release(&field->key);
        rover_string_release(&field->value);
        rover_heap_free(field);
        field = next;
    }
    entry->fields = nullptr;
}

void free_entries(RoverFileRecord* record) {
    RoverRecordEntry* entry = record->entries;
    while (entry) {
        RoverRecordEntry* next = entry->next;
        free_fields(entry);
        rover_string_release(&entry->name);
        rover_heap_free(entry);
        entry = next;
    }
    record->entries = nullptr;
}

/// Appends a chunk up to and including its first "}" to the record being read, starting one when
/// the chunk opens with "{".
///
/// @param[in,out] partial record being read; created here when null
/// @param text chunk of file text
/// @param length bytes of text
/// @param[out] out_consumed bytes taken from text
/// @param[out] out_record the completed record once its "}" arrived, else null
/// @return ok once the record closed; need_more before; malformed when a new record does not open
///     with "{"; out_of_memory; too_long past kRoverRecordTextCapacity
RoverReadStatus append_record_text(
    RoverFileRecord** partial,
    const char* text,
    int32_t length,
    int32_t* out_consumed,
    RoverFileRecord** out_record
) {
    *out_record = nullptr;
    *out_consumed = 0;
    RoverFileRecord* record = *partial;
    if (!record) {
        if (text[0] != '{')
            return RoverReadStatus::malformed;
        void* block = rover_heap_alloc(sizeof(RoverFileRecord));
        if (!block)
            return RoverReadStatus::out_of_memory;
        record = new (block) RoverFileRecord{};
        *partial = record;
    }
    const char* close = static_cast<const char*>(memchr(text, '}', static_cast<size_t>(length)));
    int32_t taken = close ? static_cast<int32_t>(close - text) + 1 : length;
    *out_consumed = taken;
    if (record->length + taken > kRoverRecordTextCapacity)
        return RoverReadStatus::too_long;
    memcpy(record->text + record->length, text, static_cast<size_t>(taken));
    record->length += taken;
    if (!close)
        return RoverReadStatus::need_more;
    *out_record = record;
    return RoverReadStatus::ok;
}

/// Feeds one chunk to the record being read; a completed record leaves the rest of the chunk, from
/// its next "{", pending for the following read.
///
/// @param[in,out] reader reader whose pending text is updated
/// @param[in,out] partial record being read; created here when null
/// @param text NUL-terminated chunk; may lie inside the reader's pending text
/// @param[out] out_record the completed record on ok, else null
/// @return the status of append_record_text
RoverReadStatus feed_record_text(
    RoverRecordReader* reader,
    RoverFileRecord** partial,
    const char* text,
    RoverFileRecord** out_record
) {
    int32_t length = static_cast<int32_t>(strlen(text));
    int32_t consumed = 0;
    RoverReadStatus status = append_record_text(partial, text, length, &consumed, out_record);
    if (status == RoverReadStatus::ok) {
        const char* rest = text + consumed;
        while (*rest && *rest != '{')
            ++rest;
        // rest may lie inside pending itself.
        memmove(reader->pending, rest, strlen(rest) + 1);
        return status;
    }
    if (status == RoverReadStatus::need_more)
        reader->pending[0] = 0;
    *out_record = nullptr;
    return status;
}

/// Splits the record text into "]"-terminated entries up to the closing "}".
///
/// @param[in,out] record record whose entry list is built; freed again on failure
/// @return false when an entry lacks its "]", memory runs out, or no "}" follows an entry
bool split_entries(RoverFileRecord* record) {
    const char* text = record->text + 1;
    int32_t remaining = record->length - 1;
    RoverRecordEntry** tail = &record->entries;
    while (remaining > 0) {
        const char* close =
            static_cast<const char*>(memchr(text, ']', static_cast<size_t>(remaining)));
        void* block = close ? rover_heap_alloc(sizeof(RoverRecordEntry)) : nullptr;
        if (!block) {
            free_entries(record);
            return false;
        }
        RoverRecordEntry* entry = new (block) RoverRecordEntry{};
        entry->text = text;
        entry->length = static_cast<int32_t>(close - text) + 1;
        *tail = entry;
        tail = &entry->next;
        text += entry->length;
        remaining -= entry->length;
        if (remaining > 0 && *text == '}') {
            record->entries_ready = true;
            return true;
        }
    }
    free_entries(record);
    return false;
}

RoverRecordField* find_field(RoverRecordEntry* entry, const char* key, uint32_t key_length) {
    for (RoverRecordField* field = entry->fields; field; field = field->next)
        if (rover_string_compare(&field->key, 0, field->key.length, key, key_length) == 0)
            return field;
    return nullptr;
}

// Inserts in key order; false when the key is already present.
bool insert_field(RoverRecordEntry* entry, RoverRecordField* field) {
    RoverRecordField** at = &entry->fields;
    while (*at) {
        int32_t order = rover_string_compare(
            &(*at)->key, 0, (*at)->key.length, rover_string_text(&field->key), field->key.length
        );
        if (order == 0)
            return false;
        if (order > 0)
            break;
        at = &(*at)->next;
    }
    field->next = *at;
    *at = field;
    return true;
}

/// Parses an entry's "[Name" header and, unless name_only, every field up to its "]".
///
/// The name is kept even when a field fails; a repeated key fails the entry, and on success the
/// fields cover the whole span.
///
/// @param[in,out] entry entry to parse
/// @param name_only true to parse only the header
/// @return true when the parts asked for parsed
bool parse_entry(RoverRecordEntry* entry, bool name_only) {
    const char* text = entry->text;
    int32_t length = entry->length;
    if (length < 1 || text[0] != '[')
        return false;
    const char* name = text + 1;
    const char* open = static_cast<const char*>(memchr(name, '<', static_cast<size_t>(length - 1)));
    if (!open || open == name)
        return false;
    int32_t name_length = static_cast<int32_t>(open - name);
    if (has_reserved_byte(name, name_length))
        return false;
    if (!entry->name_ready) {
        rover_string_assign_text(&entry->name, name, static_cast<uint32_t>(name_length));
        entry->name_ready = true;
    }
    if (name_only)
        return true;

    int32_t position = name_length + 1;
    const char* cursor = open;
    for (;;) {
        if (length - position <= 0)
            break;
        void* block = rover_heap_alloc(sizeof(RoverRecordField));
        if (!block)
            break;
        RoverRecordField* field = new (block) RoverRecordField{};
        int32_t taken = 0;
        bool parsed = rover_record_parse_field(cursor, length - position, field, &taken);
        if (parsed) {
            position += taken;
            cursor += taken;
        }
        if (!parsed || length - position <= 0 || !insert_field(entry, field)) {
            rover_string_release(&field->key);
            rover_string_release(&field->value);
            rover_heap_free(field);
            break;
        }
        if (*cursor == ']') {
            entry->fields_ready = true;
            return true;
        }
    }
    free_fields(entry);
    return false;
}

// Copies a field into bag under a fresh copy of its key: type I as signed
// decimal, U as unsigned hexadecimal, S as text copied up to its first NUL.
void add_field_to_bag(RoverPropertyBag* bag, const RoverRecordField* field) {
    const char* key_text = rover_string_text(&field->key);
    RoverString key{};
    if (field->type == 'S') {
        const char* value_text = rover_string_text(&field->value);
        RoverString value{};
        rover_string_assign_text(&value, value_text, static_cast<uint32_t>(strlen(value_text)));
        rover_string_assign_text(&key, key_text, static_cast<uint32_t>(strlen(key_text)));
        rover_set_bag_string(bag, &key, &value);
        rover_string_release(&value);
    } else {
        rover_string_assign_text(&key, key_text, static_cast<uint32_t>(strlen(key_text)));
        if (field->type == 'I')
            rover_set_field_int(bag, &key, rover_record_field_int(field, -1));
        else
            rover_set_field_uint(bag, &key, rover_record_field_uint(field, 0));
    }
    rover_string_release(&key);
}

} // namespace

void rover_record_reader_init(RoverRecordReader* reader, const char* path) {
    *reader = RoverRecordReader{};
    if (path)
        snprintf(reader->path, sizeof reader->path, "%s", path);
}

int32_t rover_record_reader_open(RoverRecordReader* reader) {
    if (reader->open || reader->path[0] == 0)
        return -1;
    RoverFile* file = platform().file_open(reader->path);
    if (!file)
        return -1;
    reader->file = RoverTextFile{};
    reader->file.file = file;
    reader->pending[0] = 0;
    reader->open = true;
    return 0;
}

void rover_record_reader_close(RoverRecordReader* reader) {
    if (!reader->open)
        return;
    reader->pending[0] = 0;
    platform().file_close(reader->file.file);
    reader->file.file = nullptr;
    reader->open = false;
}

RoverRecordReader* rover_record_reader_create(const char* path) {
    void* block = rover_heap_alloc(sizeof(RoverRecordReader));
    if (!block)
        return nullptr;
    RoverRecordReader* reader = new (block) RoverRecordReader{};
    rover_record_reader_init(reader, path);
    return reader;
}

void rover_record_reader_destroy(RoverRecordReader* reader) {
    if (!reader)
        return;
    rover_record_reader_close(reader);
    rover_heap_free(reader);
}

RoverReadStatus rover_record_reader_next(RoverRecordReader* reader, RoverFileRecord** out_record) {
    *out_record = nullptr;
    if (!reader->open)
        return RoverReadStatus::not_open;
    RoverFileRecord* partial = nullptr;
    if (reader->pending[0]) {
        RoverReadStatus status = feed_record_text(reader, &partial, reader->pending, out_record);
        if (status == RoverReadStatus::ok)
            return status;
        if (status != RoverReadStatus::need_more) {
            rover_file_record_destroy(partial);
            return status;
        }
    }
    char line[kRoverReaderLineCapacity];
    while (text_file_line(&reader->file, line, kLineReadCount)) {
        const char* text = line;
        if (!partial) {
            while (*text && *text != '{')
                ++text;
        }
        if (!*text)
            continue;
        RoverReadStatus status = feed_record_text(reader, &partial, text, out_record);
        if (status == RoverReadStatus::ok)
            return status;
        if (status != RoverReadStatus::need_more) {
            rover_file_record_destroy(partial);
            return status;
        }
    }
    rover_file_record_destroy(partial);
    return RoverReadStatus::end_of_file;
}

void rover_file_record_destroy(RoverFileRecord* record) {
    if (!record)
        return;
    free_entries(record);
    rover_heap_free(record);
}

RoverRecordEntry* rover_record_first_entry(RoverFileRecord* record) {
    if (!record->entries_ready && !split_entries(record))
        return nullptr;
    return record->entries;
}

const RoverString* rover_record_entry_name(RoverRecordEntry* entry) {
    if (!entry || (!entry->name_ready && !parse_entry(entry, true)))
        return nullptr;
    return &entry->name;
}

bool rover_record_entry_is(RoverRecordEntry* entry, const char* name) {
    const RoverString* entry_name = rover_record_entry_name(entry);
    return entry_name && rover_string_equals(entry_name, name);
}

const RoverRecordField* rover_record_entry_fields(RoverRecordEntry* entry) {
    if (!entry || (!entry->fields_ready && !parse_entry(entry, false)))
        return nullptr;
    return entry->fields;
}

const RoverRecordField* rover_record_entry_find(RoverRecordEntry* entry, const char* key) {
    if (!rover_record_entry_fields(entry))
        return nullptr;
    return find_field(entry, key, static_cast<uint32_t>(strlen(key)));
}

const char*
rover_record_entry_text(RoverRecordEntry* entry, const char* key, const char* fallback) {
    const RoverRecordField* field = rover_record_entry_find(entry, key);
    return field ? rover_string_text(&field->value) : fallback;
}

int32_t rover_record_entry_int(RoverRecordEntry* entry, const char* key, int32_t fallback) {
    const RoverRecordField* field = rover_record_entry_find(entry, key);
    return field ? rover_record_field_int(field, -1) : fallback;
}

int32_t rover_record_field_int(const RoverRecordField* field, int32_t fallback) {
    if (field->type != 'I')
        return fallback;
    return rover_parse_decimal(rover_string_text(&field->value));
}

uint32_t rover_record_field_uint(const RoverRecordField* field, uint32_t fallback) {
    if (field->type != 'U')
        return fallback;
    return rover_parse_hexadecimal(rover_string_text(&field->value));
}

bool rover_record_parse_field(
    const char* text, int32_t length, RoverRecordField* field, int32_t* out_length
) {
    *out_length = 0;
    if (length < 3 || text[0] != '<')
        return false;
    char type = text[1];
    if ((type != 'I' && type != 'S' && type != 'U') || text[2] != ':')
        return false;
    const char* key = text + 3;
    int32_t remaining = length - 3;
    if (remaining <= 0)
        return false;
    const char* equals = static_cast<const char*>(memchr(key, '=', static_cast<size_t>(remaining)));
    if (!equals || equals == key)
        return false;
    int32_t key_length = static_cast<int32_t>(equals - key);
    if (has_reserved_byte(key, key_length))
        return false;
    const char* value = equals + 1;
    remaining = length - (key_length + 4);
    if (remaining <= 0)
        return false;
    const char* close =
        static_cast<const char*>(memchr(value, '>', static_cast<size_t>(remaining)));
    if (!close || close == value)
        return false;
    int32_t value_length = static_cast<int32_t>(close - value);
    field->type = type;
    rover_string_assign_text(&field->key, key, static_cast<uint32_t>(key_length));
    if (!rover_record_decode_value(&field->value, value, value_length))
        return false;
    *out_length = key_length + value_length + kFieldDelimiterLength;
    return true;
}

bool rover_record_decode_value(RoverString* value, const char* text, int32_t length) {
    rover_string_assign_text(value, text, static_cast<uint32_t>(length));
    if (length == 0)
        return true;
    if (!value->data)
        return false;
    char* read = value->data;
    char* write = value->data;
    int32_t remaining = length;
    int32_t decoded_length = length;
    for (;;) {
        const char* percent =
            static_cast<const char*>(memchr(read, '%', static_cast<size_t>(remaining)));
        int32_t run = percent ? static_cast<int32_t>(percent - read) : remaining;
        if (read != write && run > 0)
            memmove(write, read, static_cast<size_t>(run));
        write += run;
        if (!percent)
            break;
        read += run + 1;
        remaining -= run + 1;
        if (remaining <= 0)
            return false;
        *write++ = static_cast<char>(rover_record_escaped_byte(*read++));
        --decoded_length;
        --remaining;
    }
    value->length = static_cast<uint32_t>(decoded_length);
    value->data[decoded_length] = 0;
    return true;
}

uint8_t rover_record_escaped_byte(char digit) {
    if (digit < '0' || digit > '8')
        return kInvalidEscape;
    return static_cast<uint8_t>(kReservedBytes[digit - '0']);
}

int32_t rover_parse_decimal(const char* text) {
    const uint8_t* at = reinterpret_cast<const uint8_t*>(text);
    while (is_space(*at))
        ++at;
    uint8_t sign = *at;
    if (sign == '-' || sign == '+')
        ++at;
    uint32_t total = 0;
    while (is_digit(*at))
        total = total * 10 + static_cast<uint32_t>(*at++ - '0');
    return static_cast<int32_t>(sign == '-' ? 0u - total : total);
}

uint32_t rover_parse_hexadecimal(const char* text) {
    constexpr uint32_t kBase = 16;
    constexpr uint32_t kLimit = 0xffffffffu / kBase;
    constexpr uint32_t kLastDigit = 0xffffffffu % kBase;
    const uint8_t* at = reinterpret_cast<const uint8_t*>(text);
    while (is_space(*at))
        ++at;
    bool negative = *at == '-';
    if (*at == '-' || *at == '+')
        ++at;
    if (at[0] == '0' && (at[1] == 'x' || at[1] == 'X'))
        at += 2;
    uint32_t value = 0;
    bool any_digit = false;
    bool overflow = false;
    for (;; ++at) {
        uint32_t digit;
        if (is_digit(*at))
            digit = static_cast<uint32_t>(*at - '0');
        else if (is_alpha(*at))
            digit = static_cast<uint32_t>((*at & ~0x20) - 'A' + 10);
        else
            break;
        if (digit >= kBase)
            break;
        any_digit = true;
        if (value < kLimit || (value == kLimit && digit <= kLastDigit))
            value = value * kBase + digit;
        else
            overflow = true;
    }
    if (!any_digit)
        return 0;
    if (overflow)
        value = 0xffffffffu;
    return negative ? 0u - value : value;
}

bool ROVER_CALL
rover_parse_record_file(const char* path, RoverParsedRecordFn on_record, void* user) {
    RoverRecordReader reader{};
    rover_record_reader_init(&reader, path);
    if (rover_record_reader_open(&reader) != 0)
        return false;
    RoverFileRecord* record = nullptr;
    while (rover_record_reader_next(&reader, &record) == RoverReadStatus::ok) {
        RoverRecordEntry* entry = rover_record_first_entry(record);
        const char* named[sizeof kParsedRecordKeys / sizeof kParsedRecordKeys[0]];
        for (size_t i = 0; i < sizeof named / sizeof named[0]; ++i)
            named[i] = rover_record_entry_text(entry, kParsedRecordKeys[i], "");
        RoverPropertyBag* bag = rover_create_property_bag();
        for (const RoverRecordField* field = rover_record_entry_fields(entry); field;
             field = field->next) {
            bool passed_as_text = false;
            for (const char* key : kParsedRecordKeys)
                passed_as_text = passed_as_text || rover_string_equals(&field->key, key);
            if (!passed_as_text)
                add_field_to_bag(bag, field);
        }
        on_record(named[0], named[1], named[2], named[3], named[4], named[5], named[6], bag, user);
        if (bag)
            rover_destroy_property_bag(bag);
        rover_file_record_destroy(record);
    }
    rover_record_reader_close(&reader);
    return true;
}

bool ROVER_CALL rover_enumerate_record_file(const char* path, RoverRecordFieldsFn on_record) {
    if (!on_record)
        return false;
    RoverRecordReader* reader = rover_record_reader_create(path);
    if (!reader)
        return false;
    RoverFileRecord* record = nullptr;
    if (rover_record_reader_open(reader) != 0 ||
        rover_record_reader_next(reader, &record) != RoverReadStatus::ok) {
        rover_record_reader_destroy(reader);
        return false;
    }
    for (;;) {
        RoverRecordEntry* entry = rover_record_first_entry(record);
        if (!entry) {
            rover_file_record_destroy(record);
            rover_record_reader_destroy(reader);
            return false;
        }
        RoverPropertyBag* bag = rover_create_property_bag();
        for (const RoverRecordField* field = rover_record_entry_fields(entry); field;
             field = field->next)
            add_field_to_bag(bag, field);
        RoverFileRecord* following = nullptr;
        bool last = rover_record_reader_next(reader, &following) != RoverReadStatus::ok;
        on_record(bag, last);
        if (bag)
            rover_destroy_property_bag(bag);
        rover_file_record_destroy(record);
        record = following;
        if (last)
            break;
    }
    rover_record_reader_destroy(reader);
    return true;
}
