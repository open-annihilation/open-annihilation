// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Brace-delimited record files (Profile.def, Ladders.def, the Galactic War
// sector map): Rover's text reader and the parse of the records it returns.
//
// A record is "{" entry... "}", each entry running to the first "]"; the entry
// slots read is "[Name<T:key=value>...]" with T one of I, S or U, keys and the
// name free of the reserved bytes {}[]<>:=% and values written with %0..%8
// for them. Records are split into entries, and entries into fields, only
// when first asked for, and a malformed part only fails the lookups that
// reach it.
#ifndef OA_ROVER_RECORDS_H
#define OA_ROVER_RECORDS_H

#include "oa/rover/interface.h"
#include "oa/rover/platform.h"

#include <stdint.h>

// Largest record text, braces included.
constexpr int32_t kRoverRecordTextCapacity = 0x1000;
// The reader keeps at most this many path characters.
constexpr int32_t kRoverReaderPathLength = 0x3ff;
// Line buffer of the reader; a longer line is read in several pieces.
constexpr int32_t kRoverReaderLineCapacity = 0x800;

// Outcome of reading the next record.
enum class RoverReadStatus : int32_t {
    ok = 0,
    out_of_memory = 1,
    need_more = 2, // record not closed yet; internal to the reader
    too_long = 3,  // more than kRoverRecordTextCapacity bytes before the "}"
    malformed = 4, // text did not open with "{"
    not_open = 0xb,
    end_of_file = 0xd,
};

// A file read in text mode: CR LF reads as LF and a
// Ctrl-Z byte ends the file.
struct RoverTextFile {
    RoverFile* file{};
    int32_t position{};
    int32_t length{};
    bool ended{};
    uint8_t buffer[0x1000]{};
};

// A record file reader: one open file plus the text that followed the last
// record on its line, from the next "{".
struct RoverRecordReader {
    char path[kRoverReaderPathLength + 1]{};
    bool open{};
    RoverTextFile file{};
    char pending[kRoverReaderLineCapacity]{};
};

// One "<T:key=value>" of an entry.
struct RoverRecordField {
    RoverRecordField* next{}; // ascending key order
    char type{};              // 'I', 'S' or 'U'
    RoverString key{};
    RoverString value{}; // escapes decoded
};

struct RoverRecordEntry {
    RoverRecordEntry* next{};
    const char* text{}; // from "[" through the closing "]", inside the record
    int32_t length{};
    bool name_ready{};
    bool fields_ready{};
    RoverString name{};
    RoverRecordField* fields{};
};

// One record read from a file, with its text of up to kRoverRecordTextCapacity bytes.
struct RoverFileRecord {
    int32_t length{};
    bool entries_ready{};
    RoverRecordEntry* entries{};
    char text[kRoverRecordTextCapacity]{};
};

/// Sets up a reader that is not open, keeping its path, in read-text mode, the only mode it reads
/// in.
///
/// @param[out] reader reader to set up
/// @param path file to read, at most 0x3ff characters kept; null leaves it unnamed
void rover_record_reader_init(RoverRecordReader* reader, const char* path);

/// Opens the reader's path in read-text mode.
///
/// @param[in,out] reader reader to open
/// @return 0 when opened, -1 when already open, unnamed or missing
int32_t rover_record_reader_open(RoverRecordReader* reader);

/// Closes the file and drops the pending text.
///
/// @param[in,out] reader reader to close; a closed one is left alone
void rover_record_reader_close(RoverRecordReader* reader);

/// Reads the next "{...}" record, first from the text pending after the last one, then line by
/// line; text between records is skipped.
///
/// A record still open at an error or the end of the file is freed.
///
/// @param[in,out] reader open reader
/// @param[out] out_record on ok the record, which the caller owns; otherwise null
/// @return ok; not_open; end_of_file; or the failure of the record being read
RoverReadStatus rover_record_reader_next(RoverRecordReader* reader, RoverFileRecord** out_record);

/// Closes a reader from rover_record_reader_create if it is still open and frees it.
///
/// @param reader reader to free, or null
void rover_record_reader_destroy(RoverRecordReader* reader);

/// Allocates a reader for path from Rover's heap, set up but not open.
///
/// @param path file to read
/// @return the reader, or null when memory ran out
RoverRecordReader* rover_record_reader_create(const char* path);

/// Frees a record with its entries and fields.
///
/// @param record record to free, or null
void rover_file_record_destroy(RoverFileRecord* record);

/// Returns the record's first entry, splitting the text into entries on first use.
///
/// @param[in,out] record record to read
/// @return the first entry, or null when the record has no well-formed entry list
RoverRecordEntry* rover_record_first_entry(RoverFileRecord* record);

/// Returns the entry's name, parsing only its header on first use.
///
/// @param[in,out] entry entry to read, or null
/// @return the name, or null for a null entry or a malformed header
const RoverString* rover_record_entry_name(RoverRecordEntry* entry);

/// Tests the entry's name against a text.
///
/// @param[in,out] entry entry to read, or null
/// @param name NUL-terminated name to compare with
/// @return true when the entry has a well-formed header with exactly that name
bool rover_record_entry_is(RoverRecordEntry* entry, const char* name);

/// Returns the entry's lowest-keyed field, parsing all fields on first use.
///
/// @param[in,out] entry entry to read, or null
/// @return the first field in key order, or null for a null entry, when any field is malformed or a
///     key repeats
const RoverRecordField* rover_record_entry_fields(RoverRecordEntry* entry);

/// Finds the field stored under key, compared byte for byte.
///
/// @param[in,out] entry entry to search, or null
/// @param key key to look for
/// @return the field, or null when absent or the fields do not parse
const RoverRecordField* rover_record_entry_find(RoverRecordEntry* entry, const char* key);

/// Returns a field's value text whatever its type.
///
/// @param[in,out] entry entry to search, or null
/// @param key key to look for
/// @param fallback text for an absent field
/// @return the value text, or fallback
const char* rover_record_entry_text(RoverRecordEntry* entry, const char* key, const char* fallback);

/// Returns a field's integer value.
///
/// @param[in,out] entry entry to search, or null
/// @param key key to look for
/// @param fallback value for an absent field
/// @return the value; -1 for a present field that is not type I; fallback when absent
int32_t rover_record_entry_int(RoverRecordEntry* entry, const char* key, int32_t fallback);

/// Converts a type-I field as signed decimal.
///
/// @param field field to read
/// @param fallback value for a field of another type
/// @return the value, or fallback
int32_t rover_record_field_int(const RoverRecordField* field, int32_t fallback);

/// Converts a type-U field as hexadecimal.
///
/// @param field field to read
/// @param fallback value for a field of another type
/// @return the value, or fallback
uint32_t rover_record_field_uint(const RoverRecordField* field, uint32_t fallback);

/// Parses one "<T:key=value>" at the start of text, decoding the value.
///
/// The key must be non-empty and free of reserved bytes. A string value may be empty; a number
/// may not. A missing ">" fails.
///
/// @param text text starting at the field's "<"
/// @param length bytes left in the entry
/// @param[in,out] field field receiving the type, key and decoded value
/// @param[out] out_length bytes the field took, 0 on failure
/// @return true when the field parsed
bool rover_record_parse_field(
    const char* text, int32_t length, RoverRecordField* field, int32_t* out_length
);

/// Copies escaped text into value and collapses each "%d" in place.
///
/// @param[in,out] value string receiving the decoded text
/// @param text escaped value text
/// @param length bytes of text
/// @return false when a "%" has nothing after it or the copy could not be made
/// @quirk A digit outside '0'..'8' yields 0xff rather than failing, as in 3.1c.
bool rover_record_decode_value(RoverString* value, const char* text, int32_t length);

/// Maps an escape digit to the reserved byte at that index of "{}[]<>:=%".
///
/// @param digit the byte after "%"
/// @return the reserved byte, or 0xff for anything but '0'..'8'
uint8_t rover_record_escaped_byte(char digit);

/// Reads a decimal number: optional blanks and sign, then decimal digits with
/// 32-bit wraparound.
///
/// @param text text to read
/// @return the value; 0 without digits
int32_t rover_parse_decimal(const char* text);

/// Reads a hexadecimal number: optional blanks, sign and "0x", then
/// hex digits.
///
/// @param text text to read
/// @return the value; 0 without digits; 0xffffffff on overflow
/// @quirk A "-" negates even the overflow value.
uint32_t rover_parse_hexadecimal(const char* text);

#endif
