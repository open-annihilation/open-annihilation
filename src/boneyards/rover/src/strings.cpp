// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The reference-counted string the game and Rover exchange: the buffer
// layout, growth policy and share rules decide which heap frees a buffer the
// game and Rover both hold, so they are the same as 3.1c's.

#include "oa/rover/strings.h"

#include "heap.h"

#include <string.h>

namespace {

// Largest length rover_string_grow accepts; it refuses a longer one, where
// 3.1c reports a length error.
constexpr uint32_t kMaxLength = 0xfffffffdu;
// Capacities are the requested length with these low bits set.
constexpr uint32_t kCapacityRounding = 0x1f;
// Share counts at or above this are copied instead of shared.
constexpr uint8_t kUnshareable = 0xfe;
// Share count of a buffer whose characters were handed out for writing.
constexpr uint8_t kFrozen = 0xff;

uint8_t& share_count(char* text) {
    return reinterpret_cast<uint8_t*>(text)[-1];
}

// Owned by another string too: a non-zero count that is not frozen.
bool is_shared(char* text) {
    return text && share_count(text) != 0 && share_count(text) != kFrozen;
}

/// Moves the text into a fresh buffer of at least length characters, truncating it to the new
/// capacity.
///
/// The capacity is length with its low five bits set, or length itself when that would exceed the
/// maximum. On allocation failure the string is released and emptied.
///
/// @param[in,out] string string whose text moves
/// @param length characters the new buffer must hold
/// @return true when the new buffer is in place, false when memory ran out
bool copy_to_new_buffer(RoverString* string, uint32_t length) {
    uint32_t capacity = length | kCapacityRounding;
    if (capacity > kMaxLength)
        capacity = length;
    // The count byte, capacity characters and the terminator.
    char* block = static_cast<char*>(rover_heap_alloc(size_t{capacity} + 2));
    if (!block) {
        rover_string_tidy(string, true);
        return false;
    }
    uint32_t kept = string->length < capacity ? string->length : capacity;
    if (string->length > 0)
        memmove(block + 1, string->data, kept);
    rover_string_tidy(string, true);
    block[0] = 0;
    string->data = block + 1;
    string->capacity = capacity;
    string->length = kept;
    string->data[kept] = 0;
    return true;
}

} // namespace

void rover_string_init(RoverString* string) {
    *string = RoverString{};
}

const char* rover_string_text(const RoverString* string) {
    return string->data ? string->data : "";
}

void rover_string_tidy(RoverString* string, bool built) {
    if (built && string->data) {
        uint8_t& count = share_count(string->data);
        if (count == 0 || count == kFrozen)
            rover_heap_free(&count);
        else
            --count;
    }
    string->data = nullptr;
    string->length = 0;
    string->capacity = 0;
}

void rover_string_release(RoverString* string) {
    rover_string_tidy(string, true);
}

bool rover_string_grow(RoverString* string, uint32_t length, bool trim) {
    if (length > kMaxLength)
        return false;
    if (is_shared(string->data)) {
        if (length == 0) {
            --share_count(string->data);
            rover_string_tidy(string, false);
            return false;
        }
        return copy_to_new_buffer(string, length);
    }
    if (length == 0) {
        if (trim) {
            rover_string_tidy(string, true);
        } else if (string->data) {
            string->length = 0;
            string->data[0] = 0;
        }
        return false;
    }
    if (trim) {
        if (string->capacity > kCapacityRounding || string->capacity < length) {
            rover_string_tidy(string, true);
            return copy_to_new_buffer(string, length);
        }
        return true;
    }
    if (string->capacity < length)
        return copy_to_new_buffer(string, length);
    return true;
}

void rover_string_split(RoverString* string) {
    char* text = string->data;
    if (!is_shared(text))
        return;
    rover_string_tidy(string, true);
    rover_string_assign_text(string, text, static_cast<uint32_t>(strlen(text)));
}

RoverString* rover_string_assign_text(RoverString* string, const char* text, uint32_t length) {
    if (rover_string_grow(string, length, true)) {
        memmove(string->data, text, length);
        string->length = length;
        string->data[length] = 0;
    }
    return string;
}

RoverString* rover_string_assign(RoverString* string, const RoverString* source) {
    if (string == source) {
        rover_string_split(string);
        return string;
    }
    uint32_t length = source->length;
    if (length > 0 && source->data && share_count(source->data) < kUnshareable) {
        rover_string_tidy(string, true);
        string->data = source->data;
        string->length = source->length;
        string->capacity = source->capacity;
        ++share_count(string->data);
        return string;
    }
    if (rover_string_grow(string, length, true)) {
        memmove(string->data, rover_string_text(source), length);
        string->length = length;
        string->data[length] = 0;
    }
    return string;
}

int32_t rover_string_compare(
    const RoverString* string,
    uint32_t position,
    uint32_t count,
    const char* text,
    uint32_t text_length
) {
    if (position > string->length)
        position = string->length;
    uint32_t available = string->length - position;
    if (count > available)
        count = available;
    uint32_t common = count < text_length ? count : text_length;
    int result = common ? memcmp(string->data + position, text, common) : 0;
    if (result != 0)
        return result < 0 ? -1 : 1;
    if (count < text_length)
        return -1;
    return count != text_length ? 1 : 0;
}

bool rover_string_equals(const RoverString* string, const char* text) {
    return rover_string_compare(
               string, 0, string->length, text, static_cast<uint32_t>(strlen(text))
           ) == 0;
}
