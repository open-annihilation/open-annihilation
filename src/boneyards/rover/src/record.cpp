// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby message record: building, parsing and writing the bracketed text.

#include "record.h"

#include "oa/rover/wire_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

// The bytes a name, key or value never holds raw; a value escapes each as '%'
// followed by its index here.
constexpr char kReservedBytes[] = "{}[]<>:=%";
constexpr char kEscapeMarker = '%';
constexpr char kFirstEscapeDigit = '0';
constexpr uint8_t kNotEscapable = 0xff;
constexpr char kRecordOpen = '{';
constexpr char kRecordClose = '}';
constexpr char kEntryOpen = '[';
constexpr char kEntryClose = ']';
constexpr char kFieldOpen = '<';
constexpr char kFieldClose = '>';
constexpr char kTypeSeparator = ':';
constexpr char kValueSeparator = '=';
// '<', the type letter, ':', '=' and '>' around a field's key and value.
constexpr int32_t kFieldFraming = 5;

bool is_space(char c) {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/// Measures the leading run of text holding no reserved byte; the run stops at a NUL too.
///
/// @param text text to scan
/// @param length bytes of text to scan at most
/// @return length of the run, at most length
int32_t unreserved_span(const char* text, int32_t length) {
    int32_t span = 0;
    while (span < length && text[span] && !strchr(kReservedBytes, text[span]))
        ++span;
    return span;
}

bool is_valid_label(const char* text, int32_t length) {
    return unreserved_span(text, length) >= length;
}

// The check the formatter makes of a name or key it writes.
bool has_reserved_byte(const char* text, int32_t length) {
    return rover_find_char_in_set(
               text, length, kReservedBytes, 0, static_cast<int32_t>(strlen(kReservedBytes))
           ) != kRoverCharNotFound;
}

/// Returns the escape digit of a reserved byte.
///
/// @param byte byte to escape
/// @return '0' to '8' for a reserved byte, kNotEscapable for any other byte (NUL included)
uint8_t escape_digit(char byte) {
    const char* found = byte ? strchr(kReservedBytes, byte) : nullptr;
    if (!found)
        return kNotEscapable;
    return static_cast<uint8_t>(kFirstEscapeDigit + (found - kReservedBytes));
}

/// Returns the reserved byte an escape digit stands for.
///
/// @param digit the byte after '%'
/// @return the reserved byte for '0' to '8', else 0xff
/// @quirk A digit outside '0'..'8' decodes to 0xff, which the value decoder stores rather than
///     refusing.
char unescaped_byte(char digit) {
    if (digit < kFirstEscapeDigit || digit >= kFirstEscapeDigit + 9)
        return static_cast<char>(kNotEscapable);
    return kReservedBytes[digit - kFirstEscapeDigit];
}

/// Orders two keys as the field tree does: bytes compared unsigned, then the shorter first.
///
/// @param a first key
/// @param a_length bytes of a
/// @param b second key
/// @param b_length bytes of b
/// @return negative, 0 or positive as a orders before, equal to or after b
int32_t compare_keys(const char* a, int32_t a_length, const char* b, int32_t b_length) {
    int32_t common = a_length < b_length ? a_length : b_length;
    int32_t order = memcmp(a, b, static_cast<size_t>(common));
    if (order)
        return order;
    if (a_length < b_length)
        return -1;
    return a_length != b_length ? 1 : 0;
}

// Copies text into the record's storage, NUL-terminated; null when full.
char* store_text(RoverRecord& record, const char* text, int32_t length) {
    if (length < 0 || record.storage_used + length + 1 > kRoverRecordStorage)
        return nullptr;
    char* stored = record.storage + record.storage_used;
    memcpy(stored, text, static_cast<size_t>(length));
    stored[length] = '\0';
    record.storage_used += length + 1;
    return stored;
}

RoverEntry* new_entry(RoverRecord& record) {
    if (record.entry_count >= kRoverRecordEntryLimit)
        return nullptr;
    RoverEntry* entry = &record.entries[record.entry_count++];
    *entry = RoverEntry{};
    entry->record = &record;
    return entry;
}

void unlink_entry(RoverRecord& record, RoverEntry* entry) {
    RoverEntry** link = &record.first_entry;
    while (*link && *link != entry)
        link = &(*link)->next;
    if (*link)
        *link = entry->next;
}

RoverEntry* last_entry(const RoverRecord& record) {
    RoverEntry* entry = record.first_entry;
    while (entry && entry->next)
        entry = entry->next;
    return entry;
}

// Where a field with this key belongs in the entry's key order; null when the
// key is already present.
RoverField** field_slot(RoverEntry& entry, const char* key, int32_t key_length) {
    RoverField** link = &entry.first_field;
    while (*link) {
        int32_t order = compare_keys((*link)->key, (*link)->key_length, key, key_length);
        if (order == 0)
            return nullptr;
        if (order > 0)
            break;
        link = &(*link)->next;
    }
    return link;
}

// Inserts a field in key order; false when the key is taken or no room is left.
bool insert_field(
    RoverEntry& entry,
    RoverFieldType type,
    const char* key,
    int32_t key_length,
    const char* value,
    int32_t value_length
) {
    RoverRecord& record = *entry.record;
    RoverField** slot = field_slot(entry, key, key_length);
    if (!slot || record.field_count >= kRoverRecordFieldLimit)
        return false;
    int32_t storage_mark = record.storage_used;
    const char* stored_key = store_text(record, key, key_length);
    const char* stored_value = stored_key ? store_text(record, value, value_length) : nullptr;
    if (!stored_value) {
        record.storage_used = storage_mark;
        return false;
    }
    RoverField* field = &record.fields[record.field_count++];
    *field = RoverField{};
    field->key = stored_key;
    field->key_length = key_length;
    field->value = stored_value;
    field->value_length = value_length;
    field->type = type;
    field->next = *slot;
    *slot = field;
    return true;
}

// The shared step of the three typed setters: validate the key, refuse a key
// already present, and store the value text.
bool set_field(
    RoverEntry& entry, RoverFieldType type, const char* key, const char* value, bool opaque
) {
    int32_t key_length = static_cast<int32_t>(strlen(key));
    if (!is_valid_label(key, key_length))
        return false;
    int32_t value_length = static_cast<int32_t>(strlen(value));
    if (type == RoverFieldType::String && value_length == 0)
        return false;
    if (!insert_field(entry, type, key, key_length, value, value_length))
        return false;
    entry.record->fields[entry.record->field_count - 1].opaque = opaque;
    entry.changed = true;
    return true;
}

/// Unescapes a field value.
///
/// @param escaped value as written on the wire
/// @param length bytes of escaped
/// @param[out] out buffer of at least length bytes for the decoded value
/// @return the decoded length, or -1 when the value ends in a lone '%'
int32_t decode_value(const char* escaped, int32_t length, char* out) {
    int32_t written = 0;
    int32_t at = 0;
    while (at < length) {
        char byte = escaped[at++];
        if (byte != kEscapeMarker) {
            out[written++] = byte;
            continue;
        }
        if (at >= length)
            return -1;
        out[written++] = unescaped_byte(escaped[at++]);
    }
    return written;
}

/// Parses one `<T:key=value>` field of a wire entry into the entry.
///
/// The field must close with '>' inside the text left, with a known type letter, a non-empty key
/// free of reserved bytes and a key the entry does not hold yet. A string value may be empty; a
/// number may not. A received field is parsed with its entry, so the parse the field accessors
/// would make on first use has always been made.
///
/// @param[in,out] entry entry receiving the field
/// @param text wire text starting at the field's '<'
/// @param length bytes left in the record text
/// @param[out] consumed bytes the field took, 0 on failure
/// @return true when the field was added
bool parse_field(RoverEntry& entry, const char* text, int32_t length, int32_t* consumed) {
    *consumed = 0;
    if (length < 3 || text[0] != kFieldOpen)
        return false;
    char type = text[1];
    if (type != static_cast<char>(RoverFieldType::String) &&
        type != static_cast<char>(RoverFieldType::Int) &&
        type != static_cast<char>(RoverFieldType::Uint))
        return false;
    if (text[2] != kTypeSeparator)
        return false;
    const char* key = text + 3;
    int32_t left = length - 3;
    if (left <= 0)
        return false;
    const char* equals =
        static_cast<const char*>(memchr(key, kValueSeparator, static_cast<size_t>(left)));
    if (!equals || equals == key)
        return false;
    int32_t key_length = static_cast<int32_t>(equals - key);
    if (!is_valid_label(key, key_length))
        return false;
    const char* value = equals + 1;
    left = length - (key_length + 4);
    if (left <= 0)
        return false;
    const char* close =
        static_cast<const char*>(memchr(value, kFieldClose, static_cast<size_t>(left)));
    // An empty string is a blank profile field. An empty number has no digits to read.
    if (!close || (close == value && static_cast<RoverFieldType>(type) != RoverFieldType::String))
        return false;
    int32_t escaped_length = static_cast<int32_t>(close - value);
    char decoded[kRoverRecordTextLimit];
    int32_t value_length = decode_value(value, escaped_length, decoded);
    if (value_length < 0)
        return false;
    const char* stored = decoded;
    // A string is Windows-1252 on the wire, or UTF-8 when the server accepts it. Either way the
    // field keeps UTF-8. Three bytes is the longest one wire byte becomes.
    char as_utf8[kRoverRecordTextLimit * 3 + 1];
    if (static_cast<RoverFieldType>(type) == RoverFieldType::String) {
        value_length = rover_decode_wire_text(as_utf8, sizeof as_utf8, decoded, value_length, -1);
        stored = as_utf8;
    }
    if (!insert_field(
            entry, static_cast<RoverFieldType>(type), key, key_length, stored, value_length
        ))
        return false;
    *consumed = key_length + kFieldFraming + escaped_length;
    return true;
}

/// Parses one `[Name<field>...]` wire entry: a non-empty name, at least one field, no key twice.
///
/// The entry keeps its wire text so it can be written back unchanged.
///
/// @param[in,out] entry entry to fill
/// @param text wire text starting at the entry's '['
/// @param length bytes left in the record text
/// @param[out] consumed bytes the entry took, 0 on failure
/// @return true when the entry parsed
bool parse_entry(RoverEntry& entry, const char* text, int32_t length, int32_t* consumed) {
    *consumed = 0;
    if (length < 1 || text[0] != kEntryOpen)
        return false;
    const char* name = text + 1;
    if (length - 1 <= 0)
        return false;
    const char* open =
        static_cast<const char*>(memchr(name, kFieldOpen, static_cast<size_t>(length - 1)));
    if (!open || open == name)
        return false;
    int32_t name_length = static_cast<int32_t>(open - name);
    if (!is_valid_label(name, name_length))
        return false;
    entry.name = store_text(*entry.record, name, name_length);
    if (!entry.name)
        return false;
    entry.name_length = name_length;
    int32_t used = name_length + 1;
    const char* cursor = open;
    for (;;) {
        int32_t field_length = 0;
        if (!parse_field(entry, cursor, length - used, &field_length))
            return false;
        used += field_length;
        cursor += field_length;
        if (length - used < 1)
            return false;
        if (*cursor == kEntryClose) {
            *consumed = used + 1;
            entry.wire_text = text;
            entry.wire_length = *consumed;
            return true;
        }
    }
}

/// Parses the gathered `{...}` text into entries: at least one, and the byte after each must open
/// the next or close the record.
///
/// @param[in,out] record record whose text is parsed; a failing entry is unlinked
/// @return Ok, or Malformed for bad text or when the entry table is full
RoverRecordStatus parse_entries(RoverRecord& record) {
    const char* cursor = record.text + 1;
    int32_t left = record.text_length - 1;
    do {
        RoverEntry* entry = new_entry(record);
        if (!entry)
            return RoverRecordStatus::Malformed;
        RoverEntry* tail = last_entry(record);
        if (tail)
            tail->next = entry;
        else
            record.first_entry = entry;
        int32_t used = 0;
        if (!parse_entry(*entry, cursor, left, &used)) {
            unlink_entry(record, entry);
            return RoverRecordStatus::Malformed;
        }
        cursor += used;
        left -= used;
        if (left <= 0)
            return RoverRecordStatus::Malformed;
    } while (*cursor != kRecordClose);
    return RoverRecordStatus::Ok;
}

/// Escapes a field value into out.
///
/// @param value bytes to write
/// @param value_length bytes of value
/// @param[out] out buffer for the escaped value
/// @param capacity size of out in bytes
/// @param[out] written bytes written, also on failure
/// @return false when the value does not fit or holds a NUL byte
bool encode_value(
    const char* value, int32_t value_length, char* out, int32_t capacity, int32_t* written
) {
    *written = 0;
    int32_t left = value_length;
    for (;;) {
        int32_t run = unreserved_span(value, left);
        if (run > capacity - *written)
            return false;
        memcpy(out + *written, value, static_cast<size_t>(run));
        *written += run;
        if (run >= left)
            return true;
        if (capacity - *written < 2)
            return false;
        uint8_t digit = escape_digit(value[run]);
        out[(*written)++] = kEscapeMarker;
        if (digit == kNotEscapable)
            return false;
        out[(*written)++] = static_cast<char>(digit);
        value += run + 1;
        left -= run + 1;
    }
}

/// Writes one `<T:key=value>` field.
///
/// @param field field to write
/// @param[out] out buffer for the field text
/// @param capacity size of out in bytes
/// @param[out] written bytes written, 0 on failure
/// @return Ok; BadKey for an empty key or one holding a reserved byte; EmptyValue; TooLong when it
///     does not fit
RoverRecordStatus
format_field(const RoverField& field, char* out, int32_t capacity, int32_t* written) {
    *written = 0;
    if (field.key_length == 0 || has_reserved_byte(field.key, field.key_length))
        return RoverRecordStatus::BadKey;
    if (field.value_length == 0)
        return RoverRecordStatus::EmptyValue;
    // A string is stored as UTF-8 and written as the wire carries it. The wire form is never
    // longer than the stored text, so the record's own storage holds the conversion.
    const char* value = field.value;
    int32_t value_length = field.value_length;
    char encoded[kRoverRecordStorage];
    if (field.type == RoverFieldType::String && !field.opaque) {
        int32_t source_used = 0;
        value_length = rover_encode_wire_text(
            encoded,
            static_cast<int32_t>(sizeof encoded),
            field.value,
            field.value_length,
            -1,
            &source_used
        );
        if (source_used != field.value_length)
            return RoverRecordStatus::TooLong;
        value = encoded;
    }
    int32_t head = field.key_length + 4;
    if (head + value_length + 1 > capacity)
        return RoverRecordStatus::TooLong;
    out[0] = kFieldOpen;
    out[1] = static_cast<char>(field.type);
    out[2] = kTypeSeparator;
    memcpy(out + 3, field.key, static_cast<size_t>(field.key_length));
    out[3 + field.key_length] = kValueSeparator;
    int32_t escaped_length = 0;
    if (!encode_value(value, value_length, out + head, capacity - head - 1, &escaped_length))
        return RoverRecordStatus::TooLong;
    out[head + escaped_length] = kFieldClose;
    *written = head + escaped_length + 1;
    return RoverRecordStatus::Ok;
}

/// Writes one entry: a parsed entry as it arrived, a built or changed one as `[Name` and its fields
/// in key order, then `]`.
///
/// @param entry entry to write
/// @param[out] out buffer for the entry text
/// @param capacity size of out in bytes
/// @param[out] written bytes written, 0 on failure
/// @return Ok; BadName; NoFields; a field's failure; TooLong when it does not fit
RoverRecordStatus
format_entry(const RoverEntry& entry, char* out, int32_t capacity, int32_t* written) {
    *written = 0;
    if (!entry.changed) {
        if (entry.wire_length > capacity)
            return RoverRecordStatus::TooLong;
        memcpy(out, entry.wire_text, static_cast<size_t>(entry.wire_length));
        *written = entry.wire_length;
        return RoverRecordStatus::Ok;
    }
    if (entry.name_length == 0 || has_reserved_byte(entry.name, entry.name_length))
        return RoverRecordStatus::BadName;
    if (!entry.first_field)
        return RoverRecordStatus::NoFields;
    if (entry.name_length + 2 > capacity)
        return RoverRecordStatus::TooLong;
    out[0] = kEntryOpen;
    memcpy(out + 1, entry.name, static_cast<size_t>(entry.name_length));
    int32_t used = entry.name_length + 1;
    for (const RoverField* field = entry.first_field; field; field = field->next) {
        int32_t field_length = 0;
        RoverRecordStatus status = format_field(*field, out + used, capacity - used, &field_length);
        if (status != RoverRecordStatus::Ok)
            return status;
        used += field_length;
    }
    if (used >= capacity)
        return RoverRecordStatus::TooLong;
    out[used++] = kEntryClose;
    *written = used;
    return RoverRecordStatus::Ok;
}

} // namespace

RoverRecord* rover_record_create() {
    return static_cast<RoverRecord*>(calloc(1, sizeof(RoverRecord)));
}

void rover_record_destroy(RoverRecord** record) {
    free(*record);
    *record = nullptr;
}

void rover_free_scratch_record(RoverRecord* const* record) {
    if (*record)
        free(*record);
}

RoverEntry* rover_record_add_entry(RoverRecord& record, bool at_end, const char* name) {
    int32_t name_length = static_cast<int32_t>(strlen(name));
    if (!is_valid_label(name, name_length))
        return nullptr;
    int32_t storage_mark = record.storage_used;
    const char* stored = store_text(record, name, name_length);
    if (!stored)
        return nullptr;
    RoverEntry* entry = new_entry(record);
    if (!entry) {
        record.storage_used = storage_mark;
        return nullptr;
    }
    entry->name = stored;
    entry->name_length = name_length;
    entry->changed = true;
    RoverEntry* tail = at_end ? last_entry(record) : nullptr;
    if (tail) {
        tail->next = entry;
    } else {
        entry->next = record.first_entry;
        record.first_entry = entry;
    }
    return entry;
}

RoverEntry* rover_record_end_entry(const RoverRecord& record, bool last) {
    return last ? last_entry(record) : record.first_entry;
}

int32_t rover_record_entry_count(const RoverRecord& record) {
    int32_t count = 0;
    for (const RoverEntry* entry = record.first_entry; entry; entry = entry->next)
        ++count;
    return count;
}

void rover_record_remove_first_entry(RoverRecord& record) {
    if (record.first_entry)
        record.first_entry = record.first_entry->next;
}

bool rover_entry_set_string(RoverEntry& entry, const char* key, const char* value) {
    return set_field(entry, RoverFieldType::String, key, value, false);
}

bool rover_entry_set_opaque_string(RoverEntry& entry, const char* key, const char* value) {
    return set_field(entry, RoverFieldType::String, key, value, true);
}

bool rover_entry_set_int(RoverEntry& entry, const char* key, int32_t value) {
    char text[16];
    snprintf(text, sizeof text, "%ld", static_cast<long>(value));
    return set_field(entry, RoverFieldType::Int, key, text, false);
}

bool rover_entry_set_uint(RoverEntry& entry, const char* key, uint32_t value) {
    char text[16];
    snprintf(text, sizeof text, "%lx", static_cast<unsigned long>(value));
    return set_field(entry, RoverFieldType::Uint, key, text, false);
}

int32_t rover_find_char_in_set(
    const char* text, int32_t length, const char* set, int32_t from, int32_t set_length
) {
    if (set_length == 0 || from >= length)
        return kRoverCharNotFound;
    for (int32_t at = from; at < length; ++at) {
        if (memchr(set, text[at], static_cast<size_t>(set_length)))
            return at;
    }
    return kRoverCharNotFound;
}

const RoverField* rover_entry_find(const RoverEntry& entry, const char* key) {
    int32_t key_length = static_cast<int32_t>(strlen(key));
    for (const RoverField* field = entry.first_field; field; field = field->next) {
        if (compare_keys(field->key, field->key_length, key, key_length) == 0)
            return field;
    }
    return nullptr;
}

int32_t rover_field_int(const RoverField& field, int32_t fallback) {
    if (field.type != RoverFieldType::Int)
        return fallback;
    return rover_decimal_value(field.value);
}

uint32_t rover_field_uint(const RoverField& field, uint32_t fallback) {
    if (field.type != RoverFieldType::Uint)
        return fallback;
    return rover_hex_value(field.value);
}

const char* rover_field_key(const RoverField& field) {
    return field.key;
}

RoverFieldType rover_field_type(const RoverField& field) {
    return field.type;
}

const RoverField* rover_field_next(const RoverField& field) {
    return field.next;
}

bool rover_entry_get_string(
    const RoverEntry& entry, const char* key, const char** out_value, const char* fallback
) {
    const RoverField* field = rover_entry_find(entry, key);
    *out_value = field ? field->value : fallback;
    return field != nullptr;
}

bool rover_entry_get_int(
    const RoverEntry& entry, const char* key, int32_t* out_value, int32_t fallback
) {
    const RoverField* field = rover_entry_find(entry, key);
    *out_value = field ? rover_field_int(*field, -1) : fallback;
    return field != nullptr;
}

bool rover_entry_get_uint(
    const RoverEntry& entry, const char* key, uint32_t* out_value, uint32_t fallback
) {
    const RoverField* field = rover_entry_find(entry, key);
    *out_value = field ? rover_field_uint(*field, 0) : fallback;
    return field != nullptr;
}

bool rover_text_equals(const char* text, int32_t length, const char* literal) {
    return compare_keys(text, length, literal, static_cast<int32_t>(strlen(literal))) == 0;
}

RoverRecordStatus
rover_record_format(const RoverRecord& record, char* out, int32_t capacity, int32_t* out_length) {
    *out_length = 0;
    if (!record.first_entry)
        return RoverRecordStatus::NoEntries;
    if (capacity < kRoverRecordMinimumBuffer)
        return RoverRecordStatus::TooLong;
    out[0] = kRecordOpen;
    int32_t used = 1;
    for (const RoverEntry* entry = record.first_entry; entry; entry = entry->next) {
        int32_t entry_length = 0;
        RoverRecordStatus status = format_entry(*entry, out + used, capacity - used, &entry_length);
        if (status != RoverRecordStatus::Ok)
            return status;
        used += entry_length;
    }
    if (used >= capacity)
        return RoverRecordStatus::TooLong;
    out[used++] = kRecordClose;
    *out_length = used;
    return RoverRecordStatus::Ok;
}

RoverRecordStatus rover_record_receive(
    RoverRecord** pending,
    const char* data,
    int32_t length,
    int32_t* consumed,
    RoverRecord** out_record
) {
    *out_record = nullptr;
    *consumed = 0;
    RoverRecord* record = *pending;
    if (!record) {
        if (length < 1 || data[0] != kRecordOpen)
            return RoverRecordStatus::Malformed;
        record = rover_record_create();
        if (!record)
            return RoverRecordStatus::OutOfMemory;
        *pending = record;
    }
    const char* close =
        static_cast<const char*>(memchr(data, kRecordClose, static_cast<size_t>(length)));
    int32_t taken = close ? static_cast<int32_t>(close - data) + 1 : length;
    *consumed = taken;
    if (record->text_length + taken > kRoverRecordTextLimit)
        return RoverRecordStatus::TooLong;
    memmove(record->text + record->text_length, data, static_cast<size_t>(taken));
    record->text_length += taken;
    if (!close)
        return RoverRecordStatus::Incomplete;
    RoverRecordStatus status = parse_entries(*record);
    *out_record = record;
    return status;
}

int32_t rover_decimal_value(const char* text) {
    while (is_space(*text))
        ++text;
    bool negative = *text == '-';
    if (*text == '-' || *text == '+')
        ++text;
    uint32_t total = 0;
    while (*text >= '0' && *text <= '9')
        total = total * 10u + static_cast<uint32_t>(*text++ - '0');
    return static_cast<int32_t>(negative ? 0u - total : total);
}

uint32_t rover_hex_value(const char* text) {
    constexpr uint32_t kLimit = 0xffffffffu;
    while (is_space(*text))
        ++text;
    bool negative = *text == '-';
    if (*text == '-' || *text == '+')
        ++text;
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X'))
        text += 2;
    uint32_t number = 0;
    bool overflow = false;
    bool any_digit = false;
    for (;; ++text) {
        char c = *text;
        uint32_t digit = 0;
        if (c >= '0' && c <= '9')
            digit = static_cast<uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f')
            digit = static_cast<uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            digit = static_cast<uint32_t>(c - 'A' + 10);
        else
            break;
        any_digit = true;
        if (number < kLimit / 16u || (number == kLimit / 16u && digit <= kLimit % 16u))
            number = number * 16u + digit;
        else
            overflow = true;
    }
    if (!any_digit)
        return 0;
    if (overflow)
        return kLimit;
    return negative ? 0u - number : number;
}
