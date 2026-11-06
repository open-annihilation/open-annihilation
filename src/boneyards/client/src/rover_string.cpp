// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's string runtime for the objects it passes to Rover. On native builds
// both sides allocate from the C heap, so a buffer Rover shares by bumping its
// count is freed by whichever side drops the last share.
#include "oa/boneyards/rover_string.hpp"

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace oa::boneyards {
namespace {

// A buffer block holds the share count, the characters and a terminator.
constexpr std::size_t kBlockOverhead = 2;

uint8_t& share_count(const RoverString* string) {
    return reinterpret_cast<uint8_t*>(string->data)[-1];
}

bool is_sole_owner(const RoverString* string) {
    const uint8_t count = share_count(string);
    return count == 0 || count == kRoverStringUnshared;
}

} // namespace

void rover_string_release(RoverString* string, bool release_buffer) {
    if (release_buffer && string->data != nullptr) {
        if (is_sole_owner(string))
            std::free(string->data - 1);
        else
            --share_count(string);
    }
    string->data = nullptr;
    string->length = 0;
    string->capacity = 0;
}

RoverString* rover_string_assign(RoverString* string, const char* bytes, uint32_t length) {
    if (rover_string_prepare(string, length, true)) {
        std::memmove(string->data, bytes, length);
        string->length = length;
        string->data[length] = '\0';
    }
    return string;
}

bool rover_string_prepare(RoverString* string, uint32_t length, bool discard) {
    if (string->data != nullptr && !is_sole_owner(string)) {
        if (length == 0) {
            --share_count(string);
            rover_string_release(string, false);
            return false;
        }
        rover_string_reserve(string, length);
        return true;
    }
    if (length == 0) {
        if (discard) {
            rover_string_release(string, true);
        } else if (string->data != nullptr) {
            string->length = 0;
            string->data[0] = '\0';
        }
        return false;
    }
    if (discard) {
        if (string->capacity <= kRoverStringSmallCapacity && string->capacity >= length)
            return true;
        rover_string_release(string, true);
    } else if (string->capacity >= length) {
        return true;
    }
    rover_string_reserve(string, length);
    return true;
}

void rover_string_reserve(RoverString* string, uint32_t length) {
    uint32_t capacity = length | kRoverStringGranule;
    if (capacity > kRoverStringMaxCapacity)
        capacity = length;
    const std::size_t block_size = static_cast<std::size_t>(capacity) + kBlockOverhead;
    auto* block = static_cast<char*>(std::malloc(block_size));
    if (block == nullptr)
        std::abort();
    const uint32_t kept = string->length < capacity ? string->length : capacity;
    if (kept > 0)
        std::memcpy(block + 1, string->data, kept);
    rover_string_release(string, true);
    string->data = block + 1;
    share_count(string) = 0;
    string->capacity = capacity;
    string->length = kept;
    string->data[kept] = '\0';
}

const char* rover_string_text(const RoverString* string) {
    return string->data != nullptr ? string->data : "";
}

} // namespace oa::boneyards
