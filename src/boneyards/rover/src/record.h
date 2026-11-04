// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby message record. On the wire a record is
//
//   {[Service<T:key=value><T:key=value>...][Service...]...}
//
// one or more entries, each a service name and its typed fields; T is S
// (text), I (signed decimal) or U (hexadecimal). The nine reserved bytes
// {}[]<>:=% never appear raw inside a name, key or value: a value writes
// each as '%' and the byte's index in that list, and a name or key holding
// one is refused. A record is at most 0x400 bytes, braces included.
//
// Fields of an entry are kept in key order (byte compare, shorter first), so
// an entry built in memory is written in that order. An entry parsed from
// the wire and left unchanged is written back as the text it arrived as.
#pragma once

#include <stdint.h>

// Longest record text, on the wire or accumulated from it.
constexpr int32_t kRoverRecordTextLimit = 0x400;
// Smallest buffer the formatter starts on.
constexpr int32_t kRoverRecordMinimumBuffer = 12;
// Room for entries and fields; a wire record cannot hold more than these.
constexpr int32_t kRoverRecordEntryLimit = 128;
constexpr int32_t kRoverRecordFieldLimit = 160;
// Names, keys and decoded values of one record, each NUL-terminated. A string
// value read from the wire is UTF-8, up to three bytes for each wire byte.
constexpr int32_t kRoverRecordStorage = 0x1000;

enum class RoverFieldType : char {
    String = 'S',
    Int = 'I',
    Uint = 'U',
};

// Results of the parser and formatter, numbered as 3.1c reports them.
enum class RoverRecordStatus : int32_t {
    Ok = 0,
    OutOfMemory = 1,
    Incomplete = 2, // no closing brace yet; feed more bytes
    TooLong = 3,    // over kRoverRecordTextLimit, or the buffer is too small
    Malformed = 4,
    BadKey = 5,
    EmptyValue = 6,
    BadName = 8,
    NoFields = 9,
    NoEntries = 10,
};

struct RoverRecord;

struct RoverField {
    const char* key{};
    const char* value{}; // decoded; may hold NUL bytes before value_length
    int32_t key_length{};
    int32_t value_length{};
    RoverFieldType type{};
    RoverField* next{}; // next field of the entry in key order
    bool opaque{};      // value bytes are already the wire form, so they are not converted
};

struct RoverEntry {
    RoverRecord* record{};
    const char* name{};
    int32_t name_length{};
    RoverField* first_field{};
    RoverEntry* next{};
    const char* wire_text{}; // the text a parsed entry arrived as
    int32_t wire_length{};
    bool changed{}; // built here or changed since parsing; written from its fields
};

struct RoverRecord {
    RoverRecord* next_queued{}; // link of the event queue holding the record
    RoverEntry* first_entry{};
    int32_t text_length{};
    int32_t entry_count{}; // entries used, including dropped ones
    int32_t field_count{};
    int32_t storage_used{};
    char text[kRoverRecordTextLimit]{};
    RoverEntry entries[kRoverRecordEntryLimit]{};
    RoverField fields[kRoverRecordFieldLimit]{};
    char storage[kRoverRecordStorage]{};
};

/// Allocates an empty record, zero-filled and ready for entries.
///
/// @return the record, or null when out of memory
RoverRecord* rover_record_create();

/// Frees a record and clears the caller's pointer.
///
/// @param[in,out] record pointer to the record, set to null; a null record is fine
void rover_record_destroy(RoverRecord** record);

/// Adds an empty entry named name at the front or the back of the record.
///
/// @param[in,out] record record to add to
/// @param at_end true for the back, false for the front
/// @param name service name of the entry; must hold no reserved byte
/// @return the entry, or null when the name is refused or no room is left
RoverEntry* rover_record_add_entry(RoverRecord& record, bool at_end, const char* name);

/// Returns the record's first or last entry.
///
/// @param record record to read
/// @param last true for the last entry, false for the first
/// @return the entry, or null when there is none
RoverEntry* rover_record_end_entry(const RoverRecord& record, bool last);

/// Counts the entries linked into the record.
///
/// @param record record to read
/// @return number of entries, dropped ones excluded
int32_t rover_record_entry_count(const RoverRecord& record);

/// Drops the record's first entry; its storage stays used.
///
/// @param[in,out] record record to change
void rover_record_remove_first_entry(RoverRecord& record);

/// Adds a text field to the entry in key order and marks the entry changed.
///
/// An existing field is never replaced.
///
/// @param[in,out] entry entry to add to
/// @param key field key; must hold no reserved byte
/// @param value NUL-terminated text; must not be empty
/// @return false when the key is refused or taken, a text value is empty, or no room is left
bool rover_entry_set_string(RoverEntry& entry, const char* key, const char* value);

/// Stores bytes that are already the wire form, such as a login digest.
///
/// The bytes are escaped when the record is written and are not converted as text.
///
/// @param[in,out] entry entry to change
/// @param key field key
/// @param value bytes to store; a NUL ends them
/// @return true when the field was stored
bool rover_entry_set_opaque_string(RoverEntry& entry, const char* key, const char* value);

/// Adds a signed field to the entry in key order and marks the entry changed.
///
/// An existing field is never replaced.
///
/// @param[in,out] entry entry to add to
/// @param key field key; must hold no reserved byte
/// @param value value, written in decimal
/// @return false when the key is refused or taken, a text value is empty, or no room is left
bool rover_entry_set_int(RoverEntry& entry, const char* key, int32_t value);

/// Adds a unsigned field to the entry in key order and marks the entry changed.
///
/// An existing field is never replaced.
///
/// @param[in,out] entry entry to add to
/// @param key field key; must hold no reserved byte
/// @param value value, written in lower-case hexadecimal
/// @return false when the key is refused or taken, a text value is empty, or no room is left
bool rover_entry_set_uint(RoverEntry& entry, const char* key, uint32_t value);

/// Finds a field of the entry by exact key.
///
/// @param entry entry to search
/// @param key key to look for
/// @return the field, or null when the entry has none
const RoverField* rover_entry_find(const RoverEntry& entry, const char* key);

/// Returns the value of an I field, read as rover_decimal_value reads it.
///
/// @param field field to read
/// @param fallback value for a field of another type
/// @return the signed value, or fallback
int32_t rover_field_int(const RoverField& field, int32_t fallback);

/// Returns the value of a U field, read as rover_hex_value reads it.
///
/// @param field field to read
/// @param fallback value for a field of another type
/// @return the unsigned value, or fallback
uint32_t rover_field_uint(const RoverField& field, uint32_t fallback);

/// Returns the field's key; it is key_length bytes long.
///
/// @param field field to read
/// @return the key text
const char* rover_field_key(const RoverField& field);

/// Returns the field's type letter.
///
/// @param field field to read
/// @return String, Int or Uint
RoverFieldType rover_field_type(const RoverField& field);

/// Returns the entry's field after this one in key order.
///
/// @param field field to step from
/// @return the next field, or null after the last
const RoverField* rover_field_next(const RoverField& field);

/// Reads a field's text whatever its type.
///
/// @param entry entry to search
/// @param key key to look for
/// @param[out] out_value receives the text, or fallback when absent
/// @param fallback text for a missing field
/// @return true when the field is present
bool rover_entry_get_string(
    const RoverEntry& entry, const char* key, const char** out_value, const char* fallback
);

/// Reads a field as signed, as rover_field_int does.
///
/// @param entry entry to search
/// @param key key to look for
/// @param[out] out_value receives the value (-1 when not an I field), or fallback when absent
/// @param fallback value for a missing field
/// @return true when the field is present
bool rover_entry_get_int(
    const RoverEntry& entry, const char* key, int32_t* out_value, int32_t fallback
);

/// Reads a field as unsigned, as rover_field_uint does.
///
/// @param entry entry to search
/// @param key key to look for
/// @param[out] out_value receives the value (0 when not a U field), or fallback when absent
/// @param fallback value for a missing field
/// @return true when the field is present
bool rover_entry_get_uint(
    const RoverEntry& entry, const char* key, uint32_t* out_value, uint32_t fallback
);
// What rover_find_char_in_set returns when no byte of the text is in the set.
constexpr int32_t kRoverCharNotFound = -1;

/// Finds the first byte of text[from..length) that is among the first set_length bytes of set, as
/// std::string::find_first_of does.
///
/// A NUL byte is compared too; nothing is found in an empty set or from past the end.
///
/// @param text text to scan
/// @param length bytes of text
/// @param set bytes to look for
/// @param from offset to start at
/// @param set_length bytes of set
/// @return the offset found, or kRoverCharNotFound
int32_t rover_find_char_in_set(
    const char* text, int32_t length, const char* set, int32_t from, int32_t set_length
);

/// Compares a counted text with a literal, exactly and case-sensitively.
///
/// @param text text to compare
/// @param length bytes of text
/// @param literal NUL-terminated literal
/// @return true when both hold the same bytes
bool rover_text_equals(const char* text, int32_t length, const char* literal);

/// Writes the record's text as `{` entries `}`, without a NUL.
///
/// @param record record to write
/// @param[out] out buffer for the text
/// @param capacity size of out in bytes; at least kRoverRecordMinimumBuffer
/// @param[out] out_length bytes written, 0 on failure
/// @return Ok; NoEntries; TooLong; or an entry's failure
RoverRecordStatus
rover_record_format(const RoverRecord& record, char* out, int32_t capacity, int32_t* out_length);

/// Feeds received bytes to the record being gathered and parses it once its closing brace arrives.
///
/// A new record starts at a '{'. The bytes taken run up to and including the first '}', or are all
/// of them. A field running past the record's end without its '>' is refused.
///
/// @param[in,out] pending record being gathered, created here when null; the caller clears it once
///     a record is out
/// @param data received bytes
/// @param length number of received bytes
/// @param[out] consumed bytes taken from data
/// @param[out] out_record the gathered record once its '}' arrived, also on a parse failure; null
///     before
/// @return Ok; Incomplete while no '}' has come; Malformed; OutOfMemory; TooLong past
///     kRoverRecordTextLimit
RoverRecordStatus rover_record_receive(
    RoverRecord** pending,
    const char* data,
    int32_t length,
    int32_t* consumed,
    RoverRecord** out_record
);

/// Frees the record *record points at, when there is one, leaving *record set.
///
/// @param record pointer to the record to free
void rover_free_scratch_record(RoverRecord* const* record);

/// Reads a decimal number: leading white space, one sign, decimal digits.
///
/// @param text text to read
/// @return the value, wrapping on overflow; 0 without digits
int32_t rover_decimal_value(const char* text);

/// Reads a hexadecimal number.
///
/// Leading white space, one sign and a 0x prefix are accepted; a minus negates the result.
///
/// @param text text to read
/// @return the value; 0 without digits; 0xffffffff on overflow
uint32_t rover_hex_value(const char* text);
